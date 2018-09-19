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

#ifndef SPDK_NVMF_SUBSYSTEM_H
#define SPDK_NVMF_SUBSYSTEM_H

#include "nvmf_internal.h"

#include "spdk/nvme.h"
#include "spdk/nvmf.h"

struct spdk_nvmf_session *spdk_nvmf_subsystem_get_ctrlr(struct spdk_nvmf_subsystem *subsystem,
		uint16_t cntlid);

void spdk_nvmf_get_discovery_log_page(struct spdk_nvmf_request *req, uint64_t offset,
				      uint32_t length);
extern bool spdk_nvmf_valid_nqn(const char *nqn);

extern const struct spdk_nvmf_ctrlr_ops spdk_nvmf_direct_ctrlr_ops;
extern const struct spdk_nvmf_ctrlr_ops spdk_nvmf_virtual_ctrlr_ops;
extern const struct spdk_nvmf_ctrlr_ops spdk_nvmf_discovery_ctrlr_ops;

struct spdk_nvmf_host *spdk_nvmf_find_subsystem_host(struct spdk_nvmf_subsystem *subsystem,
		const char *hostnqn);

struct spdk_nvmf_listen_addr *
spdk_nvmf_find_subsystem_listener(struct spdk_nvmf_subsystem *subsystem,
				  struct spdk_nvmf_listen_addr *listen_addr);

void
spdk_nvmf_subsystem_set_ns_changed(struct spdk_nvmf_subsystem *subsystem,
				   uint32_t nsid);

#endif /* SPDK_NVMF_SUBSYSTEM_H */
