/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2017 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Limited and/or its subsidiaries.
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

/*
 * Implementation of Fibre Channel SLI-4 functions.
 */

#include "spdk/env.h"
#include "spdk/assert.h"
#include "spdk/nvmf.h"
#include "spdk/endian.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk/likely.h"
#include "spdk/trace.h"
#include "spdk_internal/log.h"
#include "bcm_fc.h"
#include "nvmf/nvmf_internal.h"

char *fc_req_state_strs[] = {
	"SPDK_NVMF_BCM_FC_REQ_INIT",
	"SPDK_NVMF_BCM_FC_REQ_READ_BDEV",
	"SPDK_NVMF_BCM_FC_REQ_READ_XFER",
	"SPDK_NVMF_BCM_FC_REQ_READ_RSP",
	"SPDK_NVMF_BCM_FC_REQ_WRITE_BUFFS",
	"SPDK_NVMF_BCM_FC_REQ_WRITE_XFER",
	"SPDK_NVMF_BCM_FC_REQ_WRITE_BDEV",
	"SPDK_NVMF_BCM_FC_REQ_WRITE_RSP",
	"SPDK_NVMF_BCM_FC_REQ_NONE_BDEV",
	"SPDK_NVMF_BCM_FC_REQ_NONE_RSP",
	"SPDK_NVMF_BCM_FC_REQ_SUCCESS",
	"SPDK_NVMF_BCM_FC_REQ_FAILED",
	"SPDK_NVMF_BCM_FC_REQ_ABORTED",
	"SPDK_NVMF_BCM_FC_REQ_PENDING"
};

extern void spdk_post_event(void *context, struct spdk_event *event);
extern void nvmf_fc_poller_queue_sync_done(void *arg1, void *arg2);
uint32_t spdk_nvmf_bcm_fc_process_queues(struct spdk_nvmf_bcm_fc_hwqp *hwqp);
void spdk_nvmf_bcm_fc_free_req(struct spdk_nvmf_bcm_fc_request *fc_req);
int spdk_nvmf_bcm_fc_init_rqpair_buffers(struct spdk_nvmf_bcm_fc_hwqp *hwqp);
int spdk_nvmf_bcm_fc_create_req_mempool(struct spdk_nvmf_bcm_fc_hwqp *hwqp);
int spdk_nvmf_bcm_fc_xmt_ls_rsp(struct spdk_nvmf_bcm_fc_nport *tgtport,
				struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst);
int spdk_nvmf_bcm_fc_handle_rsp(struct spdk_nvmf_bcm_fc_request *req);
int spdk_nvmf_bcm_fc_send_data(struct spdk_nvmf_bcm_fc_request *fc_req);
int spdk_nvmf_bcm_fc_issue_abort(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
				 struct spdk_nvmf_bcm_fc_xri *xri,
				 bool send_abts, spdk_nvmf_bcm_fc_caller_cb cb,
				 void *cb_args);
int spdk_nvmf_bcm_fc_xmt_bls_rsp(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
				 uint16_t ox_id, uint16_t rx_id, uint16_t rpi,
				 bool rjt, uint8_t rjt_exp,
				 spdk_nvmf_bcm_fc_caller_cb cb, void *cb_args);
void spdk_nvmf_bcm_fc_req_set_state(struct spdk_nvmf_bcm_fc_request *fc_req,
				    spdk_nvmf_bcm_fc_request_state_t state);
void spdk_nvmf_bcm_fc_req_abort(struct spdk_nvmf_bcm_fc_request *fc_req,
				bool send_abts, spdk_nvmf_bcm_fc_caller_cb cb,
				void *cb_args);
void spdk_nvmf_bcm_fc_req_abort_complete(void *arg1, void *arg2);
void spdk_nvmf_bcm_fc_release_xri(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
				  struct spdk_nvmf_bcm_fc_xri *xri, bool xb, bool abts);
static int nvmf_fc_execute_nvme_rqst(struct spdk_nvmf_bcm_fc_request *fc_req);
int spdk_nvmf_bcm_fc_create_reqtag_pool(struct spdk_nvmf_bcm_fc_hwqp *hwqp);
int spdk_nvmf_bcm_fc_issue_marker(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint64_t u_id,
				  uint16_t skip_rq);

static inline uint16_t
nvmf_fc_advance_conn_sqhead(struct spdk_nvmf_conn *conn)
{
	/* advance sq_head pointer - wrap if needed */
	conn->sq_head = (conn->sq_head == conn->sq_head_max) ?
			0 : (conn->sq_head + 1);
	return conn->sq_head;
}

static inline bool
nvmf_fc_sq_90percent_full(struct spdk_nvmf_conn *conn)
{
	/* TODO: Defer to next phase: */
	return false;
}

static inline struct spdk_nvmf_bcm_fc_conn *
nvmf_fc_get_conn(struct spdk_nvmf_conn *conn)
{
	return (struct spdk_nvmf_bcm_fc_conn *)
	       ((uintptr_t)conn - offsetof(struct spdk_nvmf_bcm_fc_conn, conn));
}

static inline bool
nvmf_fc_send_ersp_required(struct spdk_nvmf_bcm_fc_request *fc_req,
			   uint32_t rsp_cnt, uint32_t xfer_len)
{
	struct spdk_nvmf_request *req = &fc_req->req;
	struct spdk_nvmf_conn *conn = req->conn;
	struct spdk_nvmf_bcm_fc_conn *fc_conn = nvmf_fc_get_conn(conn);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint16_t status = *((uint16_t *)&rsp->status);
	bool rc = false;

	/*
	 * Check if we need to send ERSP
	 * 1) For every N responses where N == ersp_ratio
	 * 2) Fabric commands.
	 * 3) Completion status failed or Completion dw0 or dw1 valid.
	 * 4) SQ == 90% full.
	 * 5) Transfer length not equal to CMD IU length
	 */

	if (!(rsp_cnt % fc_conn->esrp_ratio) ||
	    nvmf_fc_sq_90percent_full(conn) ||
	    (cmd->opc == SPDK_NVME_OPC_FABRIC) ||
	    (status & 0xFFFE) || rsp->cdw0 || rsp->rsvd1 ||
	    (req->length != xfer_len)) {
		rc = true;
	}
	return rc;
}


static inline void
nvmf_fc_queue_tail_inc(bcm_sli_queue_t *q)
{
	q->tail = (q->tail + 1) % q->max_entries;
}

static inline void
nvmf_fc_queue_head_inc(bcm_sli_queue_t *q)
{
	q->head = (q->head + 1) % q->max_entries;
}

static inline void *
nvmf_fc_queue_head_node(bcm_sli_queue_t *q)
{
	return q->address + q->head * q->size;
}

static inline void *
nvmf_fc_queue_tail_node(bcm_sli_queue_t *q)
{
	return q->address + q->tail * q->size;
}

static inline bool
nvmf_fc_queue_full(bcm_sli_queue_t *q)
{
	return (q->used >= q->max_entries);
}

static uint32_t
nvmf_fc_rqpair_get_buffer_id(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint16_t rqindex)
{
	return hwqp->queues.rq_hdr.rq_map[rqindex];
}

static fc_frame_hdr_t *
nvmf_fc_rqpair_get_frame_header(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint16_t rqindex)
{
	uint32_t buf_index = hwqp->queues.rq_hdr.rq_map[rqindex];
	return hwqp->queues.rq_hdr.buffer[buf_index].virt;
}

static bcm_buffer_desc_t *
nvmf_fc_rqpair_get_frame_buffer(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint16_t rqindex)
{
	uint32_t buf_index = hwqp->queues.rq_hdr.rq_map[rqindex]; // Use header map.
	return hwqp->queues.rq_payload.buffer + buf_index;
}

int
spdk_nvmf_bcm_fc_create_req_mempool(struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	char name[48];
	static int unique_number = 0;

	unique_number++;

	/* Name should be unique, otherwise API fails. */
	snprintf(name, sizeof(name), "NVMF_FC_REQ_POOL:%d", unique_number);
	hwqp->fc_request_pool = spdk_mempool_create(name,
				hwqp->queues.rq_hdr.num_buffers,
				sizeof(struct spdk_nvmf_bcm_fc_request),
				0, SPDK_ENV_SOCKET_ID_ANY);

	if (hwqp->fc_request_pool == NULL) {
		SPDK_ERRLOG("create fc request pool failed\n");
		return -1;
	}
	TAILQ_INIT(&hwqp->in_use_reqs);
	return 0;
}

static inline void
nvmf_fc_record_req_trace_point(struct spdk_nvmf_bcm_fc_request *fc_req,
			       spdk_nvmf_bcm_fc_request_state_t state)
{
	uint16_t tpoint_id = SPDK_TRACE_MAX_TPOINT_ID;

	switch (state) {
	case SPDK_NVMF_BCM_FC_REQ_INIT:
		/* Start IO tracing */
		spdk_trace_record(TRACE_NVMF_IO_START, fc_req->poller_lcore,
				  0, (uint64_t)(&fc_req->req), 0);
		tpoint_id = TRACE_FC_REQ_INIT;
		break;
	case SPDK_NVMF_BCM_FC_REQ_READ_BDEV:
		tpoint_id = TRACE_FC_REQ_READ_BDEV;
		break;
	case SPDK_NVMF_BCM_FC_REQ_READ_XFER:
		tpoint_id = TRACE_FC_REQ_READ_XFER;
		break;
	case SPDK_NVMF_BCM_FC_REQ_READ_RSP:
		tpoint_id = TRACE_FC_REQ_READ_RSP;
		break;
	case SPDK_NVMF_BCM_FC_REQ_WRITE_BUFFS:
		tpoint_id = TRACE_FC_REQ_WRITE_BUFFS;
		break;
	case SPDK_NVMF_BCM_FC_REQ_WRITE_XFER:
		tpoint_id = TRACE_FC_REQ_WRITE_XFER;
		break;
	case SPDK_NVMF_BCM_FC_REQ_WRITE_BDEV:
		tpoint_id = TRACE_FC_REQ_WRITE_BDEV;
		break;
	case SPDK_NVMF_BCM_FC_REQ_WRITE_RSP:
		tpoint_id = TRACE_FC_REQ_WRITE_RSP;
		break;
	case SPDK_NVMF_BCM_FC_REQ_NONE_BDEV:
		tpoint_id = TRACE_FC_REQ_NONE_BDEV;
		break;
	case SPDK_NVMF_BCM_FC_REQ_NONE_RSP:
		tpoint_id = TRACE_FC_REQ_NONE_RSP;
		break;
	case SPDK_NVMF_BCM_FC_REQ_SUCCESS:
		tpoint_id = TRACE_FC_REQ_SUCCESS;
		break;
	case SPDK_NVMF_BCM_FC_REQ_FAILED:
		tpoint_id = TRACE_FC_REQ_FAILED;
		break;
	case SPDK_NVMF_BCM_FC_REQ_ABORTED:
		tpoint_id = TRACE_FC_REQ_ABORTED;
		break;
	case SPDK_NVMF_BCM_FC_REQ_PENDING:
		tpoint_id = TRACE_FC_REQ_PENDING;
		break;
	default:
		assert(tpoint_id != SPDK_TRACE_MAX_TPOINT_ID);
		break;
	}
	if (tpoint_id != SPDK_TRACE_MAX_TPOINT_ID) {
		fc_req->req.req_state_trace[state] = spdk_get_ticks();
		spdk_trace_record(tpoint_id, fc_req->poller_lcore, 0,
				  (uint64_t)(&fc_req->req), 0);
	}
}

int
spdk_nvmf_bcm_fc_create_reqtag_pool(struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	char name[48];
	int i;
	struct fc_wrkq *wq = &hwqp->queues.wq;
	fc_reqtag_t *obj;
	static int unique_number = 0;

	unique_number++;

	snprintf(name, sizeof(name), "NVMF_FC_REQTAG_POOL:%d", unique_number);

	/* Create reqtag ring of size MAX_REQTAG_POOL_SIZE + 1 and make sure its power of 2. */
	wq->reqtag_ring = spdk_ring_create(name, (MAX_REQTAG_POOL_SIZE + 1), SPDK_ENV_SOCKET_ID_ANY, 0);
	if (!wq->reqtag_ring) {
		SPDK_ERRLOG("create fc reqtag ring failed\n");
		return -1;
	}

	/* Create ring objects */
	wq->reqtag_objs = spdk_calloc(MAX_REQTAG_POOL_SIZE, sizeof(fc_reqtag_t));
	if (!wq->reqtag_objs) {
		SPDK_ERRLOG("create fc reqtag ring objects failed\n");
		goto error;
	}

	/* Initialise index value in ring objects and queue the objects to ring */
	for (i = 0; i < MAX_REQTAG_POOL_SIZE; i ++) {
		obj = wq->reqtag_objs + i;

		obj->index = i;
		if (spdk_ring_enqueue(wq->reqtag_ring, (void *)obj)) {
			SPDK_ERRLOG("fc reqtag ring enqueue objects failed %d\n", i);
			goto error;
		}
		wq->p_reqtags[i] = NULL;
	}

	/* Init the wqec counter */
	wq->wqec_count = 0;

	return 0;
error:
	if (wq->reqtag_objs) {
		spdk_free(wq->reqtag_objs);
	}

	if (wq->reqtag_ring) {
		spdk_ring_free(wq->reqtag_ring);
	}
	return -1;
}

static fc_reqtag_t *
nvmf_fc_get_reqtag(struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	struct fc_wrkq *wq = &hwqp->queues.wq;
	fc_reqtag_t *tag;

	if (spdk_ring_dequeue(wq->reqtag_ring, (void **)&tag)) {
		return NULL;
	}

	/* Save the pointer for lookup */
	wq->p_reqtags[tag->index] = tag;
	return tag;
}

static fc_reqtag_t *
nvmf_fc_lookup_reqtag(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint16_t index)
{
	assert(index < MAX_REQTAG_POOL_SIZE);

	return hwqp->queues.wq.p_reqtags[index];
}

static int
nvmf_fc_release_reqtag(struct spdk_nvmf_bcm_fc_hwqp *hwqp, fc_reqtag_t *tag)
{
	struct fc_wrkq *wq = &hwqp->queues.wq;
	int rc;

	rc = spdk_ring_enqueue(wq->reqtag_ring, tag);

	wq->p_reqtags[tag->index] = NULL;
	tag->cb = NULL;
	tag->cb_args = NULL;

	return rc;
}

static inline struct spdk_nvmf_bcm_fc_request *
nvmf_fc_alloc_req_buf(struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	struct spdk_nvmf_bcm_fc_request *fc_req;

	fc_req = (struct spdk_nvmf_bcm_fc_request *)spdk_mempool_get(hwqp->fc_request_pool);
	if (!fc_req) {
		SPDK_ERRLOG("Alloc request buffer failed\n");
		return NULL;
	}

	memset(fc_req, 0, sizeof(struct spdk_nvmf_bcm_fc_request));
	TAILQ_INSERT_TAIL(&hwqp->in_use_reqs, fc_req, link);
	TAILQ_INIT(&fc_req->abort_cbs);
	return fc_req;
}

static inline void
nvmf_fc_free_req_buf(struct spdk_nvmf_bcm_fc_hwqp *hwqp, struct spdk_nvmf_bcm_fc_request *fc_req)
{
	if (fc_req->state != SPDK_NVMF_BCM_FC_REQ_SUCCESS) {
		/* Log an error for debug purpose. */
		spdk_nvmf_bcm_fc_req_set_state(fc_req, SPDK_NVMF_BCM_FC_REQ_FAILED);
	}

	/* set the magic to mark req as no longer valid. */
	fc_req->magic = 0xDEADBEEF;

	TAILQ_REMOVE(&hwqp->in_use_reqs, fc_req, link);
	spdk_mempool_put(hwqp->fc_request_pool, (void *)fc_req);
}

static void
nvmf_fc_release_io_buff(struct spdk_nvmf_bcm_fc_request *fc_req)
{
	if (fc_req->req.data) {
		spdk_dma_free(fc_req->req.data);
	} else if (fc_req->req.iovcnt || fc_req->req.bdev_io) {
		spdk_nvmf_request_cleanup(&fc_req->req);
	} else {
		return;
	}

	fc_req->req.data = NULL;
	fc_req->req.iovcnt  = 0;
	fc_req->req.bdev_io = NULL;
}

void
spdk_nvmf_bcm_fc_req_set_state(struct spdk_nvmf_bcm_fc_request *fc_req,
			       spdk_nvmf_bcm_fc_request_state_t state)
{
	assert(fc_req->magic != 0xDEADBEEF);

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
		      "FC Request(%p):\n\tState Old:%s New:%s\n", fc_req,
		      fc_req_state_strs[fc_req->state], fc_req_state_strs[state]);
	nvmf_fc_record_req_trace_point(fc_req, state);
	fc_req->state = state;
}


static inline bool
nvmf_fc_req_in_bdev(struct spdk_nvmf_bcm_fc_request *fc_req)
{
	switch (fc_req->state) {
	case SPDK_NVMF_BCM_FC_REQ_READ_BDEV:
	case SPDK_NVMF_BCM_FC_REQ_WRITE_BDEV:
	case SPDK_NVMF_BCM_FC_REQ_NONE_BDEV:
		return true;
	default:
		return false;
	}
}

static inline bool
nvmf_fc_req_in_get_buff(struct spdk_nvmf_bcm_fc_request *fc_req)
{
	switch (fc_req->state) {
	case SPDK_NVMF_BCM_FC_REQ_WRITE_BUFFS:
		return true;
	default:
		return false;
	}
}

static inline bool
nvmf_fc_req_in_xfer(struct spdk_nvmf_bcm_fc_request *fc_req)
{
	switch (fc_req->state) {
	case SPDK_NVMF_BCM_FC_REQ_READ_XFER:
	case SPDK_NVMF_BCM_FC_REQ_READ_RSP:
	case SPDK_NVMF_BCM_FC_REQ_WRITE_XFER:
	case SPDK_NVMF_BCM_FC_REQ_WRITE_RSP:
	case SPDK_NVMF_BCM_FC_REQ_NONE_RSP:
		return true;
	default:
		return false;
	}
}

static inline void
nvmf_fc_process_pending_req(struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	struct spdk_nvmf_bcm_fc_conn *fc_conn = NULL;
	struct spdk_nvmf_bcm_fc_request *fc_req = NULL, *tmp;
	int budget = 64;

	TAILQ_FOREACH(fc_conn, &hwqp->connection_list, link) {
		TAILQ_FOREACH_SAFE(fc_req, &fc_conn->pending_queue, pending_link, tmp) {
			if (!nvmf_fc_execute_nvme_rqst(fc_req)) {
				/* Succesfuly posted, Delete from pending. */
				TAILQ_REMOVE(&fc_conn->pending_queue, fc_req, pending_link);
			}

			if (budget) {
				budget --;
			} else {
				return;
			}
		}
	}
}

static inline bool
nvmf_fc_req_in_pending(struct spdk_nvmf_bcm_fc_request *fc_req)
{
	struct spdk_nvmf_bcm_fc_request *tmp = NULL;

	TAILQ_FOREACH(tmp, &fc_req->fc_conn->pending_queue, pending_link) {
		if (tmp == fc_req) {
			return true;
		}
	}
	return false;
}

static void
nvmf_fc_req_bdev_abort(void *arg1, void *arg2)
{
	struct spdk_nvmf_bcm_fc_request *fc_req = arg1;
	/* Initial release - we don't have to abort Admin Queue or
	 * Fabric commands. The AQ commands supported at this time are
	 * Get-Log-Page,
	 * Identify
	 * Set Features
	 * Get Features
	 * AER -> Special case and handled differently.
	 * Every one of the above Admin commands (except AER) run
	 * to completion and so an Abort of such commands doesn't
	 * make sense.
	 */
	/* Note that fabric commands are also not aborted via this
	 * mechanism. That check is present in the spdk_nvmf_request_abort
	 * function. The Fabric commands supported are
	 * Property Set
	 * Property Get
	 * Connect -> Special case (async. handling). Not sure how to
	 * handle at this point. Let it run to completion.
	 */
	spdk_nvmf_request_abort(&fc_req->req);
}

static bool
nvmf_fc_is_port_dead(struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	switch (hwqp->fc_port->hw_port_status) {
	case SPDK_FC_PORT_QUIESCED:
		return true;
	default:
		return false;
	}
}

void
spdk_nvmf_bcm_fc_req_abort_complete(void *arg1, void *arg2)
{
	struct spdk_nvmf_bcm_fc_request *fc_req =
		(struct spdk_nvmf_bcm_fc_request *)arg1;
	fc_caller_ctx_t *ctx = NULL, *tmp = NULL;

	/*
	 * Release backend IO buffers before calling cb's because
	 * cb's can release the session which is required for
	 * backend IO buffers cleanup.
	 */
	nvmf_fc_release_io_buff(fc_req);

	/* Request abort completed. Notify all the callbacks */
	TAILQ_FOREACH_SAFE(ctx, &fc_req->abort_cbs, link, tmp) {
		/* Notify */
		ctx->cb(fc_req->hwqp, 0, ctx->cb_args);
		/* Remove */
		TAILQ_REMOVE(&fc_req->abort_cbs, ctx, link);
		/* free */
		spdk_free(ctx);
	}

	SPDK_NOTICELOG("FC Request(%p) in state :%s aborted", fc_req,
		       fc_req_state_strs[fc_req->state]);

	spdk_nvmf_bcm_fc_free_req(fc_req);
}

void
spdk_nvmf_bcm_fc_req_abort(struct spdk_nvmf_bcm_fc_request *fc_req,
			   bool send_abts, spdk_nvmf_bcm_fc_caller_cb cb,
			   void *cb_args)
{
	fc_caller_ctx_t *ctx = NULL;
	struct spdk_event *event = NULL;
	bool kill_req = false;

	/* Add the cb to list */
	if (cb) {
		ctx = spdk_calloc(1, sizeof(fc_caller_ctx_t));
		if (!ctx) {
			SPDK_ERRLOG("%s: ctx alloc failed. \n", __func__);
			return;
		}
		ctx->cb = cb;
		ctx->cb_args = cb_args;

		TAILQ_INSERT_TAIL(&fc_req->abort_cbs, ctx, link);
	}

	if (!fc_req->is_aborted) {
		/* Increment aborted command counter */
		fc_req->hwqp->counters.num_aborted++;
	}

	/* If port is dead, skip abort wqe */
	kill_req = nvmf_fc_is_port_dead(fc_req->hwqp);
	if (kill_req && nvmf_fc_req_in_xfer(fc_req)) {
		fc_req->is_aborted = true;
		goto complete;
	}

	/* Check if the request is already marked for deletion */
	if (fc_req->is_aborted) {
		return;
	}

	/* Mark request as aborted */
	fc_req->is_aborted = true;

	if (nvmf_fc_req_in_bdev(fc_req)) {
		/* Notify bdev */
		nvmf_fc_req_bdev_abort(fc_req, NULL);
	} else if (nvmf_fc_req_in_xfer(fc_req)) {
		/* Notify hw */
		spdk_nvmf_bcm_fc_issue_abort(fc_req->hwqp, fc_req->xri,
					     send_abts, NULL, NULL);
	} else if (nvmf_fc_req_in_get_buff(fc_req)) {
		/* Will be completed by request_complete callback. */
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC, "Abort req when getting buffers.\n");
	} else if (nvmf_fc_req_in_pending(fc_req)) {
		/* Remove from pending */
		TAILQ_REMOVE(&fc_req->fc_conn->pending_queue, fc_req, pending_link);
		goto complete;
	} else {
		/* Should never happen */
		SPDK_ERRLOG("%s: Request in invalid state\n", __func__);
		goto complete;
	}

	return;
complete:
	spdk_nvmf_bcm_fc_req_set_state(fc_req, SPDK_NVMF_BCM_FC_REQ_ABORTED);
	event = spdk_event_allocate(fc_req->poller_lcore,
				    spdk_nvmf_bcm_fc_req_abort_complete, (void *)fc_req, NULL);
	spdk_post_event(fc_req->hwqp->context, event);
}


static inline int
nvmf_fc_find_nport_and_rport(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
			     uint32_t d_id, struct spdk_nvmf_bcm_fc_nport **nport,
			     uint32_t s_id, struct spdk_nvmf_bcm_fc_remote_port_info **rport)
{
	struct spdk_nvmf_bcm_fc_nport *n_port;
	struct spdk_nvmf_bcm_fc_remote_port_info *r_port;

	assert(hwqp);
	assert(nport);
	assert(rport);

	TAILQ_FOREACH(n_port, &hwqp->fc_port->nport_list, link) {
		if (n_port->d_id == d_id) {
			TAILQ_FOREACH(r_port, &n_port->rem_port_list, link) {
				if (r_port->s_id == s_id) {
					*nport = n_port;
					*rport = r_port;
					return 0;
				}
			}
			break;
		}
	}
	return -1;
}

static void
nvmf_fc_bcm_notify_queue(bcm_sli_queue_t *q, bool arm_queue, uint16_t num_entries)
{
	doorbell_t *reg, entry;

	reg = (doorbell_t *)q->doorbell_reg;
	entry.doorbell = 0;

	switch (q->type) {
	case BCM_FC_QUEUE_TYPE_EQ:
		entry.eqdoorbell.qt = 1;
		entry.eqdoorbell.ci = 1;
		entry.eqdoorbell.num_popped = num_entries;
		entry.eqdoorbell.eq_id = (q->qid & 0x1ff);
		entry.eqdoorbell.eq_id_ext = ((q->qid >> 9) & 0x1f);
		entry.eqdoorbell.arm = arm_queue;
		break;
	case BCM_FC_QUEUE_TYPE_CQ_WQ:
	case BCM_FC_QUEUE_TYPE_CQ_RQ:
		entry.cqdoorbell.num_popped = num_entries;
		entry.cqdoorbell.cq_id = (q->qid & 0x3ff);
		entry.cqdoorbell.cq_id_ext = ((q->qid >> 10) & 0x1f);
		entry.cqdoorbell.solicit_enable = 0;
		entry.cqdoorbell.arm = arm_queue;
		break;
	case BCM_FC_QUEUE_TYPE_WQ:
		entry.wqdoorbell.wq_id = (q->qid & 0xffff);
		entry.wqdoorbell.wq_index = (q->head & 0x00ff);
		entry.wqdoorbell.num_posted = num_entries;
		break;
	case BCM_FC_QUEUE_TYPE_RQ_HDR:
	case BCM_FC_QUEUE_TYPE_RQ_DATA:
		entry.rqdoorbell.rq_id = q->qid;
		entry.rqdoorbell.num_posted = num_entries;
		break;
	}

	reg->doorbell = entry.doorbell;
}

static uint8_t
nvmf_fc_queue_entry_is_valid(bcm_sli_queue_t *q, uint8_t *qe, uint8_t clear)
{
	uint8_t valid = 0;

	switch (q->type) {
	case BCM_FC_QUEUE_TYPE_EQ:
		valid = ((eqe_t *)qe)->valid;
		if (valid & clear) {
			((eqe_t *)qe)->valid = 0;
		}
		break;
	case BCM_FC_QUEUE_TYPE_CQ_WQ:
	case BCM_FC_QUEUE_TYPE_CQ_RQ:
		/*
		 * For both WCQE and RCQE, the valid bit
		 * is bit 31 of dword 3 (0 based)
		 */
		valid = (qe[15] & 0x80) != 0;
		if (valid & clear) {
			qe[15] &= ~0x80;
		}
		break;
	default:
		SPDK_ERRLOG("%s doesn't handle type=%#x\n", __func__, q->type);
	}

	return valid;
}

static int
nvmf_fc_read_queue_entry(bcm_sli_queue_t *q, uint8_t *entry)
{
	uint8_t	*qe;

	switch (q->type) {
	case BCM_FC_QUEUE_TYPE_EQ:
	case BCM_FC_QUEUE_TYPE_CQ_WQ:
	case BCM_FC_QUEUE_TYPE_CQ_RQ:
		break;
	default:
		SPDK_ERRLOG("%s read not handled for queue type=%#x\n",
			    __func__, q->type);
		return -1;
	}

	/* Get the tail entry */
	qe = nvmf_fc_queue_tail_node(q);

	/* Check if entry is valid */
	if (!nvmf_fc_queue_entry_is_valid(q, qe, true)) {
		return -1;
	}

	/* Make a copy if user requests */
	if (entry) {
		memcpy(entry, qe, q->size);
	}

	nvmf_fc_queue_tail_inc(q);

	return 0;
}

static int
nvmf_fc_write_queue_entry(bcm_sli_queue_t *q, uint8_t *entry)
{
	uint8_t	*qe;

	if (!entry) {
		return -1;
	}

	switch (q->type) {
	case BCM_FC_QUEUE_TYPE_WQ:
	case BCM_FC_QUEUE_TYPE_RQ_HDR:
	case BCM_FC_QUEUE_TYPE_RQ_DATA:
		break;
	default:
		SPDK_ERRLOG("%s write not handled for queue type=%#x\n",
			    __func__, q->type);
		// For other queues write is not valid.
		return -1;
	}

	/* We need to check if there is space available */
	if (nvmf_fc_queue_full(q)) {
		SPDK_ERRLOG("%s queue full for type = %#x\n", __func__, q->type);
		return -1;
	}

	/* Copy entry */
	qe = nvmf_fc_queue_head_node(q);
	memcpy(qe, entry, q->size);

	/* Update queue */
	nvmf_fc_queue_head_inc(q);

	return 0;
}

static int
nvmf_fc_post_wqe(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint8_t *entry, bool notify,
		 bcm_fc_wqe_cb cb, void *cb_args)
{
	int rc = -1;
	bcm_generic_wqe_t *wqe = (bcm_generic_wqe_t *)entry;
	struct fc_wrkq *wq = &hwqp->queues.wq;
	fc_reqtag_t *reqtag = NULL;

	if (!entry || !cb) {
		goto error;
	}

	/* Make sure queue is not full */
	if (nvmf_fc_queue_full(&wq->q)) {
		SPDK_ERRLOG("%s queue full. type = %#x\n", __func__, wq->q.type);
		goto error;
	}

	/* Alloc a reqtag */
	reqtag = nvmf_fc_get_reqtag(hwqp);
	if (!reqtag) {
		SPDK_ERRLOG("%s No reqtag available\n", __func__);
		goto error;
	}
	reqtag->cb = cb;
	reqtag->cb_args = cb_args;

	/* Update request tag in the WQE entry */
	wqe->request_tag = reqtag->index;
	wq->wqec_count ++;

	if (wq->wqec_count == MAX_WQ_WQEC_CNT) {
		wqe->wqec = 1;
	}

	rc = nvmf_fc_write_queue_entry(&wq->q, entry);
	if (rc) {
		SPDK_ERRLOG("%s: WQE write failed. \n", __func__);
		hwqp->counters.wqe_write_err++;
		goto error;
	}

	wq->q.used++;
	if (wqe->wqec) {
		/* Reset wqec count. */
		wq->wqec_count = 0;
	}

	if (notify) {
		nvmf_fc_bcm_notify_queue(&wq->q, false, 1);
	}
	return 0;
error:
	if (reqtag) {
		nvmf_fc_release_reqtag(hwqp, reqtag);
	}
	return rc;
}

#if BCM_SUPPORT_EQ_POLL
static int
nvmf_fc_parse_eq_entry(struct eqe *qe, uint16_t *cq_id)
{
	int rc = 0;

	assert(qe);
	assert(cq_id);

	if (!qe || !cq_id) {
		SPDK_ERRLOG("%s: bad parameters eq=%p, cq_id=%p\n", __func__,
			    qe, cq_id);
		return -1;
	}

	switch (qe->major_code) {
	case BCM_MAJOR_CODE_STANDARD:
		*cq_id = qe->resource_id;
		rc = 0;
		break;
	case BCM_MAJOR_CODE_SENTINEL:
		SPDK_NOTICELOG("%s: sentinel EQE\n", __func__);
		rc = 1;
		break;
	default:
		SPDK_NOTICELOG("%s: Unsupported EQE: major %x minor %x\n",
			       __func__, qe->major_code, qe->minor_code);
		rc = -1;
	}
	return rc;
}
#endif

static uint32_t
nvmf_fc_parse_cqe_ext_status(uint8_t *cqe)
{
	cqe_t *cqe_entry = (void *)cqe;
	uint32_t mask;

	switch (cqe_entry->u.wcqe.status) {
	case BCM_FC_WCQE_STATUS_FCP_RSP_FAILURE:
		mask = UINT32_MAX;
		break;
	case BCM_FC_WCQE_STATUS_LOCAL_REJECT:
	case BCM_FC_WCQE_STATUS_CMD_REJECT:
		mask = 0xff;
		break;
	case BCM_FC_WCQE_STATUS_NPORT_RJT:
	case BCM_FC_WCQE_STATUS_FABRIC_RJT:
	case BCM_FC_WCQE_STATUS_NPORT_BSY:
	case BCM_FC_WCQE_STATUS_FABRIC_BSY:
	case BCM_FC_WCQE_STATUS_LS_RJT:
		mask = UINT32_MAX;
		break;
	case BCM_FC_WCQE_STATUS_DI_ERROR:
		mask = UINT32_MAX;
		break;
	default:
		mask = 0;
	}

	return cqe_entry->u.wcqe.wqe_specific_2 & mask;
}



static int
nvmf_fc_parse_cq_entry(struct fc_eventq *cq, uint8_t *cqe, bcm_qentry_type_e *etype, uint16_t *r_id)
{
	int     rc = -1;
	cqe_t *cqe_entry = (cqe_t *)cqe;
	uint32_t ext_status = 0;

	if (!cq || !cqe || !etype || !r_id) {
		SPDK_ERRLOG("%s: bad parameters cq=%p cqe=%p etype=%p q_id=%p\n",
			    __func__, cq, cqe, etype, r_id);
		return -1;
	}

	switch (cqe_entry->u.generic.event_code) {
	case BCM_CQE_CODE_WORK_REQUEST_COMPLETION: {
		*etype = BCM_FC_QENTRY_WQ;
		*r_id = cqe_entry->u.wcqe.request_tag;
		rc = cqe_entry->u.wcqe.status;
		if (rc) {
			ext_status = nvmf_fc_parse_cqe_ext_status(cqe);
			if ((rc == BCM_FC_WCQE_STATUS_LOCAL_REJECT) &&
			    ((ext_status == BCM_FC_LOCAL_REJECT_NO_XRI) ||
			     (ext_status == BCM_FC_LOCAL_REJECT_ABORT_REQUESTED))) {
				SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
					      "WCQE: status=%#x hw_status=%#x tag=%#x w1=%#x w2=%#x\n",
					      cqe_entry->u.wcqe.status,
					      cqe_entry->u.wcqe.hw_status,
					      cqe_entry->u.wcqe.request_tag,
					      cqe_entry->u.wcqe.wqe_specific_1,
					      cqe_entry->u.wcqe.wqe_specific_2);
			} else {
				SPDK_NOTICELOG("WCQE: status=%#x hw_status=%#x tag=%#x w1=%#x w2=%#x xb=%d\n",
					       cqe_entry->u.wcqe.status,
					       cqe_entry->u.wcqe.hw_status,
					       cqe_entry->u.wcqe.request_tag,
					       cqe_entry->u.wcqe.wqe_specific_1,
					       cqe_entry->u.wcqe.wqe_specific_2,
					       cqe_entry->u.wcqe.xb);
				SPDK_NOTICELOG("  %08X %08X %08X %08X\n",
					       ((uint32_t *)cqe)[0], ((uint32_t *)cqe)[1],
					       ((uint32_t *)cqe)[2], ((uint32_t *)cqe)[3]);
			}
		}
		break;
	}
	case BCM_CQE_CODE_RQ_ASYNC: {
		*etype = BCM_FC_QENTRY_RQ;
		*r_id = cqe_entry->u.async_rcqe.rq_id;
		rc = cqe_entry->u.async_rcqe.status;
		break;
	}
	case BCM_CQE_CODE_RQ_ASYNC_V1: {
		*etype = BCM_FC_QENTRY_RQ;
		*r_id = cqe_entry->u.async_rcqe_v1.rq_id;
		rc = cqe_entry->u.async_rcqe_v1.status;
		break;
	}
	case BCM_CQE_CODE_RQ_MARKER: {
		*etype = BCM_FC_QENTRY_RQ;
		*r_id = cqe_entry->u.async_marker.rq_id;
		rc = cqe_entry->u.async_marker.status;
		break;
	}
	case BCM_CQE_CODE_XRI_ABORTED: {
		*etype = BCM_FC_QENTRY_XABT;
		*r_id = cqe_entry->u.xri_aborted_cqe.xri;
		rc = 0;
		break;
	}
	case BCM_CQE_CODE_RELEASE_WQE: {
		*etype = BCM_FC_QENTRY_WQ_RELEASE;
		*r_id = cqe_entry->u.wqec.wq_id;
		rc = 0;
		break;
	}
	default:
		SPDK_ERRLOG("%s: CQE completion code %d not handled\n", __func__,
			    cqe_entry->u.generic.event_code);
		*etype = BCM_FC_QENTRY_MAX;
		*r_id = UINT16_MAX;

	}
	return rc;
}

static int
nvmf_fc_rqe_rqid_and_index(uint8_t *cqe, uint16_t *rq_id, uint32_t *index)
{
	bcm_fc_async_rcqe_t	*rcqe = (void *)cqe;
	bcm_fc_async_rcqe_v1_t	*rcqe_v1 = (void *)cqe;
	bcm_fc_async_rcqe_marker_t *marker = (void *)cqe;
	int	rc = -1;
	uint8_t	code = 0;

	*rq_id = 0;
	*index = UINT32_MAX;

	code = cqe[BCM_CQE_CODE_OFFSET];

	if (code == BCM_CQE_CODE_RQ_ASYNC) {
		*rq_id = rcqe->rq_id;
		if (BCM_FC_ASYNC_RQ_SUCCESS == rcqe->status) {
			*index = rcqe->rq_element_index;
			rc = 0;
		} else {
			*index = rcqe->rq_element_index;
			rc = rcqe->status;
			SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
				      "%s: status=%02x rq_id=%d, index=%x pdpl=%x sof=%02x eof=%02x hdpl=%x\n",
				      __func__, rcqe->status,
				      rcqe->rq_id,
				      rcqe->rq_element_index, rcqe->payload_data_placement_length, rcqe->sof_byte,
				      rcqe->eof_byte, rcqe->header_data_placement_length);
		}
	} else if (code == BCM_CQE_CODE_RQ_ASYNC_V1) {
		*rq_id = rcqe_v1->rq_id;
		if (BCM_FC_ASYNC_RQ_SUCCESS == rcqe_v1->status) {
			*index = rcqe_v1->rq_element_index;
			rc = 0;
		} else {
			*index = rcqe_v1->rq_element_index;
			rc = rcqe_v1->status;
			SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
				      "%s: status=%02x rq_id=%d, index=%x pdpl=%x sof=%02x eof=%02x hdpl=%x\n",
				      __func__, rcqe_v1->status,
				      rcqe_v1->rq_id, rcqe_v1->rq_element_index,
				      rcqe_v1->payload_data_placement_length, rcqe_v1->sof_byte,
				      rcqe_v1->eof_byte, rcqe_v1->header_data_placement_length);
		}

	} else if (code == BCM_CQE_CODE_RQ_MARKER) {
		*rq_id = marker->rq_id;
		*index = marker->rq_element_index;
		if (BCM_FC_ASYNC_RQ_SUCCESS == marker->status) {
			rc = 0;
			SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
				      "%s: marker cqe status=%02x rq_id=%d, index=%x\n", __func__,
				      marker->status, marker->rq_id, marker->rq_element_index);
		} else {
			rc = marker->status;
			SPDK_ERRLOG("%s: marker cqe status=%02x rq_id=%d, index=%x\n", __func__,
				    marker->status, marker->rq_id, marker->rq_element_index);
		}

	} else {
		*index = UINT32_MAX;

		rc = rcqe->status;

		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
			      "%s: status=%02x rq_id=%d, index=%x pdpl=%x sof=%02x eof=%02x hdpl=%x\n", __func__,
			      rcqe->status, rcqe->rq_id, rcqe->rq_element_index, rcqe->payload_data_placement_length,
			      rcqe->sof_byte, rcqe->eof_byte, rcqe->header_data_placement_length);
	}

	return rc;
}

static int
nvmf_fc_rqpair_buffer_post(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint16_t idx, bool notify)
{
	int rc;
	struct fc_rcvq *hdr = &hwqp->queues.rq_hdr;
	struct fc_rcvq *payload = &hwqp->queues.rq_payload;
	uint32_t phys_hdr[2];

	/* Post payload buffer */
	phys_hdr[0] =  PTR_TO_ADDR32_HI(payload->buffer[idx].phys);
	phys_hdr[1] =  PTR_TO_ADDR32_LO(payload->buffer[idx].phys);
	rc = nvmf_fc_write_queue_entry(&payload->q, (uint8_t *)phys_hdr);
	if (!rc) {
		/* Post header buffer */
		phys_hdr[0] =  PTR_TO_ADDR32_HI(hdr->buffer[idx].phys);
		phys_hdr[1] =  PTR_TO_ADDR32_LO(hdr->buffer[idx].phys);
		rc = nvmf_fc_write_queue_entry(&hdr->q, (uint8_t *)phys_hdr);
		if (!rc) {

			hwqp->queues.rq_hdr.q.used++;
			hwqp->queues.rq_payload.q.used++;

			if (notify) {
				nvmf_fc_bcm_notify_queue(&hdr->q, false, 1);
			}
		}
	}
	return rc;
}

static void
nvmf_fc_rqpair_buffer_release(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint16_t buff_idx)
{
	/* Decrement used */
	hwqp->queues.rq_hdr.q.used--;
	hwqp->queues.rq_payload.q.used--;

	/* Increment tail */
	nvmf_fc_queue_tail_inc(&hwqp->queues.rq_hdr.q);
	nvmf_fc_queue_tail_inc(&hwqp->queues.rq_payload.q);

	/* Repost the freebuffer to head of queue. */
	hwqp->queues.rq_hdr.rq_map[hwqp->queues.rq_hdr.q.head] = buff_idx;
	nvmf_fc_rqpair_buffer_post(hwqp, buff_idx, true);
}

int
spdk_nvmf_bcm_fc_init_rqpair_buffers(struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	int rc = 0;
	uint16_t i;
	struct fc_rcvq *hdr = &hwqp->queues.rq_hdr;
	struct fc_rcvq *payload = &hwqp->queues.rq_payload;

	/* Init queue variables */
	hwqp->queues.eq.q.posted_limit = 16;
	hwqp->queues.cq_wq.q.posted_limit = 16;
	hwqp->queues.cq_rq.q.posted_limit = 16;

	hwqp->queues.eq.q.processed_limit = 64;
	hwqp->queues.cq_wq.q.processed_limit = 64;
	hwqp->queues.cq_rq.q.processed_limit = 64;

	hwqp->queues.eq.auto_arm_flag = false;

#if BCM_SUPPORT_EQ_POLL
	hwqp->queues.cq_wq.auto_arm_flag = true;
	hwqp->queues.cq_rq.auto_arm_flag = true;
#else
	hwqp->queues.cq_wq.auto_arm_flag = false;
	hwqp->queues.cq_rq.auto_arm_flag = false;
#endif

	hwqp->queues.eq.q.type = BCM_FC_QUEUE_TYPE_EQ;
	hwqp->queues.cq_wq.q.type = BCM_FC_QUEUE_TYPE_CQ_WQ;
	hwqp->queues.wq.q.type = BCM_FC_QUEUE_TYPE_WQ;
	hwqp->queues.cq_rq.q.type = BCM_FC_QUEUE_TYPE_CQ_RQ;
	hwqp->queues.rq_hdr.q.type = BCM_FC_QUEUE_TYPE_RQ_HDR;
	hwqp->queues.rq_payload.q.type = BCM_FC_QUEUE_TYPE_RQ_DATA;

	if (hdr->q.max_entries != payload->q.max_entries) {
		assert(0);
	}
	if (hdr->q.max_entries > MAX_RQ_ENTRIES) {
		assert(0);
	}

	for (i = 0; i < hdr->q.max_entries; i++) {
		rc = nvmf_fc_rqpair_buffer_post(hwqp, i, false);
		if (rc) {
			break;
		}
		hdr->rq_map[i] = i;
	}

#if BCM_SUPPORT_EQ_POLL
	/* Make sure CQs are in armed state */
	nvmf_fc_bcm_notify_queue(&hwqp->queues.cq_wq.q, true, 0);
	nvmf_fc_bcm_notify_queue(&hwqp->queues.cq_rq.q, true, 0);
#else
	/* Make sure CQs are not armed */
	nvmf_fc_bcm_notify_queue(&hwqp->queues.cq_wq.q, false, 0);
	nvmf_fc_bcm_notify_queue(&hwqp->queues.cq_rq.q, false, 0);
#endif

	if (!rc) {
		/* Ring doorbell for one less */
		nvmf_fc_bcm_notify_queue(&hdr->q, false, (hdr->q.max_entries - 1));
	}

	return rc;
}

static void
nvmf_fc_nvmf_add_xri_pending(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
			     struct spdk_nvmf_bcm_fc_xri *xri)
{
	struct spdk_nvmf_bcm_fc_xri *tmp;

	/* Check if its already exists. */
	TAILQ_FOREACH(tmp, &hwqp->pending_xri_list, link) {
		if (tmp == xri) {
			return;
		}
	}

	/* Add */
	TAILQ_INSERT_TAIL(&hwqp->pending_xri_list, xri, link);
}

static void
nvmf_fc_nvmf_del_xri_pending(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint32_t xri)
{
	struct spdk_nvmf_bcm_fc_xri *tmp;

	TAILQ_FOREACH(tmp, &hwqp->pending_xri_list, link) {
		if (tmp->xri == xri) {
			spdk_nvmf_bcm_fc_put_xri(hwqp, tmp);
			TAILQ_REMOVE(&hwqp->pending_xri_list, tmp, link);
			return;
		}
	}
}

static bool
nvmf_fc_abts_required(uint8_t *cqe_entry)
{
	cqe_t *cqe = (cqe_t *)cqe_entry;
	uint16_t status = cqe->u.wcqe.status;
	uint32_t ext_status = nvmf_fc_parse_cqe_ext_status(cqe_entry);
	bool send_abts = false;

	if (BCM_SUPPORT_ABTS_FOR_SEQ_ERRORS && status &&
	    !(status == BCM_FC_WCQE_STATUS_LOCAL_REJECT &&
	      ((ext_status == BCM_FC_LOCAL_REJECT_NO_XRI) ||
	       (ext_status == BCM_FC_LOCAL_REJECT_INVALID_RPI) ||
	       (ext_status == BCM_FC_LOCAL_REJECT_ABORT_REQUESTED)))) {
		send_abts = true;
	}
	return send_abts;
}

void
spdk_nvmf_bcm_fc_release_xri(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
			     struct spdk_nvmf_bcm_fc_xri *xri, bool xb, bool abts)
{
	if (xb && xri->is_active && !nvmf_fc_is_port_dead(hwqp)) {
		/* Post an abort to clean XRI state */
		spdk_nvmf_bcm_fc_issue_abort(hwqp, xri, abts, NULL, NULL);
		nvmf_fc_nvmf_add_xri_pending(hwqp, xri);
	} else if (xb && !nvmf_fc_is_port_dead(hwqp)) {
		nvmf_fc_nvmf_add_xri_pending(hwqp, xri);
	} else {
		xri->is_active = false;
		spdk_nvmf_bcm_fc_put_xri(hwqp, xri);
	}
}

void
spdk_nvmf_bcm_fc_free_req(struct spdk_nvmf_bcm_fc_request *fc_req)
{
	if (!fc_req) {
		return;
	}

	if (fc_req->xri) {
		spdk_nvmf_bcm_fc_put_xri(fc_req->hwqp, fc_req->xri);
		fc_req->xri = NULL;
	}

	/* Release IO buffers */
	nvmf_fc_release_io_buff(fc_req);

	/* Release RQ buffer */
	nvmf_fc_rqpair_buffer_release(fc_req->hwqp, fc_req->buf_index);

	/* Free Fc request */
	nvmf_fc_free_req_buf(fc_req->hwqp, fc_req);
}

static void
nvmf_fc_abort_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	struct spdk_nvmf_bcm_fc_hwqp *hwqp = ctx;
	fc_caller_ctx_t *carg = arg;

	SPDK_NOTICELOG("IO Aborted(XRI:0x%x, Status=%d)\n",
		       ((struct spdk_nvmf_bcm_fc_xri *)(carg->ctx))->xri, status);

	if (carg->cb) {
		carg->cb(hwqp, status, carg->cb_args);
	}

	spdk_free(carg);
}

static void
nvmf_fc_bls_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	struct spdk_nvmf_bcm_fc_hwqp *hwqp = ctx;
	cqe_t *cqe_entry 	= (cqe_t *)cqe;
	fc_caller_ctx_t *carg 	= arg;
	struct spdk_nvmf_bcm_fc_xri *xri = carg->ctx;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC, "BLS WQE Compl(%d) \n", status);

	spdk_nvmf_bcm_fc_release_xri(hwqp, xri, cqe_entry->u.generic.xb, false);

	if (carg->cb) {
		carg->cb(hwqp, status, carg->cb_args);
	}

	spdk_free(carg);
}

static void
nvmf_fc_srsr_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	struct spdk_nvmf_bcm_fc_hwqp *hwqp = ctx;
	cqe_t *cqe_entry 	= (cqe_t *)cqe;
	fc_caller_ctx_t *carg 	= arg;
	struct spdk_nvmf_bcm_fc_xri *xri = carg->ctx;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC, "SRSR WQE Compl(%d) \n", status);

	spdk_nvmf_bcm_fc_release_xri(hwqp, xri, cqe_entry->u.generic.xb,
				     nvmf_fc_abts_required(cqe));

	if (carg->cb) {
		carg->cb(hwqp, status, carg->cb_args);
	}

	spdk_free(carg);
}

static void
nvmf_fc_ls_rsp_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst = arg;
	struct spdk_nvmf_bcm_fc_hwqp *hwqp = ctx;
	cqe_t *cqe_entry = (cqe_t *)cqe;

	spdk_nvmf_bcm_fc_release_xri(hwqp, ls_rqst->xri, cqe_entry->u.generic.xb,
				     nvmf_fc_abts_required(cqe));

	/* Release RQ buffer */
	nvmf_fc_rqpair_buffer_release(hwqp, ls_rqst->rqstbuf.buf_index);

	if (status) {
		SPDK_ERRLOG("LS WQE Compl(%d) error\n", status);
	}
}


static void
nvmf_fc_def_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC, "DEF WQE Compl(%d) \n", status);
}

static void
nvmf_fc_io_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	struct spdk_nvmf_bcm_fc_request *fc_req = arg;
	cqe_t *cqe_entry = (cqe_t *)cqe;
	int rc;

	/* Assert if its not a valid completion. */
	assert(fc_req->magic != 0xDEADBEEF);

	if (status || fc_req->is_aborted) {
		goto io_done;
	}

	/* Write Tranfer done */
	if (fc_req->state == SPDK_NVMF_BCM_FC_REQ_WRITE_XFER) {
		fc_req->transfered_len = cqe_entry->u.generic.word1.total_data_placed;

		spdk_nvmf_bcm_fc_req_set_state(fc_req, SPDK_NVMF_BCM_FC_REQ_WRITE_BDEV);

		rc = spdk_nvmf_request_exec(&fc_req->req);
		switch (rc) {
		case SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE:
		case SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS:
			break;
		default:
			goto io_done;
		}
		return;
	}
	/* Read Tranfer done */
	else if (fc_req->state == SPDK_NVMF_BCM_FC_REQ_READ_XFER) {

		fc_req->transfered_len = cqe_entry->u.generic.word1.total_data_placed;

		spdk_nvmf_bcm_fc_req_set_state(fc_req, SPDK_NVMF_BCM_FC_REQ_READ_RSP);
		if (spdk_nvmf_bcm_fc_handle_rsp(fc_req)) {
			goto io_done;
		}
		return;
	}

	/* IO completed successfully */
	spdk_nvmf_bcm_fc_req_set_state(fc_req, SPDK_NVMF_BCM_FC_REQ_SUCCESS);

io_done:
	spdk_nvmf_bcm_fc_release_xri(fc_req->hwqp, fc_req->xri,
				     cqe_entry->u.generic.xb, nvmf_fc_abts_required(cqe));
	fc_req->xri = NULL;

	if (fc_req->is_aborted) {
		spdk_nvmf_bcm_fc_req_abort_complete(fc_req, NULL);
	} else {
		spdk_nvmf_bcm_fc_free_req(fc_req);
	}
}

static void
nvmf_fc_process_wqe_completion(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint16_t tag, int status,
			       uint8_t *cqe)
{
	fc_reqtag_t *reqtag;

	reqtag = nvmf_fc_lookup_reqtag(hwqp, tag);
	if (!reqtag) {
		SPDK_ERRLOG("Could not find reqtag(%d) for WQE Compl HWQP = %d\n",
			    tag, hwqp->hwqp_id);
		return;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC, "WQE Compl(%d)\n", status);

	/* Call the callback */
	if (reqtag->cb) {
		reqtag->cb(hwqp, cqe, status, reqtag->cb_args);
	} else {
		SPDK_ERRLOG("reqtag(%d) cb NULL for WQE Compl\n", tag);
	}

	/* Release reqtag */
	if (nvmf_fc_release_reqtag(hwqp, reqtag)) {
		SPDK_ERRLOG("%s: reqtag(%d) release failed\n", __func__, tag);
	}
}

static void
nvmf_fc_process_wqe_release(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint16_t wqid)
{
	hwqp->queues.wq.q.used -= MAX_WQ_WQEC_CNT;
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC, "WQE RELEASE\n");
}

static uint32_t
nvmf_fc_fill_sgl(struct spdk_nvmf_bcm_fc_request *fc_req)
{
	int i;
	uint32_t offset = 0;
	uint64_t iov_phys;
	bcm_sge_t *sge = NULL;
	struct spdk_nvmf_bcm_fc_rq_buf_nvme_cmd *req_buf = NULL;
	struct spdk_nvmf_bcm_fc_hwqp *hwqp = fc_req->hwqp;

	assert((fc_req->req.iovcnt) <= BCM_MAX_IOVECS);
	assert(fc_req->req.iovcnt != 0);

	/* Use RQ buffer for SGL */
	req_buf = hwqp->queues.rq_payload.buffer[fc_req->buf_index].virt;
	sge = &req_buf->sge[0];

	memset(sge, 0, (sizeof(bcm_sge_t) * BCM_MAX_IOVECS));

	if (fc_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) { /* Write */
		uint64_t xfer_rdy_phys;
		struct spdk_nvmf_fc_xfer_rdy_iu *xfer_rdy_iu;

		/* 1st SGE is tranfer ready buffer */
		xfer_rdy_iu = hwqp->queues.rq_payload.buffer[fc_req->buf_index].virt +
			      offsetof(struct spdk_nvmf_bcm_fc_rq_buf_nvme_cmd, xfer_rdy);
		xfer_rdy_iu->relative_offset = 0;
		to_be32(&xfer_rdy_iu->burst_len, fc_req->req.length);

		xfer_rdy_phys = hwqp->queues.rq_payload.buffer[fc_req->buf_index].phys +
				offsetof(struct spdk_nvmf_bcm_fc_rq_buf_nvme_cmd, xfer_rdy);

		sge->sge_type = BCM_SGE_TYPE_DATA;
		sge->buffer_address_low  = PTR_TO_ADDR32_LO(xfer_rdy_phys);
		sge->buffer_address_high = PTR_TO_ADDR32_HI(xfer_rdy_phys);
		sge->buffer_length = sizeof(struct spdk_nvmf_fc_xfer_rdy_iu);
		sge++;

	} else if (fc_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) { /* read */
		/* 1st SGE is skip. */
		sge->sge_type = BCM_SGE_TYPE_SKIP;
		sge++;
	} else {
		return 0;
	}

	/* 2nd SGE is skip. */
	sge->sge_type = BCM_SGE_TYPE_SKIP;
	sge++;

	for (i = 0; i < fc_req->req.iovcnt; i++) {
		iov_phys = spdk_vtophys(fc_req->req.iov[i].iov_base);
		sge->sge_type = BCM_SGE_TYPE_DATA;
		sge->buffer_address_low  = PTR_TO_ADDR32_LO(iov_phys);
		sge->buffer_address_high = PTR_TO_ADDR32_HI(iov_phys);
		sge->buffer_length = fc_req->req.iov[i].iov_len;
		sge->data_offset = offset;
		offset += fc_req->req.iov[i].iov_len;

		if (i == (fc_req->req.iovcnt - 1)) {
			/* last */
			sge->last = true;
		} else {
			sge++;
		}
	}
	return offset;
}

static int
nvmf_fc_recv_data(struct spdk_nvmf_bcm_fc_request *fc_req)
{
	int rc = 0;
	uint8_t wqe[128] = { 0 };
	bcm_fcp_treceive64_wqe_t *trecv = (bcm_fcp_treceive64_wqe_t *)wqe;
	struct spdk_nvmf_bcm_fc_hwqp *hwqp = fc_req->hwqp;

	if (!fc_req->req.iovcnt) {
		return -1;
	}

	trecv->xbl = true;
	if (fc_req->req.iovcnt == 1) {
		/* Data is a single physical address, use a BDE */
		uint64_t bde_phys;

		bde_phys = spdk_vtophys(fc_req->req.iov[0].iov_base);
		trecv->dbde = true;
		trecv->bde.bde_type = BCM_BDE_TYPE_BDE_64;
		trecv->bde.buffer_length = fc_req->req.length;
		trecv->bde.u.data.buffer_address_low = PTR_TO_ADDR32_LO(bde_phys);
		trecv->bde.u.data.buffer_address_high = PTR_TO_ADDR32_HI(bde_phys);
	} else {
		uint64_t sgl_phys;

		if (!nvmf_fc_fill_sgl(fc_req)) {
			return -1;
		}

		sgl_phys = hwqp->queues.rq_payload.buffer[fc_req->buf_index].phys +
			   offsetof(struct spdk_nvmf_bcm_fc_rq_buf_nvme_cmd, sge);

		trecv->bde.bde_type = BCM_BDE_TYPE_BLP;
		trecv->bde.buffer_length = fc_req->req.length;
		trecv->bde.u.blp.sgl_segment_address_low = PTR_TO_ADDR32_LO(sgl_phys);
		trecv->bde.u.blp.sgl_segment_address_high = PTR_TO_ADDR32_HI(sgl_phys);
	}

	trecv->relative_offset = 0;
	trecv->xri_tag = fc_req->xri->xri;
	trecv->context_tag = fc_req->rpi;
	trecv->pu = true;
	trecv->ar = false;

	trecv->command = BCM_WQE_FCP_TRECEIVE64;
	trecv->class = BCM_ELS_REQUEST64_CLASS_3;
	trecv->ct = BCM_ELS_REQUEST64_CONTEXT_RPI;

	trecv->remote_xid = fc_req->oxid;
	trecv->nvme 	= 1;
	trecv->iod 	= 1;
	trecv->len_loc 	= 0x2;
	trecv->timer 	= 30;

	trecv->cmd_type = BCM_CMD_FCP_TRECEIVE64_WQE;
	trecv->cq_id = 0xFFFF;
	trecv->fcp_data_receive_length = fc_req->req.length;

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)trecv, true, nvmf_fc_io_cmpl_cb, fc_req);
	if (!rc) {
		fc_req->xri->is_active = true;
	}

	return rc;
}

static int
nvmf_fc_execute_nvme_rqst(struct spdk_nvmf_bcm_fc_request *fc_req)
{
	struct spdk_nvme_cmd *cmd = &fc_req->req.cmd->nvme_cmd;
	struct spdk_nvmf_bcm_fc_conn *fc_conn = fc_req->fc_conn;

	/* Allocate an XRI */
	fc_req->xri = spdk_nvmf_bcm_fc_get_xri(fc_req->hwqp);
	if (!fc_req->xri) {
		goto pending;
	}

	if (fc_req->req.length) {
		/* Except for IO read/write, create buffers on fly. */
		if (!(fc_conn->conn.type == CONN_TYPE_IOQ &&
		      (cmd->opc == SPDK_NVME_OPC_READ ||
		       cmd->opc == SPDK_NVME_OPC_WRITE))) {

			fc_req->req.data = spdk_dma_zmalloc(fc_req->req.length, 4096, NULL);
			if (!fc_req->req.data) {
				SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
					      "Admin buffer alloc failed. Requeue\n");
				fc_req->hwqp->counters.aq_buf_alloc_err++;
				goto pending;
			}

			/* Convert data to IOV format */
			fc_req->req.iovcnt = spdk_dma_virt_to_iovec(fc_req->req.data,
					     fc_req->req.length, fc_req->req.iov, MAX_NUM_OF_IOVECTORS);
			if (!fc_req->req.iovcnt) {
				SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
					      "Admin buffer to iov failed. Requeue\n");
				fc_req->hwqp->counters.aq_buf_alloc_err++;
				goto pending;
			}
		}

		/* For IOQ Writes, alloc bdev buffers */
		else if (fc_conn->conn.type == CONN_TYPE_IOQ &&
			 cmd->opc == SPDK_NVME_OPC_WRITE) {

			spdk_nvmf_bcm_fc_req_set_state(fc_req, SPDK_NVMF_BCM_FC_REQ_WRITE_BUFFS);

			switch (spdk_nvmf_request_exec(&fc_req->req)) {
			case SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_READY:
				break;
			case SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_PENDING:
				SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
					      "Write buffer alloc failed. Requeue\n");
				fc_req->hwqp->counters.write_buf_alloc_err++;
				goto pending;
			case SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE:
			case SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS:
				/* Aborted in nvmf layer, request_complete with take of cleanup. */
				return 0;
			default:
				goto error;
			}
		}
	}

	if (fc_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC, "WRITE CMD.\n");

		spdk_nvmf_bcm_fc_req_set_state(fc_req, SPDK_NVMF_BCM_FC_REQ_WRITE_XFER);

		if (nvmf_fc_recv_data(fc_req)) {
			goto error;
		}
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC, "READ/NONE CMD\n");

		if (fc_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
			spdk_nvmf_bcm_fc_req_set_state(fc_req, SPDK_NVMF_BCM_FC_REQ_READ_BDEV);
		} else {
			spdk_nvmf_bcm_fc_req_set_state(fc_req, SPDK_NVMF_BCM_FC_REQ_NONE_BDEV);
		}

		/* Even read can fail for lack of buffers. Handle that. */
		switch (spdk_nvmf_request_exec(&fc_req->req)) {
		case SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_PENDING:
			SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
				      "read buffer alloc failed. Requeue\n");
			fc_req->hwqp->counters.read_buf_alloc_err++;
			goto pending;
		case SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE:
		case SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS:
			break;
		default:
			goto error;
		}
	}

	return 0;

pending:
	if (fc_req->xri) {
		spdk_nvmf_bcm_fc_put_xri(fc_req->hwqp, fc_req->xri);
		fc_req->xri = NULL;
	}

	if (fc_req->req.data) {
		/* This can happen when conversion to iov failed. */
		spdk_dma_free(fc_req->req.data);
		fc_req->req.data = NULL;
	}

	spdk_nvmf_bcm_fc_req_set_state(fc_req, SPDK_NVMF_BCM_FC_REQ_PENDING);

	return 1;
error:
	/* Dropped return success to caller */
	fc_req->hwqp->counters.unexpected_err++;
	spdk_nvmf_bcm_fc_free_req(fc_req);
	return 0;
}

static int
nvmf_fc_handle_nvme_rqst(struct spdk_nvmf_bcm_fc_hwqp *hwqp, struct fc_frame_hdr *frame,
			 uint32_t buf_idx, struct bcm_buffer_desc *buffer, uint32_t plen)
{
	uint16_t cmnd_len;
	uint64_t rqst_conn_id;
	struct spdk_nvmf_bcm_fc_rq_buf_nvme_cmd *req_buf = NULL;
	struct spdk_nvmf_bcm_fc_request *fc_req = NULL;
	struct spdk_nvmf_fc_cmnd_iu *cmd_iu = NULL;
	struct spdk_nvmf_bcm_fc_conn *fc_conn = NULL;
	enum spdk_nvme_data_transfer xfer;
	bool found = false;

	req_buf = (struct spdk_nvmf_bcm_fc_rq_buf_nvme_cmd *)buffer->virt;
	cmd_iu = &req_buf->cmd_iu;
	cmnd_len = req_buf->cmd_iu.cmnd_iu_len;
	cmnd_len = from_be16(&cmnd_len);

	/* check for a valid cmnd_iu format */
	if ((cmd_iu->fc_id != NVME_CMND_IU_FC_ID) ||
	    (cmd_iu->scsi_id != NVME_CMND_IU_SCSI_ID) ||
	    (cmnd_len != sizeof(struct spdk_nvmf_fc_cmnd_iu) / 4)) {
		SPDK_ERRLOG("IU CMD error\n");
		hwqp->counters.nvme_cmd_iu_err++;
		goto abort;
	}

	xfer = spdk_nvme_opc_get_data_transfer(cmd_iu->flags);
	if (xfer == SPDK_NVME_DATA_BIDIRECTIONAL) {
		SPDK_ERRLOG("IU CMD xfer error\n");
		hwqp->counters.nvme_cmd_xfer_err++;
		goto abort;
	}

	rqst_conn_id = from_be64(&cmd_iu->conn_id);

	/* Check if conn id is valid */
	TAILQ_FOREACH(fc_conn, &hwqp->connection_list, link) {
		if (fc_conn->conn_id == rqst_conn_id) {
			found = true;
			break;
		}
	}

	if (!found) {
		SPDK_ERRLOG("IU CMD conn(%ld) invalid\n", rqst_conn_id);
		hwqp->counters.invalid_conn_err++;
		goto abort;
	}

	/* If association/connection is being deleted - return */
	if (fc_conn->fc_assoc->assoc_state !=  SPDK_NVMF_BCM_FC_OBJECT_CREATED) {
		SPDK_ERRLOG("Association state not valid\n");
		goto abort;
	}

	/* Make sure xfer len is according to mdts */
	if (from_be32(&cmd_iu->data_len) > g_nvmf_tgt.opts.max_io_size) {
		SPDK_ERRLOG("IO length requested is greater than MDTS\n");
		goto abort;
	}

	/* allocate a request buffer */
	fc_req = nvmf_fc_alloc_req_buf(hwqp);
	if (fc_req == NULL) {
		/* Should not happen. Since fc_reqs == RQ buffers */
		goto abort;
	}

	fc_req->req.length = from_be32(&cmd_iu->data_len);
	fc_req->req.conn = &fc_conn->conn;
	fc_req->req.cmd = &req_buf->cmd_iu.cmd;
	fc_req->req.rsp = &fc_req->ersp.rsp;
	fc_req->req.io_rsrc_pool = hwqp->fc_port->io_rsrc_pool;
	fc_req->oxid = frame->ox_id;
	fc_req->oxid = from_be16(&fc_req->oxid);
	fc_req->rpi = fc_conn->rpi;
	fc_req->buf_index = buf_idx;
	fc_req->poller_lcore = hwqp->lcore_id;
	fc_req->hwqp = hwqp;
	fc_req->fc_conn = fc_conn;
	fc_req->req.xfer = xfer;

	nvmf_fc_record_req_trace_point(fc_req, SPDK_NVMF_BCM_FC_REQ_INIT);
	if (nvmf_fc_execute_nvme_rqst(fc_req)) {
		TAILQ_INSERT_TAIL(&fc_conn->pending_queue, fc_req, pending_link);
	}

	return 0;

abort:
	/* Issue abort for oxid */
	SPDK_ERRLOG("Aborted CMD\n");
	return -1;
}


static void
nvmf_fc_process_marker_cqe(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint8_t *cqe)
{
	bcm_fc_async_rcqe_marker_t *marker = (void *)cqe;
	struct spdk_event *event = NULL;
	uint64_t tag = 0;

	tag = (uint64_t)marker->tag_higher << 32 | marker->tag_lower;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC, "Process Marker compl for tag = %lx\n", tag);

	event = spdk_event_allocate(hwqp->lcore_id, nvmf_fc_poller_queue_sync_done,
				    (void *)hwqp, (void *)tag);
	spdk_post_event(hwqp->context, event);
}

static int
nvmf_fc_process_frame(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint32_t buff_idx, fc_frame_hdr_t *frame,
		      bcm_buffer_desc_t *buffer, uint32_t plen)
{
	int rc = SPDK_SUCCESS;
	uint32_t s_id, d_id;
	uint16_t oxid, rxid;
	struct spdk_nvmf_bcm_fc_nport *nport = NULL;
	struct spdk_nvmf_bcm_fc_remote_port_info *rport = NULL;

	s_id = (uint32_t)frame->s_id;
	d_id = (uint32_t)frame->d_id;
	s_id = from_be32(&s_id) >> 8;
	d_id = from_be32(&d_id) >> 8;
	oxid = (uint16_t)frame->ox_id;
	oxid = (uint16_t)from_be16(&oxid);
	rxid = (uint16_t)frame->rx_id;
	rxid = (uint16_t)from_be16(&rxid);

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
		      "Process NVME frame s_id:0x%x d_id:0x%x oxid:0x%x rxid:0x%x.\n",
		      s_id, d_id, oxid, rxid);

	rc = nvmf_fc_find_nport_and_rport(hwqp, d_id, &nport, s_id, &rport);
	if (rc) {
		if (nport == NULL) {
			SPDK_ERRLOG("%s: Nport not found. Dropping\n", __func__);
			/* increment invalid nport counter */
			hwqp->counters.nport_invalid++;
		} else if (rport == NULL) {
			SPDK_ERRLOG("%s: Rport not found. Dropping\n", __func__);
			/* increment invalid rport counter */
			hwqp->counters.rport_invalid++;
		}
		return rc;
	}

	if (nport->nport_state != SPDK_NVMF_BCM_FC_OBJECT_CREATED ||
	    rport->rport_state != SPDK_NVMF_BCM_FC_OBJECT_CREATED) {
		SPDK_ERRLOG("%s: %s state not created. Dropping\n", __func__,
			    nport->nport_state != SPDK_NVMF_BCM_FC_OBJECT_CREATED ?
			    "Nport" : "Rport");
		return -1;
	}

	if ((frame->r_ctl == NVME_FC_R_CTL_LS_REQUEST) &&
	    (frame->type == NVME_FC_TYPE_NVMF_DATA)) {
		struct spdk_nvmf_bcm_fc_rq_buf_ls_request *req_buf = buffer->virt;
		struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst;

		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC, "Process LS NVME frame\n");

		/* Use the RQ buffer for holding LS request. */
		ls_rqst = (struct spdk_nvmf_bcm_fc_ls_rqst *)&req_buf->ls_rqst;

		/* Fill in the LS request structure */
		ls_rqst->rqstbuf.virt = (void *)&req_buf->rqst;
		ls_rqst->rqstbuf.phys = buffer->phys +
					offsetof(struct spdk_nvmf_bcm_fc_rq_buf_ls_request, rqst);
		ls_rqst->rqstbuf.buf_index = buff_idx;
		ls_rqst->rqst_len = plen;

		ls_rqst->rspbuf.virt = (void *)&req_buf->resp;
		ls_rqst->rspbuf.phys = buffer->phys +
				       offsetof(struct spdk_nvmf_bcm_fc_rq_buf_ls_request, resp);
		ls_rqst->rsp_len = BCM_MAX_RESP_BUFFER_SIZE;

		ls_rqst->private_data = (void *)hwqp;
		ls_rqst->rpi = rport->rpi;
		ls_rqst->oxid = (uint16_t)frame->ox_id;
		ls_rqst->oxid = from_be16(&ls_rqst->oxid);
		ls_rqst->s_id = s_id;
		ls_rqst->d_id = d_id;
		ls_rqst->nport = nport;
		ls_rqst->rport = rport;

		ls_rqst->xri = spdk_nvmf_bcm_fc_get_xri(hwqp);
		if (!ls_rqst->xri) {
			/* No XRI available. Add to pending list. */
			TAILQ_INSERT_TAIL(&hwqp->ls_pending_queue, ls_rqst, ls_pending_link);
		} else {
			/* Handover the request to LS module */
			spdk_nvmf_bcm_fc_handle_ls_rqst(ls_rqst);
		}

	} else if ((frame->r_ctl == NVME_FC_R_CTL_CMD_REQ) &&
		   (frame->type == NVME_FC_TYPE_FC_EXCHANGE)) {

		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC, "Process IO NVME frame\n");
		rc = nvmf_fc_handle_nvme_rqst(hwqp, frame, buff_idx, buffer, plen);
	} else {

		SPDK_ERRLOG("%s Unknown frame received. Dropping\n", __func__);
		hwqp->counters.unknown_frame++;
		rc = -1;
	}

	return rc;
}

static int
nvmf_fc_process_rqpair(struct spdk_nvmf_bcm_fc_hwqp *hwqp, fc_eventq_t *cq, uint8_t *cqe)
{
	int rc = 0, rq_index = 0;
	uint16_t rq_id = 0;
	int32_t rq_status;
	uint32_t buff_idx = 0;
	fc_frame_hdr_t *frame = NULL;
	bcm_buffer_desc_t *payload_buffer = NULL;
	bcm_fc_async_rcqe_t *rcqe = (bcm_fc_async_rcqe_t *)cqe;
	uint8_t code = cqe[BCM_CQE_CODE_OFFSET];

	assert(hwqp);
	assert(cq);
	assert(cqe);

	rq_status = nvmf_fc_rqe_rqid_and_index(cqe, &rq_id, &rq_index);
	if (0 != rq_status) {
		switch (rq_status) {
		case BCM_FC_ASYNC_RQ_BUF_LEN_EXCEEDED:
		case BCM_FC_ASYNC_RQ_DMA_FAILURE:
			if (rq_index < 0 || rq_index >= hwqp->queues.rq_hdr.q.max_entries) {
				SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
					      "%s: status=%#x: rq_id lookup failed for id=%#x\n",
					      __func__, rq_status, rq_id);
				hwqp->counters.rq_buf_len_err++;
				break;
			}

			buff_idx = nvmf_fc_rqpair_get_buffer_id(hwqp, rq_index);
			goto buffer_release;

		case BCM_FC_ASYNC_RQ_INSUFF_BUF_NEEDED:
		case BCM_FC_ASYNC_RQ_INSUFF_BUF_FRM_DISC:
			SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
				      "%s: Warning: RCQE status=%#x, \n",
				      __func__, rq_status);
			hwqp->counters.rq_status_err++;
		default:
			break;
		}

		/* Buffer not consumed. No need to return */
		return -1;
	}

	/* Make sure rq_index is in range */
	if (rq_index >= hwqp->queues.rq_hdr.q.max_entries) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
			      "%s: Error: rq index out of range for RQ%d\n",
			      __func__, rq_id);
		hwqp->counters.rq_index_err++;
		return -1;
	}

	/* Process NVME frame */
	buff_idx = nvmf_fc_rqpair_get_buffer_id(hwqp, rq_index);
	frame = nvmf_fc_rqpair_get_frame_header(hwqp, rq_index);
	payload_buffer = nvmf_fc_rqpair_get_frame_buffer(hwqp, rq_index);

	if (code == BCM_CQE_CODE_RQ_MARKER) {
		/* Process marker completion */
		nvmf_fc_process_marker_cqe(hwqp, cqe);
	} else {
		rc = nvmf_fc_process_frame(hwqp, buff_idx, frame, payload_buffer,
					   rcqe->payload_data_placement_length);
		if (!rc) {
			return 0;
		}
	}

buffer_release:
	/* Return buffer to chip */
	nvmf_fc_rqpair_buffer_release(hwqp, buff_idx);
	return rc;
}

static int
nvmf_fc_process_cq_entry(struct spdk_nvmf_bcm_fc_hwqp *hwqp, struct fc_eventq *cq)
{
	int rc = 0, budget = cq->q.processed_limit;
	uint8_t	cqe[sizeof(cqe_t)];
	uint16_t rid = UINT16_MAX;
	uint32_t n_processed = 0, n_processed_total = 0;
	bcm_qentry_type_e ctype;     /* completion type */

	assert(hwqp);
	assert(cq);

	while (!nvmf_fc_read_queue_entry(&cq->q, &cqe[0])) {
		n_processed++;
		budget --;

		rc = nvmf_fc_parse_cq_entry(cq, cqe, &ctype, &rid);
		/*
		 * The sign of status is significant. If status is:
		 * == 0 : call completed correctly and the CQE indicated success
		 *  > 0 : call completed correctly and the CQE indicated an error
		 *  < 0 : call failed and no information is available about the CQE
		 */
		if (rc < 0) {
			if ((rc == -2) && budget) {
				/* Entry was consumed */
				continue;
			}
			break;
		}

		switch ((int)ctype) {
		case BCM_FC_QENTRY_WQ:
			nvmf_fc_process_wqe_completion(hwqp, rid, rc, cqe);
			break;
		case BCM_FC_QENTRY_WQ_RELEASE:
			nvmf_fc_process_wqe_release(hwqp, rid);
			break;
		case BCM_FC_QENTRY_RQ:
			nvmf_fc_process_rqpair(hwqp, cq, cqe);
			break;
		case BCM_FC_QENTRY_XABT:
			nvmf_fc_nvmf_del_xri_pending(hwqp, rid);
			break;
		default:
			SPDK_WARNLOG("%s: unhandled ctype=%#x rid=%#x\n",
				     __func__, ctype, rid);
			hwqp->counters.invalid_cq_type++;
			break;
		}

		if (n_processed >= (cq->q.posted_limit)) {
			nvmf_fc_bcm_notify_queue(&cq->q, false, n_processed);
			n_processed_total += n_processed;
			n_processed = 0;
		}

		if (!budget || (hwqp->state == SPDK_FC_HWQP_OFFLINE)) {
			break;
		}
	}
#if BCM_SUPPORT_EQ_POLL
	nvmf_fc_bcm_notify_queue(&cq->q, cq->auto_arm_flag, n_processed);
#else
	if (n_processed) {
		nvmf_fc_bcm_notify_queue(&cq->q, cq->auto_arm_flag, n_processed);
	}
#endif
	if (rc < 0) {
		return rc;
	} else {
		return n_processed_total + n_processed;
	}
}

static inline void
nvmf_fc_process_pending_ls_rqst(struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst = NULL, *tmp;
	struct spdk_nvmf_bcm_fc_nport *nport = NULL;
	struct spdk_nvmf_bcm_fc_remote_port_info *rport = NULL;

	TAILQ_FOREACH_SAFE(ls_rqst, &hwqp->ls_pending_queue, ls_pending_link, tmp) {
		/* lookup nport and rport again - make sure they are still valid */
		int rc = nvmf_fc_find_nport_and_rport(hwqp, ls_rqst->d_id, &nport, ls_rqst->s_id, &rport);
		if (rc) {
			if (nport == NULL) {
				SPDK_ERRLOG("%s: Nport not found. Dropping\n", __func__);
				/* increment invalid nport counter */
				hwqp->counters.nport_invalid++;
			} else if (rport == NULL) {
				SPDK_ERRLOG("%s: Rport not found. Dropping\n", __func__);
				/* increment invalid rport counter */
				hwqp->counters.rport_invalid++;
			}
			TAILQ_REMOVE(&hwqp->ls_pending_queue, ls_rqst, ls_pending_link);
			/* Return buffer to chip */
			nvmf_fc_rqpair_buffer_release(hwqp, ls_rqst->rqstbuf.buf_index);
			continue;
		}
		if (nport->nport_state != SPDK_NVMF_BCM_FC_OBJECT_CREATED ||
		    rport->rport_state != SPDK_NVMF_BCM_FC_OBJECT_CREATED) {
			SPDK_ERRLOG("%s: %s state not created. Dropping\n", __func__,
				    nport->nport_state != SPDK_NVMF_BCM_FC_OBJECT_CREATED ?
				    "Nport" : "Rport");
			TAILQ_REMOVE(&hwqp->ls_pending_queue, ls_rqst, ls_pending_link);
			/* Return buffer to chip */
			nvmf_fc_rqpair_buffer_release(hwqp, ls_rqst->rqstbuf.buf_index);
			continue;
		}

		ls_rqst->xri = spdk_nvmf_bcm_fc_get_xri(hwqp);
		if (ls_rqst->xri) {
			/* Got an XRI. */
			TAILQ_REMOVE(&hwqp->ls_pending_queue, ls_rqst, ls_pending_link);
			/* Handover the request to LS module */
			spdk_nvmf_bcm_fc_handle_ls_rqst(ls_rqst);
		} else {
			/* No more XRI. Stop processing. */
			return;
		}
	}
}

uint32_t
spdk_nvmf_bcm_fc_process_queues(struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
#if BCM_SUPPORT_EQ_POLL
	int rc = 0, budget = 0;
	uint32_t n_processed = 0;
	uint32_t n_processed_total = 0;
	uint8_t eqe[sizeof(eqe_t)] = { 0 };
	uint16_t cq_id;
	struct fc_eventq *eq;

	assert(hwqp);
	eq = &hwqp->queues.eq;

	budget = eq->q.processed_limit;

	while (!nvmf_fc_read_queue_entry(&eq->q, &eqe[0])) {
		n_processed++;
		budget --;

		rc = nvmf_fc_parse_eq_entry((struct eqe *)eqe, &cq_id);
		if (spdk_likely(rc))  {
			if (rc > 0) {
				/* EQ is full.  Process all CQs */
				nvmf_fc_process_cq_entry(hwqp, &hwqp->queues.cq_wq);
				nvmf_fc_process_cq_entry(hwqp, &hwqp->queues.cq_rq);
			} else {
				break;
			}
		} else {
			if (cq_id == hwqp->queues.cq_wq.q.qid) {
				nvmf_fc_process_cq_entry(hwqp, &hwqp->queues.cq_wq);
				/*
				 * There might be some buffers/xri freed.
				 * First give chance for pending frames
				 */
				nvmf_fc_process_pending_ls_rqst(hwqp);
				nvmf_fc_process_pending_req(hwqp);
			} else if (cq_id == hwqp->queues.cq_rq.q.qid) {
				nvmf_fc_process_cq_entry(hwqp, &hwqp->queues.cq_rq);
			} else {
				SPDK_ERRLOG("%s bad CQ_ID %#06x\n", __func__, cq_id);
				hwqp->counters.invalid_cq_id++;
			}
		}

		if (n_processed >= (eq->q.posted_limit)) {
			nvmf_fc_bcm_notify_queue(&eq->q, false, n_processed);
			n_processed_total += n_processed;
			n_processed = 0;
		}

		if (!budget || (hwqp->state == SPDK_FC_HWQP_OFFLINE)) {
			break;
		}
	}

	if (n_processed) {
		nvmf_fc_bcm_notify_queue(&eq->q, eq->auto_arm_flag, n_processed);
	}
	return (n_processed + n_processed_total);
#else
	int n_processed = 0, rc = 0;
	assert(hwqp);
	/* Check for WQE completions */
	rc = nvmf_fc_process_cq_entry(hwqp, &hwqp->queues.cq_wq);
	if (rc > 0) {
		n_processed += rc;
	}
	/*
	 * There might be some buffers/xri freed.
	 * First give chance for pending frames
	 */
	nvmf_fc_process_pending_ls_rqst(hwqp);
	nvmf_fc_process_pending_req(hwqp);

	/* Check for any new commands */
	rc = nvmf_fc_process_cq_entry(hwqp, &hwqp->queues.cq_rq);
	if (rc > 0) {
		n_processed += rc;
	}
	return n_processed;;
#endif
}

int
spdk_nvmf_bcm_fc_xmt_ls_rsp(struct spdk_nvmf_bcm_fc_nport *tgtport,
			    struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst)
{
	uint8_t wqe[128] = { 0 };
	bcm_xmit_sequence64_wqe_t *xmit = (bcm_xmit_sequence64_wqe_t *)wqe;
	struct spdk_nvmf_bcm_fc_hwqp *hwqp = NULL;
	int rc = -1;

	xmit->xbl = true;
	xmit->bde.bde_type = BCM_BDE_TYPE_BDE_64;
	xmit->bde.buffer_length = ls_rqst->rsp_len;
	xmit->bde.u.data.buffer_address_low  = PTR_TO_ADDR32_LO(ls_rqst->rspbuf.phys);
	xmit->bde.u.data.buffer_address_high = PTR_TO_ADDR32_HI(ls_rqst->rspbuf.phys);
	xmit->sequence_payload_len = ls_rqst->rsp_len;
	xmit->relative_offset = 0;

	xmit->si = 0;
	xmit->ft = 0;
	xmit->xo = 0;
	xmit->ls = 1;
	xmit->dbde = 1;

	xmit->dif 	= 0;
	xmit->pu 	= 0;
	xmit->abort_tag = 0;
	xmit->bs 	= 0;

	xmit->df_ctl	= 0;
	xmit->type 	= NVME_FC_TYPE_NVMF_DATA;
	xmit->r_ctl 	= NVME_FC_R_CTL_LS_RESPONSE;

	xmit->cmd_type 	= BCM_CMD_XMIT_SEQUENCE64_WQE;
	xmit->command 	= BCM_WQE_XMIT_SEQUENCE64;
	xmit->class 	= BCM_ELS_REQUEST64_CLASS_3;
	xmit->ct 	= BCM_ELS_REQUEST64_CONTEXT_RPI;
	xmit->iod 	= BCM_ELS_REQUEST64_DIR_WRITE;

	xmit->xri_tag	 = ls_rqst->xri->xri;
	xmit->remote_xid  = ls_rqst->oxid;
	xmit->context_tag = ls_rqst->rpi;

	xmit->len_loc 	= 2;
	xmit->cq_id 	= 0xFFFF;

	hwqp = (struct spdk_nvmf_bcm_fc_hwqp *)ls_rqst->private_data;

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)xmit, true, nvmf_fc_ls_rsp_cmpl_cb, ls_rqst);
	if (!rc) {
		ls_rqst->xri->is_active = true;
	}

	return rc;
}

int
spdk_nvmf_bcm_fc_send_data(struct spdk_nvmf_bcm_fc_request *fc_req)
{
	int rc = 0;
	uint8_t wqe[128] = { 0 };
	uint32_t xfer_len = 0;
	bcm_fcp_tsend64_wqe_t *tsend = (bcm_fcp_tsend64_wqe_t *)wqe;
	struct spdk_nvmf_bcm_fc_hwqp *hwqp = fc_req->hwqp;
	struct spdk_nvmf_conn *conn = fc_req->req.conn;
	struct spdk_nvmf_bcm_fc_conn *fc_conn = nvmf_fc_get_conn(conn);

	if (!fc_req->req.iovcnt) {
		return -1;
	}

	tsend->xbl = true;
	if (fc_req->req.iovcnt == 1) {
		/* Data is a single physical address, use a BDE */
		uint64_t bde_phys;

		bde_phys = spdk_vtophys(fc_req->req.iov[0].iov_base);
		tsend->dbde = true;
		tsend->bde.bde_type = BCM_BDE_TYPE_BDE_64;
		tsend->bde.buffer_length = fc_req->req.length;
		tsend->bde.u.data.buffer_address_low = PTR_TO_ADDR32_LO(bde_phys);
		tsend->bde.u.data.buffer_address_high = PTR_TO_ADDR32_HI(bde_phys);

		xfer_len = fc_req->req.iov[0].iov_len;
	} else {
		uint64_t sgl_phys;

		xfer_len = nvmf_fc_fill_sgl(fc_req);
		if (!xfer_len) {
			return -1;
		}

		sgl_phys = hwqp->queues.rq_payload.buffer[fc_req->buf_index].phys +
			   offsetof(struct spdk_nvmf_bcm_fc_rq_buf_nvme_cmd, sge);

		tsend->bde.bde_type = BCM_BDE_TYPE_BLP;
		tsend->bde.buffer_length = fc_req->req.length;
		tsend->bde.u.blp.sgl_segment_address_low = PTR_TO_ADDR32_LO(sgl_phys);
		tsend->bde.u.blp.sgl_segment_address_high = PTR_TO_ADDR32_HI(sgl_phys);
	}

	tsend->relative_offset = 0;
	tsend->xri_tag = fc_req->xri->xri;
	tsend->rpi = fc_req->rpi;
	tsend->pu = true;

	if (!nvmf_fc_send_ersp_required(fc_req, (fc_conn->rsp_count + 1),
					xfer_len)) {
		fc_conn->rsp_count++;
		nvmf_fc_advance_conn_sqhead(conn);
		tsend->ar = true;
		spdk_nvmf_bcm_fc_req_set_state(fc_req, SPDK_NVMF_BCM_FC_REQ_READ_RSP);
	}

	tsend->command = BCM_WQE_FCP_TSEND64;
	tsend->class = BCM_ELS_REQUEST64_CLASS_3;
	tsend->ct = BCM_ELS_REQUEST64_CONTEXT_RPI;
	tsend->remote_xid = fc_req->oxid;
	tsend->nvme = 1;
	tsend->len_loc = 0x2;

	tsend->cmd_type = BCM_CMD_FCP_TSEND64_WQE;
	tsend->cq_id = 0xFFFF;
	tsend->fcp_data_transmit_length = fc_req->req.length;

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)tsend, true, nvmf_fc_io_cmpl_cb, fc_req);
	if (!rc) {
		fc_req->xri->is_active = true;
	}

	return rc;
}

static int
nvmf_fc_xmt_rsp(struct spdk_nvmf_bcm_fc_request *fc_req, uint8_t *ersp_buf, uint32_t ersp_len)
{
	int rc = 0;
	uint8_t wqe[128] = { 0 };
	bcm_fcp_trsp64_wqe_t *trsp = (bcm_fcp_trsp64_wqe_t *)wqe;
	struct spdk_nvmf_bcm_fc_hwqp *hwqp = fc_req->hwqp;

	if (!ersp_buf) {
		/* Auto-Gen all zeroes in IU 12-byte payload */
		trsp->ag = true;
	} else {
		trsp->wqes = 1;
		trsp->irsp = 1;
		trsp->fcp_response_length = ersp_len;
		trsp->irsplen = (ersp_len >> 2) - 1;
		memcpy(&trsp->inline_rsp, ersp_buf, ersp_len);
	}

	if (fc_req->xri->is_active) {
		trsp->xc = true;
	}

	trsp->command = BCM_WQE_FCP_TRSP64;
	trsp->class = BCM_ELS_REQUEST64_CLASS_3;
	trsp->xri_tag = fc_req->xri->xri;
	trsp->remote_xid  = fc_req->oxid;
	trsp->rpi = fc_req->rpi;
	trsp->len_loc = 0x1;
	trsp->cq_id = 0xFFFF;
	trsp->cmd_type = BCM_CMD_FCP_TRSP64_WQE;
	trsp->nvme = 1;

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)trsp, true, nvmf_fc_io_cmpl_cb, fc_req);
	if (!rc) {
		fc_req->xri->is_active = true;
	}

	return rc;
}

int
spdk_nvmf_bcm_fc_handle_rsp(struct spdk_nvmf_bcm_fc_request *fc_req)
{
	int rc = 0;
	struct spdk_nvmf_request *req = &fc_req->req;
	struct spdk_nvmf_conn 	*conn = req->conn;
	struct spdk_nvmf_bcm_fc_conn *fc_conn = nvmf_fc_get_conn(conn);
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint16_t ersp_len = 0;

	/* set sq head value in resp */
	rsp->sqhd = nvmf_fc_advance_conn_sqhead(conn);

	/* Increment connection responses */
	fc_conn->rsp_count++;

	if (nvmf_fc_send_ersp_required(fc_req, fc_conn->rsp_count,
				       fc_req->transfered_len)) {

		/* Fill ERSP Len */
		to_be16(&ersp_len, (sizeof(struct spdk_nvmf_fc_ersp_iu) /
				    sizeof(uint32_t)));
		fc_req->ersp.ersp_len = ersp_len;

		/* Fill RSN */
		to_be32(&fc_req->ersp.response_seq_no, fc_conn->rsn);
		fc_conn->rsn++;

		/* Fill transfer length */
		to_be32(&fc_req->ersp.transferred_data_len, fc_req->transfered_len);

		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC, "Posting ERSP.\n");
		rc = nvmf_fc_xmt_rsp(fc_req, (uint8_t *)&fc_req->ersp,
				     sizeof(struct spdk_nvmf_fc_ersp_iu));
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC, "Posting RSP.\n");
		rc = nvmf_fc_xmt_rsp(fc_req, NULL, 0);
	}

	return rc;
}

int
spdk_nvmf_bcm_fc_issue_abort(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
			     struct spdk_nvmf_bcm_fc_xri *xri, bool send_abts,
			     spdk_nvmf_bcm_fc_caller_cb cb, void *cb_args)
{
	uint8_t wqe[128] = { 0 };
	bcm_abort_wqe_t *abort = (bcm_abort_wqe_t *)wqe;
	fc_caller_ctx_t *ctx = NULL;
	int rc = -1;

	ctx = spdk_calloc(1, sizeof(fc_caller_ctx_t));
	if (!ctx) {
		goto done;
	}
	ctx->ctx = xri;
	ctx->cb = cb;
	ctx->cb_args = cb_args;

	abort->criteria = BCM_ABORT_CRITERIA_XRI_TAG;
	abort->ia = send_abts ? 0 : 1;
	abort->ir = 1; /* Supress ABTS retries. */
	abort->command = BCM_WQE_ABORT;
	abort->qosd = true;
	abort->cq_id = UINT16_MAX;
	abort->cmd_type = BCM_CMD_ABORT_WQE;
	abort->t_tag = xri->xri;

	if (send_abts) {
		/* Increment abts sent count */
		hwqp->counters.num_abts_sent++;
	}

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)abort, true, nvmf_fc_abort_cmpl_cb, ctx);
done:
	if (rc && ctx) {
		spdk_free(ctx);
	}

	if (!rc) {
		xri->is_active = false;
		SPDK_NOTICELOG("Abort WQE posted for XRI = %d\n", xri->xri);
	}
	return rc;
}

int
spdk_nvmf_bcm_fc_xmt_bls_rsp(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
			     uint16_t ox_id, uint16_t rx_id,
			     uint16_t rpi, bool rjt, uint8_t rjt_exp,
			     spdk_nvmf_bcm_fc_caller_cb cb, void *cb_args)
{
	uint8_t wqe[128] = { 0 };
	bcm_xmit_bls_rsp_wqe_t *bls = (bcm_xmit_bls_rsp_wqe_t *)wqe;
	int rc = -1;
	fc_caller_ctx_t *ctx = NULL;
	struct spdk_nvmf_bcm_fc_xri *xri = NULL;

	xri = spdk_nvmf_bcm_fc_get_xri(hwqp);
	if (!xri) {
		goto done;
	}

	ctx = spdk_calloc(1, sizeof(fc_caller_ctx_t));
	if (!ctx) {
		goto done;
	}
	ctx->ctx = xri;
	ctx->cb = cb;
	ctx->cb_args = cb_args;

	if (rjt) {
		bls->payload_word0 = ((uint32_t)BCM_BLS_REJECT_CODE_UNABLE_TO_PERFORM << 16) |
				     ((uint32_t)rjt_exp << 8);
		bls->ar = true;
	} else {
		bls->high_seq_cnt = UINT16_MAX;
	}

	bls->ox_id       = ox_id;
	bls->rx_id       = rx_id;
	bls->ct          = BCM_ELS_REQUEST64_CONTEXT_RPI;
	bls->context_tag = rpi;
	bls->xri_tag     = xri->xri;
	bls->class       = BCM_ELS_REQUEST64_CLASS_3;
	bls->command     = BCM_WQE_XMIT_BLS_RSP;
	bls->qosd        = true;
	bls->cq_id       = UINT16_MAX;
	bls->cmd_type    = BCM_CMD_XMIT_BLS_RSP64_WQE;

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)bls, true, nvmf_fc_bls_cmpl_cb, ctx);
done:
	if (rc && ctx) {
		spdk_free(ctx);
	}

	if (rc && xri) {
		spdk_nvmf_bcm_fc_put_xri(hwqp, xri);
	}

	if (!rc) {
		xri->is_active = true;
	}

	return rc;
}

int
spdk_nvmf_bcm_fc_xmt_srsr_req(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
			      struct spdk_nvmf_bcm_fc_send_srsr *srsr,
			      spdk_nvmf_bcm_fc_caller_cb cb, void *cb_args)
{
	uint8_t wqe[128] = { 0 };
	int rc = -1;
	bcm_gen_request64_wqe_t *gen = (bcm_gen_request64_wqe_t *)wqe;
	fc_caller_ctx_t *ctx = NULL;
	struct spdk_nvmf_bcm_fc_xri *xri = NULL;
	bcm_sge_t *sge = NULL;

	if (!srsr) {
		goto done;
	}

	/* Make sure caller allocated space for two sges. */
	if (srsr->sgl.len != (2 * sizeof(bcm_sge_t))) {
		goto done;
	}
	sge = srsr->sgl.virt;
	memset(sge, 0, (sizeof(bcm_sge_t) * 2));

	xri = spdk_nvmf_bcm_fc_get_xri(hwqp);
	if (!xri) {
		/* Might be we should reserve some XRI for this */
		goto done;
	}

	ctx = spdk_calloc(1, sizeof(fc_caller_ctx_t));
	if (!ctx) {
		goto done;
	}
	ctx->ctx = xri;
	ctx->cb = cb;
	ctx->cb_args = cb_args;

	/* Fill SGL */
	sge->buffer_address_high = PTR_TO_ADDR32_HI(srsr->rqst.phys);
	sge->buffer_address_low  = PTR_TO_ADDR32_LO(srsr->rqst.phys);
	sge->sge_type = BCM_SGE_TYPE_DATA;
	sge->buffer_length = srsr->rqst.len;
	sge ++;

	sge->buffer_address_high = PTR_TO_ADDR32_HI(srsr->rsp.phys);
	sge->buffer_address_low  = PTR_TO_ADDR32_LO(srsr->rsp.phys);
	sge->sge_type = BCM_SGE_TYPE_DATA;
	sge->buffer_length = srsr->rsp.len;
	sge->last = true;

	/* Fill WQE contents */
	gen->xbl = true;
	gen->bde.bde_type = BCM_BDE_TYPE_BLP;
	gen->bde.buffer_length = 2 * sizeof(bcm_sge_t);
	gen->bde.u.data.buffer_address_low  = PTR_TO_ADDR32_LO(srsr->sgl.phys);
	gen->bde.u.data.buffer_address_high = PTR_TO_ADDR32_HI(srsr->sgl.phys);

	gen->request_payload_length = srsr->rqst.len;
	gen->max_response_payload_length = srsr->rsp.len;
	gen->df_ctl	 = 0;
	gen->type	 = NVME_FC_TYPE_NVMF_DATA;
	gen->r_ctl	 = NVME_FC_R_CTL_LS_REQUEST;
	gen->xri_tag	 = xri->xri;
	gen->ct		 = BCM_ELS_REQUEST64_CONTEXT_RPI;
	gen->context_tag = srsr->rpi;
	gen->class	 = BCM_ELS_REQUEST64_CLASS_3;
	gen->command	 = BCM_WQE_GEN_REQUEST64;
	gen->timer	 = 30;
	gen->iod	 = BCM_ELS_REQUEST64_DIR_READ;
	gen->qosd	 = true;
	gen->cmd_type	 = BCM_CMD_GEN_REQUEST64_WQE;
	gen->cq_id	 = 0xffff;

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)gen, true, nvmf_fc_srsr_cmpl_cb,
			      ctx);
done:
	if (rc && ctx) {
		spdk_free(ctx);
	}

	if (rc && xri) {
		spdk_nvmf_bcm_fc_put_xri(hwqp, xri);
	}

	if (!rc) {
		xri->is_active = true;
	}

	return rc;
}

int
spdk_nvmf_bcm_fc_issue_marker(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint64_t u_id, uint16_t skip_rq)
{
	uint8_t wqe[128] = { 0 };
	bcm_marker_wqe_t *marker = (bcm_marker_wqe_t *)wqe;

	if (skip_rq != UINT16_MAX) {
		marker->marker_catagery = BCM_MARKER_CATAGORY_ALL_RQ_EXCEPT_ONE;
		marker->rq_id = skip_rq;
	} else {
		marker->marker_catagery = BCM_MARKER_CATAGORY_ALL_RQ;
	}

	marker->tag_lower	= PTR_TO_ADDR32_LO(u_id);
	marker->tag_higher	= PTR_TO_ADDR32_HI(u_id);
	marker->command		= BCM_WQE_MARKER;
	marker->cmd_type	= BCM_CMD_MARKER_WQE;
	marker->qosd		= 1;
	marker->cq_id		= UINT16_MAX;

	return nvmf_fc_post_wqe(hwqp, (uint8_t *)marker, true, nvmf_fc_def_cmpl_cb, NULL);
}

#ifdef UNSOL_ABTS_SUPPORT
static void
nvmf_fc_default_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	if (status) {
		SPDK_ERRLOG("Generic WQE Compl(%d) error\n", status);
	}
}

static int
nvmf_fc_unsol_abort(struct spdk_nvmf_bcm_fc_hwqp *hwqp, uint32_t s_id, uint32_t d_id,
		    uint16_t ox_id)

{
	uint8_t wqe[128] = { 0 };
	uint32_t *p_hdr;
	int rc = -1;
	bcm_send_frame_wqe_t *sf = (bcm_send_frame_wqe_t *)wqe;
	fc_frame_hdr_le_t hdr;

	/* Build header */
	memset(&hdr, 0, sizeof(fc_frame_hdr_le_t));

	hdr.d_id	 = s_id;
	hdr.s_id	 = d_id;
	hdr.r_ctl	 = NVME_FC_R_CTL_BA_ABTS;
	hdr.cs_ctl	 = 0;
	hdr.f_ctl	 = NVME_FC_F_CTL_END_SEQ | NVME_FC_F_CTL_SEQ_INIT;
	hdr.type	 = NVME_FC_TYPE_BLS;
	hdr.seq_cnt	 = 0;
	hdr.df_ctl	 = 0;
	hdr.rx_id	 = 0xffff;
	hdr.ox_id	 = ox_id;
	hdr.parameter	 = 0;

	/* Assign a SEQID. */
	hdr.seq_id = hwqp->send_frame_seqid;
	hwqp->send_frame_seqid ++;

	p_hdr = (uint32_t *)&hdr;

	/* WQE */
	sf->dbde = false;

	sf->fc_header_0_1[0] = p_hdr[0];
	sf->fc_header_0_1[1] = p_hdr[1];
	sf->fc_header_2_5[0] = p_hdr[2];
	sf->fc_header_2_5[1] = p_hdr[3];
	sf->fc_header_2_5[2] = p_hdr[4];
	sf->fc_header_2_5[3] = p_hdr[5];

	sf->frame_length = 0; /* No payload */
	sf->xri_tag	 = hwqp->send_frame_xri;
	sf->pu		 = 0;
	sf->context_tag	 = 0;
	sf->ct		 = 0;
	sf->command	 = BCM_WQE_SEND_FRAME;
	sf->class	 = BCM_ELS_REQUEST64_CLASS_3;
	sf->timer	 = 3;
	sf->sof		 = 0x2e; /* SOFI3 */
	sf->eof		 = 0x42; /* EOFT */
	sf->qosd	 = 0;
	sf->lenloc	 = 1;
	sf->xc		 = 0;
	sf->xbl		 = 1;
	sf->cmd_type	 = BCM_CMD_SEND_FRAME_WQE;
	sf->cq_id	 = 0xffff;

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)sf, true, nvmf_fc_default_cmpl_cb, NULL);

	return rc;
}
#endif
