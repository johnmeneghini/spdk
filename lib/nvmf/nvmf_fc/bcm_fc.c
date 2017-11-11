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
extern int spdk_nvmf_bcm_fc_handle_rsp(struct spdk_nvmf_bcm_fc_request *fc_req);
extern int spdk_nvmf_bcm_fc_send_data(struct spdk_nvmf_bcm_fc_request *fc_req);
extern void spdk_nvmf_bcm_fc_free_req(struct spdk_nvmf_bcm_fc_request *fc_req,
				      bool free_xri);
extern int spdk_nvmf_bcm_fc_xmt_bls_rsp(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
					uint16_t ox_id, uint16_t rx_id,
					uint16_t rpi, bool rjt, uint8_t rjt_exp,
					spdk_nvmf_bcm_fc_caller_cb cb,
					void *cb_args);
extern void spdk_nvmf_bcm_fc_req_set_state(struct spdk_nvmf_bcm_fc_request *fc_req,
		spdk_nvmf_bcm_fc_request_state_t state);
extern void spdk_nvmf_bcm_fc_req_abort_complete(void *arg1, void *arg2);

/* locals */
static void nvmf_fc_queue_poller(void *arg);
static inline struct spdk_nvmf_bcm_fc_session *nvmf_fc_get_fc_session(
	struct spdk_nvmf_session *session);
static inline struct spdk_nvmf_bcm_fc_conn *nvmf_fc_get_fc_conn(struct spdk_nvmf_conn *conn);
static int nvmf_fc_fini(void);
static struct spdk_nvmf_session *nvmf_fc_session_init(void);
static void nvmf_fc_session_fini(struct spdk_nvmf_session *session);
static int nvmf_fc_request_complete(struct spdk_nvmf_request *req);
static void nvmf_fc_close_conn(struct spdk_nvmf_conn *conn);
static bool nvmf_fc_conn_is_idle(struct spdk_nvmf_conn *conn);

/* externs */
extern void spdk_nvmf_bcm_fc_process_queues(struct spdk_nvmf_bcm_fc_hwqp *hwqp);
extern int spdk_nvmf_bcm_fc_init_rqpair_buffers(struct spdk_nvmf_bcm_fc_hwqp *hwqp);
extern int spdk_nvmf_bcm_fc_create_req_mempool(struct spdk_nvmf_bcm_fc_hwqp *hwqp);

struct spdk_nvmf_fc_buf {
	SLIST_ENTRY(spdk_nvmf_fc_buf) link;
};

static TAILQ_HEAD(, spdk_nvmf_bcm_fc_port) g_spdk_nvmf_bcm_fc_port_list =
	TAILQ_HEAD_INITIALIZER(g_spdk_nvmf_bcm_fc_port_list);

/* List of FC connections that have not yet received a CONNECT capsule */
static TAILQ_HEAD(, spdk_nvmf_bcm_fc_conn)g_pending_conns =
	TAILQ_HEAD_INITIALIZER(g_pending_conns);

void
spdk_nvmf_bcm_fc_init_poller_queues(struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	spdk_nvmf_bcm_fc_init_rqpair_buffers(hwqp);
}

void
spdk_nvmf_bcm_fc_init_poller(struct spdk_nvmf_bcm_fc_port *fc_port,
			     struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	hwqp->fc_port = fc_port;

	// clear counters
	memset(&hwqp->counters, 0, sizeof(struct spdk_nvmf_bcm_fc_errors));

	spdk_nvmf_bcm_fc_init_poller_queues(hwqp);
	(void)spdk_nvmf_bcm_fc_create_req_mempool(hwqp);
}

void
spdk_nvmf_bcm_fc_add_poller(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
			    uint64_t period_microseconds)
{
	assert(hwqp);

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC,
		      "Starting Poller function on lcore_id: %d for port: %d, hwqp: %d\n",
		      hwqp->lcore_id, hwqp->fc_port->port_hdl, hwqp->hwqp_id);

	spdk_poller_register(&hwqp->poller, nvmf_fc_queue_poller,
			     (void *)hwqp, hwqp->lcore_id,
			     period_microseconds);
}


/*
 * Return a fc nport with a matching handle.
 */
struct spdk_nvmf_bcm_fc_nport *
spdk_nvmf_bcm_fc_nport_get(uint8_t port_hdl, uint16_t nport_hdl)
{
	struct spdk_nvmf_bcm_fc_port *fc_port = NULL;
	struct spdk_nvmf_bcm_fc_nport *fc_nport = NULL;

	fc_port = spdk_nvmf_bcm_fc_port_list_get(port_hdl);
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
spdk_nvmf_bcm_fc_delete_poller(struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	spdk_poller_unregister(&hwqp->poller, NULL);
}

static void
nvmf_fc_abts_handled_cb(void *cb_data, spdk_nvmf_bcm_fc_poller_api_ret_t ret)
{
	fc_abts_ctx_t *ctx = cb_data;

	if (ret != SPDK_NVMF_BCM_FC_POLLER_API_OXID_NOT_FOUND) {
		ctx->handled = true;
	}

	ctx->hwqps_responded ++;

	if (ctx->hwqps_responded < ctx->num_hwqps) {
		return; /* Wait for all pollers to complete. */
	}

	if (!ctx->handled) {
		/* Send Reject */
		spdk_nvmf_bcm_fc_xmt_bls_rsp(&ctx->nport->fc_port->ls_queue,
					     ctx->oxid, ctx->rxid, ctx->rpi, true,
					     BCM_BLS_REJECT_EXP_INVALID_OXID, NULL, NULL);
	} else {
		/* Send Accept */
		spdk_nvmf_bcm_fc_xmt_bls_rsp(&ctx->nport->fc_port->ls_queue,
					     ctx->oxid, ctx->rxid, ctx->rpi, false,
					     0, NULL, NULL);
	}

	spdk_free(ctx->free_args);
	spdk_free(ctx);
}

void
spdk_nvmf_bcm_fc_handle_abts_frame(struct spdk_nvmf_bcm_fc_nport *nport, uint16_t rpi,
				   uint16_t oxid, uint16_t rxid)
{
	fc_abts_ctx_t *ctx = NULL;
	struct spdk_nvmf_bcm_fc_poller_api_abts_recvd_args *args = NULL, *poller_arg;
	struct spdk_nvmf_bcm_fc_association *assoc = NULL;
	struct spdk_nvmf_bcm_fc_conn *conn = NULL;
	uint64_t hwqp_map = 0;
	uint32_t hwqp_cnt = 0, j = 0;

	assert(NVMF_FC_MAX_IO_QUEUES <= 64);

	TAILQ_FOREACH(assoc, &nport->fc_associations, link) {
		TAILQ_FOREACH(conn, &assoc->fc_conns, assoc_link) {
			if (conn->rpi != rpi) {
				continue;
			}
			if (!((hwqp_map >> conn->hwqp->hwqp_id) & 0x1)) {
				hwqp_map |= (0x1 << conn->hwqp->hwqp_id);
				hwqp_cnt ++;
			}
		}
	}

	if (!hwqp_cnt) {
		goto bls_rej;
	}

	args = spdk_calloc(hwqp_cnt,
			   sizeof(struct spdk_nvmf_bcm_fc_poller_api_abts_recvd_args));
	if (!args) {
		goto bls_rej;
	}

	ctx = spdk_calloc(1, sizeof(fc_abts_ctx_t));
	if (!ctx) {
		goto bls_rej;
	}
	ctx->rpi	= rpi;
	ctx->oxid	= oxid;
	ctx->rxid	= rxid;
	ctx->nport	= nport;
	ctx->free_args	= args;
	ctx->num_hwqps	= hwqp_cnt;

	for (int i = 0; i < NVMF_FC_MAX_IO_QUEUES; i ++) {
		if (!((hwqp_map >> i) & 0x1)) {
			continue;
		}
		poller_arg = args + j;
		poller_arg->hwqp = &nport->fc_port->io_queues[i];
		poller_arg->cb_info.cb_func = nvmf_fc_abts_handled_cb;
		poller_arg->cb_info.cb_data = ctx;
		poller_arg->ctx = ctx;

		spdk_nvmf_bcm_fc_poller_api(poller_arg->hwqp->lcore_id,
					    SPDK_NVMF_BCM_FC_POLLER_API_ABTS_RECEIVED,
					    poller_arg);
		j ++;
	}

	return;
bls_rej:
	if (args) {
		spdk_free(args);
	}

	/* Send Reject */
	spdk_nvmf_bcm_fc_xmt_bls_rsp(&nport->fc_port->ls_queue, oxid, rxid, rpi,
				     true, BCM_BLS_REJECT_EXP_NOINFO, NULL, NULL);
	return;
}



/*
 * Helper function to return an XRI back. If XRI is not
 * available return NULL (so that the IO can be dropped)
 */
struct spdk_nvmf_bcm_fc_xri *
spdk_nvmf_bcm_fc_get_xri(struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	struct spdk_nvmf_bcm_fc_xri *xri;

	/* Get the physical port from the hwqp and dequeue an XRI from the
	 * corresponding ring. Send this back.
	 */
	if (0 != spdk_ring_dequeue(hwqp->fc_port->xri_ring, (void **)&xri)) {
		SPDK_ERRLOG("Not enough XRIs available in the ring. Sizing messed up\n");
		hwqp->counters.no_xri++;
		return NULL;
	}

	return xri;
}

/*
 * Returns 0 if succesful
 */
int
spdk_nvmf_bcm_fc_put_xri(struct spdk_nvmf_bcm_fc_hwqp *hwqp, struct spdk_nvmf_bcm_fc_xri *xri)
{
	return spdk_ring_enqueue(hwqp->fc_port->xri_ring, xri);
}

static void
nvmf_fc_queue_poller(void *arg)
{
	struct spdk_nvmf_bcm_fc_hwqp *hwqp = arg;

	if (hwqp->state == SPDK_FC_HWQP_ONLINE) {
		spdk_nvmf_bcm_fc_process_queues(hwqp);
	}
}

/*** Accessor functions for the bcm-fc structures - BEGIN */
/*
 * Returns true if the port is in offline state.
 */
bool
spdk_nvmf_bcm_fc_port_is_offline(struct spdk_nvmf_bcm_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status == SPDK_FC_PORT_OFFLINE)) {
		return true;
	} else {
		return false;
	}
}

spdk_err_t
spdk_nvmf_bcm_fc_port_set_online(struct spdk_nvmf_bcm_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status != SPDK_FC_PORT_ONLINE)) {
		fc_port->hw_port_status = SPDK_FC_PORT_ONLINE;
		return SPDK_SUCCESS;
	} else {
		return SPDK_ERR_INTERNAL;
	}
}

spdk_err_t
spdk_nvmf_bcm_fc_port_set_offline(struct spdk_nvmf_bcm_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status != SPDK_FC_PORT_OFFLINE)) {
		fc_port->hw_port_status = SPDK_FC_PORT_OFFLINE;
		return SPDK_SUCCESS;
	} else {
		return SPDK_ERR_INTERNAL;
	}
}

spdk_err_t
spdk_nvmf_bcm_fc_port_add_nport(struct spdk_nvmf_bcm_fc_port *fc_port,
				struct spdk_nvmf_bcm_fc_nport *nport)
{
	if (fc_port) {
		TAILQ_INSERT_TAIL(&fc_port->nport_list, nport, link);
		fc_port->num_nports++;
		return SPDK_SUCCESS;
	} else {
		return SPDK_ERR_INTERNAL;
	}
}

spdk_err_t
spdk_nvmf_bcm_fc_port_remove_nport(struct spdk_nvmf_bcm_fc_port *fc_port,
				   struct spdk_nvmf_bcm_fc_nport *nport)
{
	if (fc_port && nport) {
		TAILQ_REMOVE(&fc_port->nport_list, nport, link);
		fc_port->num_nports--;
		return SPDK_SUCCESS;
	} else {
		return SPDK_ERR_INTERNAL;
	}
}

spdk_err_t
spdk_nvmf_bcm_fc_hwqp_port_set_online(struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	if (hwqp && (hwqp->state != SPDK_FC_HWQP_ONLINE)) {
		hwqp->state = SPDK_FC_HWQP_ONLINE;
		/* reset some queue counters */
		hwqp->free_q_slots = hwqp->queues.rq_payload.num_buffers;
		hwqp->num_conns = 0;
		return SPDK_SUCCESS;
	} else {
		return SPDK_ERR_INTERNAL;
	}
}

spdk_err_t
spdk_nvmf_bcm_fc_hwqp_port_set_offline(struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	if (hwqp && (hwqp->state != SPDK_FC_HWQP_OFFLINE)) {
		hwqp->state = SPDK_FC_HWQP_OFFLINE;
		return SPDK_SUCCESS;
	} else {
		return SPDK_ERR_INTERNAL;
	}
}

uint32_t
spdk_nvmf_bcm_fc_nport_get_association_count(struct spdk_nvmf_bcm_fc_nport *nport)
{
	return nport->assoc_count;
}

/* Returns true if the Nport is empty of all associations */
bool
spdk_nvmf_bcm_fc_nport_is_association_empty(struct spdk_nvmf_bcm_fc_nport *nport)
{
	if (nport && (TAILQ_EMPTY(&nport->fc_associations) ||
		      (0 == nport->assoc_count))) {
		return true;
	} else {
		return false;
	}
}

/* Returns true if the Nport is empty of all rem_ports */
bool
spdk_nvmf_bcm_fc_nport_is_rport_empty(struct spdk_nvmf_bcm_fc_nport *nport)
{
	if (nport && TAILQ_EMPTY(&nport->rem_port_list)) {
		assert(nport->rport_count == 0);
		return true;
	} else {
		return false;
	}
}

spdk_err_t
spdk_nvmf_bcm_fc_nport_set_state(struct spdk_nvmf_bcm_fc_nport *nport,
				 spdk_nvmf_bcm_fc_object_state_t state)
{
	if (nport) {
		nport->nport_state = state;
		return SPDK_SUCCESS;
	} else {
		return SPDK_ERR_INTERNAL;
	}
}

spdk_err_t
spdk_nvmf_bcm_fc_assoc_set_state(struct spdk_nvmf_bcm_fc_association *assoc,
				 spdk_nvmf_bcm_fc_object_state_t state)
{
	if (assoc) {
		assoc->assoc_state = state;
		return SPDK_SUCCESS;
	} else {
		return SPDK_ERR_INTERNAL;
	}
}

bool
spdk_nvmf_bcm_fc_nport_add_rem_port(struct spdk_nvmf_bcm_fc_nport *nport,
				    struct spdk_nvmf_bcm_fc_remote_port_info *rem_port)
{
	if (nport && rem_port) {
		TAILQ_INSERT_TAIL(&nport->rem_port_list, rem_port, link);
		nport->rport_count++;
		return SPDK_SUCCESS;
	} else {
		return SPDK_ERR_INTERNAL;
	}
}

bool
spdk_nvmf_bcm_fc_nport_remove_rem_port(struct spdk_nvmf_bcm_fc_nport *nport,
				       struct spdk_nvmf_bcm_fc_remote_port_info *rem_port)
{
	if (nport && rem_port) {
		TAILQ_REMOVE(&nport->rem_port_list, rem_port, link);
		nport->rport_count--;
		return SPDK_SUCCESS;
	} else {
		return SPDK_ERR_INTERNAL;
	}
}

uint32_t
spdk_nvmf_bcm_fc_get_prli_service_params(void)
{
	return (SPDK_NVMF_FC_DISCOVERY_SERVICE | SPDK_NVMF_FC_TARGET_FUNCTION);
}

spdk_err_t
spdk_nvmf_bcm_fc_rport_set_state(struct spdk_nvmf_bcm_fc_remote_port_info *rport,
				 spdk_nvmf_bcm_fc_object_state_t state)
{
	if (rport) {
		rport->rport_state = state;
		return SPDK_SUCCESS;
	} else {
		return SPDK_ERR_INTERNAL;
	}
}

/*** Accessor functions for the bcm-fc structures - END */


static inline struct spdk_nvmf_bcm_fc_session *
nvmf_fc_get_fc_session(struct spdk_nvmf_session *session)
{
	return (struct spdk_nvmf_bcm_fc_session *)
	       ((uintptr_t)session - offsetof(struct spdk_nvmf_bcm_fc_session, session));
}

static inline struct spdk_nvmf_bcm_fc_conn *
nvmf_fc_get_fc_conn(struct spdk_nvmf_conn *conn)
{
	return (struct spdk_nvmf_bcm_fc_conn *)
	       ((uintptr_t)conn - offsetof(struct spdk_nvmf_bcm_fc_conn, conn));
}

static inline struct spdk_nvmf_bcm_fc_request *
nvmf_fc_get_fc_req(struct spdk_nvmf_request *req)
{
	return (struct spdk_nvmf_bcm_fc_request *)
	       ((uintptr_t)req - offsetof(struct spdk_nvmf_bcm_fc_request, req));
}

struct spdk_nvmf_bcm_fc_nport *
spdk_nvmf_bcm_req_fc_nport_get(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_bcm_fc_request *fc_req = nvmf_fc_get_fc_req(req);
	return fc_req->fc_conn->fc_assoc->tgtport;
}

void
spdk_nvmf_bcm_fc_port_list_add(struct spdk_nvmf_bcm_fc_port *fc_port)
{
	TAILQ_INSERT_TAIL(&g_spdk_nvmf_bcm_fc_port_list, fc_port, link);
}

struct spdk_nvmf_bcm_fc_port *
spdk_nvmf_bcm_fc_port_list_get(uint8_t port_hdl)
{
	struct spdk_nvmf_bcm_fc_port *fc_port = NULL;

	TAILQ_FOREACH(fc_port, &g_spdk_nvmf_bcm_fc_port_list, link) {
		if (fc_port->port_hdl == port_hdl) {
			return fc_port;
		}
	}
	return NULL;
}

/*
 * Function to calculate maximum queue depth (reported in mqes)
 */
uint32_t
spdk_nvmf_bcm_fc_calc_max_q_depth(uint32_t nRQ, uint32_t RQsz,
				  uint32_t mA, uint32_t mAC,
				  uint32_t AQsz)
{
	/* adjusted max. AQ's is rounded up to neareset mult. of nRQ */
	uint32_t adj_mA = mA % nRQ == 0 ? mA : ((mA / nRQ) * nRQ) + nRQ;
	uint32_t mAQpRQ = adj_mA / nRQ; /* maximum AQ's per RQ */
	uint32_t mIOQ = (mAC - 1) * mA; /* maximum IOQ's */
	return ((RQsz - (mAQpRQ * AQsz)) / ((mIOQ / nRQ) +
					    (mIOQ % nRQ == 0 ? 0 : 1)));
}

uint32_t
spdk_nvmf_bcm_fc_get_num_assocs_in_subsystem(uint8_t port_hdl, uint16_t nport_hdl,
		struct spdk_nvmf_subsystem *subsys)
{
	struct spdk_nvmf_bcm_fc_association *assoc = NULL;
	uint32_t num_assocs = 0;
	struct spdk_nvmf_bcm_fc_nport *fc_nport = NULL;

	if (!subsys) {
		return 0;
	}

	fc_nport = spdk_nvmf_bcm_fc_nport_get(port_hdl, nport_hdl);
	if (!fc_nport) {
		return 0;
	}


	TAILQ_FOREACH(assoc, &fc_nport->fc_associations, link) {
		if (assoc->subsystem == subsys) {
			++num_assocs;
		}
	}

	return num_assocs;
}

bool
spdk_nvmf_bcm_fc_is_spdk_session_on_nport(uint8_t port_hdl, uint16_t nport_hdl,
		struct spdk_nvmf_session *session)
{
	struct spdk_nvmf_bcm_fc_nport *fc_nport = NULL;

	if (!session) {
		return false;
	}

	fc_nport = spdk_nvmf_bcm_fc_nport_get(port_hdl, nport_hdl);
	if (!fc_nport) {
		return false;
	}

	struct spdk_nvmf_bcm_fc_session *fc_session = nvmf_fc_get_fc_session(session);
	if (fc_session) {
		if (fc_session->fc_assoc) {
			if (fc_session->fc_assoc->tgtport == fc_nport) {
				return true;
			}
		}
	}
	return false;
}

spdk_err_t
spdk_nvmf_bcm_fc_find_rport_from_sid(uint32_t s_id,
				     struct spdk_nvmf_bcm_fc_nport *tgtport,
				     struct spdk_nvmf_bcm_fc_remote_port_info **rport)
{
	int rc = SPDK_ERR_INTERNAL;
	struct spdk_nvmf_bcm_fc_remote_port_info *rem_port = NULL;

	TAILQ_FOREACH(rem_port, &tgtport->rem_port_list, link) {
		if (rem_port->s_id == s_id) {
			*rport = rem_port;
			rc = SPDK_SUCCESS;
			break;
		}
	}
	return rc;
}


/* Transport API callbacks begin here */

static int
nvmf_fc_init(uint16_t max_queue_depth, uint32_t max_io_size,
	     uint32_t in_capsule_data_size)
{
	return 0;
}

static int
nvmf_fc_fini(void)
{
	struct spdk_nvmf_bcm_fc_port *fc_port = NULL;

	TAILQ_FOREACH(fc_port, &g_spdk_nvmf_bcm_fc_port_list, link) {
		spdk_nvmf_bcm_fc_ls_fini(fc_port);
	}

	return 0;
}

static int
nvmf_fc_listen_addr_add(struct spdk_nvmf_listen_addr *listen_addr)
{
	/* do nothing - FC doens't have listener addrress */
	return 0;
}

static int
nvmf_fc_listen_addr_remove(struct spdk_nvmf_listen_addr *listen_addr)
{
	/* do nothing - FC doens't have listener addrress */
	return 0;
}

static void
nvmf_fc_discover(struct spdk_nvmf_listen_addr *listen_addr,
		 struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = SPDK_NVMF_TRTYPE_FC;
	entry->adrfam = SPDK_NVMF_ADRFAM_FC;
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_SPECIFIED;

	spdk_strcpy_pad(entry->trsvcid, listen_addr->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, listen_addr->traddr, sizeof(entry->traddr), ' ');

}

static struct spdk_nvmf_session *
nvmf_fc_session_init(void)
{
	struct spdk_nvmf_bcm_fc_session *fc_sess;
	struct spdk_nvmf_session *nvmf_fc_sess = NULL;

	fc_sess = spdk_calloc(1, sizeof(struct spdk_nvmf_bcm_fc_session));

	if (fc_sess) {
		nvmf_fc_sess = &fc_sess->session;
		nvmf_fc_sess->transport = &spdk_nvmf_transport_bcm_fc;
	}

	return nvmf_fc_sess;
}

static void
nvmf_fc_session_fini(struct spdk_nvmf_session *session)
{
	struct spdk_nvmf_bcm_fc_session *sess = nvmf_fc_get_fc_session(session);

	if (sess) {
		spdk_free(sess);
	}
}

static int
nvmf_fc_session_add_conn(struct spdk_nvmf_session *session,
			 struct spdk_nvmf_conn *conn)
{
	struct spdk_nvmf_bcm_fc_session *fc_sess = nvmf_fc_get_fc_session(session);
	struct spdk_nvmf_bcm_fc_conn *fc_conn = nvmf_fc_get_fc_conn(conn);

	if (fc_sess) {
		fc_sess->fc_assoc = fc_conn->fc_assoc;
	}
	return 0;
}

static int
nvmf_fc_session_remove_conn(struct spdk_nvmf_session *session,
			    struct spdk_nvmf_conn *conn)
{
	return 0;
}

static void
nvmf_fc_request_complete_process(void *arg1, void *arg2)
{
	int rc = 0;
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)arg1;
	struct spdk_nvmf_bcm_fc_request *fc_req = nvmf_fc_get_fc_req(req);
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_event *event = NULL;

	if (fc_req->is_aborted) {
		/* Defer this to make sure we dont call io cleanup in same context. */
		event = spdk_event_allocate(fc_req->poller_lcore,
					    spdk_nvmf_bcm_fc_req_abort_complete,
					    (void *)fc_req, NULL);
		spdk_event_call(event);
	} else if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
		   req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {

		spdk_nvmf_bcm_fc_req_set_state(fc_req, SPDK_NVMF_BCM_FC_REQ_READ_XFER);

		rc = spdk_nvmf_bcm_fc_send_data(fc_req);
	} else {
		if (req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
			spdk_nvmf_bcm_fc_req_set_state(fc_req, SPDK_NVMF_BCM_FC_REQ_WRITE_RSP);
		} else {
			spdk_nvmf_bcm_fc_req_set_state(fc_req, SPDK_NVMF_BCM_FC_REQ_NONE_RSP);
		}

		rc = spdk_nvmf_bcm_fc_handle_rsp(fc_req);
	}

	if (rc) {
		SPDK_ERRLOG("Error in request complete.\n");
		spdk_nvmf_bcm_fc_free_req(fc_req, false);
	}
}

static int
nvmf_fc_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_bcm_fc_request *fc_req = nvmf_fc_get_fc_req(req);
	struct spdk_event *event = NULL;

	/* Check if we need to switch to correct lcore of HWQP. */
#ifndef NETAPP
	/* Right now spdk_env_get_current_core returns an incorrect
	 * value (0) when called from non-EAL threads. This results in
	 * an issue where the WAFL threads could potentially write or
	 * clobber the h/w queues (or some other data structures.
	 * TODO: Uncomment the below line when we solve the
	 * spdk_env_get_current_core issue.
	 */
	if (spdk_env_get_current_core() != fc_req->poller_lcore) {
#endif
		/* Switch to correct HWQP lcore. */
		event = spdk_event_allocate(fc_req->poller_lcore,
					    nvmf_fc_request_complete_process,
					    (void *)req, NULL);
		spdk_event_call(event);
#ifndef NETAPP
	} else {
		nvmf_fc_request_complete_process(req, NULL);
	}
#endif
	return 0;
}

static void
nvmf_fc_close_conn(struct spdk_nvmf_conn *conn)
{
	/* do nothing - handled in LS processor */
}

static int
nvmf_fc_conn_poll(struct spdk_nvmf_conn *conn)
{
	/* do nothing - handled by HWQP pollers */
	return 0;
}

static bool
nvmf_fc_conn_is_idle(struct spdk_nvmf_conn *conn)
{
	struct spdk_nvmf_bcm_fc_conn *nvmf_fc_conn = nvmf_fc_get_fc_conn(conn);

	if (nvmf_fc_conn->cur_queue_depth == 0 && nvmf_fc_conn->cur_fc_rw_depth == 0) {
		return true;
	}
	return false;
}

const struct spdk_nvmf_transport spdk_nvmf_transport_bcm_fc = {

	.name = NVMF_BCM_FC_TRANSPORT_NAME,
	.transport_init = nvmf_fc_init,
	.transport_fini = nvmf_fc_fini,
	.acceptor_poll = NULL,
	.listen_addr_add = nvmf_fc_listen_addr_add,
	.listen_addr_remove = nvmf_fc_listen_addr_remove,
	.listen_addr_discover = nvmf_fc_discover,
	.session_init = nvmf_fc_session_init,
	.session_fini = nvmf_fc_session_fini,
	.session_add_conn = nvmf_fc_session_add_conn,
	.session_remove_conn = nvmf_fc_session_remove_conn,
	.req_complete = nvmf_fc_request_complete,
	.conn_fini = nvmf_fc_close_conn,
	.conn_poll = nvmf_fc_conn_poll,
	.conn_is_idle = nvmf_fc_conn_is_idle,
};


SPDK_LOG_REGISTER_TRACE_FLAG("nvmf_bcm_fc", SPDK_TRACE_NVMF_BCM_FC)
