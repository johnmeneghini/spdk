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

#include "spdk/conf.h"
#include "spdk/nvmf.h"
#include "spdk/trace.h"

#include "spdk_internal/log.h"

#include "subsystem.h"
#include "transport.h"

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

#define MAX_SUBSYSTEMS 4

struct spdk_nvmf_tgt g_nvmf_tgt;

int
spdk_nvmf_tgt_opts_init(struct spdk_nvmf_tgt_opts *opts)
{
	int rc;

	if (!opts) {
		SPDK_ERRLOG("Invalid config passed during NVMF target init\n");
		return -1;
	}

	g_nvmf_tgt.opts = *opts;

	g_nvmf_tgt.discovery_genctr = 0;

	/* XXX: Throwaway with spdk17.10. Leverage spdk17.10 containers */
	g_nvmf_tgt.disc_log_allowed_fn = NULL;
	g_nvmf_tgt.current_subsystem_id = 0;
	TAILQ_INIT(&g_nvmf_tgt.subsystems);
	TAILQ_INIT(&g_nvmf_tgt.listen_addrs);

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Max Queues Per Session: %d\n",
		      g_nvmf_tgt.opts.max_queues_per_session);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Max IO Queue Depth: %d\n", g_nvmf_tgt.opts.max_io_queue_depth);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Max In Capsule Data: %d bytes\n",
		      g_nvmf_tgt.opts.in_capsule_data_size);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Max I/O Size: %d bytes\n", g_nvmf_tgt.opts.max_io_size);

	rc = spdk_nvmf_transport_init();
	if (rc < 0) {
		SPDK_ERRLOG("Transport initialization failed\n");
		return -1;
	}

	return 0;
}

int
spdk_nvmf_tgt_opts_update(struct spdk_nvmf_tgt_opts *opts)
{
	struct spdk_nvmf_subsystem	*subsystem;

	if (!opts) {
		SPDK_ERRLOG("Invalid config passed during NVMF target opts update\n");
		return -1;
	}

	/* Ensure that config is updated only when there are no subsystems
	 * other than a discovery subsystem
	 */
	TAILQ_FOREACH(subsystem, &g_nvmf_tgt.subsystems, entries) {
		if (subsystem->subtype != SPDK_NVMF_SUBTYPE_DISCOVERY) {
			SPDK_ERRLOG("NVMF config cannot be updated while there are subsystems\n");
			return -1;
		}
	}

	g_nvmf_tgt.opts = *opts;
	return 0;
}

int
spdk_nvmf_tgt_fini(void)
{
	struct spdk_nvmf_listen_addr *listen_addr, *listen_addr_tmp;

	TAILQ_FOREACH_SAFE(listen_addr, &g_nvmf_tgt.listen_addrs, link, listen_addr_tmp) {
		TAILQ_REMOVE(&g_nvmf_tgt.listen_addrs, listen_addr, link);
		g_nvmf_tgt.discovery_genctr++;

		spdk_nvmf_listen_addr_destroy(listen_addr);
	}

	spdk_nvmf_transport_fini();

	return 0;
}

/* Returns true if the listen addrs match */
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
	const struct spdk_nvmf_transport *transport;

	transport = spdk_nvmf_transport_get(trname);
	if (!transport) {
		return NULL;
	}

	listen_addr = calloc(1, sizeof(*listen_addr));
	if (!listen_addr) {
		return NULL;
	}

	listen_addr->adrfam = adrfam;

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

	return listen_addr;
}

bool
spdk_nvmf_listen_addr_delete(struct spdk_nvmf_listen_addr *addr)
{
	struct spdk_nvmf_listen_addr *listen_addr, *listen_addr_tmp;

	TAILQ_FOREACH_SAFE(listen_addr, &g_nvmf_tgt.listen_addrs, link, listen_addr_tmp) {
		if (spdk_nvmf_listen_addr_compare(addr, listen_addr)) {
			TAILQ_REMOVE(&g_nvmf_tgt.listen_addrs, listen_addr, link);
			spdk_nvmf_listen_addr_destroy(listen_addr);
			return true;
		}

	}


	return false;
}

void
spdk_nvmf_listen_addr_destroy(struct spdk_nvmf_listen_addr *addr)
{
	const struct spdk_nvmf_transport *transport;

	transport = spdk_nvmf_transport_get(addr->trname);
	assert(transport != NULL);
	if (transport == NULL) {
		return;
	}
	transport->listen_addr_remove(addr);

	spdk_nvmf_listen_addr_cleanup(addr);
}

void
spdk_nvmf_listen_addr_cleanup(struct spdk_nvmf_listen_addr *addr)
{
	free(addr->trname);
	free(addr->trsvcid);
	free(addr->traddr);
	free(addr);
}

SPDK_TRACE_REGISTER_FN(nvmf_trace)
{
	spdk_trace_register_object(OBJECT_NVMF_IO, 'r');
	spdk_trace_register_description("NVMF_IO_START", "", TRACE_NVMF_IO_START,
					OWNER_NONE, OBJECT_NVMF_IO, 1, 0, 0, "");
	spdk_trace_register_description("NVMF_RDMA_READ_START", "", TRACE_RDMA_READ_START,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_RDMA_WRITE_START", "", TRACE_RDMA_WRITE_START,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_RDMA_READ_COMPLETE", "", TRACE_RDMA_READ_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_RDMA_WRITE_COMPLETE", "", TRACE_RDMA_WRITE_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_LIB_READ_START", "", TRACE_NVMF_LIB_READ_START,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_LIB_WRITE_START", "", TRACE_NVMF_LIB_WRITE_START,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_LIB_COMPLETE", "", TRACE_NVMF_LIB_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_IO_COMPLETION_DONE", "", TRACE_NVMF_IO_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_INIT", "", TRACE_FC_REQ_INIT,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_READ_BDEV", "", TRACE_FC_REQ_READ_BDEV,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_READ_XFER", "", TRACE_FC_REQ_READ_XFER,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_READ_RSP", "", TRACE_FC_REQ_READ_RSP,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_WRITE_BUFFS", "", TRACE_FC_REQ_WRITE_BUFFS,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_WRITE_XFER", "", TRACE_FC_REQ_WRITE_XFER,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_WRITE_BDEV", "", TRACE_FC_REQ_WRITE_BDEV,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_WRITE_RSP", "", TRACE_FC_REQ_WRITE_RSP,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_NONE_BDEV", "", TRACE_FC_REQ_NONE_BDEV,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_NONE_RSP", "", TRACE_FC_REQ_NONE_RSP,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_SUCCESS", "", TRACE_FC_REQ_SUCCESS,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_FAILED", "", TRACE_FC_REQ_FAILED,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_ABORTED", "", TRACE_FC_REQ_ABORTED,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_PENDING", "", TRACE_FC_REQ_PENDING,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_FC_REQ_FUSED_WAITING", "", TRACE_FC_REQ_FUSED_WAITING,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
}
