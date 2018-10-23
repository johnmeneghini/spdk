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

#include "nvmf/nvmf_internal.h"
#include "nvmf/request.h"
#include "nvmf/session.h"
#include "nvmf/transport.h"
#include "nvmf/subsystem.h"

#include "spdk/env.h"
#include "spdk/assert.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/endian.h"
#include "spdk/error.h"
#include "spdk_internal/log.h"
#include "spdk/stdinc.h"

#include "bcm_fc.h"

/* The connection ID (8 bytes)has the following format:
 * byte0: queue number (0-255) - currently there is a max of 16 queues
 * byte1 - byte2: unique value per queue number (0-65535)
 * byte3 - byte7: unused */
#define SPDK_NVMF_FC_BCM_MRQ_CONNID_UV_SHIFT    8

/* Validation Error indexes into the string table below */
enum {
	VERR_NO_ERROR = 0,
	VERR_CR_ASSOC_LEN = 1,
	VERR_CR_ASSOC_RQST_LEN = 2,
	VERR_CR_ASSOC_CMD = 3,
	VERR_CR_ASSOC_CMD_LEN = 4,
	VERR_ERSP_RATIO = 5,
	VERR_ASSOC_ALLOC_FAIL = 6,
	VERR_CONN_ALLOC_FAIL = 7,
	VERR_CR_CONN_LEN = 8,
	VERR_CR_CONN_RQST_LEN = 9,
	VERR_ASSOC_ID = 10,
	VERR_ASSOC_ID_LEN = 11,
	VERR_NO_ASSOC = 12,
	VERR_CONN_ID = 13,
	VERR_CONN_ID_LEN = 14,
	VERR_NO_CONN = 15,
	VERR_CR_CONN_CMD = 16,
	VERR_CR_CONN_CMD_LEN = 17,
	VERR_DISCONN_LEN = 18,
	VERR_DISCONN_RQST_LEN = 19,
	VERR_DISCONN_CMD = 20,
	VERR_DISCONN_CMD_LEN = 21,
	VERR_DISCONN_SCOPE = 22,
	VERR_RS_LEN = 23,
	VERR_RS_RQST_LEN = 24,
	VERR_RS_CMD = 25,
	VERR_RS_CMD_LEN = 26,
	VERR_RS_RCTL = 27,
	VERR_RS_RO = 28,
	VERR_CONN_TOO_MANY = 29,
	VERR_SUBNQN = 30,
	VERR_HOSTNQN = 31,
	VERR_SQSIZE = 32,
	VERR_NO_RPORT = 33,
	VERR_SUBLISTENER = 34,
};

static char *validation_errors[] = {
	"OK",
	"Bad CR_ASSOC Length",
	"Bad CR_ASSOC Rqst Length",
	"Not CR_ASSOC Cmd",
	"Bad CR_ASSOC Cmd Length",
	"Bad Ersp Ratio",
	"Association Allocation Failed",
	"Queue Allocation Failed",
	"Bad CR_CONN Length",
	"Bad CR_CONN Rqst Length",
	"Not Association ID",
	"Bad Association ID Length",
	"No Association",
	"Not Connection ID",
	"Bad Connection ID Length",
	"No Connection",
	"Not CR_CONN Cmd",
	"Bad CR_CONN Cmd Length",
	"Bad DISCONN Length",
	"Bad DISCONN Rqst Length",
	"Not DISCONN Cmd",
	"Bad DISCONN Cmd Length",
	"Bad Disconnect Scope",
	"Bad RS Length",
	"Bad RS Rqst Length",
	"Not RS Cmd",
	"Bad RS Cmd Length",
	"Bad RS R_CTL",
	"Bad RS Relative Offset",
	"Too many connections for association",
	"Invalid subnqn or subsystem not found",
	"Invalid hostnqn or subsystem doesn't allow host",
	"SQ size = 0 or too big",
	"No Remote Port",
	"Bad Subsystem Port",
};

extern void spdk_post_event(void *context, struct spdk_event *event);

/* Poller API structures (arguments and callback data */

struct nvmf_fc_ls_add_conn_api_data {
	struct spdk_nvmf_bcm_fc_poller_api_add_connection_args args;
	struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst;
	struct spdk_nvmf_bcm_fc_association *assoc;
	bool assoc_conn; /* true if adding connection for new association */
};

/* Disconnect (connection) request functions */
struct nvmf_fc_ls_del_conn_api_data {
	struct spdk_nvmf_bcm_fc_poller_api_del_connection_args args;
	struct spdk_nvmf_bcm_fc_association *assoc;
	struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst;
	bool assoc_conn; /* true if deleting AQ connection */
};

/* used by LS disconnect association cmd handling */
struct nvmf_fc_ls_disconn_assoc_api_data {
	struct spdk_nvmf_bcm_fc_nport *tgtport;
	struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst;
};

/* used by delete association call */
struct nvmf_fc_delete_assoc_api_data {
	struct spdk_nvmf_bcm_fc_poller_api_del_connection_args args;
	struct spdk_nvmf_bcm_fc_association *assoc;
	bool from_ls_rqst;   /* true = request came for LS */
	spdk_nvmf_fc_del_assoc_cb del_assoc_cb;
	void *del_assoc_cb_data;
};

union nvmf_fc_ls_op_ctx {
	struct nvmf_fc_ls_add_conn_api_data add_conn;
	struct nvmf_fc_ls_del_conn_api_data del_conn;
	struct nvmf_fc_ls_disconn_assoc_api_data disconn_assoc;
	struct nvmf_fc_delete_assoc_api_data del_assoc;
	union nvmf_fc_ls_op_ctx *next_op_ctx;
};

#define be32_to_cpu(i) from_be32((i))
#define be16_to_cpu(i) from_be16((i))
#define be64_to_cpu(i) from_be64((i))

static inline __be32 cpu_to_be32(uint32_t in)
{
	uint32_t t;
	to_be32(&t, in);
	return (__be32)t;
}

static inline __be32 nvmf_fc_lsdesc_len(size_t sz)
{
	uint32_t t;
	to_be32(&t, sz - (2 * sizeof(uint32_t)));
	return (__be32)t;
}

static struct spdk_nvmf_subsystem *
nvmf_fc_ls_valid_subnqn(uint8_t *subnqn)
{
	if (!spdk_nvmf_valid_nqn((const char *) subnqn)) {
		return NULL;
	}
	return spdk_nvmf_find_subsystem((const char *)subnqn);
}

static inline int
nvmf_fc_xmt_ls_rsp(struct spdk_nvmf_bcm_fc_nport *tgtport,
		   struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst)
{
	return spdk_nvmf_bcm_fc_xmt_ls_rsp(tgtport, ls_rqst);
}

static void
nvmf_fc_ls_format_rsp_hdr(void *buf, uint8_t ls_cmd, uint32_t desc_len,
			  uint8_t rqst_ls_cmd)
{
	struct nvmf_fc_ls_acc_hdr *acc_hdr = buf;

	acc_hdr->w0.ls_cmd = ls_cmd;
	acc_hdr->desc_list_len = desc_len;
	to_be32(&acc_hdr->rqst.desc_tag, FCNVME_LSDESC_RQST);
	acc_hdr->rqst.desc_len =
		nvmf_fc_lsdesc_len(sizeof(struct nvmf_fc_lsdesc_rqst));
	acc_hdr->rqst.w0.ls_cmd = rqst_ls_cmd;
}

static int
nvmf_fc_ls_format_rjt(void *buf, uint16_t buflen, uint8_t ls_cmd,
		      uint8_t reason, uint8_t explanation, uint8_t vendor)
{
	struct nvmf_fc_ls_rjt *rjt = buf;

	bzero(buf, sizeof(struct nvmf_fc_ls_rjt));
	nvmf_fc_ls_format_rsp_hdr(buf, FCNVME_LSDESC_RQST,
				  nvmf_fc_lsdesc_len(sizeof(struct nvmf_fc_ls_rjt)),
				  ls_cmd);
	to_be32(&rjt->rjt.desc_tag, FCNVME_LSDESC_RJT);
	rjt->rjt.desc_len = nvmf_fc_lsdesc_len(sizeof(struct nvmf_fc_lsdesc_rjt));
	rjt->rjt.reason_code = reason;
	rjt->rjt.reason_explanation = explanation;
	rjt->rjt.vendor = vendor;

	return sizeof(struct nvmf_fc_ls_rjt);
}

/* ************************************************** */
/* Allocators/Deallocators (assocations, connections, */
/* poller API data)                                   */

static void
nvmf_fc_association_init(struct spdk_nvmf_bcm_fc_association *assoc)
{
	assoc->assoc_state = SPDK_NVMF_BCM_FC_OBJECT_ZOMBIE;

#ifdef NVMF_FC_LS_SEND_LS_DISCONNECT
	/* allocate rqst/resp buffers to send LS disconnect to initiator */
	assoc->snd_disconn_bufs.rqst.virt = spdk_dma_malloc(
			sizeof(struct nvmf_fc_ls_disconnect_rqst)  +
			sizeof(struct nvmf_fc_ls_rjt) +
			(2 * sizeof(bcm_sge_t)), 0, NULL);

	if (assoc->snd_disconn_bufs.rqst.virt == NULL) {
		SPDK_ERRLOG("***ERROR*** - no memory for send LS disconnect buffers");
		assert("***ERROR*** - no memory for send LS disconnect buffers" == 1);
	}

	assoc->snd_disconn_bufs.rqst.phys = spdk_vtophys(
			assoc->snd_disconn_bufs.rqst.virt);
	assoc->snd_disconn_bufs.rqst.len =
		sizeof(struct nvmf_fc_ls_disconnect_rqst);

	assoc->snd_disconn_bufs.rsp.virt = assoc->snd_disconn_bufs.rqst.virt +
					   sizeof(struct nvmf_fc_ls_disconnect_rqst);
	assoc->snd_disconn_bufs.rsp.phys = assoc->snd_disconn_bufs.rqst.phys +
					   sizeof(struct nvmf_fc_ls_disconnect_rqst);
	assoc->snd_disconn_bufs.rsp.len =
		sizeof(struct nvmf_fc_ls_rjt);

	assoc->snd_disconn_bufs.sgl.virt = assoc->snd_disconn_bufs.rsp.virt +
					   sizeof(struct nvmf_fc_ls_rjt);
	assoc->snd_disconn_bufs.sgl.phys = assoc->snd_disconn_bufs.rsp.phys +
					   sizeof(struct nvmf_fc_ls_rjt);
	assoc->snd_disconn_bufs.sgl.len = 2 * sizeof(bcm_sge_t);
#endif
}

static void
nvmf_fc_association_fini(struct spdk_nvmf_bcm_fc_association *assoc)
{
#ifdef NVMF_FC_LS_SEND_LS_DISCONNECT
	if (assoc->snd_disconn_bufs.rqst.virt) {
		spdk_dma_free(assoc->snd_disconn_bufs.rqst.virt);
		assoc->snd_disconn_bufs.rqst.virt = NULL;
	}
#endif
}

static struct spdk_nvmf_bcm_fc_association *
nvmf_fc_ls_new_association(uint32_t s_id,
			   struct spdk_nvmf_bcm_fc_nport *tgtport,
			   struct spdk_nvmf_bcm_fc_remote_port_info *rport,
			   struct nvmf_fc_lsdesc_cr_assoc_cmd *a_cmd,
			   struct spdk_nvmf_subsystem *subsys,
			   struct spdk_nvmf_host *host, uint16_t rpi)
{
	struct spdk_nvmf_bcm_fc_association *assoc;

	SPDK_NOTICELOG("New Association request for port %d nport %d rpi 0x%x\n",
		       tgtport->fc_port->port_hdl, tgtport->nport_hdl, rpi);

	assert(rport);
	if (!rport) {
		SPDK_ERRLOG("rport is null.\n");
		return NULL;
	}

	assoc = TAILQ_FIRST(&tgtport->fc_port->ls_rsrc_pool.assoc_free_list);

	if (assoc) {
		/* remove from port's free association list */
		TAILQ_REMOVE(&tgtport->fc_port->ls_rsrc_pool.assoc_free_list,
			     assoc, port_free_assoc_list_link);

		/* initialize association
		 * IMPORTANT: do not do a memset/bzero association!
		 * Some of the association fields are static */
		assoc->assoc_id = 0;
		assoc->s_id = s_id;
		assoc->tgtport = tgtport;
		assoc->rport = rport;
		assoc->subsystem = subsys;
		assoc->assoc_state = SPDK_NVMF_BCM_FC_OBJECT_CREATED;
		memcpy(assoc->host_id, a_cmd->hostid, FCNVME_ASSOC_HOSTID_LEN);
		memcpy(assoc->host_nqn, a_cmd->hostnqn, FCNVME_ASSOC_HOSTNQN_LEN);
		memcpy(assoc->sub_nqn, a_cmd->subnqn, FCNVME_ASSOC_HOSTNQN_LEN);
		assoc->conn_count = 0;

		TAILQ_INIT(&assoc->fc_conns);
		assoc->ls_del_op_ctx = NULL;
#ifdef NVMF_FC_LS_SEND_LS_DISCONNECT
		assoc->snd_disconn_bufs.rpi = rpi;
#endif

		/* Back pointer to host specific controller configuration. */
		assoc->host = host;

		/* add association to target port's association list */
		TAILQ_INSERT_TAIL(&tgtport->fc_associations, assoc, link);
		tgtport->assoc_count++;
		rport->assoc_count++;
		SPDK_NOTICELOG("New Association %p created:\n", assoc);
		SPDK_NOTICELOG("\thostnqn:%s\n", assoc->host_nqn);
		SPDK_NOTICELOG("\tsubnqn:%s\n", assoc->sub_nqn);
		SPDK_NOTICELOG("\twwpn:0x%lx\n", tgtport->fc_portname.u.wwn);
	} else {
		SPDK_ERRLOG("out of associations on port %d\n",
			    tgtport->fc_port->port_hdl);
	}

	return assoc;
}

static inline void
nvmf_fc_ls_free_association(struct spdk_nvmf_bcm_fc_association *assoc)
{
	SPDK_NOTICELOG("Freeing association, assoc_id 0x%lx\n",
		       assoc->assoc_id);
	assoc->assoc_state = SPDK_NVMF_BCM_FC_OBJECT_ZOMBIE;
	TAILQ_INSERT_TAIL(&assoc->tgtport->fc_port->ls_rsrc_pool.assoc_free_list,
			  assoc, port_free_assoc_list_link);
}

static inline void
nvmf_fc_ls_append_del_cb_ctx(struct spdk_nvmf_bcm_fc_association *assoc,
			     union nvmf_fc_ls_op_ctx *opd)
{
	/* append to delete assoc callback list */
	union nvmf_fc_ls_op_ctx *nxt =
			(union nvmf_fc_ls_op_ctx *) &assoc->ls_del_op_ctx;
	while (nxt->next_op_ctx) nxt = nxt->next_op_ctx;
	nxt->next_op_ctx = opd;
}

static struct spdk_nvmf_bcm_fc_conn *
nvmf_fc_ls_new_connection(struct spdk_nvmf_bcm_fc_association *assoc,
			  struct spdk_nvmf_host *host,
			  enum conn_type type, uint16_t qid,
			  uint16_t esrp_ratio, uint16_t rpi, uint16_t sq_size)
{
	struct spdk_nvmf_bcm_fc_conn *fc_conn;

	fc_conn = TAILQ_FIRST(&assoc->tgtport->fc_port->ls_rsrc_pool.fc_conn_free_list);

	if (fc_conn) {
		/* remove from port's free connection list */
		TAILQ_REMOVE(&assoc->tgtport->fc_port->ls_rsrc_pool.fc_conn_free_list,
			     fc_conn, port_free_conn_list_link);

		bzero((void *)fc_conn, sizeof(*fc_conn));
		fc_conn->conn.type = type;
		fc_conn->conn.transport = &spdk_nvmf_transport_bcm_fc;
		fc_conn->conn.qid = qid;
		fc_conn->conn.sq_head_max = sq_size;
		fc_conn->esrp_ratio = esrp_ratio;
		fc_conn->fc_assoc = assoc;
		fc_conn->rpi = rpi;
		fc_conn->max_queue_depth = sq_size;

		TAILQ_INIT(&fc_conn->pending_queue);
		TAILQ_INIT(&fc_conn->fused_waiting_queue);
		SPDK_NOTICELOG("New Connection %p for Association %p created:\n", fc_conn,
			       assoc);
		SPDK_NOTICELOG("\tQueue id:%u\n", fc_conn->conn.qid);
		SPDK_NOTICELOG("\tQueue size requested:%u\n", sq_size);
		SPDK_NOTICELOG("\tMax admin queue size supported:%u\n",
			       host->max_aq_depth);
		SPDK_NOTICELOG("\tMax IO queue size supported:%u\n",
			       host->max_io_queue_depth);
	} else {
		SPDK_ERRLOG("out of connections\n");
	}

	return fc_conn;
}

static inline void
nvmf_fc_ls_free_connection(struct spdk_nvmf_bcm_fc_port *fc_port,
			   struct spdk_nvmf_bcm_fc_conn *fc_conn)
{
	if (fc_conn->conn.sess) {
		/* session_remove_connt removes conn from subsystem */
		/* if last conn for session, deletes session too */
		fc_conn->conn.transport->session_remove_conn(fc_conn->conn.sess,
				&fc_conn->conn);
	}

	/* Free connection fc_req pool */
	spdk_nvmf_bcm_fc_free_conn_req_ring(fc_conn);

	TAILQ_INSERT_TAIL(&fc_port->ls_rsrc_pool.fc_conn_free_list,
			  fc_conn, port_free_conn_list_link);
}

static inline union nvmf_fc_ls_op_ctx *
	nvmf_fc_ls_new_op_ctx(void)
{
	return (union nvmf_fc_ls_op_ctx *)
	       calloc(1, sizeof(union nvmf_fc_ls_op_ctx));
}

static inline void
nvmf_fc_ls_free_op_ctx(union nvmf_fc_ls_op_ctx *ctx_ptr)
{
	free((void *)ctx_ptr);
}

/* End - Allocators/Deallocators (assocations, connections, */
/*       poller API data)                                   */
/* ******************************************************** */

static inline struct spdk_nvmf_bcm_fc_association *
nvmf_fc_ls_find_assoc(struct spdk_nvmf_bcm_fc_nport *tgtport, uint64_t assoc_id)
{
	struct spdk_nvmf_bcm_fc_association *assoc = NULL;
	TAILQ_FOREACH(assoc, &tgtport->fc_associations, link) {
		if (assoc->assoc_id == assoc_id) {
			if (assoc->assoc_state == SPDK_NVMF_BCM_FC_OBJECT_ZOMBIE) {
				assoc = NULL;
			}
			break;
		}
	}
	return assoc;
}

static inline uint64_t
nvmf_fc_gen_conn_id(uint32_t qnum, struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	uint64_t conn_id;
	struct spdk_nvmf_bcm_fc_association *assoc = NULL;
	struct spdk_nvmf_bcm_fc_conn *fc_conn = NULL;
	struct spdk_nvmf_bcm_fc_nport *fc_nport = NULL;
	struct spdk_nvmf_bcm_fc_port *fc_port = hwqp->fc_port;
	bool repeat;

	do {
		repeat = false;

		if (hwqp->cid_cnt == 0) /* make sure cid_cnt is never 0 */
			hwqp->cid_cnt++;

		conn_id = ((uint64_t) qnum |
			   (hwqp->cid_cnt << SPDK_NVMF_FC_BCM_MRQ_CONNID_UV_SHIFT));
		/*
		 * Make sure the conn-id is unique across a HW port.
		 */
		TAILQ_FOREACH(fc_nport, &fc_port->nport_list, link) {
			TAILQ_FOREACH(assoc, &fc_nport->fc_associations, link) {
				TAILQ_FOREACH(fc_conn, &assoc->fc_conns, link) {
					if (fc_conn->conn_id == conn_id) {
						repeat = true;
						goto outer_loop;
					}
				}
			}
		}

outer_loop:
		hwqp->cid_cnt++;
	} while (repeat);

	return conn_id;
}

static inline struct spdk_nvmf_bcm_fc_hwqp *
nvmf_fc_ls_assign_conn_to_q(struct spdk_nvmf_bcm_fc_association *assoc,
			    uint64_t *conn_id, uint32_t sq_size, bool for_aq)
{
	struct spdk_nvmf_bcm_fc_nport *tgtport = assoc->tgtport;
	struct spdk_nvmf_bcm_fc_port *fc_port = tgtport->fc_port;
	uint32_t sel_qind = 0;

	SPDK_NOTICELOG("Assign Connection to IO queue "
		       "for port %d nport %d assoc_id 0x%lx\n", fc_port->port_hdl,
		       tgtport->nport_hdl, assoc->assoc_id);

	if (!for_aq) {
		/* find queue with max amount of space available */
		uint32_t qind;

		sel_qind = 1; /* queue 0 for AQ's, so start with queue 1 */

		for (qind = sel_qind + 1; qind < fc_port->max_io_queues; qind++) {
			if (fc_port->io_queues[qind].free_q_slots >
			    fc_port->io_queues[sel_qind].free_q_slots)
				sel_qind = qind;
		}
		if (fc_port->io_queues[sel_qind].free_q_slots < sq_size) {
			return NULL; /* no queue has space of this connection */
		}
	}

	/* decrease the free slots now in case another connect request comes
	 * in while adding this connection in the poller thread */
	fc_port->io_queues[sel_qind].free_q_slots -= sq_size;

	fc_port->io_queues[sel_qind].num_conns++;

	/* create connection ID */
	*conn_id = nvmf_fc_gen_conn_id(sel_qind,
				       &fc_port->io_queues[sel_qind]);

	SPDK_NOTICELOG("q_num %d (free now %d), conn_id 0x%lx\n", sel_qind,
		       fc_port->io_queues[sel_qind].free_q_slots,
		       *conn_id);

	return &fc_port->io_queues[sel_qind];
}

static inline void
nvmf_fc_del_assoc_from_tgt_port(struct spdk_nvmf_bcm_fc_association *assoc)
{
	struct spdk_nvmf_bcm_fc_nport *tgtport = assoc->tgtport;
	TAILQ_REMOVE(&tgtport->fc_associations, assoc, link);
	tgtport->assoc_count--;
	assoc->rport->assoc_count--;
}

static void
nvmf_fc_ls_rsp_fail_del_conn_cb(void *cb_data, spdk_nvmf_bcm_fc_poller_api_ret_t ret)
{
	union nvmf_fc_ls_op_ctx *opd =
			(union nvmf_fc_ls_op_ctx *)cb_data;
	struct nvmf_fc_ls_del_conn_api_data *dp = &opd->del_conn;

	SPDK_NOTICELOG("Transmit LS response failure callback"
		       "for %s conn_id 0x%lx on Port %d\n",
		       (dp->assoc_conn) ? "Association" : "Connection",
		       dp->args.fc_conn->conn_id, dp->args.hwqp->fc_port->port_hdl);

	nvmf_fc_ls_free_connection(dp->args.hwqp->fc_port, dp->args.fc_conn);
	dp->args.hwqp->num_conns--;

	nvmf_fc_ls_free_op_ctx(opd);
}

static void
nvmf_fc_handle_xmt_ls_rsp_failure(struct spdk_nvmf_bcm_fc_association *assoc,
				  struct spdk_nvmf_bcm_fc_conn *fc_conn,
				  bool assoc_conn)
{
	struct nvmf_fc_ls_del_conn_api_data *api_data;
	union nvmf_fc_ls_op_ctx *opd = NULL;

	SPDK_NOTICELOG("Transmit LS response failure "
		       "for assoc_id 0x%lx conn_id 0x%lx\n", assoc->assoc_id,
		       fc_conn->conn_id);

	if (assoc_conn) {
		/* delete association */
		nvmf_fc_del_assoc_from_tgt_port(assoc);
		nvmf_fc_ls_free_association(assoc);
	} else {
		/* IOQ - give the queue slots for this connection back to the hwqp */
		fc_conn->hwqp->free_q_slots += fc_conn->max_queue_depth;
	}

	/* create context for delete connection API */
	opd = nvmf_fc_ls_new_op_ctx();
	if (!opd) { /* hopefully this doesn't happen */
		nvmf_fc_ls_free_connection(fc_conn->hwqp->fc_port, fc_conn);
		fc_conn->hwqp->num_conns--;
		SPDK_ERRLOG("Mem alloc failed for del conn op data");
		return;
	}

	api_data = &opd->del_conn;
	api_data->assoc = NULL;
	api_data->ls_rqst = NULL;
	api_data->assoc_conn = assoc_conn;
	api_data->args.fc_conn = fc_conn;
	api_data->args.send_abts = false;
	api_data->args.hwqp = fc_conn->hwqp;
	api_data->args.cb_info.cb_func = nvmf_fc_ls_rsp_fail_del_conn_cb;
	api_data->args.cb_info.cb_data = opd;

	spdk_nvmf_bcm_fc_poller_api(api_data->args.hwqp,
				    SPDK_NVMF_BCM_FC_POLLER_API_DEL_CONNECTION,
				    &api_data->args);
}

/* callback from poller's ADD_Connection event */
static void
nvmf_fc_ls_add_conn_cb(void *cb_data, spdk_nvmf_bcm_fc_poller_api_ret_t ret)
{
	union nvmf_fc_ls_op_ctx *opd =
			(union nvmf_fc_ls_op_ctx *)cb_data;
	struct nvmf_fc_ls_add_conn_api_data *dp = &opd->add_conn;
	struct spdk_nvmf_bcm_fc_association *assoc = dp->assoc;
	struct spdk_nvmf_bcm_fc_nport *tgtport = assoc->tgtport;
	struct spdk_nvmf_bcm_fc_conn *fc_conn = dp->args.fc_conn;
	struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst = dp->ls_rqst;

	SPDK_NOTICELOG("add_conn_cb: assoc_id = 0x%lx, conn_id = 0x%lx\n",
		       assoc->assoc_id, fc_conn->conn_id);

	if (assoc->assoc_state == SPDK_NVMF_BCM_FC_OBJECT_TO_BE_DELETED) {
		/* association is already being deleted - don't continue */
		nvmf_fc_ls_free_op_ctx(opd);
		return;
	}

	if (dp->assoc_conn) {
		struct nvmf_fc_ls_cr_assoc_acc *assoc_acc =
			(struct nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
		/* put connection and association ID in response */
		to_be64(&assoc_acc->conn_id.connection_id, fc_conn->conn_id);
		assoc_acc->assoc_id.association_id = assoc_acc->conn_id.connection_id;
	} else {
		struct nvmf_fc_ls_cr_conn_acc *conn_acc =
			(struct nvmf_fc_ls_cr_conn_acc *)ls_rqst->rspbuf.virt;
		/* put connection ID in response */
		to_be64(&conn_acc->conn_id.connection_id, fc_conn->conn_id);
	}

	/* send LS response */
	if (nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst) != 0) {
		SPDK_ERRLOG("Send LS response for %s failed - cleaning up\n",
			    dp->assoc_conn ? "assocation" : "connection");
		nvmf_fc_handle_xmt_ls_rsp_failure(assoc, fc_conn,
						  dp->assoc_conn);
	} else {
		SPDK_NOTICELOG("LS response (conn_id 0x%lx) sent\n", fc_conn->conn_id);
	}

	nvmf_fc_ls_free_op_ctx(opd);
}

static void
nvmf_fc_ls_add_conn_to_poller(
	struct spdk_nvmf_bcm_fc_association *assoc,
	struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst,
	struct spdk_nvmf_bcm_fc_conn *fc_conn,
	uint16_t sq_size,
	bool assoc_conn)
{
	union nvmf_fc_ls_op_ctx *opd = nvmf_fc_ls_new_op_ctx();
	struct spdk_nvmf_bcm_fc_nport *tgtport = assoc->tgtport;
	struct nvmf_fc_ls_add_conn_api_data *api_data = NULL;
	struct nvmf_fc_ls_cr_assoc_rqst *rqst =
		(struct nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
	struct nvmf_fc_ls_cr_assoc_acc *acc =
		(struct nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;

	SPDK_NOTICELOG("Add Connection to poller for "
		       "assoc_id 0x%lx conn_id 0x%lx\n", assoc->assoc_id,
		       fc_conn->conn_id);

	if (!opd) {
		SPDK_ERRLOG("allocate api data for add conn op failed\n");
		goto failure;
	}

	api_data = &opd->add_conn;

	/* assign connection to (poller) queue */
	fc_conn->hwqp = nvmf_fc_ls_assign_conn_to_q(assoc,
			&fc_conn->conn_id,
			sq_size,
			assoc_conn);
	if (!fc_conn->hwqp) {
		SPDK_ERRLOG("failed to find hwqp that could fit requested sq size\n");
		goto failure;
	}

	/* alloc fc req objects for this connection */
	if (spdk_nvmf_bcm_fc_create_conn_req_ring(fc_conn)) {
		SPDK_ERRLOG("Alloc fc_req pool for connection failed.");
		goto failure;
	}

	/* insert conn in association's connection list */
	TAILQ_INSERT_TAIL(&assoc->fc_conns, fc_conn, assoc_link);
	assoc->conn_count++;

	if (assoc_conn) {
		/* assign association ID to aq's connection id */
		assoc->assoc_id = fc_conn->conn_id;
	}


	api_data->args.fc_conn = fc_conn;
	api_data->args.cb_info.cb_func = nvmf_fc_ls_add_conn_cb;
	api_data->args.cb_info.cb_data = (void *)opd;
	api_data->assoc = assoc;
	api_data->ls_rqst = ls_rqst;
	api_data->assoc_conn = assoc_conn;

	SPDK_NOTICELOG("Add connection API called for conn_id = 0x%lx\n",
		       fc_conn->conn_id);
	spdk_nvmf_bcm_fc_poller_api(api_data->args.fc_conn->hwqp,
				    SPDK_NVMF_BCM_FC_POLLER_API_ADD_CONNECTION,
				    &api_data->args);
	SPDK_NOTICELOG("Add connection API returned "
		       "for conn_id = 0x%lx\n", fc_conn->conn_id);
	return;
failure:
	/* send failure response */
	ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
			   NVME_FC_MAX_LS_BUFFER_SIZE, rqst->w0.ls_cmd,
			   FCNVME_RJT_RC_INSUFF_RES,
			   FCNVME_RJT_EXP_NONE, 0);
	nvmf_fc_ls_free_connection(assoc->tgtport->fc_port, fc_conn);
	if (assoc_conn) {
		nvmf_fc_del_assoc_from_tgt_port(assoc);
		nvmf_fc_ls_free_association(assoc);
	}
	(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	if (opd) {
		nvmf_fc_ls_free_op_ctx(opd);
	}
}

/* Delete association functions */

static void
nvmf_fc_do_del_assoc_cbs(union nvmf_fc_ls_op_ctx *opd,
			 int ret)
{
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS,
		      "performing delete assoc. callbacks\n");
	while (opd) {
		union nvmf_fc_ls_op_ctx *nxt = opd->next_op_ctx;
		struct nvmf_fc_delete_assoc_api_data *dp =
				&opd->del_assoc;

		dp->del_assoc_cb(dp->del_assoc_cb_data, ret);
		nvmf_fc_ls_free_op_ctx(opd);

		opd = nxt;
	}

}

static void
nvmf_fc_send_ls_disconnect(struct spdk_nvmf_bcm_fc_association *assoc)
{
#ifdef NVMF_FC_LS_SEND_LS_DISCONNECT
	struct nvmf_fc_ls_disconnect_rqst *dc_rqst =
		(struct nvmf_fc_ls_disconnect_rqst *)
		assoc->snd_disconn_bufs.rqst.virt;

	bzero(dc_rqst, sizeof(struct nvmf_fc_ls_disconnect_rqst));

	/* fill in request descriptor */
	dc_rqst->w0.ls_cmd = FCNVME_LS_DISCONNECT;
	to_be32(&dc_rqst->desc_list_len,
		sizeof(struct nvmf_fc_ls_disconnect_rqst) -
		(2 * sizeof(uint32_t)));

	/* fill in disconnect command descriptor */
	to_be32(&dc_rqst->disconn_cmd.desc_tag, FCNVME_LSDESC_DISCONN_CMD);
	to_be32(&dc_rqst->disconn_cmd.desc_len,
		sizeof(struct nvmf_fc_lsdesc_disconn_cmd) -
		(2 * sizeof(uint32_t)));

	/* fill in association id descriptor */
	to_be32(&dc_rqst->assoc_id.desc_tag, FCNVME_LSDESC_ASSOC_ID),
		to_be32(&dc_rqst->assoc_id.desc_len,
			sizeof(struct nvmf_fc_lsdesc_assoc_id) -
			(2 * sizeof(uint32_t)));
	to_be64(&dc_rqst->assoc_id.association_id, assoc->assoc_id);

	SPDK_NOTICELOG("Send LS disconnect\n");
	if (spdk_nvmf_bcm_fc_xmt_srsr_req(&assoc->tgtport->fc_port->ls_queue,
					  &assoc->snd_disconn_bufs, 0, 0)) {
		SPDK_ERRLOG("Error sending LS disconnect\n");
	}
#endif
}

static void
nvmf_fc_del_all_conns_cb(void *cb_data, spdk_nvmf_bcm_fc_poller_api_ret_t ret)
{
	union nvmf_fc_ls_op_ctx *opd =
			(union nvmf_fc_ls_op_ctx *)cb_data;
	struct nvmf_fc_delete_assoc_api_data *dp = &opd->del_assoc;
	struct spdk_nvmf_bcm_fc_association *assoc = dp->assoc;
	struct spdk_nvmf_bcm_fc_conn *fc_conn = dp->args.fc_conn;

	/* Assumption here is that there will be no error (i.e. ret=success).
	 * Since connections are deleted in parallel, nothing can be
	 * done anyway if there is an error because we need to complete
	 * all connection deletes and callback to caller */

	SPDK_NOTICELOG("Delete all connections for "
		       "assoc_id 0x%lx\n", assoc->assoc_id);
	if (assoc->subsystem) {
		assoc->subsystem->disconnect_cb(assoc->subsystem->cb_ctx,
						&fc_conn->conn);
	}

	if ((fc_conn->conn_id & SPDK_NVMF_FC_BCM_MRQ_CONNID_QUEUE_MASK) != 0) {
		/* give the queue slots for this connection  back to the hwqp */
		fc_conn->hwqp->free_q_slots += fc_conn->max_queue_depth;
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS,
			      "Freed up %d slots on hwqp %d (free %d)\n",
			      fc_conn->max_queue_depth, (int)(fc_conn->conn_id &
					      SPDK_NVMF_FC_BCM_MRQ_CONNID_QUEUE_MASK),
			      fc_conn->hwqp->free_q_slots);
	}

	/* remove connection from association's connection list */
	TAILQ_REMOVE(&assoc->fc_conns, fc_conn, assoc_link);
	nvmf_fc_ls_free_connection(assoc->tgtport->fc_port, fc_conn);
	dp->args.hwqp->num_conns--;

	if (--assoc->conn_count == 0) {
		/* last connection - remove association from target port's
		 * association list */
		union nvmf_fc_ls_op_ctx *opd =
				(union nvmf_fc_ls_op_ctx *) assoc->ls_del_op_ctx;

		SPDK_NOTICELOG("remove assoc. %lx\n", assoc->assoc_id);
		nvmf_fc_del_assoc_from_tgt_port(assoc);

		if (assoc->tgtport->fc_port->hw_port_status == SPDK_FC_PORT_ONLINE) {
			/* send LS disconnect to the initiator */
			nvmf_fc_send_ls_disconnect(assoc);
		}

		nvmf_fc_ls_free_association(assoc);

		/* perform callbacks to all callers to delete association */
		nvmf_fc_do_del_assoc_cbs(opd, 0);
	}

	nvmf_fc_ls_free_op_ctx(opd);
}

static void
nvmf_fc_kill_io_del_all_conns_cb(void *cb_data, spdk_nvmf_bcm_fc_poller_api_ret_t ret)
{
	union nvmf_fc_ls_op_ctx *opd =
			(union nvmf_fc_ls_op_ctx *)cb_data;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "Callback after killing outstanding ABTS.");
	/*
	 * NOTE: We should not access any connection or association related data
	 * structures here.
	 */
	nvmf_fc_ls_free_op_ctx(opd);
}


/* Disconnect/delete (association) request functions */

static
int
nvmf_fc_delete_association(struct spdk_nvmf_bcm_fc_nport *tgtport,
			   uint64_t assoc_id, bool send_abts,
			   spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
			   void *cb_data, bool from_ls_rqst)
{

	union nvmf_fc_ls_op_ctx *opd;
	struct nvmf_fc_delete_assoc_api_data *api_data;
	struct spdk_nvmf_bcm_fc_conn *fc_conn;
	struct spdk_nvmf_bcm_fc_association *assoc =
		nvmf_fc_ls_find_assoc(tgtport, assoc_id);
	struct spdk_nvmf_bcm_fc_port *fc_port = tgtport->fc_port;
	spdk_nvmf_bcm_fc_object_state_t assoc_state;

	SPDK_NOTICELOG("Delete association, "
		       "assoc_id 0x%lx\n", assoc_id);

	if (!assoc) {
		SPDK_ERRLOG("Delete association failed: %s\n",
			    validation_errors[VERR_NO_ASSOC]);
		return VERR_NO_ASSOC;
	}

	/* create cb context to put in association's list of
	 * callbacks to call when delete association is done */
	opd = nvmf_fc_ls_new_op_ctx();
	if (!opd) {
		SPDK_ERRLOG("Mem alloc failed for del assoc cb data");
		return SPDK_ERR_NOMEM;
	}

	api_data = &opd->del_assoc;
	api_data->assoc = assoc;
	api_data->from_ls_rqst = from_ls_rqst;
	api_data->del_assoc_cb = del_assoc_cb;
	api_data->del_assoc_cb_data = cb_data;
	api_data->args.cb_info.cb_data = opd;
	nvmf_fc_ls_append_del_cb_ctx(assoc, opd);

	assoc_state = assoc->assoc_state;
	if ((assoc_state == SPDK_NVMF_BCM_FC_OBJECT_TO_BE_DELETED) &&
	    (fc_port->hw_port_status != SPDK_FC_PORT_QUIESCED)) {
		/* association already being deleted */
		return 0;
	}

	/* mark assoc. to be deleted */
	assoc->assoc_state = SPDK_NVMF_BCM_FC_OBJECT_TO_BE_DELETED;

	/* delete all of the association's connections */
	TAILQ_FOREACH(fc_conn, &assoc->fc_conns, assoc_link) {
		/* create context for delete connection API */
		opd = nvmf_fc_ls_new_op_ctx();
		if (!opd) { /* hopefully this doesn't happen */
			SPDK_ERRLOG("Mem alloc failed for del conn op data");
			return SPDK_ERR_NOMEM;
		}

		api_data = &opd->del_assoc;
		api_data->args.fc_conn = fc_conn;
		api_data->assoc = assoc;
		api_data->args.send_abts = send_abts;
		api_data->args.hwqp = spdk_nvmf_bcm_fc_get_hwqp(assoc->tgtport,
				      fc_conn->conn_id);
		api_data->args.cb_info.cb_data = opd;
		if ((fc_port->hw_port_status == SPDK_FC_PORT_QUIESCED) &&
		    (assoc_state == SPDK_NVMF_BCM_FC_OBJECT_TO_BE_DELETED)) {
			/*
			 * If there are any connections deletes or IO abts that are
			 * stuck because of firmware reset, a second invocation of
			 * SPDK_NVMF_BCM_FC_POLLER_API_DEL_CONNECTION will result in
			 * outstanding connections & requests being killed and
			 * their corresponding callbacks being executed.
			 */
			api_data->args.cb_info.cb_func = nvmf_fc_kill_io_del_all_conns_cb;
		} else {
			api_data->args.cb_info.cb_func = nvmf_fc_del_all_conns_cb;
		}
		SPDK_NOTICELOG("delete conn_id = %lx\n", fc_conn->conn_id);
		spdk_nvmf_bcm_fc_poller_api(api_data->args.hwqp,
					    SPDK_NVMF_BCM_FC_POLLER_API_DEL_CONNECTION,
					    &api_data->args);
	}

	return 0;
}

static void
nvmf_fc_ls_disconnect_assoc_cb(void *cb_data, uint32_t err)
{
	union nvmf_fc_ls_op_ctx *opd = (union nvmf_fc_ls_op_ctx *)cb_data;
	struct nvmf_fc_ls_disconn_assoc_api_data *dp = &opd->disconn_assoc;
	struct spdk_nvmf_bcm_fc_nport *tgtport = dp->tgtport;
	struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst = dp->ls_rqst;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "Disconnect association callback begin "
		      "nport %d\n", tgtport->nport_hdl);
	if (err != 0) {
		/* send failure response */
		struct nvmf_fc_ls_cr_assoc_rqst *rqst =
			(struct nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
		struct nvmf_fc_ls_cr_assoc_acc *acc =
			(struct nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   NVME_FC_MAX_LS_BUFFER_SIZE,
				   rqst->w0.ls_cmd,
				   FCNVME_RJT_RC_UNAB,
				   FCNVME_RJT_EXP_NONE,
				   0);
	}

	(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);

	nvmf_fc_ls_free_op_ctx(opd);
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "Disconnect association callback complete "
		      "nport %d err %d\n", tgtport->nport_hdl, err);
}

static void
nvmf_fc_ls_disconnect_assoc(struct spdk_nvmf_bcm_fc_nport *tgtport,
			    struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst, uint64_t assoc_id)
{
	union nvmf_fc_ls_op_ctx *opd = nvmf_fc_ls_new_op_ctx();

	if (opd) {
		struct nvmf_fc_ls_disconn_assoc_api_data *api_data =
				&opd->disconn_assoc;
		int ret;

		api_data->tgtport = tgtport;
		api_data->ls_rqst = ls_rqst;
		ret = nvmf_fc_delete_association(tgtport, assoc_id,
						 false,
						 nvmf_fc_ls_disconnect_assoc_cb,
						 api_data, true);

		if (ret != 0) {
			/* delete association failed */
			struct nvmf_fc_ls_cr_assoc_rqst *rqst =
				(struct nvmf_fc_ls_cr_assoc_rqst *)
				ls_rqst->rqstbuf.virt;
			struct nvmf_fc_ls_cr_assoc_acc *acc =
				(struct nvmf_fc_ls_cr_assoc_acc *)
				ls_rqst->rspbuf.virt;
			ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
					   NVME_FC_MAX_LS_BUFFER_SIZE,
					   rqst->w0.ls_cmd,
					   ret == VERR_NO_ASSOC ?
					   FCNVME_RJT_RC_INV_ASSOC :
					   ret == SPDK_ERR_NOMEM ?
					   FCNVME_RJT_RC_INSUFF_RES :
					   FCNVME_RJT_RC_LOGIC,
					   FCNVME_RJT_EXP_NONE, 0);
			(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
		}
	} else {
		/* send failure response */
		struct nvmf_fc_ls_cr_assoc_rqst *rqst =
			(struct nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
		struct nvmf_fc_ls_cr_assoc_acc *acc =
			(struct nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
		SPDK_ERRLOG("Allocate disconn assoc op data failed\n");
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   NVME_FC_MAX_LS_BUFFER_SIZE,
				   rqst->w0.ls_cmd,
				   FCNVME_RJT_RC_INSUFF_RES,
				   FCNVME_RJT_EXP_NONE, 0);
		(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}
}

/* **************************** */
/* LS Reqeust Handler Functions */

static void
nvmf_fc_ls_process_cass(uint32_t s_id,
			struct spdk_nvmf_bcm_fc_nport *tgtport,
			struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst)
{
	struct nvmf_fc_ls_cr_assoc_rqst *rqst =
		(struct nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
	struct nvmf_fc_ls_cr_assoc_acc *acc =
		(struct nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_bcm_fc_association *assoc;
	struct spdk_nvmf_bcm_fc_conn *fc_conn;
	struct spdk_nvmf_subsystem *subsystem = NULL;
	struct spdk_nvmf_host *host;

	int errmsg_ind = 0;
	uint8_t rc = FCNVME_RJT_RC_NONE;
	uint8_t ec = FCNVME_RJT_EXP_NONE;
	char traddr[NVMF_TGT_FC_TR_ADDR_LENGTH + 1];
	struct spdk_nvmf_listen_addr *listen_addr, *listener;

	SPDK_NOTICELOG("LS_CASS: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d, sq_size=%d\n",
		       ls_rqst->rqst_len, be32_to_cpu(&rqst->desc_list_len),
		       be32_to_cpu(&rqst->assoc_cmd.desc_len),
		       be16_to_cpu(&rqst->assoc_cmd.sqsize));

	SPDK_NOTICELOG("LS_CASS: Subnqn: %s\n",
		       rqst->assoc_cmd.subnqn);

	SPDK_NOTICELOG("LS_CASS: Hostnqn: %s\n",
		       rqst->assoc_cmd.hostnqn);

	/* Construct the listen addr */
	snprintf(traddr, NVMF_TGT_FC_TR_ADDR_LENGTH, "nn-0x%lx:pn-0x%lx",
		 be64_to_cpu(&tgtport->fc_nodename.u.wwn), be64_to_cpu(&tgtport->fc_portname.u.wwn));

	listen_addr = spdk_nvmf_listen_addr_create(NVMF_BCM_FC_TRANSPORT_NAME, SPDK_NVMF_ADRFAM_FC, traddr,
			"none");

	if (ls_rqst->rqst_len < LS_CREATE_ASSOC_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd req len = %d, should be at least %d\n",
			    ls_rqst->rqst_len, LS_CREATE_ASSOC_MIN_LEN);
		errmsg_ind = VERR_CR_ASSOC_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (be32_to_cpu(&rqst->desc_list_len) <
		   LS_CREATE_ASSOC_DESC_LIST_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd desc list len = %d, should be at least %d\n",
			    be32_to_cpu(&rqst->desc_list_len),
			    LS_CREATE_ASSOC_DESC_LIST_MIN_LEN);
		errmsg_ind = VERR_CR_ASSOC_RQST_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->assoc_cmd.desc_tag !=
		   cpu_to_be32(FCNVME_LSDESC_CREATE_ASSOC_CMD)) {
		errmsg_ind = VERR_CR_ASSOC_CMD;
		rc = FCNVME_RJT_RC_INV_PARAM;
	} else if (be32_to_cpu(&rqst->assoc_cmd.desc_len) <
		   LS_CREATE_ASSOC_CMD_DESC_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd desc len = %d, should be at least %d\n",
			    be32_to_cpu(&rqst->assoc_cmd.desc_len),
			    LS_CREATE_ASSOC_CMD_DESC_MIN_LEN);
		errmsg_ind = VERR_CR_ASSOC_CMD_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (!rqst->assoc_cmd.ersp_ratio ||
		   (from_be16(&rqst->assoc_cmd.ersp_ratio) >=
		    from_be16(&rqst->assoc_cmd.sqsize))) {
		errmsg_ind = VERR_ERSP_RATIO;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_ESRP;
	} else if ((subsystem = nvmf_fc_ls_valid_subnqn(rqst->assoc_cmd.subnqn))
		   == NULL) {
		errmsg_ind = VERR_SUBNQN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_SUBNQN;
	} else if ((host = spdk_nvmf_find_subsystem_host(subsystem,
			   (const char *) rqst->assoc_cmd.hostnqn)) == NULL) {
		errmsg_ind = VERR_HOSTNQN;
		rc = FCNVME_RJT_RC_INV_HOST;
		ec = FCNVME_RJT_EXP_INV_HOSTNQN;
	} else if (!(spdk_nvmf_validate_sqsize(host, 0, from_be16(&rqst->assoc_cmd.sqsize), __func__))) {
		errmsg_ind = VERR_SQSIZE;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_SQ_SIZE;
	} else if ((listener = spdk_nvmf_find_subsystem_listener(subsystem, listen_addr)) == NULL) {
		errmsg_ind = VERR_SUBLISTENER;
		rc = FCNVME_RJT_RC_UNAB;
		ec = FCNVME_RJT_EXP_NONE;
	} else {
		/* get new association */
		assoc = nvmf_fc_ls_new_association(s_id, tgtport, ls_rqst->rport,
						   &rqst->assoc_cmd,
						   subsystem, host,
						   ls_rqst->rpi);
		if (!assoc) {
			errmsg_ind = VERR_ASSOC_ALLOC_FAIL;
			rc = FCNVME_RJT_RC_INSUFF_RES;
			ec = FCNVME_RJT_EXP_NONE;
		} else { // alloc admin q (i.e. connection)
			fc_conn = nvmf_fc_ls_new_connection(assoc, host,
							    CONN_TYPE_AQ, 0,
							    from_be16(&rqst->assoc_cmd.ersp_ratio),
							    ls_rqst->rpi,
							    from_be16(&rqst->assoc_cmd.sqsize));
			if (!fc_conn) {
				nvmf_fc_ls_free_association(assoc);
				errmsg_ind = VERR_CONN_ALLOC_FAIL;
				rc = FCNVME_RJT_RC_INSUFF_RES;
				ec = FCNVME_RJT_EXP_NONE;
			}
		}
	}

	if (rc != FCNVME_RJT_RC_NONE) {
		SPDK_ERRLOG("Create Association LS failed: %s\n",
			    validation_errors[errmsg_ind]);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   NVME_FC_MAX_LS_BUFFER_SIZE,
				   rqst->w0.ls_cmd, rc,
				   ec, 0);
		(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}

	else {
		/* format accept response */
		bzero(acc, sizeof(*acc));

		ls_rqst->rsp_len = sizeof(*acc);

		nvmf_fc_ls_format_rsp_hdr(acc, FCNVME_LS_ACC,
					  nvmf_fc_lsdesc_len(
						  sizeof(struct nvmf_fc_ls_cr_assoc_acc)),
					  FCNVME_LS_CREATE_ASSOCIATION);
		to_be32(&acc->assoc_id.desc_tag, FCNVME_LSDESC_ASSOC_ID);
		acc->assoc_id.desc_len =
			nvmf_fc_lsdesc_len(sizeof(struct nvmf_fc_lsdesc_assoc_id));
		to_be32(&acc->conn_id.desc_tag, FCNVME_LSDESC_CONN_ID);
		acc->conn_id.desc_len =
			nvmf_fc_lsdesc_len(sizeof(struct nvmf_fc_lsdesc_conn_id));

		/* assign connection to HWQP poller - also sends response */
		nvmf_fc_ls_add_conn_to_poller(assoc, ls_rqst,
					      fc_conn, 0, true);
	}
	spdk_nvmf_listen_addr_cleanup(listen_addr);
}

static void
nvmf_fc_ls_process_cioc(struct spdk_nvmf_bcm_fc_nport *tgtport,
			struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst)
{
	struct nvmf_fc_ls_cr_conn_rqst *rqst =
		(struct nvmf_fc_ls_cr_conn_rqst *)ls_rqst->rqstbuf.virt;
	struct nvmf_fc_ls_cr_conn_acc *acc =
		(struct nvmf_fc_ls_cr_conn_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_bcm_fc_association *assoc;
	struct spdk_nvmf_bcm_fc_conn *fc_conn = NULL;
	int errmsg_ind = 0;
	uint8_t rc = FCNVME_RJT_RC_NONE;
	uint8_t ec = FCNVME_RJT_EXP_NONE;

	SPDK_NOTICELOG("LS_CIOC: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d, "
		       "assoc_id=0x%lx, sq_size=%d, esrp=%d\n",
		       ls_rqst->rqst_len, be32_to_cpu(&rqst->desc_list_len),
		       be32_to_cpu(&rqst->connect_cmd.desc_len),
		       be64_to_cpu(&rqst->assoc_id.association_id),
		       be16_to_cpu(&rqst->connect_cmd.sqsize),
		       be16_to_cpu(&rqst->connect_cmd.ersp_ratio));

	if (ls_rqst->rqst_len < sizeof(struct nvmf_fc_ls_cr_conn_rqst)) {
		errmsg_ind = VERR_CR_CONN_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->desc_list_len !=
		   nvmf_fc_lsdesc_len(sizeof(struct nvmf_fc_ls_cr_conn_rqst))) {
		errmsg_ind = VERR_CR_CONN_RQST_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->assoc_id.desc_tag !=
		   cpu_to_be32(FCNVME_LSDESC_ASSOC_ID)) {
		errmsg_ind = VERR_ASSOC_ID;
		rc = FCNVME_RJT_RC_INV_PARAM;
	} else if (rqst->assoc_id.desc_len !=
		   nvmf_fc_lsdesc_len(sizeof(struct nvmf_fc_lsdesc_assoc_id))) {
		errmsg_ind = VERR_ASSOC_ID_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->connect_cmd.desc_tag !=
		   cpu_to_be32(FCNVME_LSDESC_CREATE_CONN_CMD)) {
		errmsg_ind = VERR_CR_CONN_CMD;
		rc = FCNVME_RJT_RC_INV_PARAM;
	} else if (rqst->connect_cmd.desc_len !=
		   nvmf_fc_lsdesc_len(
			   sizeof(struct nvmf_fc_lsdesc_cr_conn_cmd))) {
		errmsg_ind = VERR_CR_CONN_CMD_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (!rqst->connect_cmd.ersp_ratio ||
		   (from_be16(&rqst->connect_cmd.ersp_ratio) >=
		    from_be16(&rqst->connect_cmd.sqsize))) {
		errmsg_ind = VERR_ERSP_RATIO;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_ESRP;
	} else {
		/* find association */
		assoc = nvmf_fc_ls_find_assoc(tgtport,
					      from_be64(&rqst->assoc_id.association_id));
		if (!assoc) {
			errmsg_ind = VERR_NO_ASSOC;
			rc = FCNVME_RJT_RC_INV_ASSOC;
		} else if (assoc->assoc_state == SPDK_NVMF_BCM_FC_OBJECT_TO_BE_DELETED) {
			/* association is being deleted - don't allow more connections */
			errmsg_ind = VERR_NO_ASSOC;
			rc = FCNVME_RJT_RC_INV_ASSOC;
		} else { // alloc IO q (i.e. connection)
			if (assoc->conn_count >= assoc->host->max_connections_allowed) {
				errmsg_ind = VERR_CONN_TOO_MANY;
				rc = FCNVME_RJT_RC_INV_PARAM;
				ec =  FCNVME_RJT_EXP_INV_Q_ID;
			} else if (!(spdk_nvmf_validate_sqsize(assoc->host,
							       from_be16(&rqst->connect_cmd.qid),
							       from_be16(&rqst->connect_cmd.sqsize),
							       __func__))) {
				errmsg_ind = VERR_SQSIZE;
				rc = FCNVME_RJT_RC_INV_PARAM;
				ec = FCNVME_RJT_EXP_SQ_SIZE;
			} else {

				fc_conn = nvmf_fc_ls_new_connection(assoc, assoc->host,
								    CONN_TYPE_IOQ,
								    from_be16(&rqst->connect_cmd.qid),
								    from_be16(&rqst->connect_cmd.ersp_ratio),
								    ls_rqst->rpi,
								    from_be16(&rqst->connect_cmd.sqsize));
				if (!fc_conn) {
					errmsg_ind = VERR_CONN_ALLOC_FAIL;
					rc = FCNVME_RJT_RC_INSUFF_RES;
					ec = FCNVME_RJT_EXP_NONE;
				}
			}
		}
	}

	if (rc != FCNVME_RJT_RC_NONE) {
		SPDK_ERRLOG("Create Connection LS failed: %s\n",
			    validation_errors[errmsg_ind]);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   NVME_FC_MAX_LS_BUFFER_SIZE,
				   rqst->w0.ls_cmd,
				   rc, ec, 0);
		(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	} else {
		/* format accept response */
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "Formatting LS accept response for "
			      "assoc_id 0x%lx conn_id 0x%lx\n", assoc->assoc_id,
			      fc_conn->conn_id);
		bzero(acc, sizeof(*acc));
		ls_rqst->rsp_len = sizeof(*acc);
		nvmf_fc_ls_format_rsp_hdr(acc, FCNVME_LS_ACC,
					  nvmf_fc_lsdesc_len(
						  sizeof(struct nvmf_fc_ls_cr_conn_acc)),
					  FCNVME_LS_CREATE_CONNECTION);
		to_be32(&acc->conn_id.desc_tag, FCNVME_LSDESC_CONN_ID);
		acc->conn_id.desc_len =
			nvmf_fc_lsdesc_len(
				sizeof(struct nvmf_fc_lsdesc_conn_id));

		/* assign connection to HWQP poller - also sends response */
		nvmf_fc_ls_add_conn_to_poller(assoc, ls_rqst, fc_conn,
					      from_be16(&rqst->connect_cmd.sqsize),
					      false);
	}
}

static void
nvmf_fc_ls_process_disc(struct spdk_nvmf_bcm_fc_nport *tgtport,
			struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst)
{
	struct nvmf_fc_ls_disconnect_rqst *rqst =
		(struct nvmf_fc_ls_disconnect_rqst *)ls_rqst->rqstbuf.virt;
	struct nvmf_fc_ls_disconnect_acc *acc =
		(struct nvmf_fc_ls_disconnect_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_bcm_fc_association *assoc;
	int errmsg_ind = 0;
	uint8_t rc = FCNVME_RJT_RC_NONE;
	uint8_t ec = FCNVME_RJT_EXP_NONE;

	SPDK_NOTICELOG("LS_DISC: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d,"
		       "assoc_id=0x%lx\n",
		       ls_rqst->rqst_len, be32_to_cpu(&rqst->desc_list_len),
		       be32_to_cpu(&rqst->disconn_cmd.desc_len),
		       be64_to_cpu(&rqst->assoc_id.association_id));

	if (ls_rqst->rqst_len < sizeof(struct nvmf_fc_ls_disconnect_rqst)) {
		errmsg_ind = VERR_DISCONN_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->desc_list_len !=
		   nvmf_fc_lsdesc_len(sizeof(struct nvmf_fc_ls_disconnect_rqst))) {
		errmsg_ind = VERR_DISCONN_RQST_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->assoc_id.desc_tag !=
		   cpu_to_be32(FCNVME_LSDESC_ASSOC_ID)) {
		errmsg_ind = VERR_ASSOC_ID;
		rc = FCNVME_RJT_RC_INV_PARAM;
	} else if (rqst->assoc_id.desc_len !=
		   nvmf_fc_lsdesc_len(sizeof(struct nvmf_fc_lsdesc_assoc_id))) {
		errmsg_ind = VERR_ASSOC_ID_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->disconn_cmd.desc_tag !=
		   cpu_to_be32(FCNVME_LSDESC_DISCONN_CMD)) {
		rc = FCNVME_RJT_RC_INV_PARAM;
		errmsg_ind = VERR_DISCONN_CMD;
	} else if (rqst->disconn_cmd.desc_len !=
		   nvmf_fc_lsdesc_len(sizeof(struct nvmf_fc_lsdesc_disconn_cmd))) {
		errmsg_ind = VERR_DISCONN_CMD_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else {
		/* match an active association */
		assoc = nvmf_fc_ls_find_assoc(tgtport,
					      from_be64(&rqst->assoc_id.association_id));
		if (!assoc) {
			errmsg_ind = VERR_NO_ASSOC;
			rc = FCNVME_RJT_RC_INV_ASSOC;
		}
	}

	if (rc != FCNVME_RJT_RC_NONE) {
		SPDK_ERRLOG("Disconnect LS failed: %s\n",
			    validation_errors[errmsg_ind]);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   NVME_FC_MAX_LS_BUFFER_SIZE,
				   rqst->w0.ls_cmd,
				   rc, ec, 0);
		(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}

	else {
		/* format response */
		bzero(acc, sizeof(*acc));
		ls_rqst->rsp_len = sizeof(*acc);

		nvmf_fc_ls_format_rsp_hdr(acc, FCNVME_LS_ACC,
					  nvmf_fc_lsdesc_len(
						  sizeof(struct nvmf_fc_ls_disconnect_acc)),
					  FCNVME_LS_DISCONNECT);

		nvmf_fc_ls_disconnect_assoc(tgtport, ls_rqst, assoc->assoc_id);
	}
}

/* ************************ */
/* external functions       */

void
spdk_nvmf_bcm_fc_ls_init(struct spdk_nvmf_bcm_fc_port *fc_port)
{
	uint32_t ioqs_per_rq;
	uint32_t max_ioqs;
	uint32_t assocs_count;

	if (fc_port->ls_rsrc_pool.assocs_mptr) {
		return;
	}

	fc_port->ls_rsrc_pool.assocs_count = 0;
	fc_port->ls_rsrc_pool.conns_count = 0;
	TAILQ_INIT(&fc_port->ls_rsrc_pool.assoc_free_list);
	TAILQ_INIT(&fc_port->ls_rsrc_pool.fc_conn_free_list);

	/*
	 * XXX Here's the logic that equates the global maximum queue depth
	 * XXX and maximum IO queue number with the per-port HWQP resources.
	 */
	ioqs_per_rq = fc_port->io_queues[1].queues.rq_payload.num_buffers /
		      g_nvmf_tgt.opts.max_io_queue_depth;
	max_ioqs = ioqs_per_rq * (fc_port->max_io_queues - 1);
	assocs_count = max_ioqs / (g_nvmf_tgt.opts.max_queues_per_session - 1);

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS,
		      "Port %d, max_ioqs=%d, max assocs=%d\n",
		      fc_port->port_hdl, max_ioqs, assocs_count);

	fc_port->ls_rsrc_pool.assocs_mptr =
		malloc(assocs_count *
		       sizeof(struct spdk_nvmf_bcm_fc_association));

	if (fc_port->ls_rsrc_pool.assocs_mptr) {
		uint32_t i;
		struct spdk_nvmf_bcm_fc_association *assoc;
		uint32_t conns_cnt;

		for (i = 0; i < assocs_count; i++) {
			assoc = (struct spdk_nvmf_bcm_fc_association *)
				(fc_port->ls_rsrc_pool.assocs_mptr + (i *
						sizeof(struct spdk_nvmf_bcm_fc_association)));
			nvmf_fc_association_init(assoc);
			TAILQ_INSERT_TAIL(&fc_port->ls_rsrc_pool.assoc_free_list,
					  assoc, port_free_assoc_list_link);
		}
		fc_port->ls_rsrc_pool.assocs_count = assocs_count;

		conns_cnt = g_nvmf_tgt.opts.max_queues_per_session * assocs_count;
		fc_port->ls_rsrc_pool.conns_mptr = malloc(conns_cnt *
						   sizeof(struct spdk_nvmf_bcm_fc_conn));

		if (fc_port->ls_rsrc_pool.conns_mptr) {
			for (i = 0; i < conns_cnt; i++) {
				struct spdk_nvmf_bcm_fc_conn *fc_conn =
					(struct spdk_nvmf_bcm_fc_conn *)
					(fc_port->ls_rsrc_pool.conns_mptr + (i *
							sizeof(struct spdk_nvmf_bcm_fc_conn)));

				TAILQ_INSERT_TAIL(&fc_port->ls_rsrc_pool.fc_conn_free_list,
						  fc_conn, port_free_conn_list_link);
			}
			fc_port->ls_rsrc_pool.conns_count = conns_cnt;

		} else {
			SPDK_ERRLOG("***ERROR*** - create NVMF FC connection"
				    "pool failed (port %d)\n", fc_port->port_hdl);
			free(fc_port->ls_rsrc_pool.assocs_mptr);
			fc_port->ls_rsrc_pool.assocs_mptr = NULL;
			fc_port->ls_rsrc_pool.assocs_count = 0;
		}

	} else {
		SPDK_ERRLOG("***ERROR*** - create NVMF LS association pool"
			    " failed (port %d)\n", fc_port->port_hdl);
	}
}

void
spdk_nvmf_bcm_fc_ls_fini(struct spdk_nvmf_bcm_fc_port *fc_port)
{
	if (fc_port->ls_rsrc_pool.assocs_mptr) {
		uint32_t i;
		struct spdk_nvmf_bcm_fc_association *assoc;

		for (i = 0; i < fc_port->ls_rsrc_pool.assocs_count; i++) {
			assoc = (struct spdk_nvmf_bcm_fc_association *)
				(fc_port->ls_rsrc_pool.assocs_mptr + (i *
						sizeof(struct spdk_nvmf_bcm_fc_association)));
			nvmf_fc_association_fini(assoc);
		}
		free(fc_port->ls_rsrc_pool.assocs_mptr);
		fc_port->ls_rsrc_pool.assocs_mptr = NULL;
		fc_port->ls_rsrc_pool.assocs_count = 0;
	}
	if (fc_port->ls_rsrc_pool.conns_mptr) {
		free(fc_port->ls_rsrc_pool.conns_mptr);
		fc_port->ls_rsrc_pool.conns_mptr = NULL;
		fc_port->ls_rsrc_pool.conns_count = 0;
	}
}

void
spdk_nvmf_bcm_fc_handle_ls_rqst(struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst)
{
	struct nvmf_fc_ls_rqst_w0 *w0 =
		(struct nvmf_fc_ls_rqst_w0 *)ls_rqst->rqstbuf.virt;
	uint32_t s_id = ls_rqst->s_id;
	struct spdk_nvmf_bcm_fc_nport *tgtport = ls_rqst->nport;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "LS cmd=%d\n", w0->ls_cmd);

	switch (w0->ls_cmd) {
	case FCNVME_LS_CREATE_ASSOCIATION:
		nvmf_fc_ls_process_cass(s_id, tgtport, ls_rqst);
		break;
	case FCNVME_LS_CREATE_CONNECTION:
		nvmf_fc_ls_process_cioc(tgtport, ls_rqst);
		break;
	case FCNVME_LS_DISCONNECT:
		nvmf_fc_ls_process_disc(tgtport, ls_rqst);
		break;
	default:
		SPDK_ERRLOG("Invalid LS cmd=%d\n", w0->ls_cmd);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(ls_rqst->rspbuf.virt,
				   NVME_FC_MAX_LS_BUFFER_SIZE, w0->ls_cmd,
				   FCNVME_RJT_RC_INVAL, FCNVME_RJT_EXP_NONE, 0);
		(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}
}

int
spdk_nvmf_bcm_fc_delete_association(struct spdk_nvmf_bcm_fc_nport *tgtport,
				    uint64_t assoc_id, bool send_abts,
				    spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
				    void *cb_data)
{
	return nvmf_fc_delete_association(tgtport, assoc_id, send_abts,
					  del_assoc_cb, cb_data, false);
}

static void
nvmf_fc_subsys_connect_event(void *arg1, void *arg2)
{
	struct spdk_nvmf_request *req = arg1;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "Subsystem connect callback for "
		      "request %p\n", req);
	spdk_nvmf_handle_connect(req);
}

/* Functions defined in struct spdk_nvmf_subsystem's ops field
 * (spdk_nvmf_ctrlr_ops) for connect & disconnnect callbacks */
void
spdk_nvmf_bcm_fc_subsys_connect_cb(void *cb_ctx, struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_bcm_fc_request *fc_req = spdk_nvmf_bcm_fc_get_fc_req(req);
	struct spdk_event *event;

	event = spdk_event_allocate(spdk_env_get_master_lcore(), nvmf_fc_subsys_connect_event, req, NULL);
	spdk_post_event(fc_req->hwqp->context, event);
}

void spdk_nvmf_bcm_fc_subsys_disconnect_cb(void *cb_ctx,
		struct spdk_nvmf_conn *conn)
{
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "Subsystem disconnect callback for "
		      "connection %p\n", conn);
	/* There are cases where a disconnect may be done before
	 * a session exists for a connection.
	 * Eg. A connection was created, but the NVMe connect
	 * command was still not received. In the meantime a
	 * link-down, adapter-dump, host-reboot etc. happens.
	 * We delete the associations and hence the connections but
	 * there is no session for that connection.
	 * Safe to do a NULL check here. The session_disconnect
	 * is No-op for FC in terms of connection removal and
	 * destruction. No leaks there.
	 */
	if (conn->sess != NULL) {
		spdk_nvmf_session_disconnect(conn);
	}
}

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf_bcm_fc_ls", SPDK_TRACE_NVMF_BCM_FC_LS)
