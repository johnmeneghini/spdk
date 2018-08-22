/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Netapp.
 *   All rights reserved.
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

#include "spdk/bdev.h"

#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/nvme_spec.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"
#include "spdk/string.h"
/* TODO: As part of RDMA, the abstractions for both
 * transports to be built out here and this should
 * be removed.
 */
#include "../nvmf/nvmf_fc/bcm_fc.h"

typedef TAILQ_HEAD(, spdk_bdev_io) need_buf_tailq_t;

struct spdk_bdev_mgr {

	TAILQ_HEAD(, spdk_bdev_module_if) bdev_modules;

	TAILQ_HEAD(, spdk_bdev) bdevs;

	struct spdk_mempool *buf_small_pool;

	spdk_bdev_poller_start_cb start_poller_fn;
	spdk_bdev_poller_stop_cb stop_poller_fn;

	bool init_complete;
	bool module_init_complete;

};

static struct spdk_bdev_mgr g_bdev_mgr = {
	.bdev_modules = TAILQ_HEAD_INITIALIZER(g_bdev_mgr.bdev_modules),
	.bdevs = TAILQ_HEAD_INITIALIZER(g_bdev_mgr.bdevs),
	.start_poller_fn = NULL,
	.stop_poller_fn = NULL,
	.init_complete = false,
	.module_init_complete = false,
};

static spdk_bdev_init_cb	g_cb_fn = NULL;
static void			*g_cb_arg = NULL;


struct spdk_bdev_desc {
	struct spdk_bdev		*bdev;
	spdk_bdev_remove_cb_t		remove_cb;
	void				*remove_ctx;
	bool				write;
	TAILQ_ENTRY(spdk_bdev_desc)	link;
};

struct spdk_bdev_channel {
	struct spdk_bdev	*bdev;

	/* The channel for the underlying device */
	struct spdk_io_channel	*channel;

	/* Channel for the bdev manager */
	struct spdk_io_channel *mgmt_channel;

	struct spdk_bdev_io_stat stat;

	/*
	 * Count of I/O submitted to bdev module and waiting for completion.
	 * Incremented before submit_request() is called on an spdk_bdev_io.
	 */
	uint64_t		io_outstanding;

};

struct spdk_bdev *
spdk_bdev_first(void)
{
	struct spdk_bdev *bdev;

	bdev = TAILQ_FIRST(&g_bdev_mgr.bdevs);
	if (bdev) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Starting bdev iteration at %s\n", bdev->name);
	}

	return bdev;
}

struct spdk_bdev *
spdk_bdev_next(struct spdk_bdev *prev)
{
	struct spdk_bdev *bdev;

	bdev = TAILQ_NEXT(prev, link);
	if (bdev) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Continuing bdev iteration at %s\n", bdev->name);
	}

	return bdev;
}

struct spdk_bdev *
spdk_bdev_get_by_name(const char *bdev_name)
{
	struct spdk_bdev *bdev = spdk_bdev_first();

	while (bdev != NULL) {
		if (strncmp(bdev_name, bdev->name, SPDK_BDEV_MAX_NAME_LENGTH) == 0) {

			return bdev;
		}
		bdev = spdk_bdev_next(bdev);
	}

	return NULL;
}

int
spdk_bdev_module_get_max_ctx_size(void)
{
	struct spdk_bdev_module_if *bdev_module;
	int max_bdev_module_size = 0;

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.bdev_modules, tailq) {
		if (bdev_module->get_ctx_size && bdev_module->get_ctx_size() > max_bdev_module_size) {
			max_bdev_module_size = bdev_module->get_ctx_size();
		}
	}

	return max_bdev_module_size;
}

void
spdk_bdev_config_text(FILE *fp)
{
	struct spdk_bdev_module_if *bdev_module;

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.bdev_modules, tailq) {
		if (bdev_module->config_text) {
			bdev_module->config_text(fp);
		}
	}
}

static int
spdk_bdev_get_buff(struct iovec *iov, int32_t *iovcnt, int32_t length)
{
	struct spdk_mempool *pool;
	void *buf = NULL;
	int32_t i = 0, max_buff_len;

	if (!iov) {
		goto error;
	}

	*iovcnt = 0;

	while (length) {
		/* Always use from small buff to be in sync with netapp wafl */
		pool = g_bdev_mgr.buf_small_pool;
		max_buff_len = SPDK_BDEV_SMALL_BUF_MAX_SIZE;

		buf = spdk_mempool_get(pool);
		if (buf) {
			iov[i].iov_base = buf;
			iov[i].iov_len  = spdk_min(length, max_buff_len);
			++ *iovcnt;
		} else {
			while (i) {
				i --;
				spdk_mempool_put(pool, iov[i].iov_base);
				iov[i].iov_base = NULL;
				iov[i].iov_len = 0;
			}
			*iovcnt = 0;
			goto error;
		}

		length -= iov[i].iov_len;
		i ++;
	}

	return 0;
error:
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Bdev buffer allocation failed\n");
	return -1;
}

static void
spdk_bdev_init_complete(int rc)
{
	spdk_bdev_init_cb cb_fn = g_cb_fn;
	void *cb_arg = g_cb_arg;

	g_bdev_mgr.init_complete = true;
	g_cb_fn = NULL;
	g_cb_arg = NULL;

	cb_fn(cb_arg, rc);
}

static void
spdk_bdev_module_init_complete(int rc)
{
	struct spdk_bdev_module_if *m;

	g_bdev_mgr.module_init_complete = true;

	if (rc != 0) {
		spdk_bdev_init_complete(rc);
	}

	/*
	 * Check all bdev modules for an examinations in progress.  If any
	 * exist, return immediately since we cannot finish bdev subsystem
	 * initialization until all are completed.
	 */
	TAILQ_FOREACH(m, &g_bdev_mgr.bdev_modules, tailq) {
		if (m->examine_in_progress > 0) {
			return;
		}
	}

	spdk_bdev_init_complete(0);
}

static int
spdk_bdev_modules_init(void)
{
	struct spdk_bdev_module_if *module;
	int rc;

	TAILQ_FOREACH(module, &g_bdev_mgr.bdev_modules, tailq) {
		rc = module->module_init();
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

#define BUF_SMALL_POOL_SIZE     8192

void
spdk_bdev_initialize(spdk_bdev_init_cb cb_fn, void *cb_arg,
		     spdk_bdev_poller_start_cb start_poller_fn,
		     spdk_bdev_poller_stop_cb stop_poller_fn)
{
	int rc = 0;
	int cache_size;

	assert(cb_fn != NULL);

	cache_size = BUF_SMALL_POOL_SIZE / (2 * spdk_env_get_core_count());
	g_bdev_mgr.buf_small_pool = spdk_mempool_create("buf_small_pool",
				    BUF_SMALL_POOL_SIZE,
				    SPDK_BDEV_SMALL_BUF_MAX_SIZE + 512,
				    cache_size,
				    SPDK_ENV_SOCKET_ID_ANY);

	if (!g_bdev_mgr.buf_small_pool) {
		SPDK_ERRLOG("create buf small pool failed\n");
		spdk_bdev_module_init_complete(-1);
		return;
	}

	g_cb_fn = cb_fn;
	g_cb_arg = cb_arg;

	g_bdev_mgr.start_poller_fn = start_poller_fn;
	g_bdev_mgr.stop_poller_fn = stop_poller_fn;

	rc = spdk_bdev_modules_init();
	spdk_bdev_module_init_complete(rc);
}

int
spdk_bdev_finish(void)
{
	struct spdk_bdev_module_if *bdev_module;

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.bdev_modules, tailq) {
		if (bdev_module->module_fini) {
			bdev_module->module_fini();
		}
	}

	spdk_mempool_free(g_bdev_mgr.buf_small_pool);

	return 0;
}

static struct spdk_bdev_io *
spdk_bdev_get_io(struct spdk_mempool *pool)
{
	struct spdk_bdev_io *bdev_io = NULL;

	if (pool == NULL) {
		assert(0);
	}
	bdev_io = spdk_mempool_get(pool);

	if (!bdev_io) {
		SPDK_ERRLOG("Unable to get spdk_bdev_io\n");
		assert("Unable to get spdk_bdev_io" == 0);
		goto out;
	}
	memset(bdev_io, 0, sizeof(*bdev_io));

	bdev_io->bdev_io_pool = pool;

out:
	return bdev_io;
}

static void
spdk_bdev_put_io(struct spdk_bdev_io *bdev_io)
{
	if (!bdev_io) {
		return;
	}
	spdk_mempool_put(bdev_io->bdev_io_pool, (void *)bdev_io);
}

static void
__submit_request(struct spdk_bdev *bdev, struct spdk_bdev_io *bdev_io)
{
	struct spdk_io_channel *ch = NULL;

	assert(bdev_io->status == SPDK_BDEV_IO_STATUS_PENDING);

	bdev_io->in_submit_request = true;
	bdev->fn_table->submit_request(ch, bdev_io);
	bdev_io->in_submit_request = false;
}

static int
spdk_bdev_io_submit(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;

	__submit_request(bdev, bdev_io);
	return 0;
}

static void
spdk_bdev_io_init(struct spdk_bdev_io *bdev_io,
		  struct spdk_bdev *bdev, void *cb_arg,
		  spdk_bdev_io_completion_cb cb)
{
	bdev_io->bdev = bdev;
	bdev_io->caller_ctx = cb_arg;
	bdev_io->cb = cb;
	bdev_io->gencnt = bdev->gencnt;
	bdev_io->status = SPDK_BDEV_IO_STATUS_PENDING;
	bdev_io->in_submit_request = false;
}

bool
spdk_bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	return bdev->fn_table->io_type_supported(bdev->ctxt, io_type);
}

uint8_t
spdk_bdev_get_ana_state(struct spdk_bdev *bdev, uint16_t cntlid)
{
	if (bdev && bdev->fn_table && bdev->fn_table->get_ana_state) {
		return bdev->fn_table->get_ana_state(bdev->ctxt, cntlid);
	}
	SPDK_ERRLOG("No callback registered returning ANA state optimized\n");
	return SPDK_NVME_ANA_OPTIMIZED;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return bdev->name;
}

const char *
spdk_bdev_get_product_name(const struct spdk_bdev *bdev)
{
	return bdev->product_name;
}

uint32_t
spdk_bdev_get_block_size(const struct spdk_bdev *bdev)
{
	return bdev->blocklen;
}

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return bdev->blockcnt;
}

uint32_t
spdk_bdev_get_max_unmap_descriptors(const struct spdk_bdev *bdev)
{
	return bdev->max_unmap_bdesc_count;
}

size_t
spdk_bdev_get_buf_align(const struct spdk_bdev *bdev)
{
	/* TODO: push this logic down to the bdev modules */
	if (bdev->need_aligned_buffer) {
		return bdev->blocklen;
	}

	return 1;
}

bool
spdk_bdev_has_write_cache(const struct spdk_bdev *bdev)
{
	return bdev->write_cache;
}

static int
spdk_bdev_io_valid(struct spdk_bdev *bdev, uint64_t offset, uint64_t nbytes)
{
	/* Return failure if nbytes is not a multiple of bdev->blocklen */
	if (nbytes % bdev->blocklen) {
		return -1;
	}

	/* Return failure if offset + nbytes is less than offset; indicates there
	 * has been an overflow and hence the offset has been wrapped around */
	if (offset + nbytes < offset) {
		return -1;
	}

	/* Return failure if offset + nbytes exceeds the size of the bdev */
	if (offset + nbytes > bdev->blockcnt * bdev->blocklen) {
		return -1;
	}

	return 0;
}

int
spdk_bdev_read(struct spdk_bdev_desc *desc, struct spdk_mempool *bdev_io_pool,
	       struct spdk_io_channel *ch,
	       void *buf, uint64_t offset, uint64_t nbytes,
	       spdk_bdev_io_completion_cb cb, void *cb_arg, struct spdk_bdev_io **result_bdev_io)
{
	assert("Netapp uses readv" == 0);
	return 0;
}

int
spdk_bdev_readv(struct spdk_bdev_io *bdev_io)
{
	int rc;

	rc = spdk_bdev_io_submit(bdev_io);

	return rc;
}

int
spdk_bdev_write(struct spdk_bdev_desc *desc, struct spdk_mempool *bdev_io_pool,
		struct spdk_io_channel *ch,
		void *buf, uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg, struct spdk_bdev_io **result_bdev_io)
{
	assert("Netapp uses writev" == 0);
	return 0;
}

int
spdk_bdev_writev(struct spdk_bdev_io *bdev_io)
{
	int rc;

	rc = spdk_bdev_io_submit(bdev_io);

	return rc;
}

int
spdk_bdev_unmap(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct spdk_scsi_unmap_bdesc *unmap_d,
		uint16_t bdesc_count,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	int rc;

	if (!desc->write) {
		return -EBADF;
	}

	if (bdesc_count == 0) {
		SPDK_ERRLOG("Invalid bdesc_count 0\n");
		return -EINVAL;
	}

	if (bdesc_count > bdev->max_unmap_bdesc_count) {
		SPDK_ERRLOG("Invalid bdesc_count %u > max_unmap_bdesc_count %u\n",
			    bdesc_count, bdev->max_unmap_bdesc_count);
		return -EINVAL;
	}

	bdev_io = spdk_bdev_get_io(NULL);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing unmap\n");
		return -ENOMEM;
	}

	bdev_io->ch = NULL;
	bdev_io->type = SPDK_BDEV_IO_TYPE_UNMAP;
	bdev_io->u.unmap.unmap_bdesc = unmap_d;
	bdev_io->u.unmap.bdesc_count = bdesc_count;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	rc = spdk_bdev_io_submit(bdev_io);
	if (rc < 0) {
		spdk_bdev_put_io(bdev_io);
		return rc;
	}

	return 0;
}

int
spdk_bdev_flush(struct spdk_bdev_desc *desc, struct spdk_mempool *bdev_io_pool,
		struct spdk_io_channel *ch,
		uint64_t offset, uint64_t length,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	int rc;

	if (!desc->write) {
		return -EBADF;
	}

	assert(bdev->status != SPDK_BDEV_STATUS_INVALID);
	bdev_io = spdk_bdev_get_io(bdev_io_pool);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing flush\n");
		return -ENOMEM;
	}

	bdev_io->ch = NULL;
	bdev_io->type = SPDK_BDEV_IO_TYPE_FLUSH;
	bdev_io->u.flush.offset = offset;
	bdev_io->u.flush.length = length;
	bdev_io->bdev_io_pool = bdev_io_pool;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	rc = spdk_bdev_io_submit(bdev_io);
	if (rc < 0) {
		spdk_bdev_put_io(bdev_io);
		return rc;
	}

	return 0;
}

int
spdk_bdev_nvme_admin_passthru(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      const struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	int rc;

	if (!desc->write) {
		return -EBADF;
	}

	bdev_io = spdk_bdev_get_io(NULL);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed during nvme_admin_passthru\n");
		return -ENOMEM;
	}

	bdev_io->ch = NULL;
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_ADMIN;
	bdev_io->u.nvme_passthru.cmd = *cmd;
	bdev_io->u.nvme_passthru.buf = buf;
	bdev_io->u.nvme_passthru.nbytes = nbytes;

	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	rc = spdk_bdev_io_submit(bdev_io);
	if (rc < 0) {
		spdk_bdev_put_io(bdev_io);
		return rc;
	}

	return 0;
}

int
spdk_bdev_nvme_io_passthru(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   const struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes,
			   spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	int rc;

	if (!desc->write) {
		/*
		 * Do not try to parse the NVMe command - we could maybe use bits in the opcode
		 *  to easily determine if the command is a read or write, but for now just
		 *  do not allow io_passthru with a read-only descriptor.
		 */
		return -EBADF;
	}

	bdev_io = spdk_bdev_get_io(NULL);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed during nvme_admin_passthru\n");
		return -ENOMEM;
	}

	bdev_io->ch = NULL;
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_IO;
	bdev_io->u.nvme_passthru.cmd = *cmd;
	bdev_io->u.nvme_passthru.buf = buf;
	bdev_io->u.nvme_passthru.nbytes = nbytes;

	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	rc = spdk_bdev_io_submit(bdev_io);
	if (rc < 0) {
		spdk_bdev_put_io(bdev_io);
		return rc;
	}

	return 0;
}

int
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io is NULL\n");
		return -1;
	}

	spdk_bdev_put_io(bdev_io);

	return 0;
}

void
bdev_io_deferred_completion(void *arg1, void *arg2)
{
	struct spdk_bdev_io *bdev_io = arg1;
	enum spdk_bdev_io_status status = (enum spdk_bdev_io_status)arg2;

	assert(bdev_io->in_submit_request == false);

	spdk_bdev_io_complete(bdev_io, status);
}

static void
_spdk_bdev_io_complete(void *ctx, void *arg2)
{
	struct spdk_bdev_io *bdev_io = ctx;

	assert(bdev_io->cb != NULL);
	bdev_io->cb(bdev_io, bdev_io->status == SPDK_BDEV_IO_STATUS_SUCCESS, bdev_io->caller_ctx);
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	bdev_io->status = status;
	/*
	 * TODO: Have to abstract the type of request when RDMA/TCP come along.
	 * Will have to use a transport API.
	 */
	struct spdk_nvmf_bcm_fc_request *fc_req = (struct spdk_nvmf_bcm_fc_request *)bdev_io->caller_ctx;
	struct spdk_event *event = NULL;

	/*
	 * Check the gencnt, to see if this I/O was issued before the most
	 * recent reset. If the gencnt is not equal, then just free the I/O
	 * without calling the callback, since the caller will have already
	 * freed its context for this I/O.
	 */
	if (bdev_io->bdev->gencnt != bdev_io->gencnt) {
		spdk_bdev_put_io(bdev_io);
		return;
	}

	if (bdev_io->in_submit_request) {
		/*
		 * Defer completion to avoid potential infinite recursion if the
		 * user's completion callback issues a new I/O.
		 */
		event = spdk_event_allocate(fc_req->poller_lcore,
					    _spdk_bdev_io_complete,
					    (void *)bdev_io, NULL);
		spdk_post_event(fc_req->hwqp->context, event);
	} else {
		_spdk_bdev_io_complete(bdev_io, NULL);
	}
}

void
spdk_bdev_io_complete_nvme_status(struct spdk_bdev_io *bdev_io, int sct, int sc)
{
	if (sct == SPDK_NVME_SCT_GENERIC && sc == SPDK_NVME_SC_SUCCESS) {
		bdev_io->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	} else {
		bdev_io->error.nvme.sct = sct;
		bdev_io->error.nvme.sc = sc;
		bdev_io->status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
	}

	spdk_bdev_io_complete(bdev_io, bdev_io->status);
}

void
spdk_bdev_io_get_nvme_status(const struct spdk_bdev_io *bdev_io, int *sct, int *sc, int *dnr)
{
	assert(sct != NULL);
	assert(sc != NULL);

	if (sct == NULL || sc == NULL) {
		return;
	}

	if (bdev_io->status == SPDK_BDEV_IO_STATUS_NVME_ERROR) {
		*sct = bdev_io->error.nvme.sct;
		*sc = bdev_io->error.nvme.sc;
		*dnr = bdev_io->error.nvme.dnr;
	} else if (bdev_io->status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		*sct = SPDK_NVME_SCT_GENERIC;
		*sc = SPDK_NVME_SC_SUCCESS;
		*dnr = 0;
	} else {
		*sct = SPDK_NVME_SCT_GENERIC;
		*sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		*dnr = 1;
	}
}

static void
_spdk_bdev_register(struct spdk_bdev *bdev)
{
	struct spdk_bdev_module_if *module;

	assert(bdev->module != NULL);

	bdev->status = SPDK_BDEV_STATUS_READY;

	/* initialize the reset generation value to zero */
	bdev->gencnt = 0;
	TAILQ_INIT(&bdev->open_descs);
	bdev->bdev_opened_for_write = false;

	TAILQ_INIT(&bdev->vbdevs);
	TAILQ_INIT(&bdev->base_bdevs);

	bdev->reset_in_progress = false;
	TAILQ_INIT(&bdev->queued_resets);


	pthread_mutex_init(&bdev->mutex, NULL);
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Inserting bdev %s into list\n", bdev->name);
	TAILQ_INSERT_TAIL(&g_bdev_mgr.bdevs, bdev, link);

	TAILQ_FOREACH(module, &g_bdev_mgr.bdev_modules, tailq) {
		if (module->examine) {
			module->examine_in_progress++;
			module->examine(bdev);
		}
	}
}

void
spdk_bdev_register(struct spdk_bdev *bdev)
{
	_spdk_bdev_register(bdev);
}

void
spdk_bdev_unregister(struct spdk_bdev *bdev)
{
	struct spdk_bdev_desc	*desc, *tmp;
	int			rc;
	bool			do_destruct = true;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Removing bdev %s from list\n", bdev->name);

	pthread_mutex_lock(&bdev->mutex);

	bdev->status = SPDK_BDEV_STATUS_REMOVING;

	TAILQ_FOREACH_SAFE(desc, &bdev->open_descs, link, tmp) {
		if (desc->remove_cb) {
			pthread_mutex_unlock(&bdev->mutex);
			do_destruct = false;
			desc->remove_cb(desc->remove_ctx);
			pthread_mutex_lock(&bdev->mutex);
		}
	}

	if (!do_destruct) {
		pthread_mutex_unlock(&bdev->mutex);
		return;
	}

	TAILQ_REMOVE(&g_bdev_mgr.bdevs, bdev, link);
	pthread_mutex_unlock(&bdev->mutex);

	pthread_mutex_destroy(&bdev->mutex);

	rc = bdev->fn_table->destruct(bdev->ctxt);
	if (rc < 0) {
		SPDK_ERRLOG("destruct failed\n");
	}
}

int
spdk_bdev_open(struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
	       void *remove_ctx, struct spdk_bdev_desc **_desc)
{
	struct spdk_bdev_desc *desc;

	desc = calloc(1, sizeof(*desc));
	if (desc == NULL) {
		return -ENOMEM;
	}

	pthread_mutex_lock(&bdev->mutex);

	if (write && (bdev->bdev_opened_for_write)) {
		SPDK_ERRLOG("failed, %s already opened for write or claimed\n", bdev->name);
		free(desc);
		pthread_mutex_unlock(&bdev->mutex);
		return -EPERM;
	}

	TAILQ_INSERT_TAIL(&bdev->open_descs, desc, link);

	if (write) {
		bdev->bdev_opened_for_write = true;
	}

	desc->bdev = bdev;
	desc->remove_cb = remove_cb;
	desc->remove_ctx = remove_ctx;
	desc->write = write;
	*_desc = desc;

	pthread_mutex_unlock(&bdev->mutex);

	return 0;
}

void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = desc->bdev;
	bool do_unregister = false;

	pthread_mutex_lock(&bdev->mutex);

	if (desc->write) {
		assert(bdev->bdev_opened_for_write);
		bdev->bdev_opened_for_write = false;
	}

	TAILQ_REMOVE(&bdev->open_descs, desc, link);
	free(desc);

	if (bdev->status == SPDK_BDEV_STATUS_REMOVING && TAILQ_EMPTY(&bdev->open_descs)) {
		do_unregister = true;
	}
	pthread_mutex_unlock(&bdev->mutex);

	if (do_unregister == true) {
		spdk_bdev_unregister(bdev);
	}
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	return desc->bdev;
}

struct spdk_bdev_io *
spdk_bdev_read_init(struct spdk_bdev_desc *desc,
		    struct spdk_io_channel *ch,
		    struct spdk_mempool *bdev_io_pool,
		    spdk_bdev_io_completion_cb cb,
		    void *cb_arg,
		    struct iovec *iov,
		    int32_t *iovcnt,
		    int32_t length,
		    uint64_t offset)
{
	int rc = 0;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev *bdev = desc->bdev;

	assert(bdev->status != SPDK_BDEV_STATUS_INVALID);
	if (spdk_bdev_io_valid(bdev, offset, length) != 0) {
		return NULL;
	}

	bdev_io = spdk_bdev_get_io(bdev_io_pool);

	if (!bdev_io) {
		SPDK_ERRLOG("spdk_bdev_io memory allocation failed during readv\n");
		return NULL;
	}

	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);


	if (!(*iovcnt)) {
		if (bdev->fn_table->init_read) {
			rc = bdev->fn_table->init_read(length, iov, iovcnt, bdev_io);
		} else {
			rc = spdk_bdev_get_buff(iov, iovcnt, length);
		}
	}

	if (rc) {
		/* In case of failure reset iovcnt */
		*iovcnt = 0;
		spdk_bdev_put_io(bdev_io);
		bdev_io = NULL;
	} else {
		bdev_io->ch = NULL;
		bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
		bdev_io->u.read.iovs = iov;
		bdev_io->u.read.iovcnt = *iovcnt;
		bdev_io->u.read.len = length;
		bdev_io->u.read.offset = offset;
		bdev_io->u.read.put_rbuf = false;
	}

	return bdev_io;
}

int
spdk_bdev_read_fini(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	return bdev->fn_table->fini_read(bdev_io);
}

struct spdk_bdev_io *
spdk_bdev_write_init(struct spdk_bdev_desc *desc,
		     struct spdk_io_channel *ch,
		     struct spdk_mempool *bdev_io_pool,
		     spdk_bdev_io_completion_cb cb,
		     void *cb_arg,
		     struct iovec *iov,
		     int32_t *iovcnt,
		     int32_t length,
		     uint64_t offset)
{
	int rc = 0;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev *bdev = desc->bdev;

	assert(bdev->status != SPDK_BDEV_STATUS_INVALID);
	if (spdk_bdev_io_valid(bdev, offset, length) != 0) {
		return NULL;
	}

	bdev_io = spdk_bdev_get_io(bdev_io_pool);

	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed during writev\n");
		return NULL;
	}

	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	if (bdev->fn_table->init_write) {
		rc = bdev->fn_table->init_write(length, iov, iovcnt, bdev_io);
	}

	if (rc) {
		/* In case of failure reset iovcnt */
		*iovcnt = 0;
		spdk_bdev_put_io(bdev_io);
		bdev_io = NULL;
	} else {
		bdev_io->ch = NULL;
		bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
		bdev_io->u.write.iovs = iov;
		bdev_io->u.write.iovcnt = *iovcnt;
		bdev_io->u.write.len = length;
		bdev_io->u.write.offset = offset;
	}

	return bdev_io;
}

int
spdk_bdev_write_fini(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	return bdev->fn_table->fini_write(bdev_io);
}

void
spdk_bdev_io_abort(struct spdk_bdev_io *bdev_io, void *abt_ctx)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	if (spdk_likely(bdev->fn_table->abort_request)) {
		bdev->fn_table->abort_request(bdev_io, abt_ctx);
	} else {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Bdev abort_request not plugged in\n");
	}
}

void
spdk_bdev_io_get_iovec(struct spdk_bdev_io *bdev_io, struct iovec **iovp, int *iovcntp)
{
	struct iovec *iovs;
	int iovcnt;

	if (bdev_io == NULL) {
		return;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		iovs = bdev_io->u.read.iovs;
		iovcnt = bdev_io->u.read.iovcnt;
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		iovs = bdev_io->u.write.iovs;
		iovcnt = bdev_io->u.write.iovcnt;
		break;
	default:
		iovs = NULL;
		iovcnt = 0;
		break;
	}

	if (iovp) {
		*iovp = iovs;
	}
	if (iovcntp) {
		*iovcntp = iovcnt;
	}
}

void
spdk_bdev_module_list_add(struct spdk_bdev_module_if *bdev_module)
{
	/*
	 * Modules with examine callbacks must be initialized first, so they are
	 *  ready to handle examine callbacks from later modules that will
	 *  register physical bdevs.
	 */
	if (bdev_module->examine != NULL) {
		TAILQ_INSERT_HEAD(&g_bdev_mgr.bdev_modules, bdev_module, tailq);
	} else {
		TAILQ_INSERT_TAIL(&g_bdev_mgr.bdev_modules, bdev_module, tailq);
	}
}
