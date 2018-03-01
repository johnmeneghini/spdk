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

#include "session.c"

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

struct spdk_nvmf_tgt g_nvmf_tgt;

struct spdk_nvmf_subsystem *
spdk_nvmf_find_subsystem_with_cntlid(uint16_t cntlid)
{
	return NULL;
}

struct spdk_nvmf_subsystem *
spdk_nvmf_find_subsystem(const char *subnqn)
{
	return NULL;
}

const struct spdk_nvme_ctrlr_data *
spdk_nvme_ctrlr_get_data(struct spdk_nvme_ctrlr *ctrlr)
{
	return NULL;
}

static void
test_foobar(void)
{
}

int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	return -1;
}

static void
test_process_aer_rsp(void)
{
	struct spdk_nvmf_subsystem *subsystem = malloc(sizeof(struct spdk_nvmf_subsystem));
	enum aer_type aer_type;
	uint8_t aer_info;
	struct spdk_nvmf_session *session = malloc(sizeof(struct spdk_nvmf_session));
	struct spdk_nvmf_request *req = malloc(sizeof(struct spdk_nvmf_request));
	union nvmf_c2h_msg *rsp = malloc(sizeof(union nvmf_c2h_msg));
	/* Validate memory allocation did not fail */
	CU_ASSERT_NOT_EQUAL(subsystem, NULL);
	CU_ASSERT_NOT_EQUAL(session, NULL);
	CU_ASSERT_NOT_EQUAL(req, NULL);
	CU_ASSERT_NOT_EQUAL(rsp, NULL);

	/* Initialize the members */
	strcpy(subsystem->subnqn, "nqn.2016-06.io.spdk:subsystem1");
	req->rsp = rsp;
	session->subsys = subsystem;
	session->aer_ctxt.is_aer_pending = false;
	session->aer_req = req;
	TAILQ_INIT(&subsystem->sessions);
	TAILQ_INSERT_TAIL(&subsystem->sessions, session, link);

	/* Check for a non AER_TYPE_NOTICE aer_type */
	aer_type = AER_TYPE_ERROR_STATUS;
	aer_info = AER_NOTICE_INFO_NS_ATTR_CHANGED;
	spdk_nvmf_queue_aer_rsp(subsystem, aer_type, aer_info);
	/* Check aer_req is intact and not used to issue aer rsp */
	CU_ASSERT_EQUAL(session->aer_req, req);
	CU_ASSERT_EQUAL(session->aer_ctxt.is_aer_pending, false);

	/* Check for aer_req value after an aer_rsp is sent */
	aer_type = AER_TYPE_NOTICE;
	spdk_nvmf_queue_aer_rsp(subsystem, aer_type, aer_info);
	CU_ASSERT_EQUAL(session->aer_req, NULL);
	CU_ASSERT_EQUAL(session->aer_ctxt.is_aer_pending, false);

	/* Check for is_aer_pending flag when aer_req is NULL */
	session->aer_req = NULL;
	spdk_nvmf_queue_aer_rsp(subsystem, aer_type, aer_info);
	CU_ASSERT_EQUAL(session->aer_req, NULL);
	CU_ASSERT_EQUAL(session->aer_ctxt.is_aer_pending, true);

	free(subsystem);
	free(session);
	free(req);
	free(rsp);
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
static const char *session_ut_small_host_nqn = "fc small host";
static const char *session_ut_big_host_nqn = "fc big host";
#endif

static struct spdk_nvmf_host session_ut_small_host = {
	.nqn = "nqn.2017-11.small_host",
	.max_aq_depth = 32,
	.max_io_queue_depth = 16,
	.max_connections_allowed = 2,
};

static struct spdk_nvmf_host session_ut_big_host = {
	.nqn = "nqn.2017-11.big_host",
	.max_aq_depth = 128,
	.max_io_queue_depth = 1024,
	.max_connections_allowed = 32,
};

struct spdk_nvmf_host *
spdk_nvmf_find_subsystem_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	if (!hostnqn) {
		SPDK_ERRLOG("hostnqn is NULL\n");
		return NULL;
	}

	if (strcmp(hostnqn, session_ut_small_host.nqn) == 0) {
		return &session_ut_small_host;
	}

	if (strcmp(hostnqn, session_ut_big_host.nqn) == 0) {
		return &session_ut_big_host;
	}

	SPDK_ERRLOG("Hostnqn %s not found on Subsystem %s\n", hostnqn, subsystem->subnqn);

	return NULL;
}


int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvmf", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "foobar", test_foobar) == NULL ||
		CU_add_test(suite, "process_aer_rsp", test_process_aer_rsp)  == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
