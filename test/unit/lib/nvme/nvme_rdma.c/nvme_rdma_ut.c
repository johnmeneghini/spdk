/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
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

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "nvme/nvme_rdma.c"
#include "common/lib/nvme/common_stubs.h"
#include "common/lib/test_rdma.c"

SPDK_LOG_REGISTER_COMPONENT(nvme)

DEFINE_STUB(spdk_mem_map_set_translation, int, (struct spdk_mem_map *map, uint64_t vaddr,
		uint64_t size, uint64_t translation), 0);
DEFINE_STUB(spdk_mem_map_clear_translation, int, (struct spdk_mem_map *map, uint64_t vaddr,
		uint64_t size), 0);

DEFINE_STUB(spdk_mem_map_alloc, struct spdk_mem_map *, (uint64_t default_translation,
		const struct spdk_mem_map_ops *ops, void *cb_ctx), NULL);
DEFINE_STUB_V(spdk_mem_map_free, (struct spdk_mem_map **pmap));

DEFINE_STUB(nvme_poll_group_connect_qpair, int, (struct spdk_nvme_qpair *qpair), 0);

DEFINE_STUB_V(nvme_qpair_resubmit_requests, (struct spdk_nvme_qpair *qpair, uint32_t num_requests));
DEFINE_STUB(spdk_nvme_poll_group_process_completions, int64_t, (struct spdk_nvme_poll_group *group,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb), 0)

DEFINE_STUB(rdma_ack_cm_event, int, (struct rdma_cm_event *event), 0);

struct nvme_rdma_ut_bdev_io {
	struct iovec iovs[NVME_RDMA_MAX_SGL_DESCRIPTORS];
	int iovpos;
};

/* essentially a simplification of bdev_nvme_next_sge and bdev_nvme_reset_sgl */
static void nvme_rdma_ut_reset_sgl(void *cb_arg, uint32_t offset)
{
	struct nvme_rdma_ut_bdev_io *bio = cb_arg;
	struct iovec *iov;

	for (bio->iovpos = 0; bio->iovpos < NVME_RDMA_MAX_SGL_DESCRIPTORS; bio->iovpos++) {
		iov = &bio->iovs[bio->iovpos];
		/* Only provide offsets at the beginning of an iov */
		if (offset == 0) {
			break;
		}

		offset -= iov->iov_len;
	}

	SPDK_CU_ASSERT_FATAL(bio->iovpos < NVME_RDMA_MAX_SGL_DESCRIPTORS);
}

static int nvme_rdma_ut_next_sge(void *cb_arg, void **address, uint32_t *length)
{
	struct nvme_rdma_ut_bdev_io *bio = cb_arg;
	struct iovec *iov;

	SPDK_CU_ASSERT_FATAL(bio->iovpos < NVME_RDMA_MAX_SGL_DESCRIPTORS);

	iov = &bio->iovs[bio->iovpos];

	*address = iov->iov_base;
	*length = iov->iov_len;
	bio->iovpos++;

	return 0;
}

static void
test_nvme_rdma_build_sgl_request(void)
{
	struct nvme_rdma_qpair rqpair;
	struct spdk_nvme_ctrlr ctrlr = {0};
	struct spdk_nvmf_cmd cmd = {{0}};
	struct spdk_nvme_rdma_req rdma_req = {0};
	struct nvme_request req = {{0}};
	struct nvme_rdma_ut_bdev_io bio;
	uint64_t i;
	int rc;

	ctrlr.max_sges = NVME_RDMA_MAX_SGL_DESCRIPTORS;
	ctrlr.cdata.nvmf_specific.msdbd = 16;
	ctrlr.ioccsz_bytes = 4096;

	rqpair.mr_map = (struct spdk_rdma_mem_map *)0xdeadbeef;
	rqpair.rdma_qp = (struct spdk_rdma_qp *)0xdeadbeef;
	rqpair.qpair.ctrlr = &ctrlr;
	rqpair.cmds = &cmd;
	cmd.sgl[0].address = 0x1111;
	rdma_req.id = 0;
	rdma_req.req = &req;

	req.payload.reset_sgl_fn = nvme_rdma_ut_reset_sgl;
	req.payload.next_sge_fn = nvme_rdma_ut_next_sge;
	req.payload.contig_or_cb_arg = &bio;
	req.qpair = &rqpair.qpair;

	for (i = 0; i < NVME_RDMA_MAX_SGL_DESCRIPTORS; i++) {
		bio.iovs[i].iov_base = (void *)i;
		bio.iovs[i].iov_len = 0;
	}

	/* Test case 1: single SGL. Expected: PASS */
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 0x1000;
	bio.iovs[0].iov_len = 0x1000;
	rc = nvme_rdma_build_sgl_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(bio.iovpos == 1);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.key == RDMA_UT_RKEY);
	CU_ASSERT(req.cmd.dptr.sgl1.address == (uint64_t)bio.iovs[0].iov_base);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));

	/* Test case 2: multiple SGL. Expected: PASS */
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 0x4000;
	for (i = 0; i < 4; i++) {
		bio.iovs[i].iov_len = 0x1000;
	}
	rc = nvme_rdma_build_sgl_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(bio.iovpos == 4);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == 4 * sizeof(struct spdk_nvme_sgl_descriptor));
	CU_ASSERT(req.cmd.dptr.sgl1.address == (uint64_t)0);
	CU_ASSERT(rdma_req.send_sgl[0].length == 4 * sizeof(struct spdk_nvme_sgl_descriptor) + sizeof(
			  struct spdk_nvme_cmd))
	for (i = 0; i < 4; i++) {
		CU_ASSERT(cmd.sgl[i].keyed.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK);
		CU_ASSERT(cmd.sgl[i].keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS);
		CU_ASSERT(cmd.sgl[i].keyed.length == bio.iovs[i].iov_len);
		CU_ASSERT(cmd.sgl[i].keyed.key == RDMA_UT_RKEY);
		CU_ASSERT(cmd.sgl[i].address == (uint64_t)bio.iovs[i].iov_base);
	}

	/* Test case 3: Multiple SGL, SGL 2X mr size. Expected: FAIL */
	bio.iovpos = 0;
	req.payload_offset = 0;
	g_mr_size = 0x800;
	rc = nvme_rdma_build_sgl_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc != 0);
	CU_ASSERT(bio.iovpos == 1);

	/* Test case 4: Multiple SGL, SGL size smaller than I/O size. Expected: FAIL */
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 0x6000;
	g_mr_size = 0x0;
	rc = nvme_rdma_build_sgl_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc != 0);
	CU_ASSERT(bio.iovpos == NVME_RDMA_MAX_SGL_DESCRIPTORS);

	/* Test case 5: SGL length exceeds 3 bytes. Expected: FAIL */
	req.payload_size = 0x1000 + (1 << 24);
	bio.iovs[0].iov_len = 0x1000;
	bio.iovs[1].iov_len = 1 << 24;
	rc = nvme_rdma_build_sgl_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc != 0);

	/* Test case 6: 4 SGL descriptors, size of SGL descriptors exceeds ICD. Expected: FAIL */
	ctrlr.ioccsz_bytes = 60;
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 0x4000;
	for (i = 0; i < 4; i++) {
		bio.iovs[i].iov_len = 0x1000;
	}
	rc = nvme_rdma_build_sgl_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == -1);
}

static void
test_nvme_rdma_build_sgl_inline_request(void)
{
	struct nvme_rdma_qpair rqpair;
	struct spdk_nvme_ctrlr ctrlr = {0};
	struct spdk_nvmf_cmd cmd = {{0}};
	struct spdk_nvme_rdma_req rdma_req = {0};
	struct nvme_request req = {{0}};
	struct nvme_rdma_ut_bdev_io bio;
	int rc;

	ctrlr.max_sges = NVME_RDMA_MAX_SGL_DESCRIPTORS;
	ctrlr.cdata.nvmf_specific.msdbd = 16;

	rqpair.mr_map = (struct spdk_rdma_mem_map *)0xdeadbeef;
	rqpair.rdma_qp = (struct spdk_rdma_qp *)0xdeadbeef;
	rqpair.qpair.ctrlr = &ctrlr;
	rqpair.cmds = &cmd;
	cmd.sgl[0].address = 0x1111;
	rdma_req.id = 0;
	rdma_req.req = &req;

	req.payload.reset_sgl_fn = nvme_rdma_ut_reset_sgl;
	req.payload.next_sge_fn = nvme_rdma_ut_next_sge;
	req.payload.contig_or_cb_arg = &bio;
	req.qpair = &rqpair.qpair;

	/* Test case 1: single inline SGL. Expected: PASS */
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 0x1000;
	bio.iovs[0].iov_base = (void *)0xdeadbeef;
	bio.iovs[0].iov_len = 0x1000;
	rc = nvme_rdma_build_sgl_inline_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(bio.iovpos == 1);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));
	CU_ASSERT(rdma_req.send_sgl[1].length == req.payload_size);
	CU_ASSERT(rdma_req.send_sgl[1].addr == (uint64_t)bio.iovs[0].iov_base);
	CU_ASSERT(rdma_req.send_sgl[1].lkey == RDMA_UT_LKEY);

	/* Test case 2: SGL length exceeds 3 bytes. Expected: PASS */
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 1 << 24;
	bio.iovs[0].iov_len = 1 << 24;
	rc = nvme_rdma_build_sgl_inline_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(bio.iovpos == 1);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));
	CU_ASSERT(rdma_req.send_sgl[1].length == req.payload_size);
	CU_ASSERT(rdma_req.send_sgl[1].addr == (uint64_t)bio.iovs[0].iov_base);
	CU_ASSERT(rdma_req.send_sgl[1].lkey == RDMA_UT_LKEY);
}

static void
test_nvme_rdma_build_contig_request(void)
{
	struct nvme_rdma_qpair rqpair;
	struct spdk_nvme_ctrlr ctrlr = {0};
	struct spdk_nvmf_cmd cmd = {{0}};
	struct spdk_nvme_rdma_req rdma_req = {0};
	struct nvme_request req = {{0}};
	int rc;

	ctrlr.max_sges = NVME_RDMA_MAX_SGL_DESCRIPTORS;
	ctrlr.cdata.nvmf_specific.msdbd = 16;

	rqpair.mr_map = (struct spdk_rdma_mem_map *)0xdeadbeef;
	rqpair.rdma_qp = (struct spdk_rdma_qp *)0xdeadbeef;
	rqpair.qpair.ctrlr = &ctrlr;
	rqpair.cmds = &cmd;
	cmd.sgl[0].address = 0x1111;
	rdma_req.id = 0;
	rdma_req.req = &req;

	req.payload.contig_or_cb_arg = (void *)0xdeadbeef;
	req.qpair = &rqpair.qpair;

	/* Test case 1: contig request. Expected: PASS */
	req.payload_offset = 0;
	req.payload_size = 0x1000;
	rc = nvme_rdma_build_contig_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.key == RDMA_UT_RKEY);
	CU_ASSERT(req.cmd.dptr.sgl1.address == (uint64_t)req.payload.contig_or_cb_arg);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));

	/* Test case 2: SGL length exceeds 3 bytes. Expected: FAIL */
	req.payload_offset = 0;
	req.payload_size = 1 << 24;
	rc = nvme_rdma_build_contig_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc != 0);
}

static void
test_nvme_rdma_build_contig_inline_request(void)
{
	struct nvme_rdma_qpair rqpair;
	struct spdk_nvme_ctrlr ctrlr = {0};
	struct spdk_nvmf_cmd cmd = {{0}};
	struct spdk_nvme_rdma_req rdma_req = {0};
	struct nvme_request req = {{0}};
	int rc;

	ctrlr.max_sges = NVME_RDMA_MAX_SGL_DESCRIPTORS;
	ctrlr.cdata.nvmf_specific.msdbd = 16;

	rqpair.mr_map = (struct spdk_rdma_mem_map *)0xdeadbeef;
	rqpair.rdma_qp = (struct spdk_rdma_qp *)0xdeadbeef;
	rqpair.qpair.ctrlr = &ctrlr;
	rqpair.cmds = &cmd;
	cmd.sgl[0].address = 0x1111;
	rdma_req.id = 0;
	rdma_req.req = &req;

	req.payload.contig_or_cb_arg = (void *)0xdeadbeef;
	req.qpair = &rqpair.qpair;

	/* Test case 1: single inline SGL. Expected: PASS */
	req.payload_offset = 0;
	req.payload_size = 0x1000;
	rc = nvme_rdma_build_contig_inline_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));
	CU_ASSERT(rdma_req.send_sgl[1].length == req.payload_size);
	CU_ASSERT(rdma_req.send_sgl[1].addr == (uint64_t)req.payload.contig_or_cb_arg);
	CU_ASSERT(rdma_req.send_sgl[1].lkey == RDMA_UT_LKEY);

	/* Test case 2: SGL length exceeds 3 bytes. Expected: PASS */
	req.payload_offset = 0;
	req.payload_size = 1 << 24;
	rc = nvme_rdma_build_contig_inline_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));
	CU_ASSERT(rdma_req.send_sgl[1].length == req.payload_size);
	CU_ASSERT(rdma_req.send_sgl[1].addr == (uint64_t)req.payload.contig_or_cb_arg);
	CU_ASSERT(rdma_req.send_sgl[1].lkey == RDMA_UT_LKEY);
}

static void
test_nvme_rdma_alloc_reqs(void)
{
	struct nvme_rdma_qpair rqpair = {};
	int rc;

	memset(&g_nvme_hooks, 0, sizeof(g_nvme_hooks));

	/* Test case 1: zero entry. Expect: FAIL */
	rqpair.num_entries = 0;

	rc = nvme_rdma_alloc_reqs(&rqpair);
	CU_ASSERT(rqpair.rdma_reqs == NULL);
	SPDK_CU_ASSERT_FATAL(rc == -ENOMEM);

	/* Test case 2: single entry. Expect: PASS */
	memset(&rqpair, 0, sizeof(rqpair));
	rqpair.num_entries = 1;

	rc = nvme_rdma_alloc_reqs(&rqpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rqpair.rdma_reqs[0].send_sgl[0].addr
		  == (uint64_t)&rqpair.cmds[0]);
	CU_ASSERT(rqpair.rdma_reqs[0].send_wr.wr_id
		  == (uint64_t)&rqpair.rdma_reqs[0].rdma_wr);
	CU_ASSERT(rqpair.rdma_reqs[0].send_wr.next == NULL);
	CU_ASSERT(rqpair.rdma_reqs[0].send_wr.opcode == IBV_WR_SEND);
	CU_ASSERT(rqpair.rdma_reqs[0].send_wr.send_flags == IBV_SEND_SIGNALED);
	CU_ASSERT(rqpair.rdma_reqs[0].send_wr.sg_list
		  == rqpair.rdma_reqs[0].send_sgl);
	CU_ASSERT(rqpair.rdma_reqs[0].send_wr.imm_data == 0);
	spdk_free(rqpair.rdma_reqs);
	spdk_free(rqpair.cmds);

	/* Test case 3: multiple entries. Expect: PASS */
	memset(&rqpair, 0, sizeof(rqpair));
	rqpair.num_entries = 5;

	rc = nvme_rdma_alloc_reqs(&rqpair);
	CU_ASSERT(rc == 0);
	for (int i = 0; i < 5; i++) {
		CU_ASSERT(rqpair.rdma_reqs[i].send_sgl[0].addr
			  == (uint64_t)&rqpair.cmds[i]);
		CU_ASSERT(rqpair.rdma_reqs[i].send_wr.wr_id
			  == (uint64_t)&rqpair.rdma_reqs[i].rdma_wr);
		CU_ASSERT(rqpair.rdma_reqs[i].send_wr.next == NULL);
		CU_ASSERT(rqpair.rdma_reqs[i].send_wr.opcode == IBV_WR_SEND);
		CU_ASSERT(rqpair.rdma_reqs[i].send_wr.send_flags
			  == IBV_SEND_SIGNALED);
		CU_ASSERT(rqpair.rdma_reqs[i].send_wr.sg_list
			  == rqpair.rdma_reqs[i].send_sgl);
		CU_ASSERT(rqpair.rdma_reqs[i].send_wr.imm_data == 0);
	}
	spdk_free(rqpair.rdma_reqs);
	spdk_free(rqpair.cmds);
}

static void
test_nvme_rdma_alloc_rsps(void)
{
	struct nvme_rdma_qpair rqpair = {};
	int rc;

	memset(&g_nvme_hooks, 0, sizeof(g_nvme_hooks));

	/* Test case 1 calloc false */
	rqpair.num_entries = 0;
	rc = nvme_rdma_alloc_rsps(&rqpair);
	CU_ASSERT(rqpair.rsp_sgls == NULL);
	SPDK_CU_ASSERT_FATAL(rc == -ENOMEM);

	/* Test case 2 calloc success */
	memset(&rqpair, 0, sizeof(rqpair));
	rqpair.num_entries = 1;

	rc = nvme_rdma_alloc_rsps(&rqpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rqpair.rsp_sgls != NULL);
	CU_ASSERT(rqpair.rsp_recv_wrs != NULL);
	CU_ASSERT(rqpair.rsps != NULL);
	nvme_rdma_free_rsps(&rqpair);
}

static void
test_nvme_rdma_ctrlr_create_qpair(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	uint16_t qid, qsize;
	struct spdk_nvme_qpair *qpair;
	struct nvme_rdma_qpair *rqpair;

	/* Test case 1: max qsize. Expect: PASS */
	qsize = 0xffff;
	qid = 1;

	qpair = nvme_rdma_ctrlr_create_qpair(&ctrlr, qid, qsize,
					     SPDK_NVME_QPRIO_URGENT, 1,
					     false);
	CU_ASSERT(qpair != NULL);
	rqpair = SPDK_CONTAINEROF(qpair, struct nvme_rdma_qpair, qpair);
	CU_ASSERT(qpair == &rqpair->qpair);
	CU_ASSERT(rqpair->num_entries == qsize);
	CU_ASSERT(rqpair->delay_cmd_submit == false);
	CU_ASSERT(rqpair->rsp_sgls != NULL);
	CU_ASSERT(rqpair->rsp_recv_wrs != NULL);
	CU_ASSERT(rqpair->rsps != NULL);

	nvme_rdma_free_reqs(rqpair);
	nvme_rdma_free_rsps(rqpair);
	nvme_rdma_free(rqpair);
	rqpair = NULL;

	/* Test case 2: queue qsize zero. ExpectL FAIL */
	qsize = 0;

	qpair = nvme_rdma_ctrlr_create_qpair(&ctrlr, qid, qsize,
					     SPDK_NVME_QPRIO_URGENT, 1,
					     false);
	SPDK_CU_ASSERT_FATAL(qpair == NULL);
}

DEFINE_STUB(ibv_create_cq, struct ibv_cq *, (struct ibv_context *context, int cqe, void *cq_context,
		struct ibv_comp_channel *channel, int comp_vector), (struct ibv_cq *)0xFEEDBEEF);
DEFINE_STUB(ibv_destroy_cq, int, (struct ibv_cq *cq), 0);

static void
test_nvme_rdma_poller_create(void)
{
	struct nvme_rdma_poll_group	group = {};
	struct ibv_context *contexts = (struct ibv_context *)0xDEADBEEF;

	/* Case: calloc and ibv not need to fail test */
	STAILQ_INIT(&group.pollers);
	group.num_pollers = 1;
	int rc = nvme_rdma_poller_create(&group, contexts);

	CU_ASSERT(rc == 0);
	CU_ASSERT(group.num_pollers = 2);
	CU_ASSERT(&group.pollers != NULL);
	CU_ASSERT(group.pollers.stqh_first->device == contexts);
	CU_ASSERT(group.pollers.stqh_first->cq == (struct ibv_cq *)0xFEEDBEEF);
	CU_ASSERT(group.pollers.stqh_first->current_num_wc == DEFAULT_NVME_RDMA_CQ_SIZE);
	CU_ASSERT(group.pollers.stqh_first->required_num_wc == 0);

	nvme_rdma_poll_group_free_pollers(&group);
}

static void
test_nvme_rdma_qpair_process_cm_event(void)
{
	struct nvme_rdma_qpair rqpair = {};
	struct rdma_cm_event	 event = {};
	struct spdk_nvmf_rdma_accept_private_data	accept_data = {};
	int rc = 0;

	/* case1: event == RDMA_CM_EVENT_ADDR_RESOLVED */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_ADDR_RESOLVED;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);

	/* case2: event == RDMA_CM_EVENT_CONNECT_REQUEST */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_CONNECT_REQUEST;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);

	/* case3: event == RDMA_CM_EVENT_CONNECT_ERROR */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_CONNECT_ERROR;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);

	/* case4: event == RDMA_CM_EVENT_UNREACHABLE */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_UNREACHABLE;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);

	/* case5: event == RDMA_CM_EVENT_CONNECT_RESPONSE */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_CONNECT_RESPONSE;
	event.param.conn.private_data = NULL;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == -1);

	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_CONNECT_RESPONSE;
	event.param.conn.private_data = &accept_data;
	accept_data.crqsize = 512;
	rqpair.num_entries = 1024;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rqpair.num_entries == 512);

	/* case6: event == RDMA_CM_EVENT_DISCONNECTED */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_DISCONNECTED;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rqpair.qpair.transport_failure_reason == SPDK_NVME_QPAIR_FAILURE_REMOTE);

	/* case7: event == RDMA_CM_EVENT_DEVICE_REMOVAL */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_DEVICE_REMOVAL;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rqpair.qpair.transport_failure_reason == SPDK_NVME_QPAIR_FAILURE_LOCAL);

	/* case8: event == RDMA_CM_EVENT_MULTICAST_JOIN */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_MULTICAST_JOIN;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);

	/* case9: event == RDMA_CM_EVENT_ADDR_CHANGE */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_ADDR_CHANGE;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rqpair.qpair.transport_failure_reason == SPDK_NVME_QPAIR_FAILURE_LOCAL);

	/* case10: event == RDMA_CM_EVENT_TIMEWAIT_EXIT */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_TIMEWAIT_EXIT;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);

	/* case11: default event == 0xFF */
	rqpair.evt = &event;
	event.event = 0xFF;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_rdma", NULL, NULL);
	CU_ADD_TEST(suite, test_nvme_rdma_build_sgl_request);
	CU_ADD_TEST(suite, test_nvme_rdma_build_sgl_inline_request);
	CU_ADD_TEST(suite, test_nvme_rdma_build_contig_request);
	CU_ADD_TEST(suite, test_nvme_rdma_build_contig_inline_request);
	CU_ADD_TEST(suite, test_nvme_rdma_alloc_reqs);
	CU_ADD_TEST(suite, test_nvme_rdma_alloc_rsps);
	CU_ADD_TEST(suite, test_nvme_rdma_ctrlr_create_qpair);
	CU_ADD_TEST(suite, test_nvme_rdma_poller_create);
	CU_ADD_TEST(suite, test_nvme_rdma_qpair_process_cm_event);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
