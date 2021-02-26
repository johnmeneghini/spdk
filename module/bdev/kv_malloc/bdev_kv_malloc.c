/*-
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

#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/likely.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk/nvme_kv.h"

#include "bdev_kv_malloc.h"

struct kv_malloc_bdev {
	struct spdk_bdev	bdev;
	TAILQ_ENTRY(kv_malloc_bdev)	tailq;
};

struct kv_malloc_io_channel {
	struct spdk_poller		*poller;
	TAILQ_HEAD(, spdk_bdev_io)	io;
};

static TAILQ_HEAD(, kv_malloc_bdev) g_kv_malloc_bdev_head = TAILQ_HEAD_INITIALIZER(
			g_kv_malloc_bdev_head);
static void *g_kv_malloc_read_buf;

static int bdev_kv_malloc_initialize(void);
static void bdev_kv_malloc_finish(void);

static struct spdk_bdev_module kv_malloc_if = {
	.name = "kv_malloc",
	.module_init = bdev_kv_malloc_initialize,
	.module_fini = bdev_kv_malloc_finish,
	.async_fini = true,
};

SPDK_BDEV_MODULE_REGISTER(kv_malloc, &kv_malloc_if)

static int
bdev_kv_malloc_destruct(void *ctx)
{
	struct kv_malloc_bdev *bdev = ctx;

	TAILQ_REMOVE(&g_kv_malloc_bdev_head, bdev, tailq);
	free(bdev->bdev.name);
	free(bdev);

	return 0;
}

static bool
bdev_kv_malloc_abort_io(struct kv_malloc_io_channel *ch, struct spdk_bdev_io *bio_to_abort)
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

static void
bdev_kv_malloc_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct kv_malloc_io_channel *ch = spdk_io_channel_get_ctx(_ch);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_KV_RETRIEVE:
		TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
		break;
	case SPDK_BDEV_IO_TYPE_KV_STORE:
		TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
		break;
	case SPDK_BDEV_IO_TYPE_KV_EXIST:
	case SPDK_BDEV_IO_TYPE_KV_LIST:
	case SPDK_BDEV_IO_TYPE_KV_DELETE:
		TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		if (bdev_kv_malloc_abort_io(ch, bdev_io->u.abort.bio_to_abort)) {
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
bdev_kv_malloc_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
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
bdev_kv_malloc_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_kv_malloc_bdev_head);
}

static void
bdev_kv_malloc_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	char uuid_str[SPDK_UUID_STRING_LEN];

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_kv_malloc_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint64(w, "capacity", bdev->blockcnt);
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);
	spdk_json_write_named_string(w, "uuid", uuid_str);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table kv_malloc_fn_table = {
	.destruct		= bdev_kv_malloc_destruct,
	.submit_request		= bdev_kv_malloc_submit_request,
	.io_type_supported	= bdev_kv_malloc_io_type_supported,
	.get_io_channel		= bdev_kv_malloc_get_io_channel,
	.write_config_json	= bdev_kv_malloc_write_config_json,
};

int
bdev_kv_malloc_create(struct spdk_bdev **bdev, const struct spdk_kv_malloc_bdev_opts *opts)
{
	struct kv_malloc_bdev *kv_malloc_disk;
	int rc;

	if (!opts) {
		SPDK_ERRLOG("No options provided for KV malloc bdev.\n");
		return -EINVAL;
	}

	if (opts->capacity == 0) {
		SPDK_ERRLOG("Device capacity must be greater than 0\n");
		return -EINVAL;
	}

	kv_malloc_disk = calloc(1, sizeof(*kv_malloc_disk));
	if (!kv_malloc_disk) {
		SPDK_ERRLOG("could not allocate kv_malloc_bdev\n");
		return -ENOMEM;
	}

	kv_malloc_disk->bdev.name = strdup(opts->name);
	if (!kv_malloc_disk->bdev.name) {
		free(kv_malloc_disk);
		return -ENOMEM;
	}
	kv_malloc_disk->bdev.product_name = "KV Malloc disk";

	kv_malloc_disk->bdev.write_cache = 0;
	kv_malloc_disk->bdev.blocklen = 1;
	kv_malloc_disk->bdev.blockcnt = opts->capacity;
	if (opts->uuid) {
		kv_malloc_disk->bdev.uuid = *opts->uuid;
	} else {
		spdk_uuid_generate(&kv_malloc_disk->bdev.uuid);
	}

	kv_malloc_disk->bdev.ctxt = kv_malloc_disk;
	kv_malloc_disk->bdev.fn_table = &kv_malloc_fn_table;
	kv_malloc_disk->bdev.module = &kv_malloc_if;

	rc = spdk_bdev_register(&kv_malloc_disk->bdev);
	if (rc) {
		free(kv_malloc_disk->bdev.name);
		free(kv_malloc_disk);
		return rc;
	}

	*bdev = &(kv_malloc_disk->bdev);

	TAILQ_INSERT_TAIL(&g_kv_malloc_bdev_head, kv_malloc_disk, tailq);

	return rc;
}

void
bdev_kv_malloc_delete(struct spdk_bdev *bdev, spdk_delete_kv_malloc_complete cb_fn, void *cb_arg)
{
	if (!bdev || bdev->module != &kv_malloc_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

static int
kv_malloc_io_poll(void *arg)
{
	struct kv_malloc_io_channel		*ch = arg;
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
kv_malloc_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct kv_malloc_io_channel *ch = ctx_buf;

	TAILQ_INIT(&ch->io);
	ch->poller = SPDK_POLLER_REGISTER(kv_malloc_io_poll, ch, 0);

	return 0;
}

static void
kv_malloc_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct kv_malloc_io_channel *ch = ctx_buf;

	spdk_poller_unregister(&ch->poller);
}

static int
bdev_kv_malloc_initialize(void)
{
	/*
	 * This will be used if upper layer expects us to allocate the read buffer.
	 *  Instead of using a real rbuf from the bdev pool, just always point to
	 *  this same zeroed buffer.
	 */
	g_kv_malloc_read_buf = spdk_zmalloc(SPDK_BDEV_LARGE_BUF_MAX_SIZE, 0, NULL,
					    SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (g_kv_malloc_read_buf == NULL) {
		return -1;
	}

	/*
	 * We need to pick some unique address as our "io device" - so just use the
	 *  address of the global tailq.
	 */
	spdk_io_device_register(&g_kv_malloc_bdev_head, kv_malloc_bdev_create_cb, kv_malloc_bdev_destroy_cb,
				sizeof(struct kv_malloc_io_channel), "kv_malloc_bdev");

	return 0;
}

static void
_bdev_kv_malloc_finish_cb(void *arg)
{
	spdk_free(g_kv_malloc_read_buf);
	spdk_bdev_module_finish_done();
}

static void
bdev_kv_malloc_finish(void)
{
	spdk_io_device_unregister(&g_kv_malloc_bdev_head, _bdev_kv_malloc_finish_cb);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_kv_malloc)
