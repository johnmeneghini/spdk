/*
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NetApp, Inc. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

extern "C" {
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/likely.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk/nvme_kv.h"
#include "spdk/nvmf_transport.h"

}
#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/options.h"
#include "rocksdb/utilities/backupable_db.h"
#include "rocksdb/table.h"
#include "rocksdb/cache.h"

#include "bdev_rocksdb.h"

struct rocksdb_bdev {
	struct spdk_bdev	bdev;
	char *db_path;
	char *db_backup_path;
	uint32_t wbs_mb; /** Write buffer cache in MB */
	bool compression;
	int compaction_style; /** level=0, universal=1, fifo=3, none=3 */
	bool sync_write;
	bool disable_write_ahead;
	uint32_t background_threads_low;
	uint32_t background_threads_high;
	uint32_t cache_size_mb; /** Block cache size in MB */
	uint32_t optimize_compaction_mb; /** memtable memory budget for compaction method */
	rocksdb::DB *db;
	rocksdb::BackupEngine *be;
	rocksdb::WriteOptions writeoptions;
	rocksdb::ReadOptions readoptions;
	TAILQ_ENTRY(rocksdb_bdev)	tailq;
};

static rocksdb::Env *env = rocksdb::Env::Default();

struct rocksdb_io_channel {
	struct spdk_poller		*poller;
	TAILQ_HEAD(, spdk_bdev_io)	io;
};

static TAILQ_HEAD(, rocksdb_bdev) g_rocksdb_bdev_head = TAILQ_HEAD_INITIALIZER(g_rocksdb_bdev_head);

static int bdev_rocksdb_initialize(void);
static void bdev_rocksdb_finish(void);

static struct spdk_bdev_module rocksdb_if = {
	.module_init = bdev_rocksdb_initialize,
	.module_fini = bdev_rocksdb_finish,
	.name = "rocksdb",
	.async_fini = true,
};

SPDK_BDEV_MODULE_REGISTER(rocksdb, &rocksdb_if)

static int
bdev_rocksdb_destruct(void *ctx)
{
	struct rocksdb_bdev *bdev = (struct rocksdb_bdev *)ctx;

	TAILQ_REMOVE(&g_rocksdb_bdev_head, bdev, tailq);
	free(bdev->bdev.name);
	free(bdev);

	return 0;
}

static bool
bdev_rocksdb_abort_io(struct rocksdb_io_channel *ch, struct spdk_bdev_io *bio_to_abort)
{
	struct spdk_bdev_io *bdev_io;

	TAILQ_FOREACH(bdev_io, &ch->io, module_link) {
		if (bdev_io == bio_to_abort) {
			TAILQ_REMOVE(&ch->io, bio_to_abort, module_link);
			spdk_bdev_io_complete(bio_to_abort, SPDK_BDEV_IO_STATUS_ABORTED);
			return true;
		}
	}

	return false;
}

static void bdev_rocksdb_store(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	enum spdk_bdev_io_status status = SPDK_BDEV_IO_STATUS_SUCCESS;
	struct rocksdb_bdev *rocksdb_disk = (struct rocksdb_bdev *)bdev_io->bdev->ctxt;
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)bdev_io->internal.caller_ctx;
	if (SPDK_DEBUGLOG_FLAG_ENABLED("bdev_rocksdb")) {
		char key_str[KV_KEY_STRING_LEN];
		spdk_kv_key_fmt_lower(key_str, sizeof(key_str), &bdev_io->u.kv.key);
		SPDK_DEBUGLOG(bdev_rocksdb, "store key:%s buf:%p, len: %u\n", key_str,
			      bdev_io->u.kv.buffer, bdev_io->u.kv.buffer_len);
	}
	do {
		if (req->cmd->nvme_kv_cmd.cdw11_bits.kv_store.kl != KV_MAX_KEY_SIZE) {
			bdev_io->internal.error.nvme.sc = SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
			status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
			break;
		}
		if (req->cmd->nvme_kv_cmd.cdw11_bits.kv_store.so.no_overwrite) {
			/** TODO: Need to figure out how to do this with rocksdb */
		}
		if (req->cmd->nvme_kv_cmd.cdw11_bits.kv_store.so.overwrite_only) {
			/** TODO: Need to figure out how to do this with rocksdb */
		}
		rocksdb::Status s = rocksdb_disk->db->Put(rocksdb_disk->writeoptions,
				    rocksdb::Slice((char *)&bdev_io->u.kv.key, sizeof(bdev_io->u.kv.key)),
				    rocksdb::Slice((const char *)bdev_io->u.kv.buffer, bdev_io->u.kv.buffer_len));
		if (!s.ok()) {
			/** TODO: I know this is wrong, but until this is a C++ source file this is all the status I can get */
			bdev_io->internal.error.nvme.sc = SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST;
			status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
		}
	} while (0);
	spdk_bdev_io_complete(bdev_io, status);
}

static void bdev_rocksdb_retrieve(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	enum spdk_bdev_io_status status = SPDK_BDEV_IO_STATUS_SUCCESS;
	struct rocksdb_bdev *rocksdb_disk = (struct rocksdb_bdev *)bdev_io->bdev->ctxt;
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)bdev_io->internal.caller_ctx;
	do {
		if (req->cmd->nvme_kv_cmd.cdw11_bits.kv_retrieve.kl != KV_MAX_KEY_SIZE) {
			bdev_io->internal.error.nvme.sc = SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
			status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
			break;
		}
		std::string tmp;
		rocksdb::Status s = rocksdb_disk->db->Get(rocksdb_disk->readoptions,
				    rocksdb::Slice((char *)&bdev_io->u.kv.key, sizeof(bdev_io->u.kv.key)), &tmp);
		if (SPDK_DEBUGLOG_FLAG_ENABLED("bdev_rocksdb")) {
			char key_str[KV_KEY_STRING_LEN];
			spdk_kv_key_fmt_lower(key_str, sizeof(key_str), &bdev_io->u.kv.key);
			SPDK_DEBUGLOG(bdev_rocksdb, "retrieve key:%s buf:%p, len: %zu, buffer_len: %u\n", key_str,
				      tmp.data(), tmp.size(), bdev_io->u.kv.buffer_len);
		}
		if (!s.ok()) {
			if (s.code() == rocksdb::Status::kNotFound) {
				bdev_io->internal.error.nvme.sc = SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST;
				status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
			} else {
				bdev_io->internal.error.nvme.sc = SPDK_NVME_SC_KV_UNRECOVERED_ERROR;
				status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
			}
		} else {
			assert(tmp.data() != NULL);
			memcpy(bdev_io->u.kv.buffer, tmp.data(), spdk_min(tmp.size(), bdev_io->u.kv.buffer_len));
		}
	} while (0);
	spdk_bdev_io_complete(bdev_io, status);
}

static void bdev_rocksdb_delete_key(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	enum spdk_bdev_io_status status = SPDK_BDEV_IO_STATUS_SUCCESS;
	struct rocksdb_bdev *rocksdb_disk = (struct rocksdb_bdev *)bdev_io->bdev->ctxt;
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)bdev_io->internal.caller_ctx;
	do {
		if (SPDK_DEBUGLOG_FLAG_ENABLED("bdev_rocksdb")) {
			char key_str[KV_KEY_STRING_LEN];
			spdk_kv_key_fmt_lower(key_str, sizeof(key_str), &bdev_io->u.kv.key);
			SPDK_DEBUGLOG(bdev_rocksdb, "delete key:%s\n", key_str);
		}
		if (req->cmd->nvme_kv_cmd.cdw11_bits.kv_del.kl != KV_MAX_KEY_SIZE) {
			bdev_io->internal.error.nvme.sc = SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
			status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
			break;
		}
		rocksdb::Status s = rocksdb_disk->db->Delete(rocksdb_disk->writeoptions,
				    rocksdb::Slice((char *)&bdev_io->u.kv.key,
						   sizeof(bdev_io->u.kv.key)));
		if (s.code() == rocksdb::Status::kNotFound) {
			bdev_io->internal.error.nvme.sc = SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST;
			status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
		} else {
			req->rsp->nvme_cpl.status.sc =
				SPDK_NVME_SC_SUCCESS; /** What should be the result in this case? */
		}
	} while (0);
	spdk_bdev_io_complete(bdev_io, status);
}

static void bdev_rocksdb_exist(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	enum spdk_bdev_io_status status = SPDK_BDEV_IO_STATUS_SUCCESS;
	struct rocksdb_bdev *rocksdb_disk = (struct rocksdb_bdev *)bdev_io->bdev->ctxt;
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)bdev_io->internal.caller_ctx;
	if (SPDK_DEBUGLOG_FLAG_ENABLED("bdev_rocksdb")) {
		char key_str[KV_KEY_STRING_LEN];
		spdk_kv_key_fmt_lower(key_str, sizeof(key_str), &bdev_io->u.kv.key);
		SPDK_DEBUGLOG(bdev_rocksdb, "exist key:%s\n", key_str);
	}
	do {
		rocksdb::Iterator *iter = rocksdb_disk->db->NewIterator(rocksdb_disk->readoptions);

		if (!iter) {
			SPDK_ERRLOG("rocksdb exist failed to allocate iter\n");
			status = SPDK_BDEV_IO_STATUS_FAILED;
			break;
		}
		iter->Seek(rocksdb::Slice((char *)&bdev_io->u.kv.key, sizeof(bdev_io->u.kv.key)));
		if (!iter->Valid() || iter->status().code() == rocksdb::Status::kNotFound) {
			/** TODO: I know this is wrong, but until this is a C++ source file this is all the status I can get */
			bdev_io->internal.error.nvme.sc = SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST;
			status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
			break;
		}
		rocksdb::Slice key = iter->key();
		if (key.size() != KV_MAX_KEY_SIZE) {
			SPDK_ERRLOG("Invalid key length %zu\n", key.size());
			/** What should be the result in this case? */
			bdev_io->internal.error.nvme.sc = SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
			status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
			break;
		}
		if (memcmp(key.data(), &bdev_io->u.kv.key, spdk_min(key.size(),
				req->cmd->nvme_kv_cmd.cdw11_bits.kv_exist.kl)) == 0) {
			req->rsp->nvme_cpl.status.sc =
				SPDK_NVME_SC_SUCCESS;
		} else {
			bdev_io->internal.error.nvme.sc = SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST;
			status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
		}
		delete iter;
	} while (0);
	spdk_bdev_io_complete(bdev_io, status);
}

static void bdev_rocksdb_list(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	enum spdk_bdev_io_status status = SPDK_BDEV_IO_STATUS_SUCCESS;
	struct rocksdb_bdev *rocksdb_disk = (struct rocksdb_bdev *)bdev_io->bdev->ctxt;
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)bdev_io->internal.caller_ctx;
	do {
		if (req->cmd->nvme_kv_cmd.cdw11_bits.kv_list.kl != KV_MAX_KEY_SIZE) {
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
			break;
		}
		rocksdb::Iterator *iter = rocksdb_disk->db->NewIterator(rocksdb_disk->readoptions);
		if (!iter) {
			SPDK_ERRLOG("rocksdb exist failed to allocate iter\n");
			status = SPDK_BDEV_IO_STATUS_FAILED;
			break;
		}

		iter->Seek(rocksdb::Slice((char *)&bdev_io->u.kv.key, sizeof(bdev_io->u.kv.key)));
		if (!iter->status().ok() && iter->status().code() != rocksdb::Status::kNotFound) {
			bdev_io->internal.error.nvme.sc = SPDK_NVME_SC_KV_UNRECOVERED_ERROR;
			status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
			break;
		}
		struct spdk_nvme_kv_ns_list_data *list_data = (struct spdk_nvme_kv_ns_list_data *)
				bdev_io->u.kv.buffer;
		uint32_t bytes_left = bdev_io->u.kv.buffer_len;
		if (bytes_left < sizeof(*list_data)) {
			/** Is there a better status code? */
			bdev_io->internal.error.nvme.sc = SPDK_NVME_SC_INVALID_FIELD;
			status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
			break;
		}
		list_data->nrk = 0;
		bytes_left -= sizeof(list_data->nrk);
		struct spdk_nvme_kv_ns_list_key_data *key_data = &list_data->keys[0];
		while (iter->Valid() && bytes_left >= sizeof(struct spdk_nvme_kv_ns_list_key_data)) {
			rocksdb::Slice key = iter->key();
			if (key.size() != KV_MAX_KEY_SIZE) {
				SPDK_ERRLOG("Invalid key length %zu\n", key.size());
				/** What should be the result in this case? */
				bdev_io->internal.error.nvme.sc = SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
				status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
				break;
			}
			key_data->kl = KV_MAX_KEY_SIZE;
			memcpy(&key_data->key, key.data(), KV_MAX_KEY_SIZE);
			list_data->nrk++;
			key_data++;
			bytes_left -= sizeof(struct spdk_nvme_kv_ns_list_key_data);
			iter->Next();
		}


		delete iter;
	} while (0);

	spdk_bdev_io_complete(bdev_io, status);
}

static void
bdev_rocksdb_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct rocksdb_io_channel *ch = (struct rocksdb_io_channel *)spdk_io_channel_get_ctx(_ch);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_KV_RETRIEVE:
		bdev_rocksdb_retrieve(_ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_KV_STORE:
		bdev_rocksdb_store(_ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_KV_EXIST:
		bdev_rocksdb_exist(_ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_KV_LIST:
		bdev_rocksdb_list(_ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_KV_DELETE:
		bdev_rocksdb_delete_key(_ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		if (bdev_rocksdb_abort_io(ch, bdev_io->u.abort.bio_to_abort)) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		} else {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
		break;
	default:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

static bool
bdev_rocksdb_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_KV_RETRIEVE:
	case SPDK_BDEV_IO_TYPE_KV_STORE:
	case SPDK_BDEV_IO_TYPE_KV_EXIST:
	case SPDK_BDEV_IO_TYPE_KV_LIST:
	case SPDK_BDEV_IO_TYPE_KV_DELETE:
		return true;
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_rocksdb_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_rocksdb_bdev_head);
}

static void
bdev_rocksdb_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct rocksdb_bdev *rocksdb_disk = (struct rocksdb_bdev *)bdev;
	char uuid_str[SPDK_UUID_STRING_LEN];

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_rocksdb_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_string(w, "db_path", rocksdb_disk->db_path);
	if (rocksdb_disk->db_backup_path) {
		spdk_json_write_named_string(w, "db_backup_path", rocksdb_disk->db_backup_path);
	}
	spdk_json_write_named_uint32(w, "wbs_mb", rocksdb_disk->wbs_mb);
	spdk_json_write_named_bool(w, "compression", rocksdb_disk->compression);
	spdk_json_write_named_uint32(w, "compaction_style", rocksdb_disk->compaction_style);
	spdk_json_write_named_bool(w, "sync_write", rocksdb_disk->sync_write);
	spdk_json_write_named_bool(w, "disable_write_ahead", rocksdb_disk->disable_write_ahead);
	spdk_json_write_named_uint32(w, "background_threads_low", rocksdb_disk->background_threads_low);
	spdk_json_write_named_uint32(w, "background_threads_high", rocksdb_disk->background_threads_high);
	spdk_json_write_named_uint32(w, "cache_size_mb", rocksdb_disk->cache_size_mb);
	spdk_json_write_named_uint32(w, "optimize_compaction_mb", rocksdb_disk->optimize_compaction_mb);
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);
	spdk_json_write_named_string(w, "uuid", uuid_str);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table rocksdb_fn_table = {
	.destruct		= bdev_rocksdb_destruct,
	.submit_request		= bdev_rocksdb_submit_request,
	.io_type_supported	= bdev_rocksdb_io_type_supported,
	.get_io_channel		= bdev_rocksdb_get_io_channel,
	.write_config_json	= bdev_rocksdb_write_config_json,
};

int
bdev_rocksdb_create(struct spdk_bdev **bdev, const struct spdk_rocksdb_bdev_opts *opts)
{
	struct rocksdb_bdev *rocksdb_disk;
	int rc;
	rocksdb::Options options;

	if (!opts) {
		SPDK_ERRLOG("No options provided for Null KV bdev.\n");
		return -EINVAL;
	}

	if (opts->db_path == 0 || strlen(opts->db_path) == 0) {
		SPDK_ERRLOG("No db path specified\n");
		return -EINVAL;
	}

	rocksdb_disk = (struct rocksdb_bdev *)calloc(1, sizeof(*rocksdb_disk));
	if (!rocksdb_disk) {
		SPDK_ERRLOG("could not allocate rocksdb_bdev\n");
		return -ENOMEM;
	}

	rocksdb_disk->bdev.name = strdup(opts->name);
	if (!rocksdb_disk->bdev.name) {
		free(rocksdb_disk);
		return -ENOMEM;
	}

	rocksdb_disk->db_path = strdup(opts->db_path);
	if (!rocksdb_disk->db_path) {
		free(rocksdb_disk);
		return -ENOMEM;
	}
	if (rocksdb_disk->db_backup_path) {
		rocksdb_disk->db_backup_path = strdup(opts->db_backup_path);
		if (!rocksdb_disk->db_backup_path) {
			free(rocksdb_disk);
			return -ENOMEM;
		}
	}
	rocksdb_disk->compression = opts->compression;
	rocksdb_disk->wbs_mb = opts->wbs_mb;
	rocksdb_disk->compaction_style = opts->compaction_style;
	rocksdb_disk->sync_write = opts->sync_write;
	rocksdb_disk->disable_write_ahead = opts->disable_write_ahead;
	rocksdb_disk->background_threads_low = opts->background_threads_low;
	rocksdb_disk->background_threads_high = opts->background_threads_high;
	rocksdb_disk->cache_size_mb = opts->cache_size_mb;
	rocksdb_disk->optimize_compaction_mb = opts->optimize_compaction_mb;

	rocksdb_disk->bdev.product_name = strdup("KV Rocksdb disk");

	rocksdb_disk->bdev.write_cache = 0;
	rocksdb_disk->bdev.blocklen = 1;
	rocksdb_disk->bdev.blockcnt = UINT64_MAX;
	if (opts->uuid) {
		rocksdb_disk->bdev.uuid = *opts->uuid;
	} else {
		spdk_uuid_generate(&rocksdb_disk->bdev.uuid);
	}

	rocksdb_disk->bdev.ctxt = rocksdb_disk;
	rocksdb_disk->bdev.fn_table = &rocksdb_fn_table;
	rocksdb_disk->bdev.module = &rocksdb_if;

	rc = spdk_bdev_register(&rocksdb_disk->bdev);
	if (rc) {
		free(rocksdb_disk->bdev.name);
		free(rocksdb_disk);
		return rc;
	}

	long cpus = sysconf(_SC_NPROCESSORS_ONLN);  /** get # of online cores */
	if (rocksdb_disk->background_threads_low != 0) {
		options.max_background_jobs = rocksdb_disk->background_threads_low;
		env->SetBackgroundThreads(rocksdb_disk->background_threads_low, rocksdb::Env::LOW);
	} else {
		options.max_background_jobs = spdk_max(cpus / 2, 1);
		env->SetBackgroundThreads(options.max_background_jobs, rocksdb::Env::LOW);
	}
	if (rocksdb_disk->background_threads_high != 0) {
		env->SetBackgroundThreads(rocksdb_disk->background_threads_high, rocksdb::Env::HIGH);
	} else {
		env->SetBackgroundThreads(1, rocksdb::Env::HIGH);
	}

	if (!rocksdb_disk->compression) {
		options.compression = rocksdb::kNoCompression;
	}

	options.max_background_jobs = 2;
	options.max_write_buffer_number = 2;
	options.write_buffer_size = rocksdb_disk->wbs_mb << 20; /* Convert to bytes */
	options.compaction_style = rocksdb::CompactionStyle(rocksdb_disk->compaction_style);
	if (rocksdb_disk->optimize_compaction_mb) {
		if (options.compaction_style == rocksdb::kCompactionStyleLevel) {
			options.OptimizeLevelStyleCompaction(512 * 1024  * 1024);
		}
		if (options.compaction_style == rocksdb::kCompactionStyleUniversal) {
			options.OptimizeUniversalStyleCompaction(512 * 1024  * 1024);
		}
	}
	options.max_open_files = 500000;

	options.max_background_compactions = 4;
	options.max_background_flushes = 2;
	options.bytes_per_sync = 1048576;
	options.compaction_pri = rocksdb::kMinOverlappingRatio;

	rocksdb::BlockBasedTableOptions table_options;
	table_options.block_size = 16 * 1024;
	table_options.cache_index_and_filter_blocks = true;
	table_options.pin_l0_filter_and_index_blocks_in_cache = true;
	if (rocksdb_disk->cache_size_mb) {
		table_options.block_cache = rocksdb::NewLRUCache(rocksdb_disk->cache_size_mb, 6, false, 0.0);

	}
	options.table_factory.reset(
		rocksdb::NewBlockBasedTableFactory(table_options));

	rocksdb_disk->writeoptions.sync = rocksdb_disk->sync_write;
	rocksdb_disk->writeoptions.disableWAL = rocksdb_disk->disable_write_ahead;

	/** create the DB if it's not already present */
	options.create_if_missing = true;

	/** open DB */
	rocksdb::Status s = rocksdb::DB::Open(options, rocksdb_disk->db_path, &rocksdb_disk->db);
	if (!s.ok()) {
		SPDK_ERRLOG("%s\n", s.ToString().c_str());
		free(rocksdb_disk);
		return -EINVAL;
	}

	if (rocksdb_disk->db_backup_path) {
		/** open Backup Engine that we will use for backing up our database */
		rocksdb::Status s = rocksdb::BackupEngine::Open(options.env,
				    rocksdb::BackupableDBOptions(rocksdb_disk->db_backup_path, nullptr, true, options.info_log.get()),
				    &rocksdb_disk->be);
		if (!s.ok()) {
			SPDK_ERRLOG("%s\n", s.ToString().c_str());
			free(rocksdb_disk);
			return -EINVAL;
		}
	}

	*bdev = &(rocksdb_disk->bdev);

	TAILQ_INSERT_TAIL(&g_rocksdb_bdev_head, rocksdb_disk, tailq);

	return rc;
}

void
bdev_rocksdb_delete(struct spdk_bdev *bdev, spdk_delete_null_complete cb_fn, void *cb_arg)
{
	struct rocksdb_bdev *rocksdb_disk = (struct rocksdb_bdev *)bdev;
	if (!bdev || bdev->module != &rocksdb_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}
	delete rocksdb_disk->be;
	delete rocksdb_disk->db;

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

static int
null_io_poll(void *arg)
{
	struct rocksdb_io_channel		*ch = (struct rocksdb_io_channel *)arg;
	TAILQ_HEAD(, spdk_bdev_io)	io;
	struct spdk_bdev_io		*bdev_io;

	TAILQ_INIT(&io);
	TAILQ_SWAP(&ch->io, &io, spdk_bdev_io, module_link);

	if (TAILQ_EMPTY(&io)) {
		return SPDK_POLLER_IDLE;
	}

	while (!TAILQ_EMPTY(&io)) {
		bdev_io = TAILQ_FIRST(&io);
		TAILQ_REMOVE(&io, bdev_io, module_link);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	}

	return SPDK_POLLER_BUSY;
}

static int
rocksdb_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct rocksdb_io_channel *ch = (struct rocksdb_io_channel *)ctx_buf;

	TAILQ_INIT(&ch->io);
	ch->poller = SPDK_POLLER_REGISTER(null_io_poll, ch, 0);

	return 0;
}

static void
rocksdb_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct rocksdb_io_channel *ch = (struct rocksdb_io_channel *)ctx_buf;

	spdk_poller_unregister(&ch->poller);
}

static int
bdev_rocksdb_initialize(void)
{

	/*
	 * We need to pick some unique address as our "io device" - so just use the
	 *  address of the global tailq.
	 */
	spdk_io_device_register(&g_rocksdb_bdev_head, rocksdb_bdev_create_cb, rocksdb_bdev_destroy_cb,
				sizeof(struct rocksdb_io_channel), "bdev_rocksdb");

	return 0;
}

static void
_bdev_rocksdb_finish_cb(void *arg)
{
	spdk_bdev_module_finish_done();
}

static void
bdev_rocksdb_finish(void)
{
	spdk_io_device_unregister(&g_rocksdb_bdev_head, _bdev_rocksdb_finish_cb);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_rocksdb)
