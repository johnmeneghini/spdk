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
#include "subsystem.h"

const struct spdk_nvmf_ctrlr_ops spdk_nvmf_direct_ctrlr_ops;
const struct spdk_nvmf_ctrlr_ops spdk_nvmf_virtual_ctrlr_ops;
const struct spdk_nvmf_ctrlr_ops spdk_nvmf_discovery_ctrlr_ops;

#include "subsystem.c"

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

struct spdk_nvmf_tgt g_nvmf_tgt;

bool
spdk_nvmf_listen_addr_compare(struct spdk_nvmf_listen_addr *a, struct spdk_nvmf_listen_addr *b)
{
	if ((strcmp(a->trname, b->trname) == 0) &&
	    (strcmp(a->traddr, b->traddr) == 0) &&
	    (strcmp(a->trsvcid, b->trsvcid) == 0)) {
		return true;
	} else {
		return false;
	}
}


struct spdk_nvmf_listen_addr *
spdk_nvmf_listen_addr_create(const char *trname, enum spdk_nvmf_adrfam adrfam, const char *traddr,
			     const char *trsvcid)
{
	struct spdk_nvmf_listen_addr *listen_addr;

	listen_addr = calloc(1, sizeof(*listen_addr));
	if (!listen_addr) {
		return NULL;
	}

	listen_addr->traddr = strdup(traddr);
	if (!listen_addr->traddr) {
		free(listen_addr);
		return NULL;
	}

	listen_addr->trsvcid = strdup(trsvcid);
	if (!listen_addr->trsvcid) {
		free(listen_addr->traddr);
		free(listen_addr);
		return NULL;
	}

	listen_addr->trname = strdup(trname);
	if (!listen_addr->trname) {
		free(listen_addr->traddr);
		free(listen_addr->trsvcid);
		free(listen_addr);
		return NULL;
	}

	listen_addr->adrfam = adrfam;

	return listen_addr;
}

void
spdk_nvmf_listen_addr_destroy(struct spdk_nvmf_listen_addr *addr)
{
	free(addr->trname);
	free(addr->trsvcid);
	free(addr->traddr);
	free(addr);
}

void
spdk_nvmf_listen_addr_cleanup(struct spdk_nvmf_listen_addr *addr)
{
	return;
}

static int
test_transport1_listen_addr_add(struct spdk_nvmf_listen_addr *listen_addr)
{
	return 0;
}

static void
test_transport1_listen_addr_discover(struct spdk_nvmf_listen_addr *listen_addr,
				     struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = 42;
}

static const struct spdk_nvmf_transport test_transport1 = {
	.listen_addr_add = test_transport1_listen_addr_add,
	.listen_addr_discover = test_transport1_listen_addr_discover,
};

const struct spdk_nvmf_transport *
spdk_nvmf_transport_get(const char *trname)
{
	if (!strcasecmp(trname, "test_transport1")) {
		return &test_transport1;
	}

	return NULL;
}

int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	return -1;
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	return -1;
}

int
spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr)
{
	return -1;
}

void
spdk_nvmf_session_destruct(struct spdk_nvmf_session *session)
{
}

int
spdk_nvmf_session_poll(struct spdk_nvmf_session *session)
{
	return -1;
}

int
spdk_bdev_open(struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
	       void *remove_ctx, struct spdk_bdev_desc **desc)
{
	return 0;
}

void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
	return;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "test";
}

uint8_t
spdk_bdev_get_ana_state(struct spdk_bdev *bdev, uint16_t cntlid)
{
	return 1;
}

static void
test_spdk_nvmf_tgt_listen(void)
{
	struct spdk_nvmf_listen_addr *listen_addr;

	/* Invalid trname */
	const char *trname  = "test_invalid_trname";
	enum spdk_nvmf_adrfam adrfam = SPDK_NVMF_ADRFAM_IPV4;
	const char *traddr  = "192.168.100.1";
	const char *trsvcid = "4420";
	CU_ASSERT(spdk_nvmf_tgt_listen(trname, adrfam, traddr, trsvcid) == NULL);

	/* Listen addr is not create and create valid listen addr */
	trname  = "test_transport1";
	adrfam = SPDK_NVMF_ADRFAM_IPV4;
	traddr  = "192.168.3.11";
	trsvcid = "3320";
	listen_addr = spdk_nvmf_tgt_listen(trname, adrfam, traddr, trsvcid);
	SPDK_CU_ASSERT_FATAL(listen_addr != NULL);
	CU_ASSERT(listen_addr->traddr != NULL);
	CU_ASSERT(listen_addr->trsvcid != NULL);
	spdk_nvmf_listen_addr_destroy(listen_addr);

}

static void
test_spdk_nvmf_subsystem_add_remove_ns(void)
{
	struct spdk_nvmf_subsystem subsystem = {
		.mode = NVMF_SUBSYSTEM_MODE_VIRTUAL,
		.dev.virt.max_nsid = 0,
		.dev.virt.ns_list = {},
	};
	struct spdk_bdev bdev1 = {}, bdev2 = {}, bdev3 = {};
	struct spdk_nvmf_ana_group *ana_group = NULL;
	uint32_t nsid;
	int ret,  app_ctxt = 3;

	/* Allow NSID to be assigned automatically without an ana group */
	nsid = spdk_nvmf_subsystem_add_ns(&subsystem, &bdev1, 0, 0);
	/* NSID 1 is the first unused ID */
	CU_ASSERT(nsid == 1);
	CU_ASSERT(subsystem.dev.virt.max_nsid == 1);
	CU_ASSERT(subsystem.dev.virt.ns_list[nsid - 1] == &bdev1);

	/* Request a specific NSID without an ana group */
	nsid = spdk_nvmf_subsystem_add_ns(&subsystem, &bdev2, 5, 0);
	CU_ASSERT(nsid == 5);
	CU_ASSERT(subsystem.dev.virt.max_nsid == 5);
	CU_ASSERT(subsystem.dev.virt.ns_list[nsid - 1] == &bdev2);

	/* Request an NSID that is already in use */
	nsid = spdk_nvmf_subsystem_add_ns(&subsystem, &bdev2, 5, 0);
	CU_ASSERT(nsid == 0);
	CU_ASSERT(subsystem.dev.virt.max_nsid == 5);

	/* Add a namespace with a non-existent ANA group */
	nsid = spdk_nvmf_subsystem_add_ns(&subsystem, &bdev3, 0, 3);
	CU_ASSERT(nsid == 0);
	/* max_nsid should be unchanged */
	CU_ASSERT(subsystem.dev.virt.max_nsid == 5);

	/* Initialize ana_groups list in subsystem */
	TAILQ_INIT(&subsystem.ana_groups);

	/* Add a new ANA group */
	CU_ASSERT(subsystem.num_ana_groups == 0);
	ret = spdk_nvmf_subsystem_add_ana_group(&subsystem, 3, NULL);
	CU_ASSERT(ret == 0);
	CU_ASSERT(subsystem.num_ana_groups == 1);

	/* Add a second ANA group */
	CU_ASSERT(subsystem.num_ana_groups == 1);
	ret = spdk_nvmf_subsystem_add_ana_group(&subsystem, 2, &app_ctxt);
	CU_ASSERT(ret == 0);
	CU_ASSERT(subsystem.num_ana_groups == 2);

	/* Make sure ana_group 2 is the first entry in the list */
	ana_group = TAILQ_FIRST(&subsystem.ana_groups);
	CU_ASSERT(ana_group->anagrpid == 2);
	CU_ASSERT(ana_group->num_nsids == 0);
	CU_ASSERT(ana_group->app_ctxt == &app_ctxt);

	/* Add a namespace with an ANA group */
	nsid = spdk_nvmf_subsystem_add_ns(&subsystem, &bdev3, 0, 2);
	CU_ASSERT(nsid == 2);
	CU_ASSERT(bdev3.anagrpid == 2);
	CU_ASSERT(ana_group->num_nsids == 1);
	/* max_nsid should be unchanged */
	CU_ASSERT(subsystem.dev.virt.max_nsid == 5);

	/* Remove ana group with existing namespaces should fail */
	CU_ASSERT(subsystem.num_ana_groups == 2);
	ret = spdk_nvmf_subsystem_remove_ana_group(&subsystem, 2);
	CU_ASSERT(ret == -1);
	CU_ASSERT(subsystem.num_ana_groups == 2);

	/* Remove a non-existent ana group */
	CU_ASSERT(subsystem.num_ana_groups == 2);
	ret = spdk_nvmf_subsystem_remove_ana_group(&subsystem, 5);
	CU_ASSERT(ret == -1);
	CU_ASSERT(subsystem.num_ana_groups == 2);

	/* Remove an ana group with no namespaces */
	CU_ASSERT(subsystem.num_ana_groups == 2);
	ret = spdk_nvmf_subsystem_remove_ana_group(&subsystem, 3);
	CU_ASSERT(ret == 0);
	CU_ASSERT(subsystem.num_ana_groups == 1);

	/* Remove a non-existent namespace */
	ret = spdk_nvmf_subsystem_remove_ns(&subsystem, 10);
	CU_ASSERT(ret == -1);
	CU_ASSERT(subsystem.dev.virt.max_nsid == 5);

	/* Remove a namespace not in any ANA group */
	ret = spdk_nvmf_subsystem_remove_ns(&subsystem, 5);
	CU_ASSERT(ret == 0);
	CU_ASSERT(subsystem.dev.virt.max_nsid == 2);

	/* Remove a namespace in an ANA group */
	ret = spdk_nvmf_subsystem_remove_ns(&subsystem, 2);
	CU_ASSERT(ret == 0);
	CU_ASSERT(subsystem.dev.virt.max_nsid == 1);
	CU_ASSERT(bdev3.anagrpid == 0);
	CU_ASSERT(ana_group->num_nsids == 0);

	/* Remove empty ANA group */
	CU_ASSERT(subsystem.num_ana_groups == 1);
	ret = spdk_nvmf_subsystem_remove_ana_group(&subsystem, 2);
	CU_ASSERT(ret == 0);
	CU_ASSERT(subsystem.num_ana_groups == 0);
}

static void
nvmf_test_create_subsystem(void)
{
	char nqn[256];
	struct spdk_nvmf_subsystem *subsystem;
	TAILQ_INIT(&g_nvmf_tgt.subsystems);

	strncpy(nqn, "nqn.2016-06.io.spdk:subsystem1", sizeof(nqn));
	subsystem = spdk_nvmf_create_subsystem(nqn, SPDK_NVMF_SUBTYPE_NVME,
					       NVMF_SUBSYSTEM_MODE_DIRECT, NULL, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_delete_subsystem(subsystem);

	/* Longest valid name */
	strncpy(nqn, "nqn.2016-06.io.spdk:", sizeof(nqn));
	memset(nqn + strlen(nqn), 'a', 223 - strlen(nqn));
	nqn[223] = '\0';
	CU_ASSERT(strlen(nqn) == 223);
	subsystem = spdk_nvmf_create_subsystem(nqn, SPDK_NVMF_SUBTYPE_NVME,
					       NVMF_SUBSYSTEM_MODE_DIRECT, NULL, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_delete_subsystem(subsystem);

	/* Name that is one byte longer than allowed */
	strncpy(nqn, "nqn.2016-06.io.spdk:", sizeof(nqn));
	memset(nqn + strlen(nqn), 'a', 224 - strlen(nqn));
	nqn[224] = '\0';
	CU_ASSERT(strlen(nqn) == 224);
	subsystem = spdk_nvmf_create_subsystem(nqn, SPDK_NVMF_SUBTYPE_NVME,
					       NVMF_SUBSYSTEM_MODE_DIRECT, NULL, NULL, NULL);
	CU_ASSERT(subsystem == NULL);
}

static void
nvmf_test_find_subsystem(void)
{
	CU_ASSERT_PTR_NULL(spdk_nvmf_find_subsystem(NULL));
	CU_ASSERT_PTR_NULL(spdk_nvmf_find_subsystem("fake"));
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
		CU_add_test(suite, "create_subsystem", nvmf_test_create_subsystem) == NULL ||
		CU_add_test(suite, "nvmf_tgt_listen", test_spdk_nvmf_tgt_listen) == NULL ||
		CU_add_test(suite, "find_subsystem", nvmf_test_find_subsystem) == NULL ||
		CU_add_test(suite, "nvmf_subsystem_add_remove_ns",
			    test_spdk_nvmf_subsystem_add_remove_ns) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
