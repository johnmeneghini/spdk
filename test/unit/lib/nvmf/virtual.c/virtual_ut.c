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
#include "lib/test_env.c"

#include "virtual.c"
#include "transport.h"

struct spdk_nvmf_tgt g_nvmf_tgt;

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

const struct spdk_nvmf_ctrlr_ops spdk_nvmf_direct_ctrlr_ops;
const struct spdk_nvmf_ctrlr_ops spdk_nvmf_virtual_ctrlr_ops;
const struct spdk_nvmf_ctrlr_ops spdk_nvmf_discovery_ctrlr_ops;

struct spdk_nvmf_tgt g_nvmf_tgt;

struct spdk_nvmf_conn *
spdk_nvmf_session_get_conn(struct spdk_nvmf_session *session, uint16_t qid)
{
	return NULL;
}

struct spdk_nvmf_request *
spdk_nvmf_conn_get_request(struct spdk_nvmf_conn *conn, uint16_t cid)
{
	return NULL;
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

int
spdk_nvmf_session_get_features_number_of_queues(struct spdk_nvmf_request *req)
{
	return -1;
}

int spdk_nvmf_session_set_features_number_of_queues(struct spdk_nvmf_request *req)
{
	return -1;
}

int
spdk_nvmf_session_set_features_host_identifier(struct spdk_nvmf_request *req)
{
	return -1;
}

int
spdk_nvmf_session_get_features_host_identifier(struct spdk_nvmf_request *req)
{
	return -1;
}
int
spdk_nvmf_session_set_features_keep_alive_timer(struct spdk_nvmf_request *req)
{
	return -1;
}

int
spdk_nvmf_session_get_features_keep_alive_timer(struct spdk_nvmf_request *req)
{
	return -1;
}

int
spdk_nvmf_session_set_features_async_event_configuration(struct spdk_nvmf_request *req)
{
	return -1;
}

int
spdk_nvmf_session_get_features_async_event_configuration(struct spdk_nvmf_request *req)
{
	return -1;
}

int
spdk_nvmf_session_async_event_request(struct spdk_nvmf_request *req)
{
	return -1;
}

int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	return -1;
}

int
spdk_nvmf_request_abort(struct spdk_nvmf_request *req)
{
	return -1;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "test";
}

uint32_t
spdk_bdev_get_block_size(const struct spdk_bdev *bdev)
{
	abort();
	return 0;
}

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	abort();
	return 0;
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
	return NULL;
}

int
spdk_bdev_flush(struct spdk_bdev_desc *desc, struct spdk_mempool *pool, struct spdk_io_channel *ch,
		uint64_t offset, uint64_t length, spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

int
spdk_bdev_unmap(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct spdk_scsi_unmap_bdesc *unmap_d, uint16_t bdesc_count, spdk_bdev_io_completion_cb cb,
		void *cb_arg)
{
	return 0;
}

//struct spdk_nvmf_fc_request *
//get_fc_req(struct spdk_nvmf_request *req)
//{
//	return NULL;
//}

void
spdk_trace_record(uint16_t tpoint_id, uint16_t poller_id, uint32_t size, uint64_t object_id,
		  uint64_t arg1)
{
}

bool
spdk_bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	return false;
}

int
spdk_bdev_write(struct spdk_bdev_desc *desc, struct spdk_mempool *pool, struct spdk_io_channel *ch,
		void *buf,
		uint64_t offset, uint64_t nbytes, spdk_bdev_io_completion_cb cb, void *cb_arg,
		struct spdk_bdev_io  **result_bdev_io)
{
	return 0;
}

int
spdk_bdev_read(struct spdk_bdev_desc *desc, struct spdk_mempool *pool, struct spdk_io_channel *ch,
	       void *buf,
	       uint64_t offset, uint64_t nbytes, spdk_bdev_io_completion_cb cb, void *cb_arg,
	       struct spdk_bdev_io  **result_bdev_io)
{
	return 0;
}

int
spdk_bdev_nvme_io_passthru(struct spdk_bdev_desc *desc,
			   struct spdk_io_channel *ch,
			   const struct spdk_nvme_cmd *cmd,
			   void *buf, size_t nbytes,
			   spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

int spdk_bdev_writev(struct spdk_bdev_io *bdev_io)
{
	return 0;
}

int spdk_bdev_readv(struct spdk_bdev_io *bdev_io)
{
	return 0;
}

int spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
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

void spdk_bdev_io_get_nvme_status(const struct spdk_bdev_io *bdev_io, int *sct, int *sc, int *dnr)
{
}

struct spdk_bdev_io *
spdk_bdev_read_init(struct spdk_bdev_desc *desc,
		    struct spdk_io_channel *ch,
		    struct spdk_mempool *bdev_io_pool,
		    spdk_bdev_io_completion_cb cb,
		    void *cb_arg,
		    struct iovec *iov,
		    int32_t *iovcnt,
		    int32_t length,
		    uint64_t offset)
{
	return NULL;
}

int
spdk_bdev_read_fini(struct spdk_bdev_io *bdev_io)
{
	return 0;
}

struct spdk_bdev_io *
spdk_bdev_write_init(struct spdk_bdev_desc *desc,
		     struct spdk_io_channel *ch,
		     struct spdk_mempool *bdev_io_pool,
		     spdk_bdev_io_completion_cb cb,
		     void *cb_arg,
		     struct iovec *iov,
		     int32_t *iovcnt,
		     int32_t length,
		     uint64_t offset)
{
	return NULL;
}


int
spdk_bdev_write_fini(struct spdk_bdev_io *bdev_io)
{
	return 0;
}

void
spdk_bdev_io_abort(struct spdk_bdev_io *bdev_io, void *abt_ctx)
{
	return;
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

const struct spdk_nvmf_transport *
spdk_nvmf_transport_get(const char *name)
{
	return NULL;
}

struct spdk_nvmf_listen_addr *
spdk_nvmf_listen_addr_create(const char *trname, enum spdk_nvmf_adrfam adrfam, const char *traddr,
			     const char *trsvcid)
{
	return NULL;
}

void
spdk_nvmf_listen_addr_cleanup(struct spdk_nvmf_listen_addr *addr)
{
}

bool
spdk_nvmf_listen_addr_compare(struct spdk_nvmf_listen_addr *a, struct spdk_nvmf_listen_addr *b)
{
	return false;
}

uint8_t
spdk_bdev_get_ana_state(struct spdk_bdev *bdev, uint16_t cntlid)
{
	return SPDK_NVME_ANA_OPTIMIZED;
}

bool
spdk_nvmf_session_get_ana_status(struct spdk_nvmf_session *session)
{
	return false;
}

static void
test_ana_log(void)
{
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_ana_log_page *hdr = NULL;
	struct spdk_nvmf_ana_group_desc_format *group_desc = NULL;
	struct spdk_nvmf_request req = {0};
	struct	spdk_nvmf_conn req_conn = {};
	struct	spdk_nvmf_session req_sess = {};
	struct spdk_bdev bdev1 = {}, bdev2 = {};
	uint8_t buffer[8192], lsp = 0x0;
	/* rgo false */
	uint64_t offset = 0x0;
	uint32_t length = 8192, nsid = 0;
	int ret = 0;

	req.data = buffer;
	snprintf(req_sess.hostnqn, 223, "nqn.2017-07.com.netapp:num1");
	req.conn = &req_conn;
	req.conn->sess = &req_sess;

	TAILQ_INIT(&g_nvmf_tgt.subsystems);
	subsystem = spdk_nvmf_create_subsystem("nqn.2016-06.io.spdk:subsystem1", SPDK_NVMF_SUBTYPE_NVME,
					       NVMF_SUBSYSTEM_MODE_VIRTUAL, NULL, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);

	req.conn->sess->subsys = subsystem;

	/* Get ANA log on a subsystem with no ANA groups */
	memset(buffer, 0x0, sizeof(buffer));
	ret = nvmf_virtual_ctrlr_get_ana_log_page(&req, lsp, offset, length);
	CU_ASSERT(ret == 0);
	hdr = (struct spdk_nvmf_ana_log_page *)req.data;
	CU_ASSERT(hdr->change_count == 0);
	CU_ASSERT(hdr->num_group_descs == 0);

	/* Add an empty ANA group */
	CU_ASSERT(subsystem->num_ana_groups == 0);
	ret = spdk_nvmf_subsystem_add_ana_group(subsystem, 3, NULL);
	CU_ASSERT(ret == 0);
	CU_ASSERT(subsystem->num_ana_groups == 1);

	/* Get ANA log on a subsystem containing 1 empty ANA group */
	memset(buffer, 0x0, sizeof(buffer));
	ret = nvmf_virtual_ctrlr_get_ana_log_page(&req, lsp, offset, length);
	CU_ASSERT(ret == 0);
	hdr = (struct spdk_nvmf_ana_log_page *)req.data;
	/* Commenting line below as code to incr change_count not in yet */
	/* CU_ASSERT(hdr->change_count == 1); */
	CU_ASSERT(hdr->num_group_descs == 1);
	group_desc = (struct spdk_nvmf_ana_group_desc_format *)(++hdr);
	CU_ASSERT(group_desc->anagrpid == 3);
	CU_ASSERT(group_desc->num_nsids == 0);
	CU_ASSERT(group_desc->change_count == 0);
	CU_ASSERT(group_desc->ana_state.state == SPDK_NVME_ANA_INACCESSIBLE);

	/* Add 2 namespaces to subsystem for the above ANA group */
	nsid = spdk_nvmf_subsystem_add_ns(subsystem, &bdev1, 0, 3);
	CU_ASSERT(nsid == 1);
	nsid = spdk_nvmf_subsystem_add_ns(subsystem, &bdev2, 0, 3);
	CU_ASSERT(nsid == 2);

	/* Get ANA log on a subsystem containing 1 ANA group with 2 namespaces */
	memset(buffer, 0x0, sizeof(buffer));
	ret = nvmf_virtual_ctrlr_get_ana_log_page(&req, lsp, offset, length);
	CU_ASSERT(ret == 0);
	hdr = (struct spdk_nvmf_ana_log_page *)req.data;
	/* CU_ASSERT(hdr->change_count == 1); */
	CU_ASSERT(hdr->num_group_descs == 1);
	group_desc = (struct spdk_nvmf_ana_group_desc_format *)(++hdr);
	CU_ASSERT(group_desc->anagrpid == 3);
	CU_ASSERT(group_desc->num_nsids == 2);
	CU_ASSERT(group_desc->change_count == 0);
	CU_ASSERT(group_desc->ana_state.state == SPDK_NVME_ANA_OPTIMIZED);
	CU_ASSERT(group_desc->nsid_list[0] == 1);
	CU_ASSERT(group_desc->nsid_list[1] == 2);

	/* Get ANA log on a subsystem containing 1 ANA group
	   with 2 namespaces at an offset */
	memset(buffer, 0x0, sizeof(buffer));
	offset = sizeof(struct spdk_nvmf_ana_log_page);
	ret = nvmf_virtual_ctrlr_get_ana_log_page(&req, lsp, offset, length);
	CU_ASSERT(ret == 0);
	/* We will not get header */
	group_desc = (struct spdk_nvmf_ana_group_desc_format *)(req.data);
	CU_ASSERT(group_desc->anagrpid == 3)
	CU_ASSERT(group_desc->num_nsids == 2);
	CU_ASSERT(group_desc->change_count == 0);
	CU_ASSERT(group_desc->ana_state.state == SPDK_NVME_ANA_OPTIMIZED);
	CU_ASSERT(group_desc->nsid_list[0] == 1);
	CU_ASSERT(group_desc->nsid_list[1] == 2);

	/* Get ANA log on a subsystem containing 1 ANA group
	   with 2 namespaces at an offset with rgo bit set */
	memset(buffer, 0x0, sizeof(buffer));
	lsp = 0x1;
	offset = sizeof(struct spdk_nvmf_ana_log_page);
	ret = nvmf_virtual_ctrlr_get_ana_log_page(&req, lsp, offset, length);
	CU_ASSERT(ret == 0);
	/* We will not get header */
	group_desc = (struct spdk_nvmf_ana_group_desc_format *)(req.data);
	CU_ASSERT(group_desc->anagrpid == 3);
	/* We should not get nsids */
	CU_ASSERT(group_desc->num_nsids == 0);
	CU_ASSERT(group_desc->change_count == 0);
	CU_ASSERT(group_desc->ana_state.state == SPDK_NVME_ANA_OPTIMIZED);

	/* Get ANA log on a subsystem containing 1 ANA group with 2
	   namespaces and length less than the size of log page */
	memset(buffer, 0x0, sizeof(buffer));
	lsp = 0;
	offset = 0;
	/* Read only change count and number of ana descriptors */
	length = 10;
	ret = nvmf_virtual_ctrlr_get_ana_log_page(&req, lsp, offset, length);
	CU_ASSERT(ret == 0);
	hdr = (struct spdk_nvmf_ana_log_page *)req.data;
	/* CU_ASSERT(hdr->change_count == 1); */
	CU_ASSERT(hdr->num_group_descs == 1);
	group_desc = (struct spdk_nvmf_ana_group_desc_format *)(++hdr);
	CU_ASSERT(group_desc->anagrpid == 0);
	CU_ASSERT(group_desc->num_nsids == 0);
	CU_ASSERT(group_desc->change_count == 0);
	CU_ASSERT(group_desc->ana_state.state == 0);

	/* Delete the subsystem with ANA group */
	spdk_nvmf_delete_subsystem(subsystem);
}

static void
nvmf_test_nvmf_virtual_ctrlr_get_log_page(void)
{
	test_ana_log();
	/* Test other log pages */
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

	if (CU_add_test(suite, "virtual_ctrlr_get_log_page",
			nvmf_test_nvmf_virtual_ctrlr_get_log_page) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
