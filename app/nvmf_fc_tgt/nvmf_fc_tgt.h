/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2017 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Limited and/or its subsidiaries.
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

#ifndef NVMF_FC_TGT_H
#define NVMF_FC_TGT_H

#include "spdk/env.h"
#include "spdk/nvmf.h"
#include "spdk/queue.h"
#include "spdk/event.h"
#include "spdk/error.h"
#include "nvmf_fc/bcm_fc.h"

#define MAX_HOSTS 255

struct spdk_host_conf {
	const char *hostnqn;
	uint16_t max_io_queue_depth;
	uint16_t max_io_queue_num;
};

struct rpc_listen_address {
	char *transport;
	char *traddr;
	char *trsvcid;
};

struct nvmf_tgt_subsystem {
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_poller *poller;

	TAILQ_ENTRY(nvmf_tgt_subsystem) tailq;

	uint32_t lcore;
};

struct nvmf_tgt_subsystem *
spdk_nvmf_bcm_fc_tgt_subsystem_first(void);

struct nvmf_tgt_subsystem *
spdk_nvmf_bcm_fc_tgt_subsystem_next(struct nvmf_tgt_subsystem *subsystem);

int spdk_nvmf_bcm_fc_parse_conf(void);

struct nvmf_tgt_subsystem *
spdk_nvmf_bcm_fc_tgt_create_subsystem(const char *name,
				      enum spdk_nvmf_subtype subtype,
				      enum spdk_nvmf_subsystem_mode mode,
				      uint32_t lcore);

void
spdk_nvmf_bcm_fc_tgt_start_subsystem(struct nvmf_tgt_subsystem *app_subsys);

int
spdk_nvmf_bcm_fc_construct_subsystem(const char *name,
				     const char *mode_str, int32_t lcore,
				     int num_listen_addresses,
				     struct rpc_listen_address *addresses,
				     bool allow_any_listener,
				     int num_hosts, struct spdk_host_conf *hosts,
				     bool allow_any_host, const char *bdf, const char *sn,
				     int num_devs, char *dev_list[],
				     char *dev_nidt[], char *dev_nid[],
				     uint32_t *anagrpids);

int
spdk_nvmf_bcm_fc_tgt_shutdown_subsystem_by_nqn(const char *nqn);

int spdk_nvmf_bcm_fc_tgt_start(struct spdk_app_opts *opts);

spdk_err_t
spdk_nvmf_bcm_fc_tgt_add_port(const char *trname, struct spdk_nvmf_bcm_fc_nport *nport);

spdk_err_t
spdk_nvmf_bcm_fc_tgt_remove_port(const char *trname, struct spdk_nvmf_bcm_fc_nport *nport);

#endif
