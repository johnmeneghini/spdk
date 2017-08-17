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

/* uncomment for trace
#define DEBUG_FC_LS_TRACE 1 */

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
#include "bcm_sli_fc.h"

static uint32_t g_max_conns_per_assoc = 0;

extern int
bcm_nvmf_fc_xmt_ls_rsp(struct spdk_nvmf_fc_nport *tgtport,
		       struct nvmf_fc_ls_rqst *ls_rqst);

/* The connection ID (8 bytes)has the following format:
 * byte0: queue number (0-255) - currently there is a max of 16 queues
 * byte1 - byte2: unique value per queue number (0-65535)
 * byte3 - byte7: unused */
#define SPDK_NVMF_FC_BCM_MRQ_CONNID_QUEUE_MASK  0xff
#define SPDK_NVMF_FC_BCM_MRQ_CONNID_UV_SHIFT    8

struct nvmf_fc_ls_rqst_w0 {
	uint8_t	ls_cmd;			/* FCNVME_LS_xxx */
	uint8_t zeros[3];
};

/* FCNVME_LSDESC_RQST */
struct nvmf_fc_lsdesc_rqst {
	__be32 desc_tag;		/* FCNVME_LSDESC_xxx */
	__be32 desc_len;
	struct nvmf_fc_ls_rqst_w0 w0;
	__be32 rsvd12;
};

// LS accept header
struct nvmf_fc_ls_acc_hdr {
	struct nvmf_fc_ls_rqst_w0 w0;
	__be32 desc_list_len;
	struct nvmf_fc_lsdesc_rqst rqst;
	/* Followed by cmd-specific ACC descriptors, see next definitions */
};

/* for this implementation, assume small single frame rqst/rsp */
#define NVME_FC_MAX_LS_BUFFER_SIZE		2048

/* FC-NVME Link Services */
enum {
	FCNVME_LS_RSVD = 0,
	FCNVME_LS_RJT = 1,
	FCNVME_LS_ACC = 2,
	FCNVME_LS_CREATE_ASSOCIATION = 3,
	FCNVME_LS_CREATE_CONNECTION	= 4,
	FCNVME_LS_DISCONNECT = 5,
};

/* FC-NVME Link Service Descriptors */
enum {
	FCNVME_LSDESC_RSVD = 0x0,
	FCNVME_LSDESC_RQST = 0x1,
	FCNVME_LSDESC_RJT = 0x2,
	FCNVME_LSDESC_CREATE_ASSOC_CMD = 0x3,
	FCNVME_LSDESC_CREATE_CONN_CMD = 0x4,
	FCNVME_LSDESC_DISCONN_CMD = 0x5,
	FCNVME_LSDESC_CONN_ID = 0x6,
	FCNVME_LSDESC_ASSOC_ID = 0x7,
};

/* Disconnect Scope Values */
enum {
	FCNVME_DISCONN_ASSOCIATION = 0,
	FCNVME_DISCONN_CONNECTION = 1,
};

struct nvmf_fc_lsdesc_conn_id {
	__be32 desc_tag;
	__be32 desc_len;
	__be64 connection_id;
};

/* FCNVME_LSDESC_ASSOC_ID */
struct nvmf_fc_lsdesc_assoc_id {
	__be32 desc_tag;
	__be32 desc_len;
	__be64 association_id;
};

/* FCNVME_LS_CREATE_ASSOCIATION */
#define FCNVME_ASSOC_HOSTID_LEN     SPDK_NVMF_FC_HOST_ID_LEN
#define FCNVME_ASSOC_HOSTNQN_LEN    SPDK_NVMF_FC_NQN_MAX_LEN
#define FCNVME_ASSOC_SUBNQN_LEN     SPDK_NVMF_FC_NQN_MAX_LEN

/* for now, we don't care about counting pad at end of cr assoc cmd desc */
#define LS_CREATE_ASSOC_MIN_LEN  592
#define LS_CREATE_ASSOC_DESC_LIST_MIN_LEN 584
#define LS_CREATE_ASSOC_CMD_DESC_MIN_LEN 576

struct nvmf_fc_lsdesc_cr_assoc_cmd {
	__be32  desc_tag;
	__be32  desc_len;
	__be16  ersp_ratio;
	__be16  rsvd10;
	__be32  rsvd12[9];
	__be16  cntlid;
	__be16  sqsize;
	__be32  rsvd52;
	uint8_t hostid[FCNVME_ASSOC_HOSTID_LEN];
	uint8_t hostnqn[FCNVME_ASSOC_HOSTNQN_LEN];
	uint8_t subnqn[FCNVME_ASSOC_SUBNQN_LEN];
	uint8_t rsvd584[432];
};

struct nvmf_fc_ls_cr_assoc_rqst {
	struct nvmf_fc_ls_rqst_w0 w0;
	__be32 desc_list_len;
	struct nvmf_fc_lsdesc_cr_assoc_cmd assoc_cmd;
};

struct nvmf_fc_ls_cr_assoc_acc {
	struct nvmf_fc_ls_acc_hdr hdr;
	struct nvmf_fc_lsdesc_assoc_id assoc_id;
	struct nvmf_fc_lsdesc_conn_id conn_id;
};

/* FCNVME_LS_CREATE_CONNECTION */
struct nvmf_fc_lsdesc_cr_conn_cmd {
	__be32 desc_tag;		/* FCNVME_LSDESC_xxx */
	__be32 desc_len;
	__be16 ersp_ratio;
	__be16 rsvd10;
	__be32 rsvd12[9];
	__be16 qid;
	__be16 sqsize;
	__be32 rsvd52;
};

struct nvmf_fc_ls_cr_conn_rqst {
	struct nvmf_fc_ls_rqst_w0 w0;
	__be32 desc_list_len;
	struct nvmf_fc_lsdesc_assoc_id assoc_id;
	struct nvmf_fc_lsdesc_cr_conn_cmd connect_cmd;
};

struct nvmf_fc_ls_cr_conn_acc {
	struct nvmf_fc_ls_acc_hdr hdr;
	struct nvmf_fc_lsdesc_conn_id conn_id;
};

/* FCNVME_LS_DISCONNECT */
struct nvmf_fc_lsdesc_disconn_cmd {
	__be32 desc_tag;		/* FCNVME_LSDESC_xxx */
	__be32 desc_len;
	uint8_t rsvd8[3];
	/* note: scope is really a 1 bit field */
	uint8_t scope;			/* FCNVME_DISCONN_xxx */
	__be32 rsvd12;
	__be64 id;
};

struct nvmf_fc_ls_disconnect_rqst {
	struct nvmf_fc_ls_rqst_w0 w0;
	__be32 desc_list_len;
	struct nvmf_fc_lsdesc_assoc_id assoc_id;
	struct nvmf_fc_lsdesc_disconn_cmd disconn_cmd;
};

struct nvmf_fc_ls_disconnect_acc {
	struct nvmf_fc_ls_acc_hdr hdr;
};

/* FC-NVME LS RJT reason_code values */
enum fcnvme_ls_rjt_reason {
	FCNVME_RJT_RC_NONE = 0,
	/* no reason - not to be sent */

	FCNVME_RJT_RC_INVAL = 0x01,
	/* invalid NVMe_LS command code */

	FCNVME_RJT_RC_LOGIC = 0x03,
	/* logical error */

	FCNVME_RJT_RC_UNAB = 0x09,
	/* unable to perform command request */

	FCNVME_RJT_RC_UNSUP = 0x0b,
	/* command not supported */

	FCNVME_RJT_RC_INPROG = 0x0e,
	/* command already in progress */

	FCNVME_RJT_RC_INV_ASSOC = 0x40,
	/* Invalid Association ID */

	FCNVME_RJT_RC_INV_CONN = 0x41,
	/* Invalid Connection ID */

	FCNVME_RJT_RC_VENDOR = 0xff,
	/* vendor specific error */
};

/* FC-NVME LS RJT reason_explanation values */
enum fcnvme_ls_rjt_explan {
	FCNVME_RJT_EXP_NONE	 = 0x00,
	/* No additional explanation */

	FCNVME_RJT_EXP_OXID_RXID = 0x17,
	/* invalid OX_ID-RX_ID combination */

	FCNVME_RJT_EXP_INSUF_RES = 0x29,
	/* insufficient resources */

	FCNVME_RJT_EXP_UNAB_DATA = 0x2a,
	/* unable to supply requested data */

	FCNVME_RJT_EXP_INV_LEN	 = 0x2d,
	/* Invalid payload length */
};

/* FCNVME_LSDESC_RJT */
struct nvmf_fc_lsdesc_rjt {
	__be32 desc_tag;		/* FCNVME_LSDESC_xxx */
	__be32 desc_len;
	uint8_t rsvd8;

	uint8_t reason_code;		/* fcnvme_ls_rjt_reason */
	uint8_t reason_explanation;	/* fcnvme_ls_rjt_explan */

	uint8_t vendor;
	__be32 	rsvd12;
};

/* FCNVME_LS_RJT */
struct nvmf_fc_ls_rjt {
	struct nvmf_fc_ls_rqst_w0 w0;
	__be32 desc_list_len;
	struct nvmf_fc_lsdesc_rqst rqst;
	struct nvmf_fc_lsdesc_rjt rjt;
};

/* Validation Error indexes into the string table below */
enum {
	VERR_NO_ERROR = 0,
	VERR_CR_ASSOC_LEN = 1,
	VERR_CR_ASSOC_RQST_LEN = 2,
	VERR_CR_ASSOC_CMD = 3,
	VERR_CR_ASSOC_CMD_LEN = 4,
	VERR_ERSP_RATIO = 5,
	VERR_ASSOC_ALLOC_FAIL = 6,
	VERR_QUEUE_ALLOC_FAIL = 7,
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
	VERR_CONN_TOO_MANY = 29
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
};

/* Poller API structures (arguments and callback data */

struct nvmf_fc_ls_add_conn_api_data {
	struct nvmf_fc_poller_api_add_connection_args args;
	struct nvmf_fc_ls_rqst *ls_rqst;
	struct spdk_nvmf_fc_association *assoc;
	bool assoc_conn; /* true if adding connection for new association */
};

/* Disconnect (connection) request functions */
struct nvmf_fc_ls_del_conn_api_data {
	struct nvmf_fc_poller_api_del_connection_args args;
	struct spdk_nvmf_fc_association *assoc;
	struct nvmf_fc_ls_rqst *ls_rqst;
};

struct nvmf_fc_ls_del_all_conns_api_data {
	struct nvmf_fc_poller_api_del_connection_args args;
	struct spdk_nvmf_fc_association *assoc;
	spdk_nvmf_fc_del_assoc_cb del_assoc_cb;
	void *del_assoc_cb_data;
};

struct nvmf_fc_ls_disconn_assoc_api_data {
	struct spdk_nvmf_fc_nport *tgtport;
	struct nvmf_fc_ls_rqst *ls_rqst;
};

union nvmf_fc_ls_op_ctx {
	struct nvmf_fc_ls_add_conn_api_data add_conn;
	struct nvmf_fc_ls_del_conn_api_data del_conn;
	struct nvmf_fc_ls_del_all_conns_api_data del_all_conns;
	struct nvmf_fc_ls_disconn_assoc_api_data del_assoc;
};

/* global LS variables */
static bool g_fc_ls_init_done = false;
struct spdk_mempool *g_fc_ls_assoc_pool = NULL;
struct spdk_mempool *g_fc_ls_conn_pool = NULL;

/* ****************************************************** */

#define be32_to_cpu(i) from_be32((i))

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

static void
nvmf_fc_xmt_ls_rsp(struct spdk_nvmf_fc_nport *tgtport,
		   struct nvmf_fc_ls_rqst *ls_rqst)
{
	bcm_nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
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

static struct spdk_nvmf_fc_association *
nvmf_fc_ls_new_association(struct spdk_nvmf_fc_nport *tgtport,
			   struct nvmf_fc_lsdesc_cr_assoc_cmd *a_cmd,
			   struct spdk_nvmf_subsystem *subsys)
{
	struct spdk_nvmf_fc_association *assoc =
		(struct spdk_nvmf_fc_association *)
		spdk_mempool_get(g_fc_ls_assoc_pool);

	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");

	if (assoc) {
		memset((void *)assoc, 0, sizeof(*assoc));
		assoc->tgtport = tgtport;
		assoc->subsystem = subsys;
		memcpy(assoc->host_id, a_cmd->hostid, FCNVME_ASSOC_HOSTID_LEN);
		memcpy(assoc->host_nqn, a_cmd->hostnqn, FCNVME_ASSOC_HOSTNQN_LEN);
		memcpy(assoc->sub_nqn, a_cmd->subnqn, FCNVME_ASSOC_HOSTNQN_LEN);
		TAILQ_INIT(&assoc->fc_conns);
		/* add association to target port's association list */
		TAILQ_INSERT_TAIL(&tgtport->fc_associations, assoc, link);
		tgtport->assoc_count++;
	} else {
		SPDK_ERRLOG("out of associations\n");
	}

	return assoc;
}

static inline void
nvmf_fc_ls_free_association(struct spdk_nvmf_fc_association *assoc)
{
	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");
	spdk_mempool_put(g_fc_ls_assoc_pool, (void *)assoc);
}

static struct spdk_nvmf_fc_conn *
nvmf_fc_ls_new_connection(struct spdk_nvmf_fc_association *assoc,
			  enum conn_type type, uint16_t qid, uint16_t max_q_size,
			  uint16_t esrp_ratio, uint16_t rpi)
{
	struct spdk_nvmf_fc_conn *fc_conn =
		(struct spdk_nvmf_fc_conn *)
		spdk_mempool_get(g_fc_ls_conn_pool);

	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");

	if (fc_conn) {
		SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");
		memset((void *)fc_conn, 0, sizeof(*fc_conn));
		fc_conn->conn.type = type;
		fc_conn->conn.transport = &spdk_nvmf_transport_bcm_fc;
		fc_conn->qid = qid;
		fc_conn->conn.sq_head_max = max_q_size;
		fc_conn->esrp_ratio = esrp_ratio;
		fc_conn->fc_assoc = assoc;
		fc_conn->rpi = rpi;

		TAILQ_INIT(&fc_conn->incoming_queue);
		TAILQ_INIT(&fc_conn->free_queue);
		TAILQ_INIT(&fc_conn->pending_data_buf_queue);
		TAILQ_INIT(&fc_conn->pending_fc_rw_queue);
	} else {
		SPDK_ERRLOG("out of connections\n");
	}

	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");

	return fc_conn;
}

static inline void
nvmf_fc_ls_free_connection(struct spdk_nvmf_fc_conn *fc_conn)
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
	return spdk_malloc(sizeof(union nvmf_fc_ls_op_ctx));
}

static inline void
nvmf_fc_ls_free_op_ctx(union nvmf_fc_ls_op_ctx *ctx_ptr)
{
	spdk_free((void *)ctx_ptr);
}

/* End - Allocators/Deallocators (assocations, connections, */
/*       poller API data)                                   */
/* ******************************************************** */

static inline struct spdk_nvmf_fc_association *
nvmf_fc_ls_find_assoc(struct spdk_nvmf_fc_nport *tgtport, uint64_t assoc_id)
{
	struct spdk_nvmf_fc_association *assoc = NULL;
	TAILQ_FOREACH(assoc, &tgtport->fc_associations, link) {
		if (assoc->assoc_id == assoc_id)
			break;
	}
	return assoc;
}

static inline struct fc_hwqp *
nvmf_fc_ls_get_hwqp(struct spdk_nvmf_fc_nport *tgtport, uint64_t conn_id)
{
	struct spdk_nvmf_fc_port *fc_port = tgtport->fc_port;
	return (&fc_port->io_queues[(conn_id &
				     SPDK_NVMF_FC_BCM_MRQ_CONNID_QUEUE_MASK) %
				    fc_port->max_io_queues]);
}

static inline uint64_t
nvmf_fc_gen_conn_id(uint32_t qnum, struct fc_hwqp *hwqp)
{
	return ((uint64_t) qnum |
		(hwqp->cid_cnt++ << SPDK_NVMF_FC_BCM_MRQ_CONNID_UV_SHIFT));
}

static inline struct fc_hwqp *
nvmf_fc_ls_assign_conn_to_q(struct spdk_nvmf_fc_association *assoc,
			    uint64_t *conn_id)
{
	struct spdk_nvmf_fc_nport *tgtport = assoc->tgtport;
	struct spdk_nvmf_fc_port *fc_port = tgtport->fc_port;
	uint32_t q_ind;
	uint32_t min_q_ind = 0;

	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");

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
	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING,
		      "q_num %d, conn_id %lx\n", min_q_ind, *conn_id);
	return &fc_port->io_queues[min_q_ind];
}

/* callback from poller's ADD_Connection event */
static void
nvmf_fc_ls_add_conn_cb(void *cb_data, nvmf_fc_poller_api_ret_t ret)
{
	union nvmf_fc_ls_op_ctx *opd =
			(union nvmf_fc_ls_op_ctx *)cb_data;
	struct nvmf_fc_ls_add_conn_api_data *dp = &opd->add_conn;
	struct spdk_nvmf_fc_association *assoc = dp->assoc;
	struct spdk_nvmf_fc_nport *tgtport = assoc->tgtport;
	struct spdk_nvmf_fc_conn *fc_conn = dp->args.fc_conn;
	struct nvmf_fc_ls_rqst *ls_rqst = dp->ls_rqst;

	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING,
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
	struct spdk_nvmf_fc_association *assoc,
	struct nvmf_fc_ls_rqst *ls_rqst,
	struct spdk_nvmf_fc_conn *fc_conn,
	bool assoc_conn)
{
	union nvmf_fc_ls_op_ctx *opd = nvmf_fc_ls_new_op_ctx();

	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");

	if (opd) {
		struct nvmf_fc_ls_add_conn_api_data *api_data =
				&opd->add_conn;
		/* assign connection to (poller) queue */
		api_data->args.hwqp = nvmf_fc_ls_assign_conn_to_q(
					      assoc, &fc_conn->conn_id);
		api_data->args.fc_conn = fc_conn;
		api_data->args.cb_info.cb_func = nvmf_fc_ls_add_conn_cb;
		api_data->args.cb_info.cb_data = (void *)opd;
		api_data->assoc = assoc;
		api_data->ls_rqst = ls_rqst;
		api_data->assoc_conn = assoc_conn;
		SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING,
			      "conn_id = %ld\n", fc_conn->conn_id);
		nvmf_fc_poller_api(api_data->args.hwqp->lcore_id,
				   NVMF_FC_POLLER_API_ADD_CONNECTION, &api_data->args);
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

	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");
}

#ifdef SINGLE_DISCONN_SUPPORT
/* for later when FC-NVMe spec allows single disconnect on association */

static void
nvmf_fc_ls_del_conn_cb(void *cb_data, nvmf_fc_poller_api_ret_t ret)
{
	union nvmf_fc_ls_op_ctx *opd =
			(union nvmf_fc_ls_op_ctx *)cb_data;
	struct nvmf_fc_ls_del_conn_api_data *dp = &opd->del_conn;
	struct spdk_nvmf_fc_association *assoc = dp->assoc;
	struct spdk_nvmf_fc_nport *tgtport = assoc->tgtport;
	struct nvmf_fc_ls_rqst *ls_rqst = dp->ls_rqst;

	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING,
		      "conn_id = %ld\n", dp->args.fc_conn->conn_id);

	if (ret == 0) {
		/* remove connection from association connection list */
		TAILQ_REMOVE(&assoc->fc_conns, dp->args.fc_conn, assoc_link);
		assoc->conn_count--;
		nvmf_fc_ls_free_connection(dp->args.fc_conn);
		dp->args.hwqp->num_conns--;
	}

	else {
		/* send failure response */
		struct nvmf_fc_ls_cr_assoc_rqst *rqst =
			(struct nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
		struct nvmf_fc_ls_cr_assoc_acc *acc =
			(struct nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
		SPDK_ERRLOG(
			"ERR %d returned by poller delete_connection (conn_id %lx)\n",
			ret, dp->args.fc_conn->conn_id);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   NVME_FC_MAX_LS_BUFFER_SIZE, rqst->w0.ls_cmd,
				   FCNVME_RJT_RC_UNAB,
				   FCNVME_RJT_EXP_NONE, 0);
	}

	/* send LS response */
	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);

	nvmf_fc_ls_free_op_ctx(opd);
}

static void
nvmf_fc_ls_del_conn(struct spdk_nvmf_fc_association *assoc,
		    struct nvmf_fc_ls_rqst *ls_rqst,
		    struct spdk_nvmf_fc_conn *fc_conn)
{
	union nvmf_fc_ls_op_ctx *opd = nvmf_fc_ls_get_op_data();

	if (opd) {
		struct nvmf_fc_ls_del_conn_api_data *api_data =
				&opd->del_conn;
		api_data->assoc = assoc;
		api_data->ls_rqst = ls_rqst;
		api_data->args.fc_conn = fc_conn;
		api_data->args.hwqp = nvmf_fc_ls_get_hwqp(assoc->tgtport,
				      fc_conn->conn_id);
		api_data->args.cb_info.cb_func = nvmf_fc_ls_del_conn_cb;
		api_data->args.cb_info.cb_data = opd;

		SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING,
			      "conn_id = %ld\n", conn_id);
		nvmf_fc_poller_api(api_data->args.hwqp->lcore_id,
				   NVMF_FC_POLLER_API_DEL_CONNECTION, &api_data->args);
	}

	else {
		/* send failure response */
		struct nvmf_fc_ls_cr_assoc_rqst *rqst =
			(struct nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
		struct nvmf_fc_ls_cr_assoc_acc *acc =
			(struct nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
		SPDK_ERRLOG("Allocate data for del conn op failed\n");
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   NVME_FC_MAX_LS_BUFFER_SIZE, rqst->w0.ls_cmd,
				   FCNVME_RJT_RC_UNAB,
				   FCNVME_RJT_EXP_INSUF_RES, 0);
		nvmf_fc_xmt_ls_rsp(assoc->tgtport, ls_rqst);
	}
}
#endif /* SINGLE_DISCONN_SUPPORT */

/* Delete association functions */

static void
nvmf_fc_del_next_conn(union nvmf_fc_ls_op_ctx *opd)
{
	struct nvmf_fc_ls_del_all_conns_api_data *api_data =
			&opd->del_all_conns;
	struct nvmf_fc_ls_del_all_conns_api_data *dp =
			&opd->del_all_conns;
	struct spdk_nvmf_fc_association *assoc = dp->assoc;

	/* get the next connection in association's list */
	api_data->args.fc_conn =
		TAILQ_FIRST(&assoc->fc_conns);

	if (api_data->args.fc_conn) {
		SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING,
			      "conn_id = %lx\n", api_data->args.fc_conn->conn_id);
		api_data->args.hwqp = nvmf_fc_ls_get_hwqp(assoc->tgtport,
				      api_data->args.fc_conn->conn_id);
		nvmf_fc_poller_api(
			api_data->args.hwqp->lcore_id,
			NVMF_FC_POLLER_API_DEL_CONNECTION,
			&api_data->args);
	} else {
		/* last connection - remove association from target port's
		 * association list */
		struct spdk_nvmf_fc_nport *tgtport = assoc->tgtport;

		SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING,
			      "remove assoc. %lx (conn_cnt=%d\n",
			      assoc->assoc_id, assoc->conn_count);
		TAILQ_REMOVE(&tgtport->fc_associations, assoc, link);
		tgtport->assoc_count--;
		nvmf_fc_ls_free_association(assoc);
		dp->del_assoc_cb(dp->del_assoc_cb_data, 0);
		nvmf_fc_ls_free_op_ctx(opd);
	}
}

static void
nvmf_fc_del_all_conns_cb(void *cb_data, nvmf_fc_poller_api_ret_t ret)
{
	union nvmf_fc_ls_op_ctx *opd =
			(union nvmf_fc_ls_op_ctx *)cb_data;
	struct nvmf_fc_ls_del_all_conns_api_data *dp = &opd->del_all_conns;
	struct spdk_nvmf_fc_association *assoc = dp->assoc;

	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");
	if (ret == 0) {
		/* remove connection from association's connection list */
		TAILQ_REMOVE(&assoc->fc_conns, dp->args.fc_conn, assoc_link);
		assoc->conn_count--;
		nvmf_fc_ls_free_connection(dp->args.fc_conn);
		dp->args.hwqp->num_conns--;

		/* delete the next connection */
		nvmf_fc_del_next_conn(opd);
	}

	else {
		/* error - stop now and return error */
		SPDK_ERRLOG("delete connection failed\n");
		dp->del_assoc_cb(dp->del_assoc_cb_data, ret);
		nvmf_fc_ls_free_op_ctx(opd);
	}
	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");
}

/* Disconnect (association) request functions */

static void
nvmf_fc_ls_disconnect_assoc_cb(void *cb_data, uint32_t err)
{
	union nvmf_fc_ls_op_ctx *opd = (union nvmf_fc_ls_op_ctx *)cb_data;
	struct nvmf_fc_ls_disconn_assoc_api_data *dp = &opd->del_assoc;
	struct spdk_nvmf_fc_nport *tgtport = dp->tgtport;
	struct nvmf_fc_ls_rqst *ls_rqst = dp->ls_rqst;


	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");
	if (err != 0) {
		/* send failure response */
		struct nvmf_fc_ls_cr_assoc_rqst *rqst =
			(struct nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
		struct nvmf_fc_ls_cr_assoc_acc *acc =
			(struct nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   NVME_FC_MAX_LS_BUFFER_SIZE, rqst->w0.ls_cmd,
				   FCNVME_RJT_RC_UNAB,
				   FCNVME_RJT_EXP_NONE, 0);
	}

	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);

	nvmf_fc_ls_free_op_ctx(opd);
	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");
}

static void
nvmf_fc_ls_disconnect_assoc(struct spdk_nvmf_fc_nport *tgtport,
			    struct nvmf_fc_ls_rqst *ls_rqst, uint64_t assoc_id)
{
	union nvmf_fc_ls_op_ctx *opd = nvmf_fc_ls_new_op_ctx();

	if (opd) {
		struct nvmf_fc_ls_disconn_assoc_api_data *api_data =
				&opd->del_assoc;
		int ret;

		api_data->tgtport = tgtport;
		api_data->ls_rqst = ls_rqst;
		ret = spdk_nvmf_fc_delete_association(tgtport, assoc_id,
						      nvmf_fc_ls_disconnect_assoc_cb, api_data);

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

static void
nvmf_fc_ls_create_association(struct spdk_nvmf_fc_nport *tgtport,
			      struct nvmf_fc_ls_rqst *ls_rqst)
{
	struct nvmf_fc_ls_cr_assoc_rqst *rqst =
		(struct nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
	struct nvmf_fc_ls_cr_assoc_acc *acc =
		(struct nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_fc_association *assoc;
	struct spdk_nvmf_fc_conn *fc_conn;
	struct spdk_nvmf_subsystem *subsys = NULL;
	int ret = 0;


	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING,
		      "ls_rst_len=%d, desc_list_len=%d, cmd_len=%d\n",
		      ls_rqst->rqst_len, be32_to_cpu(&rqst->desc_list_len),
		      be32_to_cpu(&rqst->assoc_cmd.desc_len));

	if (ls_rqst->rqst_len < LS_CREATE_ASSOC_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd req len = %d, should be at leastt %d\n",
			    ls_rqst->rqst_len, LS_CREATE_ASSOC_MIN_LEN);
		ret = VERR_CR_ASSOC_LEN;
	} else if (be32_to_cpu(&rqst->desc_list_len) <
		   LS_CREATE_ASSOC_DESC_LIST_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd desc list len = %d, should be at least %d\n",
			    be32_to_cpu(&rqst->desc_list_len),
			    LS_CREATE_ASSOC_DESC_LIST_MIN_LEN);
		ret = VERR_CR_ASSOC_RQST_LEN;
	} else if (rqst->assoc_cmd.desc_tag !=
		   cpu_to_be32(FCNVME_LSDESC_CREATE_ASSOC_CMD)) {
		ret = VERR_CR_ASSOC_CMD;
	} else if (be32_to_cpu(&rqst->assoc_cmd.desc_len) <
		   LS_CREATE_ASSOC_CMD_DESC_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd desc len = %d, should be at least %d\n",
			    be32_to_cpu(&rqst->assoc_cmd.desc_len),
			    LS_CREATE_ASSOC_CMD_DESC_MIN_LEN);
		ret = VERR_CR_ASSOC_CMD_LEN;
	} else if (!rqst->assoc_cmd.ersp_ratio ||
		   (from_be16(&rqst->assoc_cmd.ersp_ratio) >=
		    from_be16(&rqst->assoc_cmd.sqsize)))
		ret = VERR_ERSP_RATIO;
	else {
		/* new association w/ admin queue */
		/* TODO: Add check if host nqn allowed to create assoc on this subnqn */
		/* TODO: Get subsystem from subnqn */
		/* subsys = nvmf_find_subsystem(rqst->assoc_cmd.subnqn);  */
		assoc = nvmf_fc_ls_new_association(tgtport,
						   &rqst->assoc_cmd,
						   subsys);
		if (!assoc) {
			ret = VERR_ASSOC_ALLOC_FAIL;
		} else { // alloc admin q (i.e. connection)
			fc_conn = nvmf_fc_ls_new_connection(assoc,
							    CONN_TYPE_AQ, 0, rqst->assoc_cmd.sqsize,
							    rqst->assoc_cmd.ersp_ratio, ls_rqst->rpi);
			if (!fc_conn) {
				ret = VERR_QUEUE_ALLOC_FAIL;
			}
		}
	}

	if (ret) {
		SPDK_ERRLOG("Create Association LS failed: %s\n",
			    validation_errors[ret]);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   NVME_FC_MAX_LS_BUFFER_SIZE, rqst->w0.ls_cmd,
				   ret == VERR_ASSOC_ALLOC_FAIL ||
				   ret == VERR_QUEUE_ALLOC_FAIL ?
				   FCNVME_RJT_RC_UNAB :
				   FCNVME_RJT_RC_LOGIC,
				   ret == VERR_ASSOC_ALLOC_FAIL ||
				   ret == VERR_QUEUE_ALLOC_FAIL ?
				   FCNVME_RJT_EXP_INSUF_RES :
				   FCNVME_RJT_EXP_NONE, 0);
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
			nvmf_fc_lsdesc_len(
				sizeof(struct nvmf_fc_lsdesc_assoc_id));
		to_be32(&acc->conn_id.desc_tag, FCNVME_LSDESC_CONN_ID);
		acc->conn_id.desc_len =
			nvmf_fc_lsdesc_len(
				sizeof(struct nvmf_fc_lsdesc_conn_id));

		/* assign connection to HWQP poller - also sends response */
		nvmf_fc_ls_add_conn_to_poller(assoc, ls_rqst,
					      fc_conn, true);
	}
}

static void
nvmf_fc_ls_create_connection(struct spdk_nvmf_fc_nport *tgtport,
			     struct nvmf_fc_ls_rqst *ls_rqst)
{
	struct nvmf_fc_ls_cr_conn_rqst *rqst =
		(struct nvmf_fc_ls_cr_conn_rqst *)ls_rqst->rqstbuf.virt;
	struct nvmf_fc_ls_cr_conn_acc *acc =
		(struct nvmf_fc_ls_cr_conn_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_fc_association *assoc;
	struct spdk_nvmf_fc_conn *fc_conn = NULL;
	int ret = 0;

	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");

	if (ls_rqst->rqst_len < sizeof(struct nvmf_fc_ls_cr_conn_rqst))
		ret = VERR_CR_CONN_LEN;
	else if (rqst->desc_list_len !=
		 nvmf_fc_lsdesc_len(
			 sizeof(struct nvmf_fc_ls_cr_conn_rqst)))
		ret = VERR_CR_CONN_RQST_LEN;
	else if (rqst->assoc_id.desc_tag !=
		 cpu_to_be32(FCNVME_LSDESC_ASSOC_ID))
		ret = VERR_ASSOC_ID;
	else if (rqst->assoc_id.desc_len !=
		 nvmf_fc_lsdesc_len(
			 sizeof(struct nvmf_fc_lsdesc_assoc_id)))
		ret = VERR_ASSOC_ID_LEN;
	else if (rqst->connect_cmd.desc_tag !=
		 cpu_to_be32(FCNVME_LSDESC_CREATE_CONN_CMD))
		ret = VERR_CR_CONN_CMD;
	else if (rqst->connect_cmd.desc_len !=
		 nvmf_fc_lsdesc_len(
			 sizeof(struct nvmf_fc_lsdesc_cr_conn_cmd)))
		ret = VERR_CR_CONN_CMD_LEN;
	else if (!rqst->connect_cmd.ersp_ratio ||
		 (from_be16(&rqst->connect_cmd.ersp_ratio) >=
		  from_be16(&rqst->connect_cmd.sqsize)))
		ret = VERR_ERSP_RATIO;

	else {
		/* find association */
		assoc = nvmf_fc_ls_find_assoc(tgtport,
					      from_be64(&rqst->assoc_id.association_id));
		if (!assoc)
			ret = VERR_NO_ASSOC;
		else { // alloc IO q (i.e. connection)
			if (assoc->conn_count < g_max_conns_per_assoc) {
				fc_conn = nvmf_fc_ls_new_connection(assoc,
								    CONN_TYPE_IOQ,
								    rqst->connect_cmd.qid,
								    rqst->connect_cmd.sqsize,
								    rqst->connect_cmd.ersp_ratio,
								    ls_rqst->rpi);
				if (!fc_conn) {
					ret = VERR_QUEUE_ALLOC_FAIL;
				}
			} else {
				ret = VERR_CONN_TOO_MANY;
			}
		}
	}

	if (ret) {
		SPDK_ERRLOG("Create Connection LS failed: %s\n",
			    validation_errors[ret]);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   NVME_FC_MAX_LS_BUFFER_SIZE, rqst->w0.ls_cmd,
				   (ret == VERR_NO_ASSOC) ?
				   FCNVME_RJT_RC_INV_ASSOC :
				   ret == VERR_QUEUE_ALLOC_FAIL ?
				   FCNVME_RJT_RC_UNAB :
				   FCNVME_RJT_RC_LOGIC,
				   (ret == VERR_QUEUE_ALLOC_FAIL ||
				    ret == VERR_CONN_TOO_MANY) ?
				   FCNVME_RJT_EXP_INSUF_RES :
				   FCNVME_RJT_EXP_NONE, 0);
		nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}

	else {
		/* format accept response */
		SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");
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
nvmf_fc_ls_disconnect(struct spdk_nvmf_fc_nport *tgtport,
		      struct nvmf_fc_ls_rqst *ls_rqst)
{
	struct nvmf_fc_ls_disconnect_rqst *rqst =
		(struct nvmf_fc_ls_disconnect_rqst *)ls_rqst->rqstbuf.virt;
	struct nvmf_fc_ls_disconnect_acc *acc =
		(struct nvmf_fc_ls_disconnect_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_fc_association *assoc;
	struct spdk_nvmf_fc_conn *fc_conn = NULL;
	int ret = 0;

	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING, "\n");

	memset(acc, 0, sizeof(*acc));

	if (ls_rqst->rqst_len < sizeof(struct nvmf_fc_ls_disconnect_rqst))
		ret = VERR_DISCONN_LEN;
	else if (rqst->desc_list_len !=
		 nvmf_fc_lsdesc_len(
			 sizeof(struct nvmf_fc_ls_disconnect_rqst)))
		ret = VERR_DISCONN_RQST_LEN;
	else if (rqst->assoc_id.desc_tag != cpu_to_be32(FCNVME_LSDESC_ASSOC_ID))
		ret = VERR_ASSOC_ID;
	else if (rqst->assoc_id.desc_len !=
		 nvmf_fc_lsdesc_len(
			 sizeof(struct nvmf_fc_lsdesc_assoc_id)))
		ret = VERR_ASSOC_ID_LEN;
	else if (rqst->disconn_cmd.desc_tag !=
		 cpu_to_be32(FCNVME_LSDESC_DISCONN_CMD))
		ret = VERR_DISCONN_CMD;
	else if (rqst->disconn_cmd.desc_len !=
		 nvmf_fc_lsdesc_len(
			 sizeof(struct nvmf_fc_lsdesc_disconn_cmd)))
		ret = VERR_DISCONN_CMD_LEN;
	else if ((rqst->disconn_cmd.scope != FCNVME_DISCONN_ASSOCIATION) &&
		 (rqst->disconn_cmd.scope != FCNVME_DISCONN_CONNECTION))
		ret = VERR_DISCONN_SCOPE;
	else {
		/* match an active association */
		assoc = nvmf_fc_ls_find_assoc(tgtport,
					      from_be64(&rqst->assoc_id.association_id));
		if (!assoc) {
			ret = VERR_NO_ASSOC;
		} else if (rqst->disconn_cmd.scope ==
			   FCNVME_DISCONN_CONNECTION) {
			/* verify connection exists */
			TAILQ_FOREACH(fc_conn, &assoc->fc_conns, assoc_link) {
				if (fc_conn->conn_id == rqst->disconn_cmd.id) {
					break;
				}
			}
			if (!fc_conn) {
				ret = VERR_NO_CONN;
			}
		}
	}

	if (ret) {
		SPDK_ERRLOG("Disconnect LS failed: %s\n",
			    validation_errors[ret]);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   NVME_FC_MAX_LS_BUFFER_SIZE, rqst->w0.ls_cmd,
				   (ret == VERR_NO_ASSOC) ? FCNVME_RJT_RC_INV_ASSOC :
				   (ret == VERR_NO_CONN) ? FCNVME_RJT_RC_INV_CONN :
				   FCNVME_RJT_RC_LOGIC,
				   FCNVME_RJT_EXP_NONE, 0);
		nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}

	else {
		/* format response */
		ls_rqst->rsp_len = sizeof(*acc);

		nvmf_fc_ls_format_rsp_hdr(acc, FCNVME_LS_ACC,
					  nvmf_fc_lsdesc_len(
						  sizeof(struct nvmf_fc_ls_disconnect_acc)),
					  FCNVME_LS_DISCONNECT);

		/* NOTE: FC-NVMe spec will be changing so disconnect command will
		 * destroy assosication - so no need to look at scope
		 * A new command will be spec'd for single connection disconnect */
		nvmf_fc_ls_disconnect_assoc(tgtport, ls_rqst, assoc->assoc_id);
	}
}

/* ************************ */
/* external functions       */

void
spdk_nvmf_fc_ls_init(void)
{
#ifdef DEBUG_FC_LS_TRACE
	extern struct spdk_trace_flag SPDK_TRACE_FC_LS_PROCESSING;
	SPDK_TRACE_FC_LS_PROCESSING.enabled = true;
#endif

	g_max_conns_per_assoc = g_nvmf_tgt.max_queues_per_session;

	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING,
		      "max_assocations = %d, max conns = %d\n",
		      g_nvmf_tgt.max_associations, g_max_conns_per_assoc);

	if (!g_fc_ls_init_done) {
		g_fc_ls_init_done = true;
		/* allocate associations */
		g_fc_ls_assoc_pool =
			spdk_mempool_create("NVMF_FC_ASSOC_POOL",
					    (size_t)(g_nvmf_tgt.max_associations),
					    sizeof(struct spdk_nvmf_fc_association),
					    0, SPDK_ENV_SOCKET_ID_ANY);
		if (g_fc_ls_assoc_pool) {
			/* allocation connections */
			g_fc_ls_conn_pool =
				spdk_mempool_create("NVMF_FC_CONN_POOL",
						    (size_t)(g_nvmf_tgt.max_associations *
							     g_max_conns_per_assoc),
						    sizeof(struct spdk_nvmf_fc_conn),
						    0, SPDK_ENV_SOCKET_ID_ANY);
			if (g_fc_ls_conn_pool == NULL) {
				SPDK_ERRLOG("create connection pool failed\n");
			}
		} else {
			SPDK_ERRLOG("create LS association pool failed\n");
		}
	}
}

void
spdk_nvmf_fc_ls_fini(void)
{
	if (g_fc_ls_init_done) {
		if (g_fc_ls_assoc_pool) {
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
spdk_nvmf_fc_handle_ls_rqst(struct spdk_nvmf_fc_nport *tgtport,
			    struct nvmf_fc_ls_rqst *ls_rqst)
{
	struct nvmf_fc_ls_rqst_w0 *w0 =
		(struct nvmf_fc_ls_rqst_w0 *)ls_rqst->rqstbuf.virt;

	SPDK_TRACELOG(SPDK_TRACE_FC_LS_PROCESSING,
		      "LS cmd=%d\n", w0->ls_cmd);

	switch (w0->ls_cmd) {
	case FCNVME_LS_CREATE_ASSOCIATION:
		nvmf_fc_ls_create_association(tgtport, ls_rqst);
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
spdk_nvmf_fc_delete_association(struct spdk_nvmf_fc_nport *tgtport,
				uint64_t assoc_id, spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
				void *cb_data)
{
	int rc = 0;
	struct spdk_nvmf_fc_association *assoc =
		nvmf_fc_ls_find_assoc(tgtport, assoc_id);

	if (assoc) {
		/* delete all of the association's connections */
		union nvmf_fc_ls_op_ctx *opd =
				nvmf_fc_ls_new_op_ctx();

		if (opd) {
			struct nvmf_fc_ls_del_all_conns_api_data
				*api_data = &opd->del_all_conns;
			api_data->assoc = assoc;
			api_data->del_assoc_cb = del_assoc_cb;
			api_data->del_assoc_cb_data = cb_data;
			api_data->args.cb_info.cb_func =
				nvmf_fc_del_all_conns_cb;
			api_data->args.cb_info.cb_data = opd;

			/* delete first conn */
			nvmf_fc_del_next_conn(opd);

		} else { /* hopefully this doesn't happen */
			SPDK_ERRLOG(
				"Mem alloc failed for del conn op data");
			rc = SPDK_ERR_NOMEM;
		}

	} else {
		SPDK_ERRLOG("Delete association failed: %s\n",
			    validation_errors[VERR_NO_ASSOC]);
		rc = VERR_NO_ASSOC;
	}

	return rc;
}

/* Functions defined in struct spdk_nvmf_subsystem's ops field
 * (spdk_nvmf_ctrlr_ops) for connect & disconnnect callbacks */

void spdk_nvmf_fc_subsys_connect_cb(void *cb_ctx,
				    struct spdk_nvmf_request *req)
{
	spdk_nvmf_handle_connect(req);
}

void spdk_nvmf_fc_subsys_disconnect_cb(void *cb_ctx,
				       struct spdk_nvmf_conn *conn)
{
	spdk_nvmf_session_disconnect(conn);
}

SPDK_LOG_REGISTER_TRACE_FLAG("fc_ls", SPDK_TRACE_FC_LS_PROCESSING)
