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

#ifndef NVMF_REQUEST_H
#define NVMF_REQUEST_H

#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/queue.h"

typedef enum _spdk_nvmf_request_exec_status {
	SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_ERROR = -1,
	SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE,
	SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS,
	SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_READY,
	SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_PENDING
} spdk_nvmf_request_exec_status;

union nvmf_h2c_msg {
	struct spdk_nvmf_capsule_cmd			nvmf_cmd;
	struct spdk_nvme_cmd				nvme_cmd;
	struct spdk_nvmf_fabric_prop_set_cmd		prop_set_cmd;
	struct spdk_nvmf_fabric_prop_get_cmd		prop_get_cmd;
	struct spdk_nvmf_fabric_connect_cmd		connect_cmd;
};
SPDK_STATIC_ASSERT(sizeof(union nvmf_h2c_msg) == 64, "Incorrect size");

union nvmf_c2h_msg {
	struct spdk_nvme_cpl				nvme_cpl;
	struct spdk_nvmf_fabric_prop_get_rsp		prop_get_rsp;
	struct spdk_nvmf_fabric_connect_rsp		connect_rsp;
};
SPDK_STATIC_ASSERT(sizeof(union nvmf_c2h_msg) == 16, "Incorrect size");

#define MAX_NUM_OF_IOVECTORS 17
#define MAX_REQ_STATES       16

struct spdk_nvmf_request {
	struct spdk_nvmf_conn		*conn;
	uint32_t			length;
	enum spdk_nvme_data_transfer	xfer;
	struct iovec 			iov[MAX_NUM_OF_IOVECTORS];
	int 				iovcnt;
	void				*data;
	union nvmf_h2c_msg		*cmd;
	union nvmf_c2h_msg		*rsp;
	struct spdk_scsi_unmap_bdesc	*unmap_bdesc;
	struct spdk_bdev_io 		*bdev_io;
	uint64_t			req_state_trace[MAX_REQ_STATES];
	struct spdk_mempool		*io_rsrc_pool;
	struct spdk_nvmf_request        *fused_partner;
	bool 				is_fused_partner_failed;
	bool                            fail_with_fused_aborted;
	spdk_nvmf_set_sge		set_sge;
	bool				sgl_filled;
};

spdk_nvmf_request_exec_status spdk_nvmf_request_setup_dma(struct spdk_nvmf_request *req,
		uint32_t max_io_size);

int spdk_nvmf_request_exec(struct spdk_nvmf_request *req);

int spdk_nvmf_request_complete(struct spdk_nvmf_request *req);

void spdk_nvmf_request_cleanup(struct spdk_nvmf_request *req);

int spdk_nvmf_request_abort(struct spdk_nvmf_request *req);
#endif
