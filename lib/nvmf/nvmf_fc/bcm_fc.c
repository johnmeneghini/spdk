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
 * NVMe_FC transport functions.
 */

#include "spdk/env.h"
#include "spdk/assert.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk/likely.h"
#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk_internal/log.h"

#include "nvmf/nvmf_internal.h"
#include "nvmf/subsystem.h"
#include "nvmf/transport.h"

#include "bcm_fc.h"

/* externs */
extern int bcm_nvmf_fc_handle_rsp(struct spdk_nvmf_request *req);
extern int bcm_nvmf_fc_send_data(struct spdk_nvmf_fc_request *fc_req);
extern void bcm_nvmf_fc_free_req(struct spdk_nvmf_fc_request *fc_req, bool free_xri);
extern int bcm_nvmf_fc_xmt_bls_rsp(struct fc_hwqp *hwqp, uint16_t ox_id, uint16_t rx_id,
				   uint16_t rpi, bool rjt, uint8_t rjt_exp, bcm_fc_caller_cb cb, void *cb_args);

/* locals */
static void bcm_fc_queue_poller(void *arg);
static inline struct spdk_nvmf_fc_session *get_fc_session(struct spdk_nvmf_session *session);
static inline struct spdk_nvmf_fc_conn *get_fc_conn(struct spdk_nvmf_conn *conn);
static inline struct spdk_nvmf_fc_request *get_fc_req(struct spdk_nvmf_request *req);
static int spdk_nvmf_fc_fini(void);
static struct spdk_nvmf_session *spdk_nvmf_fc_session_init(void);
static void spdk_nvmf_fc_session_fini(struct spdk_nvmf_session *session);
static int spdk_nvmf_fc_request_complete(struct spdk_nvmf_request *req);
static void spdk_nvmf_fc_close_conn(struct spdk_nvmf_conn *conn);
static bool spdk_nvmf_fc_conn_is_idle(struct spdk_nvmf_conn *conn);

/* externs */
extern void bcm_process_queues(struct fc_hwqp *hwqp);
extern int bcm_init_rqpair_buffers(struct fc_hwqp *hwqp);
extern int bcm_create_fc_req_mempool(struct fc_hwqp *hwqp);

struct spdk_nvmf_fc_buf {
	SLIST_ENTRY(spdk_nvmf_fc_buf) link;
};

static TAILQ_HEAD(, spdk_nvmf_fc_port) g_spdk_nvmf_fc_port_list =
	TAILQ_HEAD_INITIALIZER(g_spdk_nvmf_fc_port_list);

/* List of FC connections that have not yet received a CONNECT capsule */
static TAILQ_HEAD(, spdk_nvmf_fc_conn)g_pending_conns = TAILQ_HEAD_INITIALIZER(g_pending_conns);

void
spdk_nvmf_fc_init_poller(struct spdk_nvmf_fc_port *fc_port, struct fc_hwqp *hwqp)
{
	hwqp->fc_port = fc_port;
	hwqp->queues.eq.q.posted_limit = 64;
	hwqp->queues.cq_wq.q.posted_limit = 64;
	hwqp->queues.cq_rq.q.posted_limit = 64;

	hwqp->queues.eq.auto_arm_flag = FALSE;
	hwqp->queues.cq_wq.auto_arm_flag = TRUE;
	hwqp->queues.cq_rq.auto_arm_flag = TRUE;

	hwqp->queues.eq.q.type = BCM_FC_QUEUE_TYPE_EQ;
	hwqp->queues.cq_wq.q.type = BCM_FC_QUEUE_TYPE_CQ_WQ;
	hwqp->queues.wq.q.type = BCM_FC_QUEUE_TYPE_WQ;
	hwqp->queues.cq_rq.q.type = BCM_FC_QUEUE_TYPE_CQ_RQ;
	hwqp->queues.rq_hdr.q.type = BCM_FC_QUEUE_TYPE_RQ_HDR;
	hwqp->queues.rq_payload.q.type = BCM_FC_QUEUE_TYPE_RQ_DATA;

	memset(&hwqp->counters, 0, sizeof(struct nvmf_fc_errors));  // clear counters

	bcm_init_rqpair_buffers(hwqp);
	(void)bcm_create_fc_req_mempool(hwqp);
}

void
spdk_nvmf_fc_add_poller(struct fc_hwqp *hwqp)
{
	assert(hwqp);

	SPDK_NOTICELOG("Starting Poller function on lcore_id: %d for port: %d, hwqp: %d\n",  hwqp->lcore_id,
		       hwqp->fc_port->port_hdl, hwqp->hwqp_id);

	spdk_poller_register(&hwqp->poller, bcm_fc_queue_poller,
			     (void *)hwqp, hwqp->lcore_id, HWQP_POLLER_TIMER_MSEC);
}


/*
 * Return a fc nport with a matching handle.
 */
struct spdk_nvmf_fc_nport *
spdk_nvmf_fc_nport_get(uint8_t port_hdl, uint32_t nport_hdl)
{
	struct spdk_nvmf_fc_port *fc_port = NULL;
	struct spdk_nvmf_fc_nport *fc_nport = NULL;

	fc_port = spdk_nvmf_fc_port_list_get(port_hdl);
	if (fc_port) {
		TAILQ_FOREACH(fc_nport, &fc_port->nport_list, link) {
			if (fc_nport->nport_hdl == nport_hdl) {
				return fc_nport;
			}
		}
	}
	return NULL;
}

void
spdk_nvmf_fc_delete_poller(struct fc_hwqp *hwqp)
{
	spdk_poller_unregister(&hwqp->poller, NULL);
}

static void
spdk_nvmf_fc_abts_handled_cb(void *cb_data, nvmf_fc_poller_api_ret_t ret)
{
	fc_abts_ctx_t *ctx = cb_data;

	if (ret != NVMF_FC_POLLER_API_OXID_NOT_FOUND) {
		ctx->handled = TRUE;
	}

	ctx->hwqps_responded ++;

	if (ctx->hwqps_responded < NVMF_FC_MAX_IO_QUEUES) {
		return; /* Wait for all pollers to complete. */
	}

	if (!ctx->handled) {
		/* Send Reject */
		bcm_nvmf_fc_xmt_bls_rsp(&ctx->nport->fc_port->ls_queue,
					ctx->oxid, ctx->rxid, ctx->rpi, TRUE,
					BCM_BLS_REJECT_EXP_INVALID_OXID, NULL, NULL);
	} else {
		/* Send Accept */
		bcm_nvmf_fc_xmt_bls_rsp(&ctx->nport->fc_port->ls_queue,
					ctx->oxid, ctx->rxid, ctx->rpi, FALSE,
					0, NULL, NULL);
	}

	spdk_free(ctx->free_args);
	spdk_free(ctx);
}

void
spdk_nvmf_fc_handle_abts_frame(struct spdk_nvmf_fc_nport *nport, uint16_t rpi,
			       uint16_t oxid, uint16_t rxid)
{
	fc_abts_ctx_t *ctx = NULL;
	struct nvmf_fc_poller_api_abts_recvd_args *args = NULL, *poller_arg;

	args = spdk_calloc(NVMF_FC_MAX_IO_QUEUES,
			   sizeof(struct nvmf_fc_poller_api_abts_recvd_args));
	if (!args) {
		goto bls_rej;
	}

	ctx = spdk_calloc(1, sizeof(fc_abts_ctx_t));
	if (!ctx) {
		goto bls_rej;
	}
	ctx->rpi  = rpi;
	ctx->oxid = oxid;
	ctx->rxid = rxid;
	ctx->nport = nport;
	ctx->free_args = args;

	/* Post ABTS to all HWQPs */
	for (int i = 0; i < NVMF_FC_MAX_IO_QUEUES; i ++) {
		poller_arg = args + i;
		poller_arg->hwqp = &nport->fc_port->io_queues[i];
		poller_arg->cb_info.cb_func = spdk_nvmf_fc_abts_handled_cb;
		poller_arg->cb_info.cb_data = ctx;
		poller_arg->ctx = ctx;

		nvmf_fc_poller_api(poller_arg->hwqp->lcore_id,
				   NVMF_FC_POLLER_API_ABTS_RECEIVED,
				   poller_arg);
	}

	return;
bls_rej:
	if (ctx) {
		spdk_free(ctx);
	}

	if (args) {
		spdk_free(args);
	}

	/* Send Reject */
	bcm_nvmf_fc_xmt_bls_rsp(&nport->fc_port->ls_queue, oxid, rxid, rpi,
				TRUE, BCM_BLS_REJECT_EXP_NOINFO, NULL, NULL);
	return;
}



/*
 * Helper function to return an XRI back. If XRI is not
 * available return NULL (so that the IO can be dropped)
 */
fc_xri_t *
spdk_nvmf_fc_get_xri(struct fc_hwqp *hwqp)
{
	fc_xri_t *xri;

	/* Get the physical port from the hwqp and dequeue an XRI from the
	 * corresponding ring. Send this back.
	 */
	if (0 != spdk_ring_dequeue(hwqp->fc_port->xri_ring, (void **)&xri)) {
		SPDK_ERRLOG("Not enough XRIs available in the ring. Sizing messed up\n");
		hwqp->counters.no_xri++;
		return 0;
	}

	return xri;
}

/*
 * Returns 0 if succesful
 */
int
spdk_nvmf_fc_put_xri(struct fc_hwqp *hwqp, fc_xri_t *xri)
{
	return spdk_ring_enqueue(hwqp->fc_port->xri_ring, xri);
}

void
bcm_fc_queue_poller(void *arg)
{
	struct fc_hwqp *hwqp = arg;

	if (hwqp->state == SPDK_FC_PORT_ONLINE) {
		bcm_process_queues(hwqp);
	}
}

static inline struct spdk_nvmf_fc_session *
get_fc_session(struct spdk_nvmf_session *session)
{
	return (struct spdk_nvmf_fc_session *)
	       ((uintptr_t)session - offsetof(struct spdk_nvmf_fc_session, session));
}

static inline struct spdk_nvmf_fc_conn *
get_fc_conn(struct spdk_nvmf_conn *conn)
{
	return (struct spdk_nvmf_fc_conn *)
	       ((uintptr_t)conn - offsetof(struct spdk_nvmf_fc_conn, conn));
}

static inline struct spdk_nvmf_fc_request *
get_fc_req(struct spdk_nvmf_request *req)
{
	return (struct spdk_nvmf_fc_request *)
	       ((uintptr_t)req - offsetof(struct spdk_nvmf_fc_request, req));
}

void
spdk_nvmf_fc_port_list_add(struct spdk_nvmf_fc_port *fc_port)
{
	TAILQ_INSERT_TAIL(&g_spdk_nvmf_fc_port_list, fc_port, link);
}

struct spdk_nvmf_fc_port *
spdk_nvmf_fc_port_list_get(uint8_t port_hdl)
{
	struct spdk_nvmf_fc_port *fc_port = NULL;

	TAILQ_FOREACH(fc_port, &g_spdk_nvmf_fc_port_list, link) {
		if (fc_port->port_hdl == port_hdl) {
			return fc_port;
		}
	}
	return NULL;
}

/* Public API callbacks begin here */

static int
spdk_nvmf_fc_init(uint16_t max_queue_depth, uint32_t max_io_size,
		  uint32_t in_capsule_data_size)
{
	SPDK_NOTICELOG("*** FC Transport Init ***\n");

	spdk_nvmf_fc_ls_init();

	return 0;
}

static int
spdk_nvmf_fc_fini(void)
{
	spdk_nvmf_fc_ls_fini();
	return 0;
}

static int
spdk_nvmf_fc_listen_addr_add(struct spdk_nvmf_listen_addr *listen_addr)
{
	/* do nothing - FC doens't have listener addrress */
	return 0;
}

static int
spdk_nvmf_fc_listen_addr_remove(struct spdk_nvmf_listen_addr *listen_addr)
{
	/* do nothing - FC doens't have listener addrress */
	return 0;
}

static void
spdk_nvmf_fc_discover(struct spdk_nvmf_listen_addr *listen_addr,
		      struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = SPDK_NVMF_TRTYPE_FC;
	entry->adrfam = SPDK_NVMF_ADRFAM_FC;
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_SPECIFIED;

	spdk_strcpy_pad(entry->trsvcid, listen_addr->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, listen_addr->traddr, sizeof(entry->traddr), ' ');

}

static struct spdk_nvmf_session *
spdk_nvmf_fc_session_init(void)
{
	struct spdk_nvmf_fc_session *fc_sess;

	fc_sess = spdk_calloc(1, sizeof(struct spdk_nvmf_fc_session));

	return (fc_sess ? &fc_sess->session : NULL);
}

static void
spdk_nvmf_fc_session_fini(struct spdk_nvmf_session *session)
{
	struct spdk_nvmf_fc_session *sess = get_fc_session(session);

	if (sess) {
		spdk_free(sess);
	}
}

static int
spdk_nvmf_fc_session_add_conn(struct spdk_nvmf_session *session,
			      struct spdk_nvmf_conn *conn)
{
	struct spdk_nvmf_fc_session *fc_sess = get_fc_session(session);
	struct spdk_nvmf_fc_conn *fc_conn = get_fc_conn(conn);

	if (fc_sess) {
		fc_sess->fc_assoc = fc_conn->fc_assoc;
	}
	return 0;
}

static int
spdk_nvmf_fc_session_remove_conn(struct spdk_nvmf_session *session,
				 struct spdk_nvmf_conn *conn)
{
	return 0;
}

static void
spdk_nvmf_fc_request_complete_process(void *arg1, void *arg2)
{
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)arg1;
	struct spdk_nvmf_fc_request *fc_req = get_fc_req(req);
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	int rc = 0;

	if (fc_req->is_aborted) {
		bcm_nvmf_fc_free_req(fc_req, !fc_req->xri_activated);
	} else if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
		   req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		rc = bcm_nvmf_fc_send_data(fc_req);
	} else {
		rc = bcm_nvmf_fc_handle_rsp(req);
	}

	if (rc) {
		SPDK_ERRLOG("Error in request complete.\n");
	}
}

static int
spdk_nvmf_fc_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fc_request *fc_req = get_fc_req(req);
	struct spdk_event *event = NULL;

	/* Check if we need to switch to correct lcore of HWQP. */
	if (spdk_env_get_current_core() != fc_req->poller_lcore) {
		/* Switch to correct HWQP lcore. */
		event = spdk_event_allocate(fc_req->poller_lcore,
					    spdk_nvmf_fc_request_complete_process,
					    (void *)req, NULL);
		spdk_event_call(event);
	} else {
		spdk_nvmf_fc_request_complete_process(req, NULL);
	}

	return 0;
}

static void
spdk_nvmf_fc_close_conn(struct spdk_nvmf_conn *conn)
{
	/* do nothing - handled in LS processor */
}

static int
spdk_nvmf_fc_conn_poll(struct spdk_nvmf_conn *conn)
{
	/* do nothing - handled by HWQP pollers */
	return 0;
}

static bool
spdk_nvmf_fc_conn_is_idle(struct spdk_nvmf_conn *conn)
{
	struct spdk_nvmf_fc_conn *nvmf_fc_conn = get_fc_conn(conn);

	if (nvmf_fc_conn->cur_queue_depth == 0 && nvmf_fc_conn->cur_fc_rw_depth == 0) {
		return true;
	}
	return false;
}

const struct spdk_nvmf_transport spdk_nvmf_transport_bcm_fc = {

	.name = NVMF_BCM_FC_TRANSPORT_NAME,
	.transport_init = spdk_nvmf_fc_init,
	.transport_fini = spdk_nvmf_fc_fini,
	.acceptor_poll = NULL,
	.listen_addr_add = spdk_nvmf_fc_listen_addr_add,
	.listen_addr_remove = spdk_nvmf_fc_listen_addr_remove,
	.listen_addr_discover = spdk_nvmf_fc_discover,
	.session_init = spdk_nvmf_fc_session_init,
	.session_fini = spdk_nvmf_fc_session_fini,
	.session_add_conn = spdk_nvmf_fc_session_add_conn,
	.session_remove_conn = spdk_nvmf_fc_session_remove_conn,
	.req_complete = spdk_nvmf_fc_request_complete,
	.conn_fini = spdk_nvmf_fc_close_conn,
	.conn_poll = spdk_nvmf_fc_conn_poll,
	.conn_is_idle = spdk_nvmf_fc_conn_is_idle,
};


SPDK_LOG_REGISTER_TRACE_FLAG("bcm_nvmf_fc", SPDK_TRACE_BCM_FC_NVME)
