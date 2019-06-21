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

#ifndef __NVMF_INTERNAL_H__
#define __NVMF_INTERNAL_H__

#include "spdk/stdinc.h"

#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/assert.h"
#include "spdk/queue.h"
#include "spdk/util.h"

#define SPDK_NVMF_DEFAULT_NUM_SESSIONS_PER_LCORE 1

/* XXX: Throwaway with spdk17.10. Leverage spdk17.10 containers */
typedef bool (*disc_log_allowed_fn_t)(struct spdk_nvmf_request *, struct spdk_nvmf_subsystem *);

struct spdk_nvmf_tgt {
	struct spdk_nvmf_tgt_opts               opts;
	uint64_t				discovery_genctr;
	disc_log_allowed_fn_t                   disc_log_allowed_fn; /* XXX: Throwaway with spdk17.10 */
	TAILQ_HEAD(, spdk_nvmf_subsystem)	subsystems;
	TAILQ_HEAD(, spdk_nvmf_listen_addr)	listen_addrs;
	uint32_t				current_subsystem_id;
};

extern struct spdk_nvmf_tgt g_nvmf_tgt;

struct spdk_nvmf_listen_addr *spdk_nvmf_listen_addr_create(const char *trname,
		enum spdk_nvmf_adrfam adrfam, const char *traddr, const char *trsvcid);
bool spdk_nvmf_listen_addr_compare(struct spdk_nvmf_listen_addr *a,
				   struct spdk_nvmf_listen_addr *b);
void spdk_nvmf_listen_addr_destroy(struct spdk_nvmf_listen_addr *addr);
void spdk_nvmf_listen_addr_cleanup(struct spdk_nvmf_listen_addr *addr);

static inline int
spdk_nvmf_is_fused_command(struct spdk_nvme_cmd *cmd)
{
	bool fused = false;

	if ((cmd->fuse == SPDK_NVME_FUSED_CMD1) || (cmd->fuse == SPDK_NVME_FUSED_CMD2)) {
		fused = true;
	}

	return fused;
}


#define OBJECT_NVMF_IO				0x30

#define TRACE_GROUP_NVMF			0x3
#define TRACE_NVMF_IO_START			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x00)
#define TRACE_RDMA_READ_START			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x01)
#define TRACE_RDMA_WRITE_START			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x02)
#define TRACE_RDMA_READ_COMPLETE      		SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x03)
#define TRACE_RDMA_WRITE_COMPLETE		SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x04)
#define TRACE_NVMF_LIB_READ_START		SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x05)
#define TRACE_NVMF_LIB_WRITE_START		SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x06)
#define TRACE_NVMF_LIB_COMPLETE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x07)
#define TRACE_NVMF_IO_COMPLETE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x08)
#define TRACE_FC_REQ_INIT                       SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x09)
#define TRACE_FC_REQ_READ_BDEV                  SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x0A)
#define TRACE_FC_REQ_READ_XFER                  SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x0B)
#define TRACE_FC_REQ_READ_RSP                   SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x0C)
#define TRACE_FC_REQ_WRITE_BUFFS                SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x0D)
#define TRACE_FC_REQ_WRITE_XFER                 SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x0E)
#define TRACE_FC_REQ_WRITE_BDEV                 SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x0F)
#define TRACE_FC_REQ_WRITE_RSP                  SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x10)
#define TRACE_FC_REQ_NONE_BDEV                  SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x11)
#define TRACE_FC_REQ_NONE_RSP                   SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x12)
#define TRACE_FC_REQ_SUCCESS                    SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x13)
#define TRACE_FC_REQ_FAILED                     SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x14)
#define TRACE_FC_REQ_ABORTED                    SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x15)
#define TRACE_FC_REQ_PENDING                    SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x16)
#define TRACE_FC_REQ_FUSED_WAITING              SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x17)

#endif /* __NVMF_INTERNAL_H__ */
