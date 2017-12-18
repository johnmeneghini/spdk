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

#include "spdk/env.h"
#include "spdk_cunit.h"
#include "spdk/nvmf.h"
#include "spdk_internal/event.h"
#include "spdk/endian.h"
#include "log/log_syslog.c"

#include "nvmf_fc/bcm_fc.h"
#include "nvmf/transport.h"
#include "nvmf/nvmf_internal.h"
#include "nvmf/subsystem.h"
#include "nvmf/session.h"
#include "spdk/trace.h"
#include "spdk_internal/log.h"

extern struct spdk_trace_flag SPDK_TRACE_NVMF_BCM_FC_LS;

struct spdk_trace_flag SPDK_TRACE_NVMF = {
	.name = "nvmf",
	.enabled = false,   /* make this true to enable TRACE output */
};

/*
 * SPDK Stuff
 */

void spdk_post_event(void *context, struct spdk_event *event);

int spdk_nvmf_bcm_fc_xmt_ls_rsp(struct spdk_nvmf_bcm_fc_nport *tgtport,
				struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst);
void spdk_nvmf_bcm_fc_req_abort(struct spdk_nvmf_bcm_fc_request *fc_req, bool send_abts,
				spdk_nvmf_bcm_fc_caller_cb cb, void *cb_args);

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

uint64_t spdk_vtophys(void *buf)
{
	return (uint64_t)buf;
}

struct __spdk_mempool_entry {
	void *ptr;
	TAILQ_ENTRY(__spdk_mempool_entry) link;
};

struct __spdk_mempool {
	char name[256];
	TAILQ_HEAD(, __spdk_mempool_entry) avail_list;
};

struct spdk_mempool *
spdk_mempool_create(const char *name, size_t count,
		    size_t ele_size, size_t cache_size, int socket_id)

{
	void *p = malloc(sizeof(struct __spdk_mempool) + (count *
			 (sizeof(struct __spdk_mempool_entry) + ele_size)));

	if (p) {
		struct __spdk_mempool *mp = (struct __spdk_mempool *) p;
		size_t i;

		TAILQ_INIT(&mp->avail_list);

		p += sizeof(struct __spdk_mempool);

		for (i = 0; i < count; i++) {
			struct __spdk_mempool_entry *mpe = p;
			mpe->ptr = p + sizeof(struct __spdk_mempool_entry);
			TAILQ_INSERT_TAIL(&mp->avail_list, mpe, link);
			p += (sizeof(struct __spdk_mempool_entry) + ele_size);
		}

		if (name) {
			strcpy(mp->name, name);
		} else {
			mp->name[0] = 0;
		}

		return (struct spdk_mempool *) mp;
	}

	return NULL;
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
	if (mp) {
		struct __spdk_mempool *m = (struct __spdk_mempool *) mp;
		struct __spdk_mempool_entry *mpe = TAILQ_FIRST(&m->avail_list);

		if (mpe) {
			TAILQ_REMOVE(&m->avail_list, mpe, link);
			return mpe->ptr;
		}
	}

	return NULL;
}

void
spdk_mempool_put(struct spdk_mempool *mp, void *ele)
{
	if (mp && ele) {
		struct __spdk_mempool *m = (struct __spdk_mempool *) mp;
		struct __spdk_mempool_entry *mpe =
			ele - sizeof(struct __spdk_mempool_entry);

		TAILQ_INSERT_TAIL(&m->avail_list, mpe, link);
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

void
spdk_post_event(void *context, struct spdk_event *event)
{
	spdk_event_call(event);
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

struct spdk_nvmf_subsystem *
spdk_nvmf_find_subsystem_with_cntlid(uint16_t cntlid)
{
	return NULL;
}

int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	return -1;
}

static int
nvmf_fc_ls_ut_remove_conn(struct spdk_nvmf_session *session,
			  struct spdk_nvmf_conn *conn)
{
	return 0;
}

struct spdk_nvmf_subsystem g_nvmf_subsys;

struct spdk_nvmf_subsystem *
nvmf_find_subsystem(const char *subnqn)
{
	g_nvmf_subsys.disconnect_cb = spdk_nvmf_bcm_fc_subsys_disconnect_cb;
	return &g_nvmf_subsys;
}

bool
spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem,
				 const char *hostnqn)
{
	return true;
}

bool
spdk_nvmf_valid_nqn(const char *nqn)
{
	if (!memchr(nqn, '\0', SPDK_NVMF_NQN_MAX_LEN)) {
		SPDK_ERRLOG("Invalid NQN length > max %d\n", SPDK_NVMF_NQN_MAX_LEN - 1);
		return false;
	}

	if (strncmp(nqn, "nqn.", 4) != 0) {
		SPDK_ERRLOG("Invalid NQN \"%s\": NQN must begin with \"nqn.\".\n", nqn);
		return false;
	}

	/* yyyy-mm. */
	if (!(isdigit(nqn[4]) && isdigit(nqn[5]) && isdigit(nqn[6]) && isdigit(nqn[7]) &&
	      nqn[8] == '-' && isdigit(nqn[9]) && isdigit(nqn[10]) && nqn[11] == '.')) {
		SPDK_ERRLOG("Invalid date code in NQN \"%s\"\n", nqn);
		return false;
	}

	return true;
}

#if 0
static const char *fc_ut_bad_subsystem =
	"nqn.com.broadcom:sn.300a0989adc53390c0dc7c87011e786b:subsystem.bad";

static struct spdk_nvmf_host fc_ut_bad_host = {
	.nqn = "nqn.bad_fc_host",
	.max_aq_depth = 128,
	.max_queue_depth = 1024,
	.max_connections_allowed = 32,
};
#endif

static const char *fc_ut_good_subsystem =
	"nqn.2017-11.com.broadcom:sn.390c0dc7c87011e786b300a0989adc53:subsystem.good";

static struct spdk_nvmf_host fc_ut_target = {
	.nqn = "nqn.2017-11.fc_host",
	.max_aq_depth = 32,
	.max_queue_depth = 16,
	.max_connections_allowed = 2,
};

static struct spdk_nvmf_host fc_ut_initiator = {
	.nqn = "nqn.2017-11.fc_host",
	.max_aq_depth = 31,
	.max_queue_depth = 15,
	.max_connections_allowed = 2,
};

static struct spdk_nvmf_host *fc_ut_host = &fc_ut_initiator;

struct spdk_nvmf_host *
spdk_nvmf_find_subsystem_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	if (!hostnqn) {
		SPDK_ERRLOG("hostnqn is NULL\n");
		return NULL;
	}

	if (strcmp(hostnqn, fc_ut_target.nqn) == 0) {
		return &fc_ut_target;
	}

	SPDK_ERRLOG("Hostnqn %s not found on Subsystem %s\n", hostnqn, subsystem->subnqn);

	return NULL;
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

/*
 *  The Tests
 */

static uint32_t g_test_run_type = 0;

enum _test_run_type {
	TEST_RUN_TYPE_CREATE_ASSOC = 1,
	TEST_RUN_TYPE_CREATE_CONN,
	TEST_RUN_TYPE_DISCONNECT,
	TEST_RUN_TYPE_CONN_BAD_ASSOC,
	TEST_RUN_TYPE_DIR_DISCONN_CALL,
};

static uint64_t g_curr_assoc_id = 0;
static uint32_t g_create_conn_test_cnt = 0;
static int g_last_rslt = 0;
static bool g_spdk_nvmf_bcm_fc_xmt_srsr_req = false;


static void
run_create_assoc_test(const char *subnqn,
		      struct spdk_nvmf_host *host,
		      struct spdk_nvmf_bcm_fc_nport *tgtport)
{
	struct spdk_nvmf_bcm_fc_ls_rqst ls_rqst;
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
	to_be16(&ca_rqst.assoc_cmd.ersp_ratio, (host->max_aq_depth / 2));
	to_be16(&ca_rqst.assoc_cmd.sqsize, host->max_aq_depth);
	snprintf(&ca_rqst.assoc_cmd.subnqn[0], strlen(subnqn) + 1, "%s", subnqn);
	snprintf(&ca_rqst.assoc_cmd.hostnqn[0], strlen(host->nqn) + 1, "%s", host->nqn);
	ls_rqst.rqstbuf.virt = &ca_rqst;
	ls_rqst.rspbuf.virt = respbuf;
	ls_rqst.rqst_len = sizeof(struct nvmf_fc_ls_cr_assoc_rqst);
	ls_rqst.rsp_len = 0;
	ls_rqst.rpi = 5000;
	ls_rqst.private_data = NULL;

	spdk_nvmf_bcm_fc_handle_ls_rqst(0, tgtport, &ls_rqst);
}


static void
run_create_conn_test(struct spdk_nvmf_host *host,
		     struct spdk_nvmf_bcm_fc_nport *tgtport,
		     uint64_t assoc_id)
{
	struct spdk_nvmf_bcm_fc_ls_rqst ls_rqst;
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

	to_be16(&cc_rqst.connect_cmd.ersp_ratio, (host->max_queue_depth / 2));
	to_be16(&cc_rqst.connect_cmd.sqsize, host->max_queue_depth);

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

	spdk_nvmf_bcm_fc_handle_ls_rqst(0, tgtport, &ls_rqst);
}

static void
run_disconn_test(struct spdk_nvmf_bcm_fc_nport *tgtport,
		 uint64_t assoc_id)
{
	struct spdk_nvmf_bcm_fc_ls_rqst ls_rqst;
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

	spdk_nvmf_bcm_fc_handle_ls_rqst(0, tgtport, &ls_rqst);
}

static void
disconnect_assoc_cb(void *cb_data, uint32_t err)
{
	CU_ASSERT(err == 0);
}

static void
run_direct_disconn_test(struct spdk_nvmf_bcm_fc_nport *tgtport,
			uint64_t assoc_id, bool send_abts)
{
	int ret;

	g_spdk_nvmf_bcm_fc_xmt_srsr_req = false;

	ret = spdk_nvmf_bcm_fc_delete_association(tgtport, assoc_id,
			send_abts,
			disconnect_assoc_cb, 0);

	CU_ASSERT(ret == 0);
#if 0
	if (ret == 0) {
		/* check that LS disconnect was sent */
		CU_ASSERT(g_spdk_nvmf_bcm_fc_xmt_srsr_req);
	}
#endif
}

static int
handle_ca_rsp(struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst)
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
			CU_FAIL("Unexpected reject response for create association");
		}
	} else {
		CU_FAIL("Response not for create association");
	}

	return 1;
}

static int
handle_cc_rsp(struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst)
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
			if (g_create_conn_test_cnt == fc_ut_host->max_connections_allowed) {
				/* expected to get reject for too many connections */
				CU_ASSERT(rjt->rjt.reason_code ==
					  FCNVME_RJT_RC_INV_PARAM);
				CU_ASSERT(rjt->rjt.reason_explanation ==
					  FCNVME_RJT_EXP_INV_Q_ID);
			} else {
				CU_FAIL("Unexpected reject response create connection");
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
handle_disconn_rsp(struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst)
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
handle_conn_bad_assoc_rsp(struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst)
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

static struct spdk_nvmf_bcm_fc_port fcport;
static struct spdk_nvmf_bcm_fc_nport tgtport;
static struct spdk_nvmf_bcm_fc_remote_port_info rport;

static uint64_t assoc_id[1024];
struct spdk_nvmf_tgt g_nvmf_tgt;

static void
ls_tests_init(void)
{
	uint16_t i;

	bzero(&g_nvmf_tgt, sizeof(g_nvmf_tgt));
	g_nvmf_tgt.opts.max_aq_depth = 32;
	g_nvmf_tgt.opts.max_queue_depth = 1024;
	g_nvmf_tgt.opts.max_queues_per_session = 4;

	bzero(&fcport, sizeof(struct spdk_nvmf_bcm_fc_port));
	fcport.hw_port_status = SPDK_FC_PORT_ONLINE;
	fcport.max_io_queues = 16;
	for (i = 0; i < fcport.max_io_queues; i++) {
		fcport.io_queues[i].lcore_id = i;
		fcport.io_queues[i].fc_port = &fcport;
		fcport.io_queues[i].num_conns = 0;
		fcport.io_queues[i].cid_cnt = 0;
		fcport.io_queues[i].free_q_slots = 1024;
		fcport.io_queues[i].queues.rq_payload.num_buffers = 1024;
		TAILQ_INIT(&fcport.io_queues[i].connection_list);
		TAILQ_INIT(&fcport.io_queues[i].in_use_reqs);
	}

	spdk_nvmf_bcm_fc_ls_init(&fcport);

	bzero(&tgtport, sizeof(struct spdk_nvmf_bcm_fc_nport));
	tgtport.fc_port = &fcport;
	TAILQ_INIT(&tgtport.rem_port_list);
	TAILQ_INIT(&tgtport.fc_associations);

	bzero(&rport, sizeof(struct spdk_nvmf_bcm_fc_remote_port_info));
	TAILQ_INSERT_TAIL(&tgtport.rem_port_list, &rport, link);
}

static void
ls_tests_fini(void)
{
	spdk_nvmf_bcm_fc_ls_fini(&fcport);
}

static void
create_assoc_test(void)
{
	/* main test driver */
	g_test_run_type = TEST_RUN_TYPE_CREATE_ASSOC;
	run_create_assoc_test(fc_ut_good_subsystem, fc_ut_host, &tgtport);

	if (g_last_rslt == 0) {
		g_test_run_type = TEST_RUN_TYPE_CREATE_CONN;
		/* create connections until we get too many connections error */
		while (g_last_rslt == 0)
			run_create_conn_test(fc_ut_host, &tgtport, g_curr_assoc_id);

		/* disconnect the association */
		g_test_run_type = TEST_RUN_TYPE_DISCONNECT;
		run_disconn_test(&tgtport, g_curr_assoc_id);
		g_create_conn_test_cnt = 0;
	}
}

static void
invalid_connection_test(void)
{
	/* run test to create connection to invalid association */
	g_test_run_type = TEST_RUN_TYPE_CONN_BAD_ASSOC;
	run_create_conn_test(fc_ut_host, &tgtport, g_curr_assoc_id);
}

static void
create_max_assoc_conns_test(void)
{
	/* run test to create max. associations with max. connections */
	uint16_t i;

	g_last_rslt = 0;
	for (i = 0; i < fcport.ls_rsrc_pool.assocs_count &&
	     g_last_rslt == 0; i++) {
		g_test_run_type = TEST_RUN_TYPE_CREATE_ASSOC;
		run_create_assoc_test(fc_ut_good_subsystem, fc_ut_host, &tgtport);
		if (g_last_rslt == 0) {
			int j;
			assoc_id[i] = g_curr_assoc_id;
			g_test_run_type = TEST_RUN_TYPE_CREATE_CONN;
			for (j = 1; j < fc_ut_host->max_connections_allowed; j++) {
				if (g_last_rslt == 0) {
					run_create_conn_test(fc_ut_host, &tgtport, g_curr_assoc_id);
				}
			}
		}
	}
}

static void
direct_delete_assoc_test(void)
{
	uint16_t i;

	if (g_last_rslt == 0) {
		/* remove associations by calling delete directly */
		g_test_run_type = TEST_RUN_TYPE_DIR_DISCONN_CALL;
		for (i = 0; i < fcport.ls_rsrc_pool.assocs_count; i++) {
			run_direct_disconn_test(&tgtport,
						from_be64(&assoc_id[i]),
						true);
		}
	}
}

/*
 * SPDK functions that are called by LS processing
 */

int
spdk_nvmf_bcm_fc_xmt_ls_rsp(struct spdk_nvmf_bcm_fc_nport *tgtport,
			    struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst)
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

int
spdk_nvmf_bcm_fc_xmt_srsr_req(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
			      struct spdk_nvmf_bcm_fc_send_srsr *srsr,
			      spdk_nvmf_bcm_fc_caller_cb cb, void *cb_args)
{
	struct nvmf_fc_ls_disconnect_rqst *dc_rqst =
		(struct nvmf_fc_ls_disconnect_rqst *)
		srsr->rqst.virt;

	CU_ASSERT(dc_rqst->w0.ls_cmd == FCNVME_LS_DISCONNECT);
	CU_ASSERT(from_be32(&dc_rqst->desc_list_len) ==
		  sizeof(struct nvmf_fc_ls_disconnect_rqst) -
		  (2 * sizeof(uint32_t)));
	CU_ASSERT(from_be32(&dc_rqst->assoc_id.desc_tag) ==
		  FCNVME_LSDESC_ASSOC_ID);
	CU_ASSERT(from_be32(&dc_rqst->assoc_id.desc_len) ==
		  sizeof(struct nvmf_fc_lsdesc_assoc_id) -
		  (2 * sizeof(uint32_t)));

	/*  gets called from spkd_nvmf_bcm_fc_delete association() test */
	g_spdk_nvmf_bcm_fc_xmt_srsr_req = true;


	return 0;
}

void
spdk_nvmf_bcm_fc_req_abort(struct spdk_nvmf_bcm_fc_request *fc_req,
			   bool send_abts, spdk_nvmf_bcm_fc_caller_cb cb,
			   void *cb_args)
{
	return;
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

static void
usage(const char *program_name)
{
	printf("%s options\n", program_name);
	printf("\t[-d <value> Enable trace level 0..2 (default: 0)]\n");
	printf("\t[-a <value> Admin Queue depth (default: %u)]\n",
	       fc_ut_target.max_aq_depth);
	printf("\t[-q <value> IO Queue depth (default: %u)]\n",
	       fc_ut_target.max_queue_depth);
	printf("\t[-c <value> IO Queue count (default: %u)]\n",
	       fc_ut_target.max_connections_allowed);
}

int main(int argc, char **argv)
{
	unsigned int	num_failures = 0;
	CU_pSuite	suite = NULL;
	int trace_level = 0;
	uint16_t max_aq_depth = 32;
	uint16_t max_queue_depth = 16;
	uint16_t max_connections_allowed = 2;
	int op;

	while ((op = getopt(argc, argv, "a:q:c:d:")) != -1) {
		switch (op) {
		case 'a':
			max_aq_depth = (uint16_t) atoi(optarg);
			break;
		case 'q':
			max_queue_depth = (uint16_t) atoi(optarg);
			break;
		case 'c':
			max_connections_allowed = atoi(optarg);
			break;
		case 'd':
			trace_level = atoi(optarg);
			break;
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	fc_ut_target.max_aq_depth = max_aq_depth;
	fc_ut_target.max_queue_depth = max_queue_depth;
	fc_ut_target.max_connections_allowed = max_connections_allowed;

	switch (trace_level) {
	case 2:
		SPDK_TRACE_NVMF_BCM_FC_LS.enabled = true;
	/* fall through */
	case 1:
		SPDK_TRACE_NVMF.enabled = true;
	/* fall through */
	default:
		break;
	}

	fc_ut_initiator.max_aq_depth = max_aq_depth - 1;
	fc_ut_initiator.max_queue_depth = max_queue_depth - 1;
	fc_ut_initiator.max_connections_allowed = max_connections_allowed;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("FC-NVMe LS", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "CASS/CIOC/DISC", create_assoc_test) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}


	if (CU_add_test(suite, "CIOC to bad assoc_id", invalid_connection_test) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "Max. assocs/conns", create_max_assoc_conns_test) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "Delete assoc API", direct_delete_assoc_test) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	ls_tests_init();

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	ls_tests_fini();

	return num_failures;
}
