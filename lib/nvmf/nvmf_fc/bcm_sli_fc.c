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
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk/likely.h"
#include "spdk_internal/log.h"
#include "bcm_fc.h"

void bcm_process_queues(struct fc_hwqp *hwqp);
void bcm_nvmf_fc_free_req(struct spdk_nvmf_fc_request *fc_req, bool free_xri);
int bcm_init_rqpair_buffers(struct fc_hwqp *hwqp);
int bcm_create_fc_req_mempool(struct fc_hwqp *hwqp);
int bcm_nvmf_fc_recv_data(struct spdk_nvmf_fc_request *fc_req);
int bcm_nvmf_fc_xmt_ls_rsp(struct spdk_nvmf_fc_nport *tgtport, struct nvmf_fc_ls_rqst *ls_rqst);
int bcm_nvmf_fc_handle_rsp(struct spdk_nvmf_fc_request *req);
int bcm_nvmf_fc_send_data(struct spdk_nvmf_fc_request *fc_req);
int bcm_nvmf_fc_issue_abort(struct fc_hwqp *hwqp, fc_xri_t *xri, bool send_abts,
			    bcm_fc_caller_cb cb, void *cb_args);
int bcm_nvmf_fc_xmt_bls_rsp(struct fc_hwqp *hwqp, uint16_t ox_id, uint16_t rx_id,
			    uint16_t rpi, bool rjt, uint8_t rjt_exp, bcm_fc_caller_cb cb, void *cb_args);

static inline struct spdk_nvmf_fc_conn *
bcm_get_fc_conn(struct spdk_nvmf_conn *conn)
{
	return (struct spdk_nvmf_fc_conn *)
	       ((uintptr_t)conn - offsetof(struct spdk_nvmf_fc_conn, conn));
}

static inline void
bcm_queue_tail_inc(bcm_sli_queue_t *q)
{
	q->tail = (q->tail + 1) % q->max_entries;
}

static inline void
bcm_queue_head_inc(bcm_sli_queue_t *q)
{
	q->head = (q->head + 1) % q->max_entries;
}

static inline void *
bcm_queue_head_node(bcm_sli_queue_t *q)
{
	return q->address + q->head * q->size;
}

static inline void *
bcm_queue_tail_node(bcm_sli_queue_t *q)
{
	return q->address + q->tail * q->size;
}

static inline bool
bcm_queue_full(bcm_sli_queue_t *q)
{
	return (q->used >= q->max_entries);
}

static uint32_t
bcm_rqpair_get_buffer_id(struct fc_hwqp *hwqp, uint16_t rqindex)
{
	return hwqp->queues.rq_hdr.rq_map[rqindex];
}


static fc_frame_hdr_t *
bcm_rqpair_get_frame_header(struct fc_hwqp *hwqp, uint16_t rqindex)
{
	uint32_t buf_index = hwqp->queues.rq_hdr.rq_map[rqindex];
	return hwqp->queues.rq_hdr.buffer[buf_index].virt;
}

static bcm_buffer_desc_t *
bcm_rqpair_get_frame_buffer(struct fc_hwqp *hwqp, uint16_t rqindex)
{
	uint32_t buf_index = hwqp->queues.rq_hdr.rq_map[rqindex]; // Use header map.
	return hwqp->queues.rq_payload.buffer + buf_index;
}

int
bcm_create_fc_req_mempool(struct fc_hwqp *hwqp)
{
	char *name = NULL;
	static int unique_number = 0;

	unique_number++;

	/* Name should be unique, otherwise API fails. */
	name = spdk_sprintf_alloc("NVMF_FC_REQ_POOL:%d", unique_number);
	hwqp->fc_request_pool = spdk_mempool_create(name,
				hwqp->queues.rq_hdr.num_buffers,
				sizeof(struct spdk_nvmf_fc_request),
				0, SPDK_ENV_SOCKET_ID_ANY);

	if (hwqp->fc_request_pool == NULL) {
		SPDK_ERRLOG("create fc request pool failed\n");
		return -1;
	}
	TAILQ_INIT(&hwqp->in_use_reqs);

	return 0;
}

static inline struct spdk_nvmf_fc_request *
spdk_nvmf_fc_alloc_req_buf(struct fc_hwqp *hwqp)
{
	struct spdk_nvmf_fc_request *fc_req;

	fc_req = (struct spdk_nvmf_fc_request *)spdk_mempool_get(hwqp->fc_request_pool);
	if (!fc_req) {
		SPDK_ERRLOG("Alloc request buffer failed\n");
		return NULL;
	}

	memset(fc_req, 0, sizeof(struct spdk_nvmf_fc_request));
	TAILQ_INSERT_TAIL(&hwqp->in_use_reqs, fc_req, link);
	return fc_req;
}

static inline void
spdk_nvmf_fc_free_req_buf(struct fc_hwqp *hwqp, struct spdk_nvmf_fc_request *fc_req)
{
	spdk_mempool_put(hwqp->fc_request_pool, (void *)fc_req);
	TAILQ_REMOVE(&hwqp->in_use_reqs, fc_req, link);
}

static int
find_nport_from_sid(struct fc_hwqp *hwqp, uint32_t s_id, uint32_t d_id,
		    struct spdk_nvmf_fc_nport **tgtport, uint16_t *rpi)
{
	int rc = -1;  // TODO: set default error code
	struct spdk_nvmf_fc_nport *n_port = NULL;
	struct spdk_nvmf_fc_rem_port_info *rem_port = NULL;

	assert(hwqp);
	assert(tgtport);
	assert(rpi);

	TAILQ_FOREACH(n_port, &hwqp->fc_port->nport_list, link) {
		if (n_port->d_id == d_id) {
			TAILQ_FOREACH(rem_port, &n_port->rem_port_list, link) {
				if (rem_port->s_id == s_id) {
					*tgtport = n_port;
					*rpi = rem_port->rpi;
					return 0;
				}
			}
		}
	}
	return rc;
}

static void
bcm_notify_queue(bcm_sli_queue_t *q, bool arm_queue, uint16_t num_entries)
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
bcm_queue_entry_is_valid(bcm_sli_queue_t *q, uint8_t *qe, uint8_t clear)
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
bcm_read_queue_entry(bcm_sli_queue_t *q, uint8_t *entry)
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
	qe = bcm_queue_tail_node(q);

	/* Check if entry is valid */
	if (!bcm_queue_entry_is_valid(q, qe, TRUE)) {
		return -1;
	}

	/* Make a copy if user requests */
	if (entry) {
		memcpy(entry, qe, q->size);
	}

	bcm_queue_tail_inc(q);

	return 0;
}

static int
bcm_write_queue_entry(bcm_sli_queue_t *q, uint8_t *entry)
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
	if (bcm_queue_full(q)) {
		SPDK_ERRLOG("%s queue full for type = %#x\n", __func__, q->type);
		return -1;
	}

	/* Copy entry */
	qe = bcm_queue_head_node(q);
	memcpy(qe, entry, q->size);

	/* Update queue */
	bcm_queue_head_inc(q);

	return 0;
}

static int
bcm_post_wqe(struct fc_hwqp *hwqp, uint8_t *entry, bool notify,
	     bcm_fc_wqe_cb cb, void *cb_args)
{
	int rc = -1;
	fc_wqe_ctx_t *wqe_ctx;
	bcm_generic_wqe_t *wqe = (bcm_generic_wqe_t *)entry;
	struct fc_wrkq *wq = &hwqp->queues.wq;

	if (!entry || !cb) {
		goto done;
	}

	/* Fill the context for callback */
	wqe_ctx = &wq->ctx_map[wq->q.head];
	wqe_ctx->cb = cb;
	wqe_ctx->cb_args = cb_args;

	/* Update request tag in the WQE entry */
	wqe->request_tag = wq->q.head;

	rc = bcm_write_queue_entry(&wq->q, entry);
	if (rc) {
		SPDK_ERRLOG("%s: WQE write failed. \n", __func__);
		hwqp->counters.wqe_write_err++;
		goto done;
	}
	wq->q.used++;

	if (notify) {
		bcm_notify_queue(&wq->q, FALSE, 1);
	}
done:
	return rc;
}

static int
bcm_parse_eq_entry(struct eqe *qe, uint16_t *cq_id)
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

static int
bcm_parse_cq_entry(struct fc_eventq *cq, uint8_t *cqe, bcm_qentry_type_e *etype, uint16_t *r_id)
{
	int     rc = -1;
	cqe_t *cqe_entry = (cqe_t *)cqe;

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

		// Flag errors except for FCP_RSP_FAILURE
		if (rc && (rc != BCM_FC_WCQE_STATUS_FCP_RSP_FAILURE)) {

			SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME,
				      "WCQE: status=%#x hw_status=%#x tag=%#x w1=%#x w2=%#x xb=%d\n",
				      cqe_entry->u.wcqe.status,
				      cqe_entry->u.wcqe.hw_status,
				      cqe_entry->u.wcqe.request_tag,
				      cqe_entry->u.wcqe.wqe_specific_1,
				      cqe_entry->u.wcqe.wqe_specific_2,
				      cqe_entry->u.wcqe.xb);
			SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME, "      %08X %08X %08X %08X\n",
				      ((uint32_t *)cqe)[0], ((uint32_t *)cqe)[1],
				      ((uint32_t *)cqe)[2], ((uint32_t *)cqe)[3]);
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
bcm_rqe_rqid_and_index(uint8_t *cqe, uint16_t *rq_id, uint32_t *index)
{
	bcm_fc_async_rcqe_t	*rcqe = (void *)cqe;
	bcm_fc_async_rcqe_v1_t	*rcqe_v1 = (void *)cqe;
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
			SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME,
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
			SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME,
				      "%s: status=%02x rq_id=%d, index=%x pdpl=%x sof=%02x eof=%02x hdpl=%x\n",
				      __func__, rcqe_v1->status,
				      rcqe_v1->rq_id, rcqe_v1->rq_element_index,
				      rcqe_v1->payload_data_placement_length, rcqe_v1->sof_byte,
				      rcqe_v1->eof_byte, rcqe_v1->header_data_placement_length);
		}
	} else {
		*index = UINT32_MAX;

		rc = rcqe->status;

		SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME,
			      "%s: status=%02x rq_id=%d, index=%x pdpl=%x sof=%02x eof=%02x hdpl=%x\n", __func__,
			      rcqe->status, rcqe->rq_id, rcqe->rq_element_index, rcqe->payload_data_placement_length,
			      rcqe->sof_byte, rcqe->eof_byte, rcqe->header_data_placement_length);
	}

	return rc;
}

static int
bcm_rqpair_buffer_post(struct fc_hwqp *hwqp, uint16_t idx, bool notify)
{
	int rc;
	struct fc_rcvq *hdr = &hwqp->queues.rq_hdr;
	struct fc_rcvq *payload = &hwqp->queues.rq_payload;
	uint32_t phys_hdr[2];

	/* Post payload buffer */
	phys_hdr[0] =  PTR_TO_ADDR32_HI(payload->buffer[idx].phys);
	phys_hdr[1] =  PTR_TO_ADDR32_LO(payload->buffer[idx].phys);
	rc = bcm_write_queue_entry(&payload->q, (uint8_t *)phys_hdr);
	if (!rc) {
		/* Post header buffer */
		phys_hdr[0] =  PTR_TO_ADDR32_HI(hdr->buffer[idx].phys);
		phys_hdr[1] =  PTR_TO_ADDR32_LO(hdr->buffer[idx].phys);
		rc = bcm_write_queue_entry(&hdr->q, (uint8_t *)phys_hdr);
		if (!rc) {

			hwqp->queues.rq_hdr.q.used++;
			hwqp->queues.rq_payload.q.used++;

			if (notify) {
				bcm_notify_queue(&hdr->q, FALSE, 1);
			}
		}
	}
	return rc;
}

static void
bcm_rqpair_buffer_release(struct fc_hwqp *hwqp, uint16_t buff_idx)
{
	/* Decrement used */
	hwqp->queues.rq_hdr.q.used--;
	hwqp->queues.rq_payload.q.used--;

	/* Increment tail */
	bcm_queue_tail_inc(&hwqp->queues.rq_hdr.q);
	bcm_queue_tail_inc(&hwqp->queues.rq_payload.q);

	/* Repost the freebuffer to head of queue. */
	hwqp->queues.rq_hdr.rq_map[hwqp->queues.rq_hdr.q.head] = buff_idx;
	bcm_rqpair_buffer_post(hwqp, buff_idx, TRUE);
}

int
bcm_init_rqpair_buffers(struct fc_hwqp *hwqp)
{
	int rc = 0;
	uint16_t i;
	struct fc_rcvq *hdr = &hwqp->queues.rq_hdr;
	struct fc_rcvq *payload = &hwqp->queues.rq_payload;

	/* Make sure CQs are in armed state */
	bcm_notify_queue(&hwqp->queues.cq_wq.q, TRUE, 0);
	bcm_notify_queue(&hwqp->queues.cq_rq.q, TRUE, 0);

	assert(hdr->q.max_entries == payload->q.max_entries);
	assert(hdr->q.max_entries <= MAX_RQ_ENTRIES);

	hdr->q.head = hdr->q.tail = 0;
	payload->q.head = payload->q.tail = 0;

	for (i = 0; i < hdr->q.max_entries; i++) {
		rc = bcm_rqpair_buffer_post(hwqp, i, FALSE);
		if (rc) {
			break;
		}
		hdr->rq_map[i] = i;
	}

	if (!rc) {
		/* Ring doorbell for one less */
		bcm_notify_queue(&hdr->q, FALSE, (hdr->q.max_entries - 1));
	}

	return rc;
}

static void
spdk_nvmf_fc_post_nvme_rqst(void *arg1, void *arg2)
{
	int rc = 0;
	struct spdk_nvmf_fc_request *fc_req = (struct spdk_nvmf_fc_request *)arg1;

	rc = spdk_nvmf_request_exec(&fc_req->req);
	switch (rc) {
	case SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE:
	case SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS:
		/* Posted */
		return;
	default:
		break;
	}

	/* Issue abort for oxid */
	SPDK_ERRLOG("Aborted CMD\n");
}

static void
bcm_nvmf_add_xri_pending(struct fc_hwqp *hwqp, fc_xri_t *xri)
{
	fc_xri_t *tmp;

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
bcm_nvmf_del_xri_pending(struct fc_hwqp *hwqp, uint32_t xri)
{
	fc_xri_t *tmp;

	TAILQ_FOREACH(tmp, &hwqp->pending_xri_list, link) {
		if (tmp->xri == xri) {
			spdk_nvmf_fc_put_xri(hwqp, tmp);
			TAILQ_REMOVE(&hwqp->pending_xri_list, tmp, link);
			return;
		}
	}
}

void
bcm_nvmf_fc_free_req(struct spdk_nvmf_fc_request *fc_req, bool free_xri)
{
	if (!fc_req) {
		return;
	}

	if (fc_req->xri) {
		if (!free_xri) {
			/* This will be cleaned up by XRI_ABORTED_CQE. */
			bcm_nvmf_add_xri_pending(fc_req->hwqp, fc_req->xri);
		} else {
			spdk_nvmf_fc_put_xri(fc_req->hwqp, fc_req->xri);
		}
	}
	fc_req->xri = NULL;

	if (fc_req->req.data) {
		spdk_dma_free(fc_req->req.data);
		fc_req->req.data = NULL;
	} else if (fc_req->req.iovcnt) {
		/* Release BDEV IOV if acquired */
		spdk_nvmf_request_cleanup(&fc_req->req);
		fc_req->req.iovcnt = 0;
	}

	/* Release RQ buffer */
	bcm_rqpair_buffer_release(fc_req->hwqp, fc_req->buf_index);

	/* Free Fc request */
	spdk_nvmf_fc_free_req_buf(fc_req->hwqp, fc_req);
}

static void
bcm_abort_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	struct fc_hwqp *hwqp = ctx;
	fc_caller_ctx_t *carg = arg;
	fc_xri_t *xri = carg->ctx;

	SPDK_NOTICELOG("IO Aborted(XRI:%d, Status=%d)\n", xri->xri, status);

	if (!status) {
		/* This will be cleaned up by XRI_ABORTED_CQE. */
		bcm_nvmf_add_xri_pending(hwqp, xri);
	}

	if (carg->cb) {
		carg->cb(hwqp, status, carg->cb_args);
	}

	spdk_free(carg);
}

static void
bcm_bls_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	struct fc_hwqp *hwqp = ctx;
	fc_caller_ctx_t *carg = arg;
	fc_xri_t *xri = carg->ctx;
	cqe_t *cqe_entry = (cqe_t *)cqe;

	SPDK_NOTICELOG("BLS WQE Compl(%d) \n", status);

	if (!cqe_entry->u.generic.xb) {
		spdk_nvmf_fc_put_xri(hwqp, xri);
	} else {
		bcm_nvmf_add_xri_pending(hwqp, xri);
	}

	if (carg->cb) {
		carg->cb(hwqp, status, carg->cb_args);
	}

	spdk_free(carg);
}

static void
bcm_ls_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	struct nvmf_fc_ls_rqst *ls_rqst = arg;
	struct fc_hwqp *hwqp = ctx;

	/* Release XRI */
	spdk_nvmf_fc_put_xri(hwqp, ls_rqst->xri);

	/* Release RQ buffer */
	bcm_rqpair_buffer_release(hwqp, ls_rqst->rqstbuf.buf_index);

	if (status) {
		SPDK_ERRLOG("LS WQE Compl(%d) error\n", status);
	}
}

static void
bcm_io_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	struct spdk_nvmf_fc_request *fc_req = arg;
	struct spdk_nvmf_fc_conn *fc_conn = fc_req->fc_conn;
	struct spdk_nvme_cmd *cmd = &fc_req->req.cmd->nvme_cmd;
	cqe_t *cqe_entry = (cqe_t *)cqe;
	bool free_xri;

	free_xri = (!cqe_entry->u.generic.xb ? TRUE : FALSE);

	if (status) {
		SPDK_ERRLOG("IO WQE Compl(%d)\n", status);
		goto free_req;
	}

	if (fc_req->is_aborted) {
		/*
		 * Abort is posted after the WQE is completed.
		 * XRI will be cleaned as part of XRI_ABORTED_CQE.
		 */
		free_xri = FALSE;
		goto free_req;
	}

	if (!fc_req->rsp_sent) { /* Data Xfer done */
		fc_req->transfered_len = cqe_entry->u.generic.word1.total_data_placed;

		if (fc_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
			struct spdk_event *event = NULL;

			if ((fc_conn->qid == NVME_ADMIN_QUEUE_ID) ||
			    (cmd->opc == SPDK_NVME_OPC_FABRIC)) {
				/* Switch to master lcore for AQ cmds */
				event = spdk_event_allocate(spdk_env_get_master_lcore(),
							    spdk_nvmf_fc_post_nvme_rqst,
							    (void *)fc_req, NULL);
				spdk_event_call(event);
			} else {
				spdk_nvmf_fc_post_nvme_rqst(fc_req, NULL);
			}

		} else {
			if (bcm_nvmf_fc_handle_rsp(fc_req)) {
				goto free_req;
			}
		}
		return;
	}

	/* IO completed successfully */

free_req:
	bcm_nvmf_fc_free_req(fc_req, free_xri);
}

static void
bcm_process_wqe_completion(struct fc_hwqp *hwqp, uint16_t req_tag, int status, uint8_t *cqe)
{
	fc_wqe_ctx_t *wqe_ctx;

	SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME, "WQE Compl(%d)\n", status);

	/* Free WQE slot */
	hwqp->queues.wq.q.used--;
	bcm_queue_tail_inc(&hwqp->queues.wq.q);

	/* Call the callback */
	wqe_ctx = &hwqp->queues.wq.ctx_map[req_tag];
	wqe_ctx->cb(hwqp, cqe, status, wqe_ctx->cb_args);
}

static int
spdk_nvmf_fc_execute_nvme_rqst(struct spdk_nvmf_fc_request *fc_req)
{
	int rc = 0;
	struct spdk_nvme_cmd *cmd = &fc_req->req.cmd->nvme_cmd;
	struct spdk_nvmf_fc_conn *fc_conn = fc_req->fc_conn;
	struct spdk_event *event = NULL;

	if (fc_req->req.length) {
		/* Create buffers for all AQ commands and
		 * IOQ commands except for Read/Write
		 */
		if (fc_req->req.conn->type == CONN_TYPE_AQ ||
		    (fc_req->req.conn->type == CONN_TYPE_IOQ &&
		     cmd->opc != SPDK_NVME_OPC_READ &&
		     cmd->opc != SPDK_NVME_OPC_WRITE)) {

			fc_req->req.data = spdk_dma_zmalloc(fc_req->req.length, 4096, NULL);
			if (!fc_req->req.data) {
				SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME, "No buffer. Requeue\n");
				TAILQ_INSERT_TAIL(&fc_conn->pending_data_buf_queue,
						  fc_req, pending_link);
				fc_req->hwqp->counters.aq_buf_alloc_err++;
				goto done;
			}

			/*
			 * Request module will only use data pointer since it is set.
			 * Below we are using req.iov memory for storing ->data phys
			 * segment address.
			 */
			fc_req->req.iovcnt = spdk_dma_virt_to_iovec(fc_req->req.data,
					     fc_req->req.length, fc_req->req.iov, MAX_NUM_OF_IOVECTORS);
			if (!fc_req->req.iovcnt) {
				spdk_dma_free(fc_req->req.data);
				fc_req->req.data = NULL;
				goto error;
			}
		}
	}

	if (fc_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME, "WRITE CMD.\n");
		/* Check if we have buffer already. If not acquire from bdev */
		if (!fc_req->req.data) {
			rc = spdk_nvmf_request_exec(&fc_req->req);
			switch (rc) {
			case SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_READY:
				break;
			case SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_PENDING:
				TAILQ_INSERT_TAIL(&fc_conn->pending_data_buf_queue,
						  fc_req, pending_link);
				goto done;
			case SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_ERROR:
			default:
				fc_req->hwqp->counters.fc_req_buf_err++;
				goto error;
			}

			SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME, "Got IOV buffers from bdev.\n");
		}

		/* Got the buffer. Post trecv. */
		if (bcm_nvmf_fc_recv_data(fc_req)) {
			goto error;
		}

	} else {
		SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME, "READ/NONE CMD\n");
		if ((fc_conn->qid == NVME_ADMIN_QUEUE_ID) || (cmd->opc == SPDK_NVME_OPC_FABRIC)) {
			/* Switch to master lcore for admin queue commands. */
			event = spdk_event_allocate(spdk_env_get_master_lcore(),
						    spdk_nvmf_fc_post_nvme_rqst, (void *)fc_req, NULL);
			spdk_event_call(event);
		} else {
			spdk_nvmf_fc_post_nvme_rqst(fc_req, NULL);
		}
	}
done:
	return 0;
error:
	return -1;
}

static int
spdk_nvmf_fc_handle_nvme_rqst(struct fc_hwqp *hwqp, struct fc_frame_hdr *frame,
			      uint32_t buf_idx, struct bcm_buffer_desc *buffer, uint32_t plen)
{
	uint16_t cmnd_len;
	uint64_t rqst_conn_id;
	struct nvmf_fc_rq_buf_nvme_cmd *req_buf = NULL;
	struct spdk_nvmf_fc_request *fc_req = NULL;
	struct nvme_cmnd_iu *cmd_iu = NULL;
	struct spdk_nvmf_fc_conn *fc_conn = NULL;
	enum spdk_nvme_data_transfer xfer;
	bool found = false;

	req_buf = (struct nvmf_fc_rq_buf_nvme_cmd *)buffer->virt;
	cmd_iu = &req_buf->cmd_iu;
	cmnd_len = req_buf->cmd_iu.cmnd_iu_len;
	cmnd_len = from_be16(&cmnd_len);

	/* check for a valid cmnd_iu format */
	if ((cmd_iu->fc_id != NVME_CMND_IU_FC_ID) ||
	    (cmd_iu->scsi_id != NVME_CMND_IU_SCSI_ID) ||
	    (cmnd_len != NVME_CMND_IU_SIZE / 4)) {
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

	/* allocate a request buffer */
	fc_req = spdk_nvmf_fc_alloc_req_buf(hwqp);
	if (fc_req == NULL) {
		goto abort;
	}

	fc_req->req.length = from_be32(&cmd_iu->data_len);
	fc_req->req.conn = &fc_conn->conn;
	fc_req->req.cmd = &req_buf->cmd_iu.cmd;
	fc_req->req.rsp = &fc_req->ersp.rsp;
	fc_req->oxid = frame->ox_id;
	fc_req->oxid = from_be16(&fc_req->oxid);
	fc_req->rpi = fc_conn->rpi;
	fc_req->buf_index = buf_idx;
	fc_req->poller_lcore = hwqp->lcore_id;
	fc_req->hwqp = hwqp;
	fc_req->fc_conn = fc_conn;
	fc_req->xri = spdk_nvmf_fc_get_xri(hwqp);
	fc_req->req.xfer = xfer;

	/* Save the fc request pointer in RQ buff */
	req_buf->fc_req = fc_req;

	if (spdk_nvmf_fc_execute_nvme_rqst(fc_req)) {
		goto abort;
	}

	return 0;

abort:
	/* Issue abort for oxid */
	SPDK_ERRLOG("Aborted CMD\n");
	return -1;
}

static int
spdk_nvmf_fc_process_frame(struct fc_hwqp *hwqp, uint32_t buff_idx, fc_frame_hdr_t *frame,
			   bcm_buffer_desc_t *buffer, uint32_t plen)
{
	int rc;
	uint32_t s_id, d_id;
	uint16_t rpi;
	struct spdk_nvmf_fc_nport *nport;
	struct nvmf_fc_ls_rqst *ls_rqst;

	s_id = (uint32_t)frame->s_id;
	d_id = (uint32_t)frame->d_id;
	s_id = from_be32(&s_id) >> 8;
	d_id = from_be32(&d_id) >> 8;

	SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME, "Process NVME frame\n");

	rc = find_nport_from_sid(hwqp, s_id, d_id, &nport, &rpi);
	if (rc) {
		SPDK_ERRLOG("%s Nport not found. Dropping\n", __func__);
		hwqp->counters.nport_invalid++;
		return rc;
	}

	if ((frame->r_ctl == NVME_FC_R_CTL_LS_REQUEST) &&
	    (frame->type == NVME_FC_TYPE_NVMF_DATA)) {

		SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME, "Process LS NVME frame\n");
		struct nvmf_fc_rq_buf_ls_request *req_buf = buffer->virt;

		/* Use the RQ buffer for holding LS request. */
		ls_rqst = (struct nvmf_fc_ls_rqst *)&req_buf->ls_rqst;

		/* Fill in the LS request structure */
		ls_rqst->rqstbuf.virt = (void *)&req_buf->rqst;
		ls_rqst->rqstbuf.phys = buffer->phys +
					offsetof(struct nvmf_fc_rq_buf_ls_request, rqst);
		ls_rqst->rqstbuf.buf_index = buff_idx;
		ls_rqst->rqst_len = plen;

		ls_rqst->rspbuf.virt = (void *)&req_buf->resp;
		ls_rqst->rspbuf.phys = buffer->phys +
				       offsetof(struct nvmf_fc_rq_buf_ls_request, resp);
		ls_rqst->rsp_len = BCM_MAX_RESP_BUFFER_SIZE;

		ls_rqst->private_data = (void *)hwqp;
		ls_rqst->rpi = rpi;
		ls_rqst->oxid = (uint16_t)frame->ox_id;
		ls_rqst->oxid = from_be16(&ls_rqst->oxid);

		/* get an XRI for this request */
		ls_rqst->xri = spdk_nvmf_fc_get_xri(hwqp);

		/* Handle the request to LS module */
		spdk_nvmf_fc_handle_ls_rqst(nport, ls_rqst);

	} else if ((frame->r_ctl == NVME_FC_R_CTL_CMD_REQ) &&
		   (frame->type == NVME_FC_TYPE_FC_EXCHANGE)) {

		SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME, "Process IO NVME frame\n");
		rc = spdk_nvmf_fc_handle_nvme_rqst(hwqp, frame, buff_idx, buffer, plen);
	} else {

		SPDK_ERRLOG("%s Unknown frame received. Dropping\n", __func__);
		hwqp->counters.unknown_frame++;
		rc = -1;
	}

	return rc;
}

static int
bcm_process_rqpair(struct fc_hwqp *hwqp, fc_eventq_t *cq, uint8_t *cqe)
{
	int rc = -1, rq_index = 0;
	uint16_t rq_id = 0;
	int32_t rq_status;
	uint32_t buff_idx = 0;
	fc_frame_hdr_t *frame = NULL;
	bcm_buffer_desc_t *payload_buffer = NULL;
	bcm_fc_async_rcqe_t *rcqe = (bcm_fc_async_rcqe_t *)cqe;

	assert(hwqp);
	assert(cq);
	assert(cqe);

	rq_status = bcm_rqe_rqid_and_index(cqe, &rq_id, &rq_index);
	if (0 != rq_status) {
		switch (rq_status) {
		case BCM_FC_ASYNC_RQ_BUF_LEN_EXCEEDED:
		case BCM_FC_ASYNC_RQ_DMA_FAILURE:
			if (rq_index < 0 || rq_index >= hwqp->queues.rq_hdr.q.max_entries) {
				SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME,
					      "%s: status=%#x: rq_id lookup failed for id=%#x\n",
					      __func__, rq_status, rq_id);
				hwqp->counters.rq_buf_len_err++;
				break;
			}

			buff_idx = bcm_rqpair_get_buffer_id(hwqp, rq_index);
			goto buffer_release;

		case BCM_FC_ASYNC_RQ_INSUFF_BUF_NEEDED:
		case BCM_FC_ASYNC_RQ_INSUFF_BUF_FRM_DISC:
			SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME,
				      "%s: Warning: RCQE status=%#x, \n",
				      __func__, rq_status);
			hwqp->counters.rq_status_err++;
		default:
			break;
		}

		/* Buffer not consumed. No need to return */
		return rc;
	}

	/* Make sure rq_index is in range */
	if (rq_index >= hwqp->queues.rq_hdr.q.max_entries) {
		SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME,
			      "%s: Error: rq index out of range for RQ%d\n",
			      __func__, rq_id);
		hwqp->counters.rq_index_err++;
		return rc;
	}

	/* Process NVME frame */
	buff_idx = bcm_rqpair_get_buffer_id(hwqp, rq_index);
	frame = bcm_rqpair_get_frame_header(hwqp, rq_index);
	payload_buffer = bcm_rqpair_get_frame_buffer(hwqp, rq_index);

	rc = spdk_nvmf_fc_process_frame(hwqp, buff_idx, frame, payload_buffer,
					rcqe->payload_data_placement_length);
	if (!rc) {
		return 0;
	}

buffer_release:
	/* Return buffer to chip */
	bcm_rqpair_buffer_release(hwqp, buff_idx);
	return rc;
}

static int
bcm_process_cq_entry(struct fc_hwqp *hwqp, struct fc_eventq *cq)
{
	int rc = 0;
	uint8_t	cqe[sizeof(cqe_t)];
	uint16_t rid = UINT16_MAX;
	uint32_t n_processed = 0;
	bcm_qentry_type_e ctype;     /* completion type */

	assert(hwqp);
	assert(cq);

	while (!bcm_read_queue_entry(&cq->q, &cqe[0])) {
		n_processed++;

		rc = bcm_parse_cq_entry(cq, cqe, &ctype, &rid);
		/*
		 * The sign of status is significant. If status is:
		 * == 0 : call completed correctly and the CQE indicated success
		 *  > 0 : call completed correctly and the CQE indicated an error
		 *  < 0 : call failed and no information is available about the CQE
		 */
		if (rc < 0) {
			if (rc == -2) {
				/* Entry was consumed */
				continue;
			}
			break;
		}

		switch ((int)ctype) {
		case BCM_FC_QENTRY_WQ:
			bcm_process_wqe_completion(hwqp, rid, rc, cqe);
			break;
		case BCM_FC_QENTRY_WQ_RELEASE:
			SPDK_WARNLOG("%s: WQE Release not implemented.\n", __func__);
			break;
		case BCM_FC_QENTRY_RQ:
			bcm_process_rqpair(hwqp, cq, cqe);
			break;
		case BCM_FC_QENTRY_XABT:
			bcm_nvmf_del_xri_pending(hwqp, rid);
			break;
		default:
			SPDK_WARNLOG("%s: unhandled ctype=%#x rid=%#x\n",
				     __func__, ctype, rid);
			hwqp->counters.invalid_cq_type++;
			break;
		}

		if (n_processed >= (cq->q.posted_limit)) {
			bcm_notify_queue(&cq->q, FALSE, n_processed);
			n_processed = 0;
		}
	}

	bcm_notify_queue(&cq->q, cq->auto_arm_flag, n_processed);

	return rc;
}

void
bcm_process_queues(struct fc_hwqp *hwqp)
{
	int rc = 0;
	uint32_t n_processed = 0;
	uint8_t eqe[sizeof(eqe_t)] = { 0 };
	uint16_t cq_id;
	struct fc_eventq *eq;

	assert(hwqp);
	eq = &hwqp->queues.eq;

	while (!bcm_read_queue_entry(&eq->q, &eqe[0])) {
		n_processed++;

		rc = bcm_parse_eq_entry((struct eqe *)eqe, &cq_id);
		if (spdk_likely(rc))  {
			if (rc > 0) {
				/* EQ is full.  Process all CQs */
				bcm_process_cq_entry(hwqp, &hwqp->queues.cq_wq);
				bcm_process_cq_entry(hwqp, &hwqp->queues.cq_rq);
				continue;
			} else {
				break;
			}
		} else {
			if (cq_id == hwqp->queues.cq_wq.q.qid) {
				bcm_process_cq_entry(hwqp, &hwqp->queues.cq_wq);
			} else if (cq_id == hwqp->queues.cq_rq.q.qid) {
				bcm_process_cq_entry(hwqp, &hwqp->queues.cq_rq);
			} else {
				SPDK_ERRLOG("%s bad CQ_ID %#06x\n", __func__, cq_id);
				hwqp->counters.invalid_cq_id++;
			}

			if (n_processed >= (eq->q.posted_limit)) {
				bcm_notify_queue(&eq->q, FALSE, n_processed);
				n_processed = 0;
			}
		}
	}

	bcm_notify_queue(&eq->q, eq->auto_arm_flag, n_processed);
	return;
}

int
bcm_nvmf_fc_xmt_ls_rsp(struct spdk_nvmf_fc_nport *tgtport,
		       struct nvmf_fc_ls_rqst *ls_rqst)
{
	uint8_t wqe[128] = { 0 };
	bcm_xmit_sequence64_wqe_t *xmit = (bcm_xmit_sequence64_wqe_t *)wqe;
	struct fc_hwqp *hwqp = NULL;
	int rc = -1;

	xmit->xbl = TRUE;
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
	xmit->abort_tag  = 0;
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

	hwqp = (struct fc_hwqp *)ls_rqst->private_data;

	rc = bcm_post_wqe(hwqp, (uint8_t *)xmit, TRUE, bcm_ls_cmpl_cb, ls_rqst);

	return rc;
}

static int
bcm_nvmf_fc_fill_sgl(struct spdk_nvmf_fc_request *fc_req)
{
	int offset = 0, i;
	uint64_t iov_phys;
	bcm_sge_t *sge = NULL;
	struct nvmf_fc_rq_buf_nvme_cmd *req_buf = NULL;
	struct fc_hwqp *hwqp = fc_req->hwqp;

	assert((fc_req->req.iovcnt) <= BCM_MAX_IOVECS);
	assert(fc_req->req.iovcnt != 0);

	/* Use RQ buffer for SGL */
	req_buf = hwqp->queues.rq_payload.buffer[fc_req->buf_index].virt;
	sge = &req_buf->sge[0];

	if (fc_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) { /* Write */
		uint64_t xfer_rdy_phys;
		struct nvme_xfer_rdy_iu *xfer_rdy_iu;

		/* 1st SGE is tranfer ready buffer */
		xfer_rdy_iu = hwqp->queues.rq_payload.buffer[fc_req->buf_index].virt +
			      offsetof(struct nvmf_fc_rq_buf_nvme_cmd, xfer_rdy);
		xfer_rdy_iu->relative_offset = 0;
		to_be32(&xfer_rdy_iu->burst_len, fc_req->req.length);

		xfer_rdy_phys = hwqp->queues.rq_payload.buffer[fc_req->buf_index].phys +
				offsetof(struct nvmf_fc_rq_buf_nvme_cmd, xfer_rdy);

		sge->sge_type = BCM_SGE_TYPE_DATA;
		sge->buffer_address_low  = PTR_TO_ADDR32_LO(xfer_rdy_phys);
		sge->buffer_address_high = PTR_TO_ADDR32_HI(xfer_rdy_phys);
		sge->buffer_length = sizeof(struct nvme_xfer_rdy_iu);
		sge++;

	} else if (fc_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) { /* read */
		/* 1st SGE is skip. */
		sge->sge_type = BCM_SGE_TYPE_SKIP;
		sge++;
	} else {
		return -1;
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
			sge->last = TRUE;
		} else {
			sge++;
		}
	}
	return 0;
}

int
bcm_nvmf_fc_send_data(struct spdk_nvmf_fc_request *fc_req)
{
	int rc = 0;
	uint8_t wqe[128] = { 0 };
	bcm_fcp_tsend64_wqe_t *tsend = (bcm_fcp_tsend64_wqe_t *)wqe;
	struct fc_hwqp *hwqp = fc_req->hwqp;
	struct spdk_nvmf_request *req = &fc_req->req;
	struct spdk_nvmf_conn 	*conn = req->conn;
	struct spdk_nvmf_fc_conn *fc_conn = bcm_get_fc_conn(conn);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	tsend->xbl = TRUE;
	if (fc_req->req.iovcnt == 1) {
		/* Data is a single physical address, use a BDE */
		uint64_t bde_phys;

		bde_phys = spdk_vtophys(fc_req->req.iov[0].iov_base);
		tsend->dbde = TRUE;
		tsend->bde.bde_type = BCM_BDE_TYPE_BDE_64;
		tsend->bde.buffer_length = fc_req->req.length;

		tsend->bde.u.data.buffer_address_low = PTR_TO_ADDR32_LO(bde_phys);
		tsend->bde.u.data.buffer_address_high = PTR_TO_ADDR32_HI(bde_phys);

	} else {
		uint64_t sgl_phys;

		rc = bcm_nvmf_fc_fill_sgl(fc_req);
		if (rc) {
			return -1;
		}

		sgl_phys = hwqp->queues.rq_payload.buffer[fc_req->buf_index].phys +
			   offsetof(struct nvmf_fc_rq_buf_nvme_cmd, sge);

		tsend->bde.bde_type = BCM_BDE_TYPE_BLP;
		tsend->bde.buffer_length = fc_req->req.length;
		tsend->bde.u.blp.sgl_segment_address_low = PTR_TO_ADDR32_LO(sgl_phys);
		tsend->bde.u.blp.sgl_segment_address_high = PTR_TO_ADDR32_HI(sgl_phys);
	}

	tsend->relative_offset = 0;
	tsend->xri_tag = fc_req->xri->xri;
	tsend->rpi = fc_req->rpi;
	tsend->pu = TRUE;

	/* For non fabric and when we dont reach ersp ratio, use auto resp. */
	if (((fc_conn->rsp_count + 1) % fc_conn->esrp_ratio) &&
	    (cmd->opc != SPDK_NVME_OPC_FABRIC)) {
		fc_conn->rsp_count++;
		tsend->ar = TRUE;
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

	rc = bcm_post_wqe(hwqp, (uint8_t *)tsend, TRUE, bcm_io_cmpl_cb, fc_req);
	if (!rc) {
		if (tsend->ar) {
			fc_req->rsp_sent = TRUE;
		}
		fc_req->xri_activated = TRUE;
	}

	return rc;
}

int
bcm_nvmf_fc_recv_data(struct spdk_nvmf_fc_request *fc_req)
{
	int rc = 0;
	uint8_t wqe[128] = { 0 };
	bcm_fcp_treceive64_wqe_t *trecv = (bcm_fcp_treceive64_wqe_t *)wqe;
	struct fc_hwqp *hwqp = fc_req->hwqp;

	trecv->xbl = TRUE;
	if (fc_req->req.iovcnt == 1) {
		/* Data is a single physical address, use a BDE */
		uint64_t bde_phys;

		bde_phys = spdk_vtophys(fc_req->req.iov[0].iov_base);
		trecv->dbde = TRUE;
		trecv->bde.bde_type = BCM_BDE_TYPE_BDE_64;
		trecv->bde.buffer_length = fc_req->req.length;
		trecv->bde.u.data.buffer_address_low = PTR_TO_ADDR32_LO(bde_phys);
		trecv->bde.u.data.buffer_address_high = PTR_TO_ADDR32_HI(bde_phys);
	} else {
		uint64_t sgl_phys;

		rc = bcm_nvmf_fc_fill_sgl(fc_req);
		if (rc) {
			return -1;
		}

		sgl_phys = hwqp->queues.rq_payload.buffer[fc_req->buf_index].phys +
			   offsetof(struct nvmf_fc_rq_buf_nvme_cmd, sge);

		trecv->bde.bde_type = BCM_BDE_TYPE_BLP;
		trecv->bde.buffer_length = fc_req->req.length;
		trecv->bde.u.blp.sgl_segment_address_low = PTR_TO_ADDR32_LO(sgl_phys);
		trecv->bde.u.blp.sgl_segment_address_high = PTR_TO_ADDR32_HI(sgl_phys);
	}

	trecv->relative_offset = 0;
	trecv->xri_tag = fc_req->xri->xri;
	trecv->context_tag = fc_req->rpi;
	trecv->pu = TRUE;
	trecv->ar = FALSE;

	trecv->command = BCM_WQE_FCP_TRECEIVE64;
	trecv->class = BCM_ELS_REQUEST64_CLASS_3;
	trecv->ct = BCM_ELS_REQUEST64_CONTEXT_RPI;

	trecv->remote_xid = fc_req->oxid;
	trecv->nvme = 1;
	trecv->iod = 1;
	trecv->len_loc = 0x2;

	trecv->cmd_type = BCM_CMD_FCP_TRECEIVE64_WQE;
	trecv->cq_id = 0xFFFF;
	trecv->fcp_data_receive_length = fc_req->req.length;

	rc = bcm_post_wqe(hwqp, (uint8_t *)trecv, TRUE, bcm_io_cmpl_cb, fc_req);
	if (!rc) {
		fc_req->xri_activated = TRUE;
	}

	return rc;
}

static int
bcm_nvmf_fc_xmt_rsp(struct spdk_nvmf_fc_request *fc_req, uint8_t *ersp_buf, uint32_t ersp_len)
{
	int rc = 0;
	uint8_t wqe[128] = { 0 };
	bcm_fcp_trsp64_wqe_t *trsp = (bcm_fcp_trsp64_wqe_t *)wqe;
	struct fc_hwqp *hwqp = fc_req->hwqp;

	if (!ersp_buf) {
		/* Auto-Gen all zeroes in IU 12-byte payload */
		trsp->ag = TRUE;
	} else {
		trsp->ag = TRUE;
		trsp->wqes = 1;
		trsp->irsp = 1;
		trsp->fcp_response_length = ersp_len;
		trsp->irsplen = (ersp_len >> 2) - 1;
		memcpy(&trsp->inline_rsp, ersp_buf, ersp_len);
	}

	if (fc_req->xri_activated) {
		trsp->xc = TRUE;
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

	rc = bcm_post_wqe(hwqp, (uint8_t *)trsp, TRUE, bcm_io_cmpl_cb, fc_req);
	if (!rc) {
		fc_req->rsp_sent = TRUE;
		fc_req->xri_activated = TRUE;
	}

	return rc;
}

int
bcm_nvmf_fc_handle_rsp(struct spdk_nvmf_fc_request *fc_req)
{
	int rc = 0;
	struct spdk_nvmf_request *req = &fc_req->req;
	struct spdk_nvmf_conn 	*conn = req->conn;
	struct spdk_nvmf_fc_conn *fc_conn = bcm_get_fc_conn(conn);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint16_t status = *((uint16_t *)&rsp->status), ersp_len = 0;

	rsp->sqhd = conn->sq_head;

	/* Advance our sq_head pointer */
	if (conn->sq_head == conn->sq_head_max) {
		conn->sq_head = 0;
	} else {
		conn->sq_head++;
	}

	/* Increment connection responses */
	fc_conn->rsp_count++;

	/*
	 * Check if we need to send rsp or ERSP
	 * 1) For every N responses where N == ersp_ratio
	 * 2) Fabric commands.
	 * 3) Completion status failed or Completion dw0 or dw1 valid.
	 * 4) SQ == 90% full. TODO
	 * 5) CMD is fused. TODO
	 * 6) Transfer length not equal to CMD IU length
	 */
	if (!(fc_conn->rsp_count % fc_conn->esrp_ratio) ||
	    (cmd->opc == SPDK_NVME_OPC_FABRIC) ||
	    (status & 0xFFFE) || rsp->cdw0 || rsp->rsvd1 ||
	    (req->length != fc_req->transfered_len)) {

		/* Fill ERSP Len */
		to_be16(&ersp_len, (sizeof(struct nvme_ersp_iu) / sizeof(uint32_t)));
		fc_req->ersp.ersp_len = ersp_len;

		/* Fill RSN */
		to_be32(&fc_req->ersp.response_seq_no, fc_conn->rsn);
		fc_conn->rsn++;

		/* Fill transfer length */
		to_be32(&fc_req->ersp.transferred_data_len, fc_req->transfered_len);

		SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME, "Posting ERSP.\n");
		rc = bcm_nvmf_fc_xmt_rsp(fc_req, (uint8_t *)&fc_req->ersp,
					 sizeof(struct nvme_ersp_iu));
	} else {
		SPDK_TRACELOG(SPDK_TRACE_BCM_FC_NVME, "Posting RSP.\n");
		rc = bcm_nvmf_fc_xmt_rsp(fc_req, NULL, 0);
	}
	return rc;
}

int
bcm_nvmf_fc_issue_abort(struct fc_hwqp *hwqp, fc_xri_t *xri, bool send_abts,
			bcm_fc_caller_cb cb, void *cb_args)
{
	uint8_t wqe[128] = { 0 };
	bcm_abort_wqe_t *abort = (bcm_abort_wqe_t *)wqe;
	fc_caller_ctx_t *ctx = NULL;
	int rc = -1;

	ctx = spdk_malloc(sizeof(fc_caller_ctx_t));
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
	abort->qosd = TRUE;
	abort->cq_id = UINT16_MAX;
	abort->cmd_type = BCM_CMD_ABORT_WQE;
	abort->t_tag = xri->xri;

	rc = bcm_post_wqe(hwqp, (uint8_t *)abort, TRUE, bcm_abort_cmpl_cb, ctx);
done:
	if (rc && ctx) {
		spdk_free(ctx);
	}

	return rc;
}

int
bcm_nvmf_fc_xmt_bls_rsp(struct fc_hwqp *hwqp, uint16_t ox_id, uint16_t rx_id,
			uint16_t rpi, bool rjt, uint8_t rjt_exp, bcm_fc_caller_cb cb, void *cb_args)
{
	uint8_t wqe[128] = { 0 };
	bcm_xmit_bls_rsp_wqe_t *bls = (bcm_xmit_bls_rsp_wqe_t *)wqe;
	int rc = -1;
	fc_caller_ctx_t *ctx = NULL;
	fc_xri_t *xri = NULL;

	xri = spdk_nvmf_fc_get_xri(hwqp);
	if (!xri) {
		goto done;
	}

	ctx = spdk_malloc(sizeof(fc_caller_ctx_t));
	if (!ctx) {
		goto done;
	}
	ctx->ctx = xri;
	ctx->cb = cb;
	ctx->cb_args = cb_args;

	if (rjt) {
		bls->payload_word0 = ((uint32_t)BCM_BLS_REJECT_CODE_UNABLE_TO_PERFORM << 16) |
				     ((uint32_t)rjt_exp << 8);
		bls->ar = TRUE;
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
	bls->qosd        = TRUE;
	bls->cq_id       = UINT16_MAX;
	bls->cmd_type    = BCM_CMD_XMIT_BLS_RSP64_WQE;

	rc = bcm_post_wqe(hwqp, (uint8_t *)bls, TRUE, bcm_bls_cmpl_cb, ctx);
done:
	if (rc && ctx) {
		spdk_free(ctx);
	}

	if (rc && xri) {
		spdk_nvmf_fc_put_xri(hwqp, xri);
	}

	return rc;
}
