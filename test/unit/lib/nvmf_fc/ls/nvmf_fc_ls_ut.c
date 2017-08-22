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

/* NVMF FC LS Command Processor Unit Test */

#include <spdk/stdinc.h>

#include "spdk_cunit.h"
#include "spdk/nvmf.h"
#include "spdk_internal/event.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "log/log.c"

#include "nvmf_fc/bcm_fc.h"
#include "nvmf/transport.h"
#include "nvmf/nvmf_internal.h"


/* defines from SDPK */

struct spdk_nvmf_tgt g_nvmf_tgt = {
	.config = {
		.max_associations = 4,
		.max_aq_depth = 32,
		.max_queue_depth = 1024,
		.max_queues_per_session = 4,
	}
};

struct nvmf_fc_ls_rqst_w0 {
	uint8_t	ls_cmd;			/* FCNVME_LS_xxx */
	uint8_t zeros[3];
};

/* FCNVME_LSDESC_RQST */
struct nvmf_fc_lsdesc_rqst {
	uint32_t desc_tag;              /* FCNVME_LSDESC_xxx */
	uint32_t desc_len;
	struct nvmf_fc_ls_rqst_w0 w0;
	uint32_t rsvd12;
};

// LS accept header
struct nvmf_fc_ls_acc_hdr {
	struct nvmf_fc_ls_rqst_w0 w0;
	uint32_t desc_list_len;
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
	uint32_t desc_tag;
	uint32_t desc_len;
	uint64_t connection_id;
};

/* FCNVME_LSDESC_ASSOC_ID */
struct nvmf_fc_lsdesc_assoc_id {
	uint32_t desc_tag;
	uint32_t desc_len;
	uint64_t association_id;
};

/* FCNVME_LS_CREATE_ASSOCIATION */
#define FCNVME_ASSOC_HOSTID_LEN     SPDK_NVMF_FC_HOST_ID_LEN
#define FCNVME_ASSOC_HOSTNQN_LEN    SPDK_NVMF_FC_NQN_MAX_LEN
#define FCNVME_ASSOC_SUBNQN_LEN     SPDK_NVMF_FC_NQN_MAX_LEN

struct nvmf_fc_lsdesc_cr_assoc_cmd {
	uint32_t  desc_tag;
	uint32_t  desc_len;
	uint16_t  ersp_ratio;
	uint16_t  rsvd10;
	uint32_t  rsvd12[9];
	uint16_t  cntlid;
	uint16_t  sqsize;
	uint32_t  rsvd52;
	uint8_t hostid[FCNVME_ASSOC_HOSTID_LEN];
	uint8_t hostnqn[FCNVME_ASSOC_HOSTNQN_LEN];
	uint8_t subnqn[FCNVME_ASSOC_SUBNQN_LEN];
	uint8_t rsvd584[432];
};

struct nvmf_fc_ls_cr_assoc_rqst {
	struct nvmf_fc_ls_rqst_w0 w0;
	uint32_t desc_list_len;
	struct nvmf_fc_lsdesc_cr_assoc_cmd assoc_cmd;
};

struct nvmf_fc_ls_cr_assoc_acc {
	struct nvmf_fc_ls_acc_hdr hdr;
	struct nvmf_fc_lsdesc_assoc_id assoc_id;
	struct nvmf_fc_lsdesc_conn_id conn_id;
};

/* FCNVME_LS_CREATE_CONNECTION */
struct nvmf_fc_lsdesc_cr_conn_cmd {
	uint32_t desc_tag;              /* FCNVME_LSDESC_xxx */
	uint32_t desc_len;
	uint16_t ersp_ratio;
	uint16_t rsvd10;
	uint32_t rsvd12[9];
	uint16_t qid;
	uint16_t sqsize;
	uint32_t rsvd52;
};

struct nvmf_fc_ls_cr_conn_rqst {
	struct nvmf_fc_ls_rqst_w0 w0;
	uint32_t desc_list_len;
	struct nvmf_fc_lsdesc_assoc_id assoc_id;
	struct nvmf_fc_lsdesc_cr_conn_cmd connect_cmd;
};

struct nvmf_fc_ls_cr_conn_acc {
	struct nvmf_fc_ls_acc_hdr hdr;
	struct nvmf_fc_lsdesc_conn_id conn_id;
};

/* FCNVME_LS_DISCONNECT */
struct nvmf_fc_lsdesc_disconn_cmd {
	uint32_t desc_tag;              /* FCNVME_LSDESC_xxx */
	uint32_t desc_len;
	uint8_t rsvd8[3];
	/* note: scope is really a 1 bit field */
	uint8_t scope;                  /* FCNVME_DISCONN_xxx */
	uint32_t rsvd12;
	uint64_t id;
};

struct nvmf_fc_ls_disconnect_rqst {
	struct nvmf_fc_ls_rqst_w0 w0;
	uint32_t desc_list_len;
	struct nvmf_fc_lsdesc_assoc_id assoc_id;
	struct nvmf_fc_lsdesc_disconn_cmd disconn_cmd;
};

struct nvmf_fc_ls_disconnect_acc {
	struct nvmf_fc_ls_acc_hdr hdr;
};

/* FC-NVME LS RJT reason_code values */
enum fcnvme_ls_rjt_reason {
	FCNVME_RJT_RC_NONE = 0,
	FCNVME_RJT_RC_INVAL = 0x01,
	FCNVME_RJT_RC_LOGIC = 0x03,
	FCNVME_RJT_RC_UNAB = 0x09,
	FCNVME_RJT_RC_UNSUP = 0x0b,
	FCNVME_RJT_RC_INPROG = 0x0e,
	FCNVME_RJT_RC_INV_ASSOC = 0x40,
	FCNVME_RJT_RC_INV_CONN = 0x41,
	FCNVME_RJT_RC_INV_PARAM = 0x42,
	FCNVME_RJT_RC_INSUFF_RES = 0x43,
	FCNVME_RJT_RC_INV_HOST = 0x44,
	FCNVME_RJT_RC_VENDOR = 0xff,
};

/* FC-NVME LS RJT reason_explanation values */
enum fcnvme_ls_rjt_explan {
	FCNVME_RJT_EXP_NONE  = 0x00,
	FCNVME_RJT_EXP_OXID_RXID = 0x17,
	FCNVME_RJT_EXP_INSUF_RES = 0x29,
	FCNVME_RJT_EXP_UNAB_DATA = 0x2a,
	FCNVME_RJT_EXP_INV_LEN   = 0x2d,
	FCNVME_RJT_EXP_INV_ESRP = 0x40,
	FCNVME_RJT_EXP_INV_CTL_ID = 0x41,
	FCNVME_RJT_EXP_INV_Q_ID = 0x42,
	FCNVME_RJT_EXP_SQ_SIZE = 0x43,
	FCNVME_RJT_EXP_INV_HOST_ID = 0x44,
	FCNVME_RJT_EXP_INV_HOSTNQN = 0x45,
	FCNVME_RJT_EXP_INV_SUBNQN = 0x46,
};

/* FCNVME_LSDESC_RJT */
struct nvmf_fc_lsdesc_rjt {
	uint32_t desc_tag;              /* FCNVME_LSDESC_xxx */
	uint32_t desc_len;
	uint8_t rsvd8;

	uint8_t reason_code;            /* fcnvme_ls_rjt_reason */
	uint8_t reason_explanation;     /* fcnvme_ls_rjt_explan */

	uint8_t vendor;
	uint32_t        rsvd12;
};

/* FCNVME_LS_RJT */
struct nvmf_fc_ls_rjt {
	struct nvmf_fc_ls_rqst_w0 w0;
	uint32_t desc_list_len;
	struct nvmf_fc_lsdesc_rqst rqst;
	struct nvmf_fc_lsdesc_rjt rjt;
};

int bcm_nvmf_fc_xmt_ls_rsp(struct spdk_nvmf_fc_nport *tgtport,
			   struct nvmf_fc_ls_rqst *ls_rqst);
int bcm_nvmf_fc_issue_abort(struct fc_hwqp *hwqp, fc_xri_t *xri, bool send_abts,
			    bcm_fc_caller_cb cb, void *cb_args);

static uint32_t g_test_run_type = 0;
#define TEST_RUN_TYPE_CREATE_ASSOC   1
#define TEST_RUN_TYPE_CREATE_CONN    2
#define TEST_RUN_TYPE_DISCONNECT     3
#define TEST_RUN_TYPE_CONN_BAD_ASSOC 4

static uint64_t g_curr_assoc_id = 0;
static uint32_t g_create_conn_test_cnt = 0;
static int g_last_rslt = 0;

/* ********************************************** */

void *
spdk_dma_malloc(size_t size, size_t align, uint64_t *phys_addr)
{
	void *p = malloc(size);

	if (p && phys_addr) {
		*phys_addr = (uint64_t) p;
	}

	return p;
}

void spdk_dma_free(void *buf)
{
	free(buf);
}

struct __spdk_mempool {
	size_t count;
	size_t ele_size;
	size_t free;
};

struct spdk_mempool *
spdk_mempool_create(const char *name, size_t count,
		    size_t ele_size, size_t cache_size, int socket_id)

{
	struct __spdk_mempool *mp = malloc(sizeof(struct __spdk_mempool));
	if (mp) {
		mp->count = mp->free = count;
		mp->ele_size = ele_size;
	}
	return (struct spdk_mempool *) mp;
}

void
spdk_mempool_free(struct spdk_mempool *mp)
{
	if (mp) {
		free((void *)mp);
	}
}

void *
spdk_mempool_get(struct spdk_mempool *mp)
{
	void *rp = NULL;
	if (mp) {
		struct __spdk_mempool *sm = (struct __spdk_mempool *) mp;
		if (sm->free > 0) {
			sm->free--;
			rp = malloc(sm->ele_size);
		}
	}

	return rp;
}

void
spdk_mempool_put(struct spdk_mempool *mp, void *ele)
{
	if (mp && ele) {
		struct __spdk_mempool *sm = (struct __spdk_mempool *) mp;
		free(ele);
		sm->free++;
	}
}

struct spdk_event *
spdk_event_allocate(uint32_t lcore, spdk_event_fn fn, void *arg1, void *arg2)
{
	struct spdk_event *event = malloc(sizeof(struct spdk_event));

	if (event == NULL) {
		return NULL;
	}

	event->lcore = lcore;
	event->fn = fn;
	event->arg1 = arg1;
	event->arg2 = arg2;

	return event;
}

void
spdk_event_call(struct spdk_event *event)
{
	if (event) {
		if (event->fn) {
			event->fn(event->arg1, event->arg2);
		}
		free(event);
	}
}

unsigned
spdk_env_get_master_lcore(void)
{
	return 0;
}

void
spdk_nvmf_handle_connect(struct spdk_nvmf_request *req)
{
}

void
spdk_nvmf_session_disconnect(struct spdk_nvmf_conn *conn)
{
}

static int
nvmf_fc_ls_ut_remove_conn(struct spdk_nvmf_session *session,
			  struct spdk_nvmf_conn *conn)
{
	return 0;
}

struct spdk_nvmf_subsystem *
spdk_nvmf_find_subsystem(const char *subnqn)
{
	/* don't care about subsystem check - return subnqn */
	return (struct spdk_nvmf_subsystem *) subnqn;
}

bool
spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem,
				 const char *hostnqn)
{
	return true;
}

/* ********* the tests ********* */

static void
run_create_assoc_test(struct spdk_nvmf_fc_nport *tgtport)
{
	struct nvmf_fc_ls_rqst ls_rqst;
	struct nvmf_fc_ls_cr_assoc_rqst ca_rqst;
	uint8_t respbuf[128];

	memset(&ca_rqst, 0, sizeof(struct nvmf_fc_ls_cr_assoc_rqst));

	ca_rqst.w0.ls_cmd = FCNVME_LS_CREATE_ASSOCIATION;
	to_be32(&ca_rqst.desc_list_len,
		sizeof(struct nvmf_fc_ls_cr_assoc_rqst) -
		(2 * sizeof(uint32_t)));
	to_be32(&ca_rqst.assoc_cmd.desc_tag, FCNVME_LSDESC_CREATE_ASSOC_CMD);
	to_be32(&ca_rqst.assoc_cmd.desc_len,
		sizeof(struct nvmf_fc_lsdesc_cr_assoc_cmd) -
		(2 * sizeof(uint32_t)));
	to_be16(&ca_rqst.assoc_cmd.ersp_ratio, 5);
	to_be16(&ca_rqst.assoc_cmd.sqsize, 32);

	ls_rqst.rqstbuf.virt = &ca_rqst;
	ls_rqst.rspbuf.virt = respbuf;
	ls_rqst.rqst_len = sizeof(struct nvmf_fc_ls_cr_assoc_rqst);
	ls_rqst.rsp_len = 0;
	ls_rqst.rpi = 5000;
	ls_rqst.private_data = NULL;

	spdk_nvmf_fc_handle_ls_rqst(tgtport, &ls_rqst);
}


static void
run_create_conn_test(struct spdk_nvmf_fc_nport *tgtport,
		     uint64_t assoc_id)
{
	struct nvmf_fc_ls_rqst ls_rqst;
	struct nvmf_fc_ls_cr_conn_rqst cc_rqst;
	uint8_t respbuf[128];

	memset(&cc_rqst, 0, sizeof(struct nvmf_fc_ls_cr_conn_rqst));

	/* fill in request descriptor */
	cc_rqst.w0.ls_cmd = FCNVME_LS_CREATE_CONNECTION;
	to_be32(&cc_rqst.desc_list_len,
		sizeof(struct nvmf_fc_ls_cr_conn_rqst) -
		(2 * sizeof(uint32_t)));

	/* fill in connect command descriptor */
	to_be32(&cc_rqst.connect_cmd.desc_tag, FCNVME_LSDESC_CREATE_CONN_CMD);
	to_be32(&cc_rqst.connect_cmd.desc_len,
		sizeof(struct nvmf_fc_lsdesc_cr_conn_cmd) -
		(2 * sizeof(uint32_t)));
	to_be16(&cc_rqst.connect_cmd.ersp_ratio, 20);
	to_be16(&cc_rqst.connect_cmd.sqsize, 1024);

	/* fill in association id descriptor */
	to_be32(&cc_rqst.assoc_id.desc_tag, FCNVME_LSDESC_ASSOC_ID),
		to_be32(&cc_rqst.assoc_id.desc_len,
			sizeof(struct nvmf_fc_lsdesc_assoc_id) -
			(2 * sizeof(uint32_t)));
	cc_rqst.assoc_id.association_id = assoc_id; /* alreday be64 */

	ls_rqst.rqstbuf.virt = &cc_rqst;
	ls_rqst.rspbuf.virt = respbuf;
	ls_rqst.rqst_len = sizeof(struct nvmf_fc_ls_cr_conn_rqst);
	ls_rqst.rsp_len = 0;
	ls_rqst.rpi = 5000;
	ls_rqst.private_data = NULL;

	spdk_nvmf_fc_handle_ls_rqst(tgtport, &ls_rqst);
}

static void
run_disconn_test(struct spdk_nvmf_fc_nport *tgtport,
		 uint64_t assoc_id)
{
	struct nvmf_fc_ls_rqst ls_rqst;
	struct nvmf_fc_ls_disconnect_rqst dc_rqst;
	uint8_t respbuf[128];

	memset(&dc_rqst, 0, sizeof(struct nvmf_fc_ls_disconnect_rqst));

	/* fill in request descriptor */
	dc_rqst.w0.ls_cmd = FCNVME_LS_DISCONNECT;
	to_be32(&dc_rqst.desc_list_len,
		sizeof(struct nvmf_fc_ls_disconnect_rqst) -
		(2 * sizeof(uint32_t)));

	/* fill in disconnect command descriptor */
	to_be32(&dc_rqst.disconn_cmd.desc_tag, FCNVME_LSDESC_DISCONN_CMD);
	to_be32(&dc_rqst.disconn_cmd.desc_len,
		sizeof(struct nvmf_fc_lsdesc_disconn_cmd) -
		(2 * sizeof(uint32_t)));

	/* fill in association id descriptor */
	to_be32(&dc_rqst.assoc_id.desc_tag, FCNVME_LSDESC_ASSOC_ID),
		to_be32(&dc_rqst.assoc_id.desc_len,
			sizeof(struct nvmf_fc_lsdesc_assoc_id) -
			(2 * sizeof(uint32_t)));
	dc_rqst.assoc_id.association_id = assoc_id; /* alreday be64 */

	ls_rqst.rqstbuf.virt = &dc_rqst;
	ls_rqst.rspbuf.virt = respbuf;
	ls_rqst.rqst_len = sizeof(struct nvmf_fc_ls_disconnect_rqst);
	ls_rqst.rsp_len = 0;
	ls_rqst.rpi = 5000;
	ls_rqst.private_data = NULL;

	spdk_nvmf_fc_handle_ls_rqst(tgtport, &ls_rqst);
}

static int
handle_ca_rsp(struct nvmf_fc_ls_rqst *ls_rqst)
{
	struct nvmf_fc_ls_acc_hdr *acc_hdr =
		(struct nvmf_fc_ls_acc_hdr *) ls_rqst->rspbuf.virt;


	if (acc_hdr->rqst.w0.ls_cmd == FCNVME_LS_CREATE_ASSOCIATION) {
		if (acc_hdr->w0.ls_cmd == FCNVME_LS_ACC) {
			struct nvmf_fc_ls_cr_assoc_acc *acc =
				(struct nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;

			CU_ASSERT(from_be32(&acc_hdr->desc_list_len) ==
				  sizeof(struct nvmf_fc_ls_cr_assoc_acc) - 8);
			CU_ASSERT(from_be32(&acc_hdr->rqst.desc_len) ==
				  sizeof(struct nvmf_fc_lsdesc_rqst) - 8);
			CU_ASSERT(from_be32(&acc_hdr->rqst.desc_tag) ==
				  FCNVME_LSDESC_RQST);
			CU_ASSERT(from_be32(&acc->assoc_id.desc_tag) ==
				  FCNVME_LSDESC_ASSOC_ID);
			CU_ASSERT(from_be32(&acc->assoc_id.desc_len) ==
				  sizeof(struct nvmf_fc_lsdesc_assoc_id) - 8);
			CU_ASSERT(from_be32(&acc->conn_id.desc_tag) ==
				  FCNVME_LSDESC_CONN_ID);
			CU_ASSERT(from_be32(&acc->conn_id.desc_len) ==
				  sizeof(struct nvmf_fc_lsdesc_conn_id) - 8);

			g_curr_assoc_id = acc->assoc_id.association_id;
			g_create_conn_test_cnt++;
			return 0;
		} else {
			CU_FAIL("Reject response for create association");
		}
	} else {
		CU_FAIL("Response not for create association");
	}

	return 1;
}

static int
handle_cc_rsp(struct nvmf_fc_ls_rqst *ls_rqst)
{
	struct nvmf_fc_ls_acc_hdr *acc_hdr =
		(struct nvmf_fc_ls_acc_hdr *) ls_rqst->rspbuf.virt;

	if (acc_hdr->rqst.w0.ls_cmd == FCNVME_LS_CREATE_CONNECTION) {
		if (acc_hdr->w0.ls_cmd == FCNVME_LS_ACC) {
			struct nvmf_fc_ls_cr_conn_acc *acc =
				(struct nvmf_fc_ls_cr_conn_acc *)ls_rqst->rspbuf.virt;

			CU_ASSERT(from_be32(&acc_hdr->desc_list_len) ==
				  sizeof(struct nvmf_fc_ls_cr_conn_acc) - 8);
			CU_ASSERT(from_be32(&acc_hdr->rqst.desc_len) ==
				  sizeof(struct nvmf_fc_lsdesc_rqst) - 8);
			CU_ASSERT(from_be32(&acc_hdr->rqst.desc_tag) ==
				  FCNVME_LSDESC_RQST);
			CU_ASSERT(from_be32(&acc->conn_id.desc_tag) ==
				  FCNVME_LSDESC_CONN_ID);
			CU_ASSERT(from_be32(&acc->conn_id.desc_len) ==
				  sizeof(struct nvmf_fc_lsdesc_conn_id) - 8);
			g_create_conn_test_cnt++;
			return 0;
		}

		if (acc_hdr->w0.ls_cmd == FCNVME_LS_RJT) {
			struct nvmf_fc_ls_rjt *rjt =
				(struct nvmf_fc_ls_rjt *)ls_rqst->rspbuf.virt;
			if (g_create_conn_test_cnt == g_nvmf_tgt.config.max_queues_per_session) {
				CU_ASSERT(rjt->rjt.reason_code ==
					  FCNVME_RJT_RC_INV_PARAM);
				CU_ASSERT(rjt->rjt.reason_explanation ==
					  FCNVME_RJT_EXP_INV_Q_ID);
			} else {
				CU_FAIL("Unexpected reject response for create connection");
			}
		} else {
			CU_FAIL("Unexpected response code for create connection");
		}
	} else {
		CU_FAIL("Response not for create connection");
	}

	return 1;
}

static int
handle_disconn_rsp(struct nvmf_fc_ls_rqst *ls_rqst)
{
	struct nvmf_fc_ls_acc_hdr *acc_hdr =
		(struct nvmf_fc_ls_acc_hdr *) ls_rqst->rspbuf.virt;

	if (acc_hdr->rqst.w0.ls_cmd == FCNVME_LS_DISCONNECT) {
		if (acc_hdr->w0.ls_cmd == FCNVME_LS_ACC) {
			CU_ASSERT(from_be32(&acc_hdr->desc_list_len) ==
				  sizeof(struct nvmf_fc_ls_disconnect_acc) - 8);
			CU_ASSERT(from_be32(&acc_hdr->rqst.desc_len) ==
				  sizeof(struct nvmf_fc_lsdesc_rqst) - 8);
			CU_ASSERT(from_be32(&acc_hdr->rqst.desc_tag) ==
				  FCNVME_LSDESC_RQST);
			return 0;
		} else {
			CU_FAIL("Unexpected reject response for disconnect");
		}
	} else {
		CU_FAIL("Response not for create connection");
	}

	return 1;
}

static int
handle_conn_bad_assoc_rsp(struct nvmf_fc_ls_rqst *ls_rqst)
{
	struct nvmf_fc_ls_acc_hdr *acc_hdr =
		(struct nvmf_fc_ls_acc_hdr *) ls_rqst->rspbuf.virt;

	if (acc_hdr->rqst.w0.ls_cmd == FCNVME_LS_CREATE_CONNECTION) {
		if (acc_hdr->w0.ls_cmd == FCNVME_LS_RJT) {
			struct nvmf_fc_ls_rjt *rjt =
				(struct nvmf_fc_ls_rjt *)ls_rqst->rspbuf.virt;

			CU_ASSERT(from_be32(&rjt->desc_list_len) ==
				  sizeof(struct nvmf_fc_ls_rjt) - 8);
			CU_ASSERT(from_be32(&rjt->rqst.desc_tag) ==
				  FCNVME_LSDESC_RQST);
			CU_ASSERT(from_be32(&rjt->rjt.desc_len) ==
				  sizeof(struct nvmf_fc_lsdesc_rjt) - 8);
			CU_ASSERT(from_be32(&rjt->rjt.desc_tag) ==
				  FCNVME_LSDESC_RJT);
			CU_ASSERT(rjt->rjt.reason_code ==
				  FCNVME_RJT_RC_INV_ASSOC);
			CU_ASSERT(rjt->rjt.reason_explanation ==
				  FCNVME_RJT_EXP_NONE);
			return 0;
		} else {
			CU_FAIL("Unexpected accept response for create conn. on bad assoc_id");
		}
	} else {
		CU_FAIL("Response not for create connection on bad assoc_id");
	}

	return 1;
}

static void
test_process_ls_cmds(void)
{
	struct spdk_nvmf_fc_port fcport;
	struct spdk_nvmf_fc_nport tgtport;
	uint32_t i;
	uint64_t assoc_id[1024];

	spdk_nvmf_fc_ls_init();

	fcport.max_io_queues = 16;
	for (i = 0; i < fcport.max_io_queues; i++) {
		fcport.io_queues[i].lcore_id = i;
		fcport.io_queues[i].fc_port = &fcport;
		fcport.io_queues[i].num_conns = 0;
		fcport.io_queues[i].cid_cnt = 0;
		TAILQ_INIT(&fcport.io_queues[i].connection_list);
		TAILQ_INIT(&fcport.io_queues[i].in_use_reqs);
	}

	tgtport.fc_port = &fcport;
	TAILQ_INIT(&tgtport.rem_port_list);
	TAILQ_INIT(&tgtport.fc_associations);

	g_test_run_type = TEST_RUN_TYPE_CREATE_ASSOC;

	run_create_assoc_test(&tgtport);
	if (g_last_rslt == 0) {
		g_test_run_type = TEST_RUN_TYPE_CREATE_CONN;
		while (g_last_rslt == 0)
			run_create_conn_test(&tgtport, g_curr_assoc_id);
		g_test_run_type = TEST_RUN_TYPE_DISCONNECT;
		run_disconn_test(&tgtport, g_curr_assoc_id);
		g_create_conn_test_cnt = 0;
		g_test_run_type = TEST_RUN_TYPE_CONN_BAD_ASSOC;
		run_create_conn_test(&tgtport, g_curr_assoc_id);
	}

	for (i = 0; i < g_nvmf_tgt.config.max_associations; i++) {
		g_test_run_type = TEST_RUN_TYPE_CREATE_ASSOC;
		run_create_assoc_test(&tgtport);
		if (g_last_rslt == 0) {
			int j;
			assoc_id[i] = g_curr_assoc_id;
			g_test_run_type = TEST_RUN_TYPE_CREATE_CONN;
			for (j = 1; j < g_nvmf_tgt.config.max_queues_per_session; j++) {
				if (g_last_rslt == 0) {
					run_create_conn_test(&tgtport, g_curr_assoc_id);
				}
			}
		} else  {
			break;
		}
	}

	g_test_run_type = TEST_RUN_TYPE_DISCONNECT;
	for (i = 0; i < g_nvmf_tgt.config.max_associations; i++) {
		if (g_last_rslt == 0) {
			run_disconn_test(&tgtport, assoc_id[i]);
		}
	}

	spdk_nvmf_fc_ls_fini();
}

int
bcm_nvmf_fc_xmt_ls_rsp(struct spdk_nvmf_fc_nport *tgtport,
		       struct nvmf_fc_ls_rqst *ls_rqst)
{
	switch (g_test_run_type) {
	case TEST_RUN_TYPE_CREATE_ASSOC:
		g_last_rslt = handle_ca_rsp(ls_rqst);
		break;
	case TEST_RUN_TYPE_CREATE_CONN:
		g_last_rslt = handle_cc_rsp(ls_rqst);
		break;
	case TEST_RUN_TYPE_DISCONNECT:
		g_last_rslt = handle_disconn_rsp(ls_rqst);
		break;
	case TEST_RUN_TYPE_CONN_BAD_ASSOC:
		g_last_rslt = handle_conn_bad_assoc_rsp(ls_rqst);
		break;

	default:
		CU_FAIL("LS Response for Invalid Test Type");
		g_last_rslt = 1;
	}

	return g_last_rslt;
}

int bcm_nvmf_fc_issue_abort(struct fc_hwqp *hwqp, fc_xri_t *xri, bool send_abts,
			    bcm_fc_caller_cb cb, void *cb_args)
{
	return 0;
}


int main(int argc, char **argv)
{
	unsigned int	num_failures = 0;

	CU_pSuite	suite = NULL;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvmf", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "process_ls_commands", test_process_ls_cmds) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}

const struct spdk_nvmf_transport spdk_nvmf_transport_bcm_fc = {

	.name = NVMF_BCM_FC_TRANSPORT_NAME,
	.transport_init = NULL,
	.transport_fini = NULL,

	.acceptor_poll = NULL,

	.listen_addr_add = NULL,
	.listen_addr_remove = NULL,
	.listen_addr_discover = NULL,

	.session_init = NULL,
	.session_fini = NULL,
	.session_add_conn = NULL,
	.session_remove_conn = nvmf_fc_ls_ut_remove_conn,

	.req_complete = NULL,

	.conn_fini = NULL,
	.conn_poll = NULL,
	.conn_is_idle = NULL

};
