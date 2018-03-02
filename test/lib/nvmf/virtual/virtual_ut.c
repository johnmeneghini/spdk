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

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

#include "spdk_cunit.h"
#include "lib/test_env.c"

#include "virtual.c"

struct spdk_nvmf_tgt g_nvmf_tgt;

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

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

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev *bdev, uint32_t priority)
{
	return NULL;
}

struct spdk_bdev_io *spdk_bdev_flush(struct spdk_bdev *bdev, struct spdk_mempool *pool,
				     struct spdk_io_channel *ch,
				     uint64_t offset, uint64_t length, spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return NULL;
}

struct spdk_bdev_io *spdk_bdev_unmap(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
				     struct spdk_scsi_unmap_bdesc *unmap_d, uint16_t bdesc_count, spdk_bdev_io_completion_cb cb,
				     void *cb_arg)
{
	return NULL;
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

struct spdk_bdev_io *spdk_bdev_write(struct spdk_bdev *bdev, struct spdk_mempool *pool,
				     struct spdk_io_channel *ch, void *buf,
				     uint64_t offset, uint64_t nbytes, spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return NULL;
}

struct spdk_bdev_io *spdk_bdev_read(struct spdk_bdev *bdev, struct spdk_mempool *pool,
				    struct spdk_io_channel *ch, void *buf,
				    uint64_t offset, uint64_t nbytes, spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return NULL;
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

void spdk_bdev_unclaim(struct spdk_bdev *bdev)
{
}

const char *spdk_nvmf_subsystem_get_nqn(struct spdk_nvmf_subsystem *subsystem)
{
	return NULL;
}

void spdk_bdev_io_get_nvme_status(const struct spdk_bdev_io *bdev_io, int *sct, int *sc, int *dnr)
{
}

struct spdk_bdev_io *
spdk_bdev_read_init(struct spdk_bdev *bdev,
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
spdk_bdev_write_init(struct spdk_bdev *bdev,
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


static void
nvmf_test_nvmf_virtual_ctrlr_get_log_page(void)
{
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
