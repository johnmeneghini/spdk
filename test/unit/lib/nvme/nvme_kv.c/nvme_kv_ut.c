/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2021 NetApp.  All Rights Reserved.
 *   The term "NetApp" refers to NetApp Inc. and/or its subsidiaries.
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

#include "spdk_cunit.h"

#include "spdk/env.h"

#include "nvme/nvme_kv.c"

#include "common/lib/test_env.c"

SPDK_LOG_REGISTER_COMPONENT(nvme_kv)

static struct nvme_request *g_request = NULL;

pid_t g_spdk_nvme_pid;

enum spdk_nvme_csi
spdk_nvme_ns_get_csi(const struct spdk_nvme_ns *ns) {
	return ns->csi;
}

uint32_t
spdk_nvme_ns_get_max_io_xfer_size(struct spdk_nvme_ns *ns)
{
	return ns->ctrlr->max_xfer_size;
}

int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	g_request = req;

	return 0;
}

static void
test_nvme_kv_parse(void)
{
	struct spdk_nvme_kv_key_t key;
	struct spdk_nvme_kv_cmd cmd;
	char key_str[KV_KEY_STRING_LEN];

	int rv = spdk_kv_key_parse("", &key);
	CU_ASSERT(rv != 0);

	rv = spdk_kv_key_parse("hello", &key);
	CU_ASSERT(rv == 0);
	cmd.opc = SPDK_NVME_OPC_KV_STORE;
	spdk_nvme_kv_cmd_set_key(&key, &cmd);
	spdk_kv_cmd_fmt_lower(&cmd, key_str, sizeof(key_str));
	CU_ASSERT(key.kl == strlen("hello"));
	CU_ASSERT(strcmp("0x68656c6c-6f", key_str) == 0);

	rv = spdk_kv_key_parse("0x0", &key);
	CU_ASSERT(rv == 0);
	cmd.opc = SPDK_NVME_OPC_KV_RETRIEVE;
	spdk_nvme_kv_cmd_set_key(&key, &cmd);
	spdk_kv_cmd_fmt_lower(&cmd, key_str, sizeof(key_str));
	CU_ASSERT(key.kl == 1);
	CU_ASSERT(strcmp("0x00", key_str) == 0);

	rv = spdk_kv_key_parse("0x00", &key);
	CU_ASSERT(rv == 0);
	cmd.opc = SPDK_NVME_OPC_KV_DELETE;
	spdk_nvme_kv_cmd_set_key(&key, &cmd);
	spdk_kv_cmd_fmt_lower(&cmd, key_str, sizeof(key_str));
	CU_ASSERT(key.kl == 1);
	CU_ASSERT(strcmp("0x00", key_str) == 0);

	rv = spdk_kv_key_parse("0x11", &key);
	CU_ASSERT(rv == 0);
	cmd.opc = SPDK_NVME_OPC_KV_EXIST;
	spdk_nvme_kv_cmd_set_key(&key, &cmd);
	spdk_kv_cmd_fmt_lower(&cmd, key_str, sizeof(key_str));
	CU_ASSERT(key.kl == 1);
	CU_ASSERT(strcmp("0x11", key_str) == 0);

	rv = spdk_kv_key_parse("0x112", &key);
	CU_ASSERT(rv == 0);
	cmd.opc = SPDK_NVME_OPC_KV_LIST;
	spdk_nvme_kv_cmd_set_key(&key, &cmd);
	spdk_kv_cmd_fmt_lower(&cmd, key_str, sizeof(key_str));
	CU_ASSERT(key.kl == 2);
	CU_ASSERT(strcmp("0x1102", key_str) == 0);

	rv = spdk_kv_key_parse("0x11223344", &key);
	CU_ASSERT(rv == 0);
	cmd.opc = SPDK_NVME_OPC_KV_STORE;
	spdk_nvme_kv_cmd_set_key(&key, &cmd);
	spdk_kv_cmd_fmt_lower(&cmd, key_str, sizeof(key_str));
	CU_ASSERT(key.kl == 4);
	CU_ASSERT(strcmp("0x11223344", key_str) == 0);

	rv = spdk_kv_key_parse("0x11223344-11223344-11223344-11223344", &key);
	CU_ASSERT(rv == 0);
	cmd.opc = SPDK_NVME_OPC_KV_RETRIEVE;
	spdk_nvme_kv_cmd_set_key(&key, &cmd);
	spdk_kv_cmd_fmt_lower(&cmd, key_str, sizeof(key_str));
	CU_ASSERT(key.kl == 16);
	CU_ASSERT(strcmp("0x11223344-11223344-11223344-11223344", key_str) == 0);
}

static void
prepare_for_test(struct spdk_nvme_ns *ns, struct spdk_nvme_ctrlr *ctrlr,
		 struct spdk_nvme_qpair *qpair,
		 uint32_t max_xfer_size, struct spdk_nvme_kv_key_t *key)
{
	uint32_t num_requests = 32;
	uint32_t i;

	ctrlr->max_xfer_size = max_xfer_size;
	/*
	 * Clear the flags field - we especially want to make sure the SGL_SUPPORTED flag is not set
	 *  so that we test the SGL splitting path.
	 */
	ctrlr->flags = 0;
	ctrlr->min_page_size = 4096;
	ctrlr->page_size = 4096;
	memset(&ctrlr->opts, 0, sizeof(ctrlr->opts));
	memset(ns, 0, sizeof(*ns));
	ns->ctrlr = ctrlr;
	ns->csi = SPDK_NVME_CSI_KV;
	ns->kv_key_max_len = KV_MAX_KEY_SIZE;
	ns->kv_value_max_len = 1 << 20;
	ns->kv_max_num_keys = 0;

	memset(qpair, 0, sizeof(*qpair));
	qpair->ctrlr = ctrlr;
	qpair->req_buf = calloc(num_requests, sizeof(struct nvme_request));
	SPDK_CU_ASSERT_FATAL(qpair->req_buf != NULL);

	for (i = 0; i < num_requests; i++) {
		struct nvme_request *req = qpair->req_buf + i * sizeof(struct nvme_request);

		req->qpair = qpair;
		STAILQ_INSERT_HEAD(&qpair->free_req, req, stailq);
	}

	spdk_kv_key_parse("hello", key);

	g_request = NULL;
}

static void
cleanup_after_test(struct spdk_nvme_qpair *qpair)
{
	free(qpair->req_buf);
}

static void
_nvme_kv_cmd_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{

}

static void
test_nvme_ns_cmd_store(void)
{
	struct spdk_nvme_ns		ns;
	struct spdk_nvme_ctrlr		ctrlr;
	struct spdk_nvme_qpair		qpair;
	struct spdk_nvme_kv_key_t   key;
	struct spdk_nvme_kv_key_t   req_key;
	int				rc = 0;
	void				*cb_arg;
	uint8_t buf[64];
	uint32_t buffer_size = sizeof(buf);

	cb_arg = malloc(512);
	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	rc = spdk_nvme_kv_cmd_store(&ns, &qpair, &key, buf, buffer_size, _nvme_kv_cmd_cb, cb_arg, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_KV_STORE);
	CU_ASSERT(nvme_payload_type(&g_request->payload) == NVME_PAYLOAD_TYPE_CONTIG);
	CU_ASSERT(g_request->payload.contig_or_cb_arg == buf);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	struct spdk_nvme_kv_cmd *cmd = (struct spdk_nvme_kv_cmd *)&g_request->cmd;
	CU_ASSERT(cmd->cdw11_bits.kv_store.kl == key.kl);
	CU_ASSERT(cmd->cdw10_bits.kv_store.vs == buffer_size);
	spdk_nvme_kv_cmd_get_key(cmd, &req_key);
	CU_ASSERT(req_key.kl == key.kl);
	CU_ASSERT(memcmp(req_key.key, key.key, req_key.kl) == 0);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	memset(&key, 0, sizeof(key));
	rc = spdk_nvme_kv_cmd_store(&ns, &qpair, &key, buf, buffer_size, _nvme_kv_cmd_cb, cb_arg, 0);
	SPDK_CU_ASSERT_FATAL(rc == SPDK_NVME_SC_KV_INVALID_KEY_SIZE);
	cleanup_after_test(&qpair);

	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	memset(&key, 0, sizeof(key));
	key.kl = spdk_nvme_kv_get_max_key_len(&ns) + 1;
	rc = spdk_nvme_kv_cmd_store(&ns, &qpair, &key, buf, buffer_size, _nvme_kv_cmd_cb, cb_arg, 0);
	SPDK_CU_ASSERT_FATAL(rc == SPDK_NVME_SC_KV_INVALID_KEY_SIZE);
	cleanup_after_test(&qpair);

	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	rc = spdk_nvme_kv_cmd_store(&ns, &qpair, &key, buf, spdk_nvme_kv_get_max_value_len(&ns) + 1,
				    _nvme_kv_cmd_cb, cb_arg, 0);
	SPDK_CU_ASSERT_FATAL(rc == SPDK_NVME_SC_KV_INVALID_VALUE_SIZE);
	cleanup_after_test(&qpair);

	free(cb_arg);
}

static void
test_nvme_ns_cmd_retrieve(void)
{
	struct spdk_nvme_ns		ns;
	struct spdk_nvme_ctrlr		ctrlr;
	struct spdk_nvme_qpair		qpair;
	struct spdk_nvme_kv_key_t   key;
	struct spdk_nvme_kv_key_t   req_key;
	int				rc = 0;
	void				*cb_arg;
	uint8_t buf[64];
	uint32_t buffer_size = sizeof(buf);

	cb_arg = malloc(512);
	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	rc = spdk_nvme_kv_cmd_retrieve(&ns, &qpair, &key, buf, buffer_size, _nvme_kv_cmd_cb, cb_arg, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_KV_RETRIEVE);
	CU_ASSERT(nvme_payload_type(&g_request->payload) == NVME_PAYLOAD_TYPE_CONTIG);
	CU_ASSERT(g_request->payload.contig_or_cb_arg == buf);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	struct spdk_nvme_kv_cmd *cmd = (struct spdk_nvme_kv_cmd *)&g_request->cmd;
	CU_ASSERT(cmd->cdw11_bits.kv_retrieve.kl == key.kl);
	spdk_nvme_kv_cmd_get_key(cmd, &req_key);
	CU_ASSERT(req_key.kl == key.kl);
	CU_ASSERT(memcmp(req_key.key, key.key, req_key.kl) == 0);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	memset(&key, 0, sizeof(key));
	rc = spdk_nvme_kv_cmd_retrieve(&ns, &qpair, &key, buf, buffer_size, _nvme_kv_cmd_cb, cb_arg, 0);
	SPDK_CU_ASSERT_FATAL(rc == SPDK_NVME_SC_KV_INVALID_KEY_SIZE);
	cleanup_after_test(&qpair);

	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	memset(&key, 0, sizeof(key));
	key.kl = spdk_nvme_kv_get_max_key_len(&ns) + 1;
	rc = spdk_nvme_kv_cmd_retrieve(&ns, &qpair, &key, buf, buffer_size, _nvme_kv_cmd_cb, cb_arg, 0);
	SPDK_CU_ASSERT_FATAL(rc == SPDK_NVME_SC_KV_INVALID_KEY_SIZE);
	cleanup_after_test(&qpair);

	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	rc = spdk_nvme_kv_cmd_retrieve(&ns, &qpair, &key, buf, spdk_nvme_kv_get_max_value_len(&ns) + 1,
				       _nvme_kv_cmd_cb, cb_arg, 0);
	SPDK_CU_ASSERT_FATAL(rc == SPDK_NVME_SC_KV_INVALID_VALUE_SIZE);
	cleanup_after_test(&qpair);

	free(cb_arg);
}

static void
test_nvme_ns_cmd_delete(void)
{
	struct spdk_nvme_ns		ns;
	struct spdk_nvme_ctrlr		ctrlr;
	struct spdk_nvme_qpair		qpair;
	struct spdk_nvme_kv_key_t   key;
	struct spdk_nvme_kv_key_t   req_key;
	int				rc = 0;
	void				*cb_arg;

	cb_arg = malloc(512);
	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	rc = spdk_nvme_kv_cmd_delete(&ns, &qpair, &key, _nvme_kv_cmd_cb, cb_arg);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_KV_DELETE);
	CU_ASSERT(nvme_payload_type(&g_request->payload) == NVME_PAYLOAD_TYPE_CONTIG);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	struct spdk_nvme_kv_cmd *cmd = (struct spdk_nvme_kv_cmd *)&g_request->cmd;
	CU_ASSERT(cmd->cdw11_bits.kv_del.kl == key.kl);
	spdk_nvme_kv_cmd_get_key(cmd, &req_key);
	CU_ASSERT(req_key.kl == key.kl);
	CU_ASSERT(memcmp(req_key.key, key.key, req_key.kl) == 0);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	memset(&key, 0, sizeof(key));
	rc = spdk_nvme_kv_cmd_delete(&ns, &qpair, &key, _nvme_kv_cmd_cb, cb_arg);
	SPDK_CU_ASSERT_FATAL(rc == SPDK_NVME_SC_KV_INVALID_KEY_SIZE);
	cleanup_after_test(&qpair);

	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	memset(&key, 0, sizeof(key));
	key.kl = spdk_nvme_kv_get_max_key_len(&ns) + 1;
	rc = spdk_nvme_kv_cmd_delete(&ns, &qpair, &key, _nvme_kv_cmd_cb, cb_arg);
	SPDK_CU_ASSERT_FATAL(rc == SPDK_NVME_SC_KV_INVALID_KEY_SIZE);
	cleanup_after_test(&qpair);

	free(cb_arg);
}

static void
test_nvme_ns_cmd_exist(void)
{
	struct spdk_nvme_ns		ns;
	struct spdk_nvme_ctrlr		ctrlr;
	struct spdk_nvme_qpair		qpair;
	struct spdk_nvme_kv_key_t   key;
	struct spdk_nvme_kv_key_t   req_key;
	int				rc = 0;
	void				*cb_arg;

	cb_arg = malloc(512);
	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	rc = spdk_nvme_kv_cmd_exist(&ns, &qpair, &key, _nvme_kv_cmd_cb, cb_arg);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_KV_EXIST);
	CU_ASSERT(nvme_payload_type(&g_request->payload) == NVME_PAYLOAD_TYPE_CONTIG);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	struct spdk_nvme_kv_cmd *cmd = (struct spdk_nvme_kv_cmd *)&g_request->cmd;
	CU_ASSERT(cmd->cdw11_bits.kv_exist.kl == key.kl);
	spdk_nvme_kv_cmd_get_key(cmd, &req_key);
	CU_ASSERT(req_key.kl == key.kl);
	CU_ASSERT(memcmp(req_key.key, key.key, req_key.kl) == 0);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	memset(&key, 0, sizeof(key));
	rc = spdk_nvme_kv_cmd_exist(&ns, &qpair, &key, _nvme_kv_cmd_cb, cb_arg);
	SPDK_CU_ASSERT_FATAL(rc == SPDK_NVME_SC_KV_INVALID_KEY_SIZE);
	cleanup_after_test(&qpair);

	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	memset(&key, 0, sizeof(key));
	key.kl = spdk_nvme_kv_get_max_key_len(&ns) + 1;
	rc = spdk_nvme_kv_cmd_exist(&ns, &qpair, &key, _nvme_kv_cmd_cb, cb_arg);
	SPDK_CU_ASSERT_FATAL(rc == SPDK_NVME_SC_KV_INVALID_KEY_SIZE);
	cleanup_after_test(&qpair);

	free(cb_arg);
}

static void
test_nvme_ns_cmd_list(void)
{
	struct spdk_nvme_ns		ns;
	struct spdk_nvme_ctrlr		ctrlr;
	struct spdk_nvme_qpair		qpair;
	struct spdk_nvme_kv_key_t   key;
	struct spdk_nvme_kv_key_t   req_key;
	int				rc = 0;
	void				*cb_arg;
	uint8_t buf[64];
	uint32_t buffer_size = sizeof(buf);

	cb_arg = malloc(512);
	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	rc = spdk_nvme_kv_cmd_list(&ns, &qpair, &key, buf, buffer_size, _nvme_kv_cmd_cb, cb_arg);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_KV_LIST);
	CU_ASSERT(nvme_payload_type(&g_request->payload) == NVME_PAYLOAD_TYPE_CONTIG);
	CU_ASSERT(g_request->payload.contig_or_cb_arg == buf);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	struct spdk_nvme_kv_cmd *cmd = (struct spdk_nvme_kv_cmd *)&g_request->cmd;
	CU_ASSERT(cmd->cdw11_bits.kv_store.kl == key.kl);
	CU_ASSERT(cmd->cdw10_bits.kv_store.vs == buffer_size);
	spdk_nvme_kv_cmd_get_key(cmd, &req_key);
	CU_ASSERT(req_key.kl == key.kl);
	CU_ASSERT(memcmp(req_key.key, key.key, req_key.kl) == 0);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	memset(&key, 0, sizeof(key));
	rc = spdk_nvme_kv_cmd_list(&ns, &qpair, &key, buf, buffer_size, _nvme_kv_cmd_cb, cb_arg);
	SPDK_CU_ASSERT_FATAL(rc == SPDK_NVME_SC_KV_INVALID_KEY_SIZE);
	cleanup_after_test(&qpair);

	prepare_for_test(&ns, &ctrlr, &qpair, 128 * 1024, &key);
	memset(&key, 0, sizeof(key));
	key.kl = spdk_nvme_kv_get_max_key_len(&ns) + 1;
	rc = spdk_nvme_kv_cmd_list(&ns, &qpair, &key, buf, buffer_size, _nvme_kv_cmd_cb, cb_arg);
	SPDK_CU_ASSERT_FATAL(rc == SPDK_NVME_SC_KV_INVALID_KEY_SIZE);
	cleanup_after_test(&qpair);

	free(cb_arg);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_kv", NULL, NULL);

	CU_ADD_TEST(suite, test_nvme_kv_parse);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_store);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_retrieve);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_delete);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_exist);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_list);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
