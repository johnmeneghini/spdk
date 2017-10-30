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

#include "bcm_fc.h"

static uint32_t g_max_conns_per_assoc = 0;

/* The connection ID (8 bytes)has the following format:
 * byte0: queue number (0-255) - currently there is a max of 16 queues
 * byte1 - byte2: unique value per queue number (0-65535)
 * byte3 - byte7: unused */
#define SPDK_NVMF_FC_BCM_MRQ_CONNID_QUEUE_MASK  0xff
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
	VERR_NO_RPORT = 33
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
	"SQ size = 0 or too big"
	"No Remote Port"
};

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
	bool send_disconn;
	bool send_abts;
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

/* global LS variables */
static bool g_fc_ls_init_done = false;
struct spdk_mempool *g_fc_ls_assoc_pool = NULL;
struct spdk_mempool *g_fc_ls_conn_pool = NULL;

#define be32_to_cpu(i) from_be32((i))
#define be16_to_cpu(i) from_be16((i))

static inline uint32_t cpu_to_be32(uint32_t in)
{
	uint32_t t;
	to_be32(&t, in);
	return (uint32_t)t;
}

static inline uint32_t nvmf_fc_lsdesc_len(size_t sz)
{
	uint32_t t;
	to_be32(&t, sz - (2 * sizeof(uint32_t)));
	return (uint32_t)t;
}

static struct spdk_nvmf_subsystem *
nvmf_fc_ls_valid_subnqn(uint8_t *subnqn)
{
	if (!memchr(subnqn, '\0', FCNVME_ASSOC_SUBNQN_LEN))
		return NULL;
	return spdk_nvmf_find_subsystem((const char *)subnqn);
}

static bool
nvmf_fc_ls_valid_hostnqn(struct spdk_nvmf_subsystem *subsystem,
			 uint8_t *hostnqn)
{
	if (!memchr(hostnqn, '\0', FCNVME_ASSOC_HOSTNQN_LEN))
		return false;
	return spdk_nvmf_subsystem_host_allowed(subsystem, (const char *)hostnqn);
}

static inline void
nvmf_fc_xmt_ls_rsp(struct spdk_nvmf_bcm_fc_nport *tgtport,
		   struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst)
{
	spdk_nvmf_bcm_fc_xmt_ls_rsp(tgtport, ls_rqst);
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

	/* allocate rqst/resp buffers to send LS disconnect to initiator */
	assoc->snd_disconn_bufs.rqst.virt = spdk_dma_malloc(
			sizeof(struct nvmf_fc_lsdesc_disconn_cmd)  +
			sizeof(struct nvmf_fc_ls_disconnect_acc) +
			(2 * sizeof(bcm_sge_t)), 0, NULL);

	if (assoc->snd_disconn_bufs.rqst.virt == NULL) {
		SPDK_ERRLOG("***ERROR*** - no memory for send LS disconnect buffers");
		abort();
	}

	assoc->snd_disconn_bufs.rqst.phys = spdk_vtophys(
			assoc->snd_disconn_bufs.rqst.virt);
	assoc->snd_disconn_bufs.rqst.len =
		sizeof(struct nvmf_fc_lsdesc_disconn_cmd);

	assoc->snd_disconn_bufs.rsp.virt = assoc->snd_disconn_bufs.rqst.virt +
					   sizeof(struct nvmf_fc_lsdesc_disconn_cmd);
	assoc->snd_disconn_bufs.rsp.phys = assoc->snd_disconn_bufs.rqst.phys +
					   sizeof(struct nvmf_fc_lsdesc_disconn_cmd);
	assoc->snd_disconn_bufs.rsp.len =
		sizeof(struct nvmf_fc_ls_disconnect_acc);

	assoc->snd_disconn_bufs.sgl.virt = assoc->snd_disconn_bufs.rsp.virt +
					   sizeof(struct nvmf_fc_ls_disconnect_acc);
	assoc->snd_disconn_bufs.sgl.phys = assoc->snd_disconn_bufs.rsp.phys +
					   sizeof(struct nvmf_fc_ls_disconnect_acc);
	assoc->snd_disconn_bufs.sgl.len = 2 * sizeof(bcm_sge_t);
}

static void
nvmf_fc_association_fini(struct spdk_nvmf_bcm_fc_association *assoc)
{
	if (assoc->snd_disconn_bufs.rqst.virt) {
		spdk_dma_free(assoc->snd_disconn_bufs.rqst.virt);
		assoc->snd_disconn_bufs.rqst.virt = NULL;
	}
}

static struct spdk_nvmf_bcm_fc_association *
nvmf_fc_ls_new_association(uint32_t s_id,
			   struct spdk_nvmf_bcm_fc_nport *tgtport,
			   struct spdk_nvmf_bcm_fc_remote_port_info *rport,
			   struct nvmf_fc_lsdesc_cr_assoc_cmd *a_cmd,
			   struct spdk_nvmf_subsystem *subsys, uint16_t rpi)
{
	struct spdk_nvmf_bcm_fc_association *assoc =
		(struct spdk_nvmf_bcm_fc_association *)
		spdk_mempool_get(g_fc_ls_assoc_pool);

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");

	assert(rport);
	if (!rport) {
		SPDK_ERRLOG("rport is null.\n");
		return NULL;
	}

	if (assoc) {
		/* initialize association
		 * IMPORTANT: do not do a memset on association to clear it!
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
		assoc->snd_disconn_bufs.rpi = rpi;

		/* add association to target port's association list */
		TAILQ_INSERT_TAIL(&tgtport->fc_associations, assoc, link);
		tgtport->assoc_count++;
		rport->assoc_count++;
	} else {
		SPDK_ERRLOG("out of associations\n");
	}

	return assoc;
}

static inline void
nvmf_fc_ls_free_association(struct spdk_nvmf_bcm_fc_association *assoc)
{
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");
	assoc->assoc_state = SPDK_NVMF_BCM_FC_OBJECT_ZOMBIE;
	spdk_mempool_put(g_fc_ls_assoc_pool, (void *)assoc);
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
			  enum conn_type type, uint16_t qid, uint16_t max_q_size,
			  uint16_t esrp_ratio, uint16_t rpi)
{
	struct spdk_nvmf_bcm_fc_conn *fc_conn =
		(struct spdk_nvmf_bcm_fc_conn *)
		spdk_mempool_get(g_fc_ls_conn_pool);

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");

	if (fc_conn) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");
		memset((void *)fc_conn, 0, sizeof(*fc_conn));
		fc_conn->conn.type = type;
		fc_conn->conn.transport = &spdk_nvmf_transport_bcm_fc;
		fc_conn->conn.qid = qid;
		fc_conn->conn.sq_head_max = max_q_size;
		fc_conn->esrp_ratio = esrp_ratio;
		fc_conn->fc_assoc = assoc;
		fc_conn->rpi = rpi;

		TAILQ_INIT(&fc_conn->pending_queue);
	} else {
		SPDK_ERRLOG("out of connections\n");
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");

	return fc_conn;
}

static inline void
nvmf_fc_ls_free_connection(struct spdk_nvmf_bcm_fc_conn *fc_conn)
{
	if (fc_conn->conn.sess) {
		/* session_remove_connt removes conn from subsystem */
		/* if last conn for session, deletes session too */
		fc_conn->conn.transport->session_remove_conn(fc_conn->conn.sess,
				&fc_conn->conn);
	}

	spdk_mempool_put(g_fc_ls_conn_pool, (void *)fc_conn);
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
		if (assoc->assoc_id == assoc_id)
			break;
	}
	return assoc;
}

static inline struct spdk_nvmf_bcm_fc_hwqp *
nvmf_fc_ls_get_hwqp(struct spdk_nvmf_bcm_fc_nport *tgtport, uint64_t conn_id)
{
	struct spdk_nvmf_bcm_fc_port *fc_port = tgtport->fc_port;
	return (&fc_port->io_queues[(conn_id &
				     SPDK_NVMF_FC_BCM_MRQ_CONNID_QUEUE_MASK) %
				    fc_port->max_io_queues]);
}

static inline uint64_t
nvmf_fc_gen_conn_id(uint32_t qnum, struct spdk_nvmf_bcm_fc_hwqp *hwqp)
{
	return ((uint64_t) qnum |
		(hwqp->cid_cnt++ << SPDK_NVMF_FC_BCM_MRQ_CONNID_UV_SHIFT));
}

static inline struct spdk_nvmf_bcm_fc_hwqp *
nvmf_fc_ls_assign_conn_to_q(struct spdk_nvmf_bcm_fc_association *assoc,
			    uint64_t *conn_id)
{
	struct spdk_nvmf_bcm_fc_nport *tgtport = assoc->tgtport;
	struct spdk_nvmf_bcm_fc_port *fc_port = tgtport->fc_port;
	uint32_t q_ind;
	uint32_t min_q_ind = 0;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");

	/* find queue with minimum number of connections */
	for (q_ind = 1; q_ind < fc_port->max_io_queues; q_ind++) {
		if (fc_port->io_queues[q_ind].num_conns <
		    fc_port->io_queues[min_q_ind].num_conns)
			min_q_ind = q_ind;
	}

	/* bump the number of connection count now in case
	 * another connection request comes in while processing this
	 * conn in the poller */
	fc_port->io_queues[min_q_ind].num_conns++;

	/* create connection ID */
	*conn_id = nvmf_fc_gen_conn_id(min_q_ind,
				       &fc_port->io_queues[min_q_ind]);
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS,
		      "q_num %d, conn_id %lx\n", min_q_ind, *conn_id);
	return &fc_port->io_queues[min_q_ind];
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

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS,
		      "conn_id = %lx\n", fc_conn->conn_id);

	/* insert conn in association's connection list */
	TAILQ_INSERT_TAIL(&assoc->fc_conns, fc_conn, assoc_link);
	assoc->conn_count++;

	if (dp->assoc_conn) {
		struct nvmf_fc_ls_cr_assoc_acc *acc =
			(struct nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
		assoc->assoc_id = fc_conn->conn_id; /* assoc_id = conn_id */
		/* put connection and association IDin response */
		to_be64(&acc->conn_id.connection_id, fc_conn->conn_id);
		acc->assoc_id.association_id = acc->conn_id.connection_id;
	} else {
		struct nvmf_fc_ls_cr_conn_acc *acc =
			(struct nvmf_fc_ls_cr_conn_acc *)ls_rqst->rspbuf.virt;
		/* put connection ID in response */
		to_be64(&acc->conn_id.connection_id, fc_conn->conn_id);
	}

	/* send LS response */
	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);

	nvmf_fc_ls_free_op_ctx(opd);
}

static void
nvmf_fc_ls_add_conn_to_poller(
	struct spdk_nvmf_bcm_fc_association *assoc,
	struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst,
	struct spdk_nvmf_bcm_fc_conn *fc_conn,
	bool assoc_conn)
{
	union nvmf_fc_ls_op_ctx *opd = nvmf_fc_ls_new_op_ctx();

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");

	if (opd) {
		struct nvmf_fc_ls_add_conn_api_data *api_data =
				&opd->add_conn;
		/* assign connection to (poller) queue */
		fc_conn->hwqp = nvmf_fc_ls_assign_conn_to_q(assoc, &fc_conn->conn_id);
		api_data->args.fc_conn = fc_conn;
		api_data->args.cb_info.cb_func = nvmf_fc_ls_add_conn_cb;
		api_data->args.cb_info.cb_data = (void *)opd;
		api_data->assoc = assoc;
		api_data->ls_rqst = ls_rqst;
		api_data->assoc_conn = assoc_conn;
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS,
			      "conn_id = %ld\n", fc_conn->conn_id);
		spdk_nvmf_bcm_fc_poller_api(api_data->args.fc_conn->hwqp->lcore_id,
					    SPDK_NVMF_BCM_FC_POLLER_API_ADD_CONNECTION,
					    &api_data->args);
	} else {
		/* send failure response */
		struct nvmf_fc_ls_cr_assoc_rqst *rqst =
			(struct nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
		struct nvmf_fc_ls_cr_assoc_acc *acc =
			(struct nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
		SPDK_ERRLOG("allocate data for add conn op failed\n");
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   NVME_FC_MAX_LS_BUFFER_SIZE, rqst->w0.ls_cmd,
				   FCNVME_RJT_RC_UNAB,
				   FCNVME_RJT_EXP_INSUF_RES, 0);
		nvmf_fc_xmt_ls_rsp(assoc->tgtport, ls_rqst);
		nvmf_fc_ls_free_connection(fc_conn);
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");
}

/* Delete association functions */

static void
nvmf_fc_do_del_assoc_cbs(struct spdk_nvmf_bcm_fc_association *assoc,
			 int ret)
{
	union nvmf_fc_ls_op_ctx *opd =
			(union nvmf_fc_ls_op_ctx *) assoc->ls_del_op_ctx;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS,
		      "peforming delete assoc. callbacks\n");
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
	struct nvmf_fc_ls_disconnect_rqst *dc_rqst =
		(struct nvmf_fc_ls_disconnect_rqst *)
		assoc->snd_disconn_bufs.rqst.virt;

	memset(dc_rqst, 0, sizeof(struct nvmf_fc_ls_disconnect_rqst));

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

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "Send LS disconnect\n");
	if (spdk_nvmf_bcm_fc_xmt_srsr_req(&assoc->tgtport->fc_port->ls_queue,
					  &assoc->snd_disconn_bufs, 0, 0)) {
		SPDK_ERRLOG("Error sending LS disconnect\n");
	}
}

static void
nvmf_fc_check_and_send_disconnect(struct spdk_nvmf_bcm_fc_association *assoc)
{
	if (assoc->ls_del_op_ctx) {
		/* first one on delete list will have send disconnect flag */
		union nvmf_fc_ls_op_ctx *opd =
				(union nvmf_fc_ls_op_ctx *)assoc->ls_del_op_ctx;
		if (opd->del_assoc.send_disconn) {
			nvmf_fc_send_ls_disconnect(assoc);
		}
	}
}

static void
nvmf_fc_del_all_conns_cb(void *cb_data, spdk_nvmf_bcm_fc_poller_api_ret_t ret)
{
	union nvmf_fc_ls_op_ctx *opd =
			(union nvmf_fc_ls_op_ctx *)cb_data;
	struct nvmf_fc_delete_assoc_api_data *dp = &opd->del_assoc;
	struct spdk_nvmf_bcm_fc_association *assoc = dp->assoc;

	/* Assumption here is that there will be no error (i.e. ret=success).
	 * Since connections are deleted in parallel, nothing can be
	 * done anyway if there is an error because we need to complete
	 * all connection deletes and callback to caller */

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");
	if (assoc->subsystem) {
		assoc->subsystem->disconnect_cb(assoc->subsystem->cb_ctx,
						&dp->args.fc_conn->conn);
	}

	/* remove connection from association's connection list */
	TAILQ_REMOVE(&assoc->fc_conns, dp->args.fc_conn, assoc_link);
	nvmf_fc_ls_free_connection(dp->args.fc_conn);
	dp->args.hwqp->num_conns--;

	if (--assoc->conn_count == 0) {
		/* last connection - remove association from target port's
		 * association list */
		struct spdk_nvmf_bcm_fc_nport *tgtport = assoc->tgtport;

		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS,
			      "remove assoc. %lx\n", assoc->assoc_id);
		TAILQ_REMOVE(&tgtport->fc_associations, assoc, link);
		tgtport->assoc_count--;
		assoc->rport->assoc_count--;

		nvmf_fc_check_and_send_disconnect(assoc);

		/* perform callbacks to all callers to delete association */
		nvmf_fc_do_del_assoc_cbs(assoc, 0);

		nvmf_fc_ls_free_association(assoc);
	}

	nvmf_fc_ls_free_op_ctx(opd);
}

/* Disconnect (association) request functions */

static void
nvmf_fc_ls_disconnect_assoc_cb(void *cb_data, uint32_t err)
{
	union nvmf_fc_ls_op_ctx *opd = (union nvmf_fc_ls_op_ctx *)cb_data;
	struct nvmf_fc_ls_disconn_assoc_api_data *dp = &opd->disconn_assoc;
	struct spdk_nvmf_bcm_fc_nport *tgtport = dp->tgtport;
	struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst = dp->ls_rqst;


	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");
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

	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);

	nvmf_fc_ls_free_op_ctx(opd);
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");
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
		ret = spdk_nvmf_bcm_fc_delete_association(tgtport, assoc_id,
				false, false,
				nvmf_fc_ls_disconnect_assoc_cb,
				api_data);

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
					   FCNVME_RJT_RC_UNAB :
					   FCNVME_RJT_RC_LOGIC,
					   ret == SPDK_ERR_NOMEM ?
					   FCNVME_RJT_EXP_INSUF_RES :
					   FCNVME_RJT_EXP_NONE, 0);
			nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
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
				   FCNVME_RJT_RC_UNAB,
				   FCNVME_RJT_EXP_INSUF_RES, 0);
		nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}
}

/* **************************** */
/* LS Reqeust Handler Functions */

static spdk_err_t
nvmf_fc_ls_find_rport_from_sid(uint32_t s_id,
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

static void
nvmf_fc_ls_create_association(uint32_t s_id,
			      struct spdk_nvmf_bcm_fc_nport *tgtport,
			      struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst)
{
	struct nvmf_fc_ls_cr_assoc_rqst *rqst =
		(struct nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
	struct nvmf_fc_ls_cr_assoc_acc *acc =
		(struct nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_bcm_fc_association *assoc;
	struct spdk_nvmf_bcm_fc_conn *fc_conn;
	struct spdk_nvmf_subsystem *subsys = NULL;
	struct spdk_nvmf_bcm_fc_remote_port_info *rport = NULL;
	int errmsg_ind = 0;
	uint8_t rc = FCNVME_RJT_RC_NONE;
	uint8_t ec = FCNVME_RJT_EXP_NONE;
	spdk_err_t err;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS,
		      "LS_CA: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d, sq_size=%d\n",
		      ls_rqst->rqst_len, be32_to_cpu(&rqst->desc_list_len),
		      be32_to_cpu(&rqst->assoc_cmd.desc_len),
		      be16_to_cpu(&rqst->assoc_cmd.sqsize));
	if (ls_rqst->rqst_len < LS_CREATE_ASSOC_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd req len = %d, should be at leastt %d\n",
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
	} else if ((subsys = nvmf_fc_ls_valid_subnqn(rqst->assoc_cmd.subnqn))
		   == NULL) {
		errmsg_ind = VERR_SUBNQN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_SUBNQN;
	} else if (!nvmf_fc_ls_valid_hostnqn(subsys, rqst->assoc_cmd.hostnqn)) {
		errmsg_ind = VERR_HOSTNQN;
		rc = FCNVME_RJT_RC_INV_HOST;
		ec = FCNVME_RJT_EXP_INV_HOSTNQN;
	} else if (rqst->assoc_cmd.sqsize == 0 ||
		   from_be16(&rqst->assoc_cmd.sqsize) >
		   g_nvmf_tgt.config.max_aq_depth) {
		errmsg_ind = VERR_SQSIZE;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_SQ_SIZE;
	} else {
		/* new association w/ admin queue */
		err = nvmf_fc_ls_find_rport_from_sid(s_id, tgtport, &rport);
		if (err != SPDK_SUCCESS) {
			SPDK_ERRLOG("Remote port not found.\n");
			errmsg_ind = VERR_NO_RPORT;
			rc = FCNVME_RJT_RC_INSUFF_RES;
			ec = FCNVME_RJT_EXP_INSUF_RES;
		} else {
			assoc = nvmf_fc_ls_new_association(s_id, tgtport, rport,
							   &rqst->assoc_cmd,
							   subsys,
							   ls_rqst->rpi);
			if (!assoc) {
				errmsg_ind = VERR_ASSOC_ALLOC_FAIL;
				rc = FCNVME_RJT_RC_INSUFF_RES;
				ec = FCNVME_RJT_EXP_INSUF_RES;
			} else { // alloc admin q (i.e. connection)
				fc_conn = nvmf_fc_ls_new_connection(assoc,
								    CONN_TYPE_AQ, 0,
								    from_be16(&rqst->assoc_cmd.sqsize),
								    from_be16(&rqst->assoc_cmd.ersp_ratio),
								    ls_rqst->rpi);
				if (!fc_conn) {
					errmsg_ind = VERR_CONN_ALLOC_FAIL;
					rc = FCNVME_RJT_RC_INSUFF_RES;
					ec = FCNVME_RJT_EXP_INSUF_RES;
				}
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
		nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}

	else {
		/* format accept response */
		memset(acc, 0, sizeof(*acc));

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
					      fc_conn, true);
	}
}

static void
nvmf_fc_ls_create_connection(struct spdk_nvmf_bcm_fc_nport *tgtport,
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

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS,
		      "LS_CC: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d, sq_size=%d\n",
		      ls_rqst->rqst_len, be32_to_cpu(&rqst->desc_list_len),
		      be32_to_cpu(&rqst->connect_cmd.desc_len),
		      be16_to_cpu(&rqst->connect_cmd.sqsize));

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
	} else if (rqst->connect_cmd.sqsize == 0 ||
		   from_be16(&rqst->connect_cmd.sqsize) >
		   g_nvmf_tgt.config.max_queue_depth) {
		errmsg_ind = VERR_SQSIZE;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_SQ_SIZE;
	} else {
		/* find association */
		assoc = nvmf_fc_ls_find_assoc(tgtport,
					      from_be64(&rqst->assoc_id.association_id));
		if (!assoc) {
			errmsg_ind = VERR_NO_ASSOC;
			rc = FCNVME_RJT_RC_INV_ASSOC;
		} else { // alloc IO q (i.e. connection)
			if (assoc->conn_count < g_max_conns_per_assoc) {
				fc_conn = nvmf_fc_ls_new_connection(assoc,
								    CONN_TYPE_IOQ,
								    from_be16(&rqst->connect_cmd.qid),
								    from_be16(&rqst->connect_cmd.sqsize),
								    from_be16(&rqst->connect_cmd.ersp_ratio),
								    ls_rqst->rpi);
				if (!fc_conn) {
					errmsg_ind = VERR_CONN_ALLOC_FAIL;
					rc = FCNVME_RJT_RC_INSUFF_RES;
					ec = FCNVME_RJT_EXP_INSUF_RES;
				}
			} else {
				errmsg_ind = VERR_CONN_TOO_MANY;
				rc = FCNVME_RJT_RC_INV_PARAM;
				ec =  FCNVME_RJT_EXP_INV_Q_ID;
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
		nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}

	else {
		/* format accept response */
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");
		memset(acc, 0, sizeof(*acc));
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
		nvmf_fc_ls_add_conn_to_poller(assoc, ls_rqst, fc_conn, false);
	}
}

static void
nvmf_fc_ls_disconnect(struct spdk_nvmf_bcm_fc_nport *tgtport,
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

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS,
		      "LS_DC: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d\n",
		      ls_rqst->rqst_len, be32_to_cpu(&rqst->desc_list_len),
		      be32_to_cpu(&rqst->disconn_cmd.desc_len));

	memset(acc, 0, sizeof(*acc));

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

		/* check if association already in process of being deleted */
		else if (assoc->assoc_state == SPDK_NVMF_BCM_FC_OBJECT_TO_BE_DELETED) {
			errmsg_ind = VERR_NO_ASSOC;
		}
	}

	if (rc != FCNVME_RJT_RC_NONE) {
		SPDK_ERRLOG("Disconnect LS failed: %s\n",
			    validation_errors[errmsg_ind]);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   NVME_FC_MAX_LS_BUFFER_SIZE,
				   rqst->w0.ls_cmd,
				   rc, ec, 0);
		nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}

	else {
		/* format response */
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
spdk_nvmf_bcm_fc_ls_init(void)
{
	g_max_conns_per_assoc = g_nvmf_tgt.config.max_queues_per_session;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS,
		      "max_assocations = %d, max conns = %d\n",
		      g_nvmf_tgt.config.max_associations, g_max_conns_per_assoc);

	if (!g_fc_ls_init_done) {
		g_fc_ls_init_done = true;
		/* allocate associations */
		g_fc_ls_assoc_pool =
			spdk_mempool_create("NVMF_FC_ASSOC_POOL",
					    (size_t)(g_nvmf_tgt.config.max_associations),
					    sizeof(struct spdk_nvmf_bcm_fc_association),
					    0, SPDK_ENV_SOCKET_ID_ANY);
		if (g_fc_ls_assoc_pool) {
			uint32_t i;
			struct spdk_nvmf_bcm_fc_association *assoc;

			for (i = 0; i < g_nvmf_tgt.config.max_associations; i++) {
				assoc = (struct spdk_nvmf_bcm_fc_association *)
					spdk_mempool_get(g_fc_ls_assoc_pool);
				if (assoc) {
					nvmf_fc_association_init(assoc);
					spdk_mempool_put(g_fc_ls_assoc_pool, assoc);
				}
			}

			/* allocate connections */
			g_fc_ls_conn_pool =
				spdk_mempool_create("NVMF_FC_CONN_POOL",
						    (size_t)(g_nvmf_tgt.config.max_associations *
							     g_max_conns_per_assoc),
						    sizeof(struct spdk_nvmf_bcm_fc_conn),
						    0, SPDK_ENV_SOCKET_ID_ANY);
			if (g_fc_ls_conn_pool == NULL) {
				SPDK_ERRLOG("***ERROR*** - create NVMF FC connection pool failed\n");
				abort();
			}

		} else {
			SPDK_ERRLOG("***ERROR*** - create NVMF LS association pool failed - aborting\n");
			abort();
		}
	}
}

void
spdk_nvmf_bcm_fc_ls_fini(void)
{
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");

	if (g_fc_ls_init_done) {
		if (g_fc_ls_assoc_pool) {
			uint32_t i;
			struct spdk_nvmf_bcm_fc_association *assoc;

			for (i = 0; i < g_nvmf_tgt.config.max_associations; i++) {
				assoc = (struct spdk_nvmf_bcm_fc_association *)
					spdk_mempool_get(g_fc_ls_assoc_pool);
				if (assoc) {
					nvmf_fc_association_fini(assoc);
					spdk_mempool_put(g_fc_ls_assoc_pool, assoc);
				}
			}

			spdk_mempool_free(g_fc_ls_assoc_pool);
			g_fc_ls_assoc_pool = NULL;
		}
		if (g_fc_ls_conn_pool) {
			spdk_mempool_free(g_fc_ls_conn_pool);
			g_fc_ls_conn_pool = NULL;
		}
		g_fc_ls_init_done = false;
	}
}

void
spdk_nvmf_bcm_fc_handle_ls_rqst(uint32_t s_id,
				struct spdk_nvmf_bcm_fc_nport *tgtport,
				struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst)
{
	struct nvmf_fc_ls_rqst_w0 *w0 =
		(struct nvmf_fc_ls_rqst_w0 *)ls_rqst->rqstbuf.virt;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS,
		      "LS cmd=%d\n", w0->ls_cmd);

	switch (w0->ls_cmd) {
	case FCNVME_LS_CREATE_ASSOCIATION:
		nvmf_fc_ls_create_association(s_id, tgtport, ls_rqst);
		break;
	case FCNVME_LS_CREATE_CONNECTION:
		nvmf_fc_ls_create_connection(tgtport, ls_rqst);
		break;
	case FCNVME_LS_DISCONNECT:
		nvmf_fc_ls_disconnect(tgtport, ls_rqst);
		break;
	default:
		SPDK_ERRLOG("Invalid LS cmd=%d\n", w0->ls_cmd);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(ls_rqst->rspbuf.virt,
				   NVME_FC_MAX_LS_BUFFER_SIZE, w0->ls_cmd,
				   FCNVME_RJT_RC_INVAL, FCNVME_RJT_EXP_NONE, 0);
		nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}
}

int
spdk_nvmf_bcm_fc_delete_association(struct spdk_nvmf_bcm_fc_nport *tgtport,
				    uint64_t assoc_id, bool send_disconn, bool send_abts,
				    spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
				    void *cb_data)
{
	union nvmf_fc_ls_op_ctx *opd;
	struct nvmf_fc_delete_assoc_api_data *api_data;
	struct spdk_nvmf_bcm_fc_conn *fc_conn;
	struct spdk_nvmf_bcm_fc_association *assoc =
		nvmf_fc_ls_find_assoc(tgtport, assoc_id);

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");

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
	api_data->del_assoc_cb = del_assoc_cb;
	api_data->del_assoc_cb_data = cb_data;
	api_data->args.cb_info.cb_data = opd;
	api_data->send_disconn = send_disconn;
	nvmf_fc_ls_append_del_cb_ctx(assoc, opd);

	if (assoc->assoc_state == SPDK_NVMF_BCM_FC_OBJECT_TO_BE_DELETED) {
		/* association already being deleted */
		api_data->send_disconn = false; /* don't send disconn for this one */
		return 0;
	}

	/* mark assoc. to be deleted */
	assoc->assoc_state = SPDK_NVMF_BCM_FC_OBJECT_TO_BE_DELETED;

	/* delete all of the association's connections */
	TAILQ_FOREACH(fc_conn, &assoc->fc_conns, assoc_link) {
		/* create context for delete connection API */
		opd = nvmf_fc_ls_new_op_ctx();
		if (opd) {
			api_data = &opd->del_assoc;
			api_data->args.fc_conn = fc_conn;
			api_data->assoc = assoc;
			api_data->args.cb_info.cb_func = nvmf_fc_del_all_conns_cb;
			api_data->args.cb_info.cb_data = opd;
			api_data->send_abts = send_abts;
			api_data->args.hwqp = nvmf_fc_ls_get_hwqp(assoc->tgtport,
					      api_data->args.fc_conn->conn_id);

			SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS,
				      "conn_id = %lx\n", fc_conn->conn_id);
			spdk_nvmf_bcm_fc_poller_api(api_data->args.hwqp->lcore_id,
						    SPDK_NVMF_BCM_FC_POLLER_API_DEL_CONNECTION,
						    &api_data->args);

		} else { /* hopefully this doesn't happen */
			SPDK_ERRLOG("Mem alloc failed for del conn op data");
			return SPDK_ERR_NOMEM;
		}

	}

	return 0;
}

/* Functions defined in struct spdk_nvmf_subsystem's ops field
 * (spdk_nvmf_ctrlr_ops) for connect & disconnnect callbacks */

void spdk_nvmf_bcm_fc_subsys_connect_cb(void *cb_ctx,
					struct spdk_nvmf_request *req)
{
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");
	spdk_nvmf_handle_connect(req);
}

void spdk_nvmf_bcm_fc_subsys_disconnect_cb(void *cb_ctx,
		struct spdk_nvmf_conn *conn)
{
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_LS, "\n");
	spdk_nvmf_session_disconnect(conn);
}

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf_bcm_fc_ls", SPDK_TRACE_NVMF_BCM_FC_LS)
