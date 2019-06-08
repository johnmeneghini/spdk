/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (c) Intel Corporation.
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
#include "spdk/scsi_spec.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"
#include "spdk/string.h"

#ifdef SPDK_CONFIG_VTUNE
#include "ittnotify.h"
#include "ittnotify_types.h"
int __itt_init_ittlib(const char *, __itt_group_id);
#endif

typedef TAILQ_HEAD(, spdk_bdev_io) need_buf_tailq_t;

struct spdk_bdev_mgmt_channel {
	need_buf_tailq_t need_buf_small;
	need_buf_tailq_t need_buf_large;
};

extern struct spdk_bdev_mgr g_bdev_mgr;
extern bool spdk_bdev_g_use_global_pools;

static void
spdk_bdev_io_set_buf(struct spdk_bdev_io *bdev_io, void *buf)
{
	assert(bdev_io->get_buf_cb != NULL);
	assert(buf != NULL);
	assert(bdev_io->u.read.iovs != NULL);

	if (buf == NULL || bdev_io->get_buf_cb == NULL || bdev_io->u.read.iovs == NULL) {
		return;
	}

	bdev_io->buf = buf;
	bdev_io->u.read.iovs[0].iov_base = (void *)((unsigned long)((char *)buf + 512) & ~511UL);
	bdev_io->u.read.iovs[0].iov_len = bdev_io->u.read.len;
	bdev_io->u.read.iovcnt = 1;
	bdev_io->u.read.put_rbuf = true;
	bdev_io->get_buf_cb(bdev_io->ch->channel, bdev_io);
}

static void
spdk_bdev_io_put_buf(struct spdk_bdev_io *bdev_io)
{
	struct spdk_mempool *pool;
	struct spdk_bdev_io *tmp;
	void *buf;
	need_buf_tailq_t *tailq;
	uint64_t length;
	struct spdk_bdev_mgmt_channel *ch;

	if (!bdev_io->buf) {
		return;
	}

	assert(spdk_bdev_g_use_global_pools);
	assert(bdev_io->u.read.iovcnt == 1);

	length = bdev_io->u.read.len;
	buf = bdev_io->buf;
	bdev_io->buf = NULL;

	ch = spdk_io_channel_get_ctx(bdev_io->ch->mgmt_channel);

	if (length <= SPDK_BDEV_SMALL_BUF_MAX_SIZE) {
		pool = g_bdev_mgr.buf_small_pool;
		tailq = &ch->need_buf_small;
	} else {
		pool = g_bdev_mgr.buf_large_pool;
		tailq = &ch->need_buf_large;
	}

	if (TAILQ_EMPTY(tailq)) {
		spdk_mempool_put(pool, buf);
	} else {
		tmp = TAILQ_FIRST(tailq);
		TAILQ_REMOVE(tailq, tmp, buf_link);
		spdk_bdev_io_set_buf(tmp, buf);
	}
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb)
{
	uint64_t len = bdev_io->u.read.len;
	struct spdk_mempool *pool;
	need_buf_tailq_t *tailq;
	void *buf = NULL;
	struct spdk_bdev_mgmt_channel *ch;

	assert(cb != NULL);
	assert(bdev_io->u.read.iovs != NULL);
	assert(spdk_bdev_g_use_global_pools);

	if (spdk_unlikely(bdev_io->u.read.iovs[0].iov_base != NULL)) {
		/* Buffer already present */
		cb(bdev_io->ch->channel, bdev_io);
		return;
	}

	ch = spdk_io_channel_get_ctx(bdev_io->ch->mgmt_channel);

	bdev_io->get_buf_cb = cb;
	if (len <= SPDK_BDEV_SMALL_BUF_MAX_SIZE) {
		pool = g_bdev_mgr.buf_small_pool;
		tailq = &ch->need_buf_small;
	} else {
		pool = g_bdev_mgr.buf_large_pool;
		tailq = &ch->need_buf_large;
	}

	buf = spdk_mempool_get(pool);

	if (!buf) {
		TAILQ_INSERT_TAIL(tailq, bdev_io, buf_link);
	} else {
		spdk_bdev_io_set_buf(bdev_io, buf);
	}
}

static int
spdk_bdev_mgmt_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_mgmt_channel *ch = ctx_buf;

	TAILQ_INIT(&ch->need_buf_small);
	TAILQ_INIT(&ch->need_buf_large);

	return 0;
}

static void
spdk_bdev_mgmt_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_mgmt_channel *ch = ctx_buf;

	if (!TAILQ_EMPTY(&ch->need_buf_small) || !TAILQ_EMPTY(&ch->need_buf_large)) {
		SPDK_ERRLOG("Pending I/O list wasn't empty on channel destruction\n");
	}
}

void
spdk_bdev_mgr_register_channel(void)
{
#ifdef SPDK_CONFIG_VTUNE
	g_bdev_mgr.domain = __itt_domain_create("spdk_bdev");
#endif
	spdk_io_device_register(&g_bdev_mgr, spdk_bdev_mgmt_channel_create,
				spdk_bdev_mgmt_channel_destroy,
				sizeof(struct spdk_bdev_mgmt_channel));
}

void
spdk_bdev_mgr_unregister_channel(void)
{
	spdk_io_device_unregister(&g_bdev_mgr, NULL);
}

void
spdk_bdev_poller_start(struct spdk_bdev_poller **ppoller,
		       spdk_bdev_poller_fn fn,
		       void *arg,
		       uint32_t lcore,
		       uint64_t period_microseconds)
{
	g_bdev_mgr.start_poller_fn(ppoller, fn, arg, lcore, period_microseconds);
}

void
spdk_bdev_poller_stop(struct spdk_bdev_poller **ppoller)
{
	g_bdev_mgr.stop_poller_fn(ppoller);
}

int
spdk_bdev_dump_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	if (bdev->fn_table->dump_config_json) {
		return bdev->fn_table->dump_config_json(bdev->ctxt, w);
	}

	return 0;
}

static int
spdk_bdev_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_bdev		*bdev = io_device;
	struct spdk_bdev_channel	*ch = ctx_buf;

	ch->bdev = io_device;
	ch->channel = bdev->fn_table->get_io_channel(bdev->ctxt);
	ch->mgmt_channel = spdk_get_io_channel(&g_bdev_mgr);
	memset(&ch->stat, 0, sizeof(ch->stat));
	ch->io_outstanding = 0;

#ifdef SPDK_CONFIG_VTUNE
	{
		char *name;
		__itt_init_ittlib(NULL, 0);
		name = spdk_sprintf_alloc("spdk_bdev_%s_%p", ch->bdev->name, ch);
		if (!name) {
			return -1;
		}
		ch->handle = __itt_string_handle_create(name);
		free(name);
		ch->start_tsc = spdk_get_ticks();
		ch->interval_tsc = spdk_get_ticks_hz() / 100;
	}
#endif

	return 0;
}

static void
_spdk_bdev_abort_io(need_buf_tailq_t *queue, struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_io *bdev_io, *tmp;

	TAILQ_FOREACH_SAFE(bdev_io, queue, buf_link, tmp) {
		if (bdev_io->ch == ch) {
			TAILQ_REMOVE(queue, bdev_io, buf_link);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static void
spdk_bdev_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_channel	*ch = ctx_buf;
	struct spdk_bdev_mgmt_channel	*mgmt_channel;

	mgmt_channel = spdk_io_channel_get_ctx(ch->mgmt_channel);

	_spdk_bdev_abort_io(&mgmt_channel->need_buf_small, ch);
	_spdk_bdev_abort_io(&mgmt_channel->need_buf_large, ch);

	spdk_put_io_channel(ch->channel);
	spdk_put_io_channel(ch->mgmt_channel);
	assert(ch->io_outstanding == 0);
}

void
spdk_bdev_register_channel(void *io_device)
{
	spdk_io_device_register(io_device, spdk_bdev_channel_create, spdk_bdev_channel_destroy,
				sizeof(struct spdk_bdev_channel));
}

void
spdk_bdev_unregister_channel(void *io_device)
{
	spdk_io_device_unregister(io_device, NULL);
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
	return spdk_get_io_channel(desc->bdev);
}



void
spdk_bdev_io_complete_scsi_status(struct spdk_bdev_io *bdev_io, enum spdk_scsi_status sc,
				  enum spdk_scsi_sense sk, uint8_t asc, uint8_t ascq)
{
	if (sc == SPDK_SCSI_STATUS_GOOD) {
		bdev_io->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	} else {
		bdev_io->status = SPDK_BDEV_IO_STATUS_SCSI_ERROR;
		bdev_io->error.scsi.sc = sc;
		bdev_io->error.scsi.sk = sk;
		bdev_io->error.scsi.asc = asc;
		bdev_io->error.scsi.ascq = ascq;
	}

	spdk_bdev_io_complete(bdev_io, bdev_io->status);
}

void
spdk_bdev_io_get_scsi_status(const struct spdk_bdev_io *bdev_io,
			     int *sc, int *sk, int *asc, int *ascq)
{
	assert(sc != NULL);
	assert(sk != NULL);
	assert(asc != NULL);
	assert(ascq != NULL);

	switch (bdev_io->status) {
	case SPDK_BDEV_IO_STATUS_SUCCESS:
		*sc = SPDK_SCSI_STATUS_GOOD;
		*sk = SPDK_SCSI_SENSE_NO_SENSE;
		*asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
		*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
		break;
	case SPDK_BDEV_IO_STATUS_NVME_ERROR:
		spdk_scsi_nvme_translate(bdev_io, sc, sk, asc, ascq);
		break;
	case SPDK_BDEV_IO_STATUS_SCSI_ERROR:
		*sc = bdev_io->error.scsi.sc;
		*sk = bdev_io->error.scsi.sk;
		*asc = bdev_io->error.scsi.asc;
		*ascq = bdev_io->error.scsi.ascq;
		break;
	default:
		*sc = SPDK_SCSI_STATUS_CHECK_CONDITION;
		*sk = SPDK_SCSI_SENSE_ABORTED_COMMAND;
		*asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
		*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
		break;
	}
}

int
spdk_bdev_module_claim_bdev(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			    struct spdk_bdev_module_if *module)
{
	if (bdev->claim_module != NULL) {
		SPDK_ERRLOG("bdev %s already claimed by module %s\n", bdev->name,
			    bdev->claim_module->name);
		return -EPERM;
	}

	if ((!desc || bdev->write_protect_flags.write_protect) && bdev->bdev_opened_for_write) {
		SPDK_ERRLOG("bdev %s already opened with write access\n", bdev->name);
		return -EPERM;
	}

	if (desc && bdev->write_protect_flags.write_protect) {
		bdev->bdev_opened_for_write = true;
		bdev->write_protect_flags.write_protect = 1;
	}

	bdev->claim_module = module;
	return 0;
}

void
spdk_bdev_module_release_bdev(struct spdk_bdev *bdev)
{
	assert(bdev->claim_module != NULL);
	bdev->claim_module = NULL;
}

int
spdk_bdev_read(struct spdk_bdev_desc *desc, struct spdk_mempool *bdev_io_pool,
	       struct spdk_io_channel *ch,
	       void *buf, uint64_t offset, uint64_t nbytes,
	       spdk_bdev_io_completion_cb cb, void *cb_arg, struct spdk_bdev_io **result_bdev_io)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	int rc;

	if (spdk_bdev_io_valid(bdev, offset, nbytes) != 0) {
		return -EINVAL;
	}

	bdev_io = spdk_bdev_get_io(bdev_io_pool);
	if (!bdev_io) {
		SPDK_ERRLOG("spdk_bdev_io memory allocation failed duing read\n");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	bdev_io->u.read.iov.iov_base = buf;
	bdev_io->u.read.iov.iov_len = nbytes;
	bdev_io->u.read.iovs = &bdev_io->u.read.iov;
	bdev_io->u.read.iovcnt = 1;
	bdev_io->u.read.len = nbytes;
	bdev_io->u.read.offset = offset;
	bdev_io->bdev_io_pool = bdev_io_pool;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb, NULL, NULL);

	rc = spdk_bdev_submit_io(bdev_io);
	if (rc < 0) {
		spdk_bdev_io_put_buf(bdev_io);
		spdk_bdev_put_io(bdev_io);
		return rc;
	}

	if (result_bdev_io) {
		*result_bdev_io = bdev_io;
	}

	return 0;
}

int
spdk_bdev_compare_and_write(enum spdk_nvme_nvm_opcode opcode,
			    struct spdk_bdev_desc *desc, struct spdk_mempool *bdev_io_pool,
			    struct spdk_io_channel *ch,
			    void *buf, uint64_t offset, uint64_t nbytes,
			    spdk_bdev_io_completion_cb cb, void *cb_arg, struct spdk_bdev_io **result_bdev_io, bool fused)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	int rc;

	if ((opcode == SPDK_NVME_OPC_WRITE) && bdev->write_protect_flags.write_protect) {
		return -EBADF;
	}

	if ((opcode == SPDK_NVME_OPC_WRITE) && !fused) {
		return -EINVAL;
	}

	if (spdk_bdev_io_valid(bdev, offset, nbytes) != 0) {
		return -EINVAL;
	}

	bdev_io = spdk_bdev_get_io(bdev_io_pool);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing %s\n",
			    (opcode == SPDK_NVME_OPC_WRITE) ? "compare and write" : "compare");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = (opcode == SPDK_NVME_OPC_COMPARE) ? SPDK_BDEV_IO_TYPE_COMPARE :
			SPDK_BDEV_IO_TYPE_WRITE;
	bdev_io->u.write.iov.iov_base = buf;
	bdev_io->u.write.iov.iov_len = nbytes;
	bdev_io->u.write.iovs = &bdev_io->u.write.iov;
	bdev_io->u.write.iovcnt = 1;
	bdev_io->u.write.len = nbytes;
	bdev_io->u.write.offset = offset;
	bdev_io->bdev_io_pool = bdev_io_pool;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb, NULL, NULL);

	rc = spdk_bdev_submit_io(bdev_io);
	if (rc < 0) {
		spdk_bdev_io_put_buf(bdev_io);
		spdk_bdev_put_io(bdev_io);
		return rc;
	}

	if (result_bdev_io) {
		*result_bdev_io = bdev_io;
	}

	return 0;
}

int
spdk_bdev_write(struct spdk_bdev_desc *desc, struct spdk_mempool *bdev_io_pool,
		struct spdk_io_channel *ch,
		void *buf, uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg, struct spdk_bdev_io **result_bdev_io)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	int rc;

	if (bdev->write_protect_flags.write_protect) {
		return -EBADF;
	}

	if (spdk_bdev_io_valid(bdev, offset, nbytes) != 0) {
		return -EINVAL;
	}

	bdev_io = spdk_bdev_get_io(bdev_io_pool);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing %s\n",  "write");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	bdev_io->u.write.iov.iov_base = buf;
	bdev_io->u.write.iov.iov_len = nbytes;
	bdev_io->u.write.iovs = &bdev_io->u.write.iov;
	bdev_io->u.write.iovcnt = 1;
	bdev_io->u.write.len = nbytes;
	bdev_io->u.write.offset = offset;
	bdev_io->bdev_io_pool = bdev_io_pool;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb, NULL, NULL);

	rc = spdk_bdev_submit_io(bdev_io);
	if (rc < 0) {
		spdk_bdev_io_put_buf(bdev_io);
		spdk_bdev_put_io(bdev_io);
		return rc;
	}

	if (result_bdev_io) {
		*result_bdev_io = bdev_io;
	}

	return 0;
}

int
spdk_bdev_unmap(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct spdk_scsi_unmap_bdesc *unmap_d,
		uint16_t bdesc_count,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	int rc;

	if (bdev->write_protect_flags.write_protect) {
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

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_UNMAP;
	bdev_io->u.unmap.unmap_bdesc = unmap_d;
	bdev_io->u.unmap.bdesc_count = bdesc_count;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb, NULL, NULL);

	rc = spdk_bdev_submit_io(bdev_io);
	if (rc < 0) {
		spdk_bdev_io_put_buf(bdev_io);
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
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	int rc;

	if (bdev->write_protect_flags.write_protect) {
		return -EBADF;
	}

	assert(bdev->status != SPDK_BDEV_STATUS_INVALID);
	bdev_io = spdk_bdev_get_io(bdev_io_pool);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing flush\n");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_FLUSH;
	bdev_io->u.flush.offset = offset;
	bdev_io->u.flush.length = length;
	bdev_io->bdev_io_pool = bdev_io_pool;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb, NULL, NULL);

	rc = spdk_bdev_submit_io(bdev_io);
	if (rc < 0) {
		spdk_bdev_io_put_buf(bdev_io);
		spdk_bdev_put_io(bdev_io);
		return rc;
	}

	return 0;
}

static void
_spdk_bdev_reset_dev(void *io_device, void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;
	int rc;

	rc = spdk_bdev_submit_io(bdev_io);
	if (rc < 0) {
		spdk_bdev_io_put_buf(bdev_io);
		spdk_bdev_put_io(bdev_io);
		SPDK_ERRLOG("reset failed\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
_spdk_bdev_reset_abort_channel(void *io_device, struct spdk_io_channel *ch,
			       void *ctx)
{
	struct spdk_bdev_channel	*channel;
	struct spdk_bdev_mgmt_channel	*mgmt_channel;

	channel = spdk_io_channel_get_ctx(ch);
	mgmt_channel = spdk_io_channel_get_ctx(channel->mgmt_channel);

	_spdk_bdev_abort_io(&mgmt_channel->need_buf_small, channel);
	_spdk_bdev_abort_io(&mgmt_channel->need_buf_large, channel);
}

static void
_spdk_bdev_start_reset(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;

	spdk_for_each_channel(bdev_io->bdev, _spdk_bdev_reset_abort_channel,
			      bdev_io, _spdk_bdev_reset_dev);
}

static void
_spdk_bdev_start_next_reset(struct spdk_bdev *bdev)
{
	struct spdk_bdev_io *bdev_io;
	struct spdk_thread *thread;

	pthread_mutex_lock(&bdev->mutex);

	if (bdev->reset_in_progress || TAILQ_EMPTY(&bdev->queued_resets)) {
		pthread_mutex_unlock(&bdev->mutex);
		return;
	} else {
		bdev_io = TAILQ_FIRST(&bdev->queued_resets);
		TAILQ_REMOVE(&bdev->queued_resets, bdev_io, link);
		bdev->reset_in_progress = true;
		thread = spdk_io_channel_get_thread(bdev_io->ch->channel);
		spdk_thread_send_msg(thread, _spdk_bdev_start_reset, bdev_io);
	}

	pthread_mutex_unlock(&bdev->mutex);
}

int
spdk_bdev_reset(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	assert(bdev->status != SPDK_BDEV_STATUS_INVALID);
	bdev_io = spdk_bdev_get_io(NULL);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing reset\n");
		return -ENOMEM;;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_RESET;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb, NULL, NULL);

	pthread_mutex_lock(&bdev->mutex);
	TAILQ_INSERT_TAIL(&bdev->queued_resets, bdev_io, link);
	pthread_mutex_unlock(&bdev->mutex);

	_spdk_bdev_start_next_reset(bdev);

	return 0;
}

void
spdk_bdev_get_io_stat(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		      struct spdk_bdev_io_stat *stat)
{
#ifdef SPDK_CONFIG_VTUNE
	SPDK_ERRLOG("Calling spdk_bdev_get_io_stat is not allowed when VTune integration is enabled.\n");
	memset(stat, 0, sizeof(*stat));
	return;
#endif

	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	*stat = channel->stat;
	memset(&channel->stat, 0, sizeof(channel->stat));
}

int
spdk_bdev_nvme_admin_passthru(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      const struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	int rc;

	if (bdev->write_protect_flags.write_protect) {
		return -EBADF;
	}

	bdev_io = spdk_bdev_get_io(NULL);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed during nvme_admin_passthru\n");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_ADMIN;
	bdev_io->u.nvme_passthru.cmd = *cmd;
	bdev_io->u.nvme_passthru.buf = buf;
	bdev_io->u.nvme_passthru.nbytes = nbytes;

	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb, NULL, NULL);

	rc = spdk_bdev_submit_io(bdev_io);
	if (rc < 0) {
		spdk_bdev_io_put_buf(bdev_io);
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
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	int rc;

	if (bdev->write_protect_flags.write_protect) {
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

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_IO;
	bdev_io->u.nvme_passthru.cmd = *cmd;
	bdev_io->u.nvme_passthru.buf = buf;
	bdev_io->u.nvme_passthru.nbytes = nbytes;

	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb, NULL, NULL);

	rc = spdk_bdev_submit_io(bdev_io);
	if (rc < 0) {
		spdk_bdev_io_put_buf(bdev_io);
		spdk_bdev_put_io(bdev_io);
		return rc;
	}

	return 0;
}

#ifndef NETAPP
static void
_spdk_bdev_io_complete(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;

	assert(bdev_io->cb != NULL);
	if (bdev_io->cb != NULL) {
		bdev_io->cb(bdev_io, bdev_io->status == SPDK_BDEV_IO_STATUS_SUCCESS, bdev_io->caller_ctx);
	}
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	bdev_io->status = status;
	if (bdev_io->ch) {
		assert(bdev_io->ch->io_outstanding > 0);
		bdev_io->ch->io_outstanding--;
	}
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_RESET) {
		/* Successful reset */
		if (status == SPDK_BDEV_IO_STATUS_SUCCESS) {
			/* Increase the bdev generation */
			bdev_io->bdev->gencnt++;
		}
		bdev_io->bdev->reset_in_progress = false;
		_spdk_bdev_start_next_reset(bdev_io->bdev);
	} else {
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
	}

	if (status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		switch (bdev_io->type) {
		case SPDK_BDEV_IO_TYPE_READ:
			bdev_io->ch->stat.bytes_read += bdev_io->u.read.len;
			bdev_io->ch->stat.num_read_ops++;
			break;
		case SPDK_BDEV_IO_TYPE_WRITE:
			bdev_io->ch->stat.bytes_written += bdev_io->u.write.len;
			bdev_io->ch->stat.num_write_ops++;
			break;
		default:
			break;
		}
	}

#ifdef SPDK_CONFIG_VTUNE
	uint64_t now_tsc = spdk_get_ticks();
	if (now_tsc > (bdev_io->ch->start_tsc + bdev_io->ch->interval_tsc)) {
		uint64_t data[5];

		data[0] = bdev_io->ch->stat.num_read_ops;
		data[1] = bdev_io->ch->stat.bytes_read;
		data[2] = bdev_io->ch->stat.num_write_ops;
		data[3] = bdev_io->ch->stat.bytes_written;
		data[4] = bdev_io->bdev->fn_table->get_spin_time ?
			  bdev_io->bdev->fn_table->get_spin_time(bdev_io->ch->channel) : 0;

		__itt_metadata_add(g_bdev_mgr.domain, __itt_null, bdev_io->ch->handle,
				   __itt_metadata_u64, 5, data);

		memset(&bdev_io->ch->stat, 0, sizeof(bdev_io->ch->stat));
		bdev_io->ch->start_tsc = now_tsc;
	}
#endif

	if (bdev_io->in_submit_request || bdev_io->type == SPDK_BDEV_IO_TYPE_RESET) {
		/*
		 * Defer completion to avoid potential infinite recursion if the
		 * user's completion callback issues a new I/O.
		 */
		spdk_thread_send_msg(spdk_io_channel_get_thread(bdev_io->ch->channel),
				     _spdk_bdev_io_complete, bdev_io);
	} else {
		_spdk_bdev_io_complete(bdev_io);
	}
}
#endif

