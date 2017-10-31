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



/*
 * SPDK Stuff
 */

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

struct spdk_nvmf_subsystem g_nvmf_subsys;



struct spdk_nvmf_subsystem *
spdk_nvmf_find_subsystem(const char *subnqn)
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
run_create_assoc_test(
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
	to_be16(&ca_rqst.assoc_cmd.ersp_ratio, 5);
	to_be16(&ca_rqst.assoc_cmd.sqsize, 32);

	ls_rqst.rqstbuf.virt = &ca_rqst;
	ls_rqst.rspbuf.virt = respbuf;
	ls_rqst.rqst_len = sizeof(struct nvmf_fc_ls_cr_assoc_rqst);
	ls_rqst.rsp_len = 0;
	ls_rqst.rpi = 5000;
	ls_rqst.private_data = NULL;

	spdk_nvmf_bcm_fc_handle_ls_rqst(0, tgtport, &ls_rqst);
}


static void
run_create_conn_test(struct spdk_nvmf_bcm_fc_nport *tgtport,
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
			uint64_t assoc_id, bool send_disconn, bool send_abts)
{
	int ret;

	g_spdk_nvmf_bcm_fc_xmt_srsr_req = false;

	ret = spdk_nvmf_bcm_fc_delete_association(tgtport, assoc_id,
			send_disconn, send_abts,
			disconnect_assoc_cb, 0);

	CU_ASSERT(ret == 0);
	if (ret == 0) {
		if (send_disconn) {
			CU_ASSERT(g_spdk_nvmf_bcm_fc_xmt_srsr_req);
		} else {
			/* should not have called xmt_srsr_req */
			CU_ASSERT(!g_spdk_nvmf_bcm_fc_xmt_srsr_req);
		}
	}
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
			CU_FAIL("Reject response for create association");
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
			if (g_create_conn_test_cnt == g_nvmf_tgt.opts.max_queues_per_session) {
				/* expected to get reject for too many connections */
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

	g_nvmf_tgt.opts.max_associations = 4;
	g_nvmf_tgt.opts.max_aq_depth = 32;
	g_nvmf_tgt.opts.max_queue_depth = 1024;
	g_nvmf_tgt.opts.max_queues_per_session = 4;

	spdk_nvmf_bcm_fc_ls_init();

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

	memset(&rport, 0, sizeof(struct spdk_nvmf_bcm_fc_remote_port_info));
	TAILQ_INSERT_TAIL(&tgtport.rem_port_list, &rport, link);
}

static void
ls_tests_fini(void)
{
	spdk_nvmf_bcm_fc_ls_fini();
}

static void
create_assoc_test(void)
{
	/* main test driver */
	g_test_run_type = TEST_RUN_TYPE_CREATE_ASSOC;
	run_create_assoc_test(&tgtport);

	if (g_last_rslt == 0) {
		g_test_run_type = TEST_RUN_TYPE_CREATE_CONN;
		/* create connections until we get too many connections error */
		while (g_last_rslt == 0)
			run_create_conn_test(&tgtport, g_curr_assoc_id);

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
	run_create_conn_test(&tgtport, g_curr_assoc_id);
}

static void
create_max_assoc_conns_test(void)
{
	/* run test to create max. associations with max. connections */
	uint16_t i;

	g_last_rslt = 0;
	for (i = 0; i < g_nvmf_tgt.opts.max_associations && g_last_rslt == 0; i++) {
		g_test_run_type = TEST_RUN_TYPE_CREATE_ASSOC;
		run_create_assoc_test(&tgtport);
		if (g_last_rslt == 0) {
			int j;
			assoc_id[i] = g_curr_assoc_id;
			g_test_run_type = TEST_RUN_TYPE_CREATE_CONN;
			for (j = 1; j < g_nvmf_tgt.opts.max_queues_per_session; j++) {
				if (g_last_rslt == 0) {
					run_create_conn_test(&tgtport, g_curr_assoc_id);
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
		i = 0;
		g_test_run_type = TEST_RUN_TYPE_DIR_DISCONN_CALL;
		run_direct_disconn_test(&tgtport, from_be64(&assoc_id[i++]),
					false, false);
		run_direct_disconn_test(&tgtport, from_be64(&assoc_id[i++]),
					true, false);
		run_direct_disconn_test(&tgtport, from_be64(&assoc_id[i++]),
					false, true);
		run_direct_disconn_test(&tgtport, from_be64(&assoc_id[i++]),
					true, true);

		/* remove any remaining associations */
		g_test_run_type = TEST_RUN_TYPE_DISCONNECT;
		for (; i < g_nvmf_tgt.opts.max_associations; i++) {
			run_disconn_test(&tgtport, assoc_id[i]);
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

int main(int argc, char **argv)
{
	unsigned int	num_failures = 0;

	CU_pSuite	suite = NULL;

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
