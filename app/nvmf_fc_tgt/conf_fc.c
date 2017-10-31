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

#include "nvmf_fc_tgt.h"
#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/log.h"
#include "spdk/bdev.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/string.h"
#include "spdk/util.h"

#define MAX_LISTEN_ADDRESSES 255
#define MAX_HOSTS 255
#define PORTNUMSTRLEN 32
#define SPDK_NVMF_DEFAULT_SIN_PORT ((uint16_t)4420)

struct spdk_nvmf_probe_ctx {
	struct nvmf_tgt_subsystem	*app_subsystem;
	bool				any;
	bool				found;
	struct spdk_nvme_transport_id	trid;
};

#define SPDK_NVMF_CONFIG_ASSOC_DEFAULT 8
#define SPDK_NVMF_CONFIG_ASSOC_MIN 2
#define SPDK_NVMF_CONFIG_ASSOC_MAX 65535

#define SPDK_NVMF_CONFIG_AQ_DEPTH_DEFAULT 32
#define SPDK_NVMF_CONFIG_AQ_DEPTH_MIN 32
#define SPDK_NVMF_CONFIG_AQ_DEPTH_MAX 1024

#define SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_DEFAULT 4
#define SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_MIN 2
#define SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_MAX 1024

#define SPDK_NVMF_CONFIG_QUEUE_DEPTH_DEFAULT 128
#define SPDK_NVMF_CONFIG_QUEUE_DEPTH_MIN 16
#define SPDK_NVMF_CONFIG_QUEUE_DEPTH_MAX 1024

#define SPDK_NVMF_CONFIG_MAX_IO_SIZE_DEFAULT 65536
#define SPDK_NVMF_CONFIG_MAX_IO_SIZE_MIN 4096
#define SPDK_NVMF_CONFIG_MAX_IO_SIZE_MAX 65536

#define SPDK_NVMF_CONFIG_ARBITRATION_BURST_DEFAULT 6
#define SPDK_NVMF_CONFIG_ERR_LOG_PAGE_ENTRIES_DEFAULT 127
#define SPDK_NVMF_CONFIG_KEEP_ALIVE_INTERVAL_DEFAULT 10
#define SPDK_NVMF_CONFIG_VOLATILE_WRITE_CACHE_DEFAULT 1
#define SPDK_NVMF_CONFIG_SGL_SUPPORT_DEFAULT 5

static int32_t g_last_core = -1;

static uint64_t
nvmf_fc_parse_nvmf_core_mask(char *coremask)
{
	int i;

	if (coremask == NULL) {
		return 0;
	}

	/*
	 * Remove all blank characters ahead and after.
	 * Remove 0x/0X if exists.
	 */
	while (isblank(*coremask)) {
		coremask++;
	}

	if (coremask[0] == '0' && ((coremask[1] == 'x') || (coremask[1] == 'X'))) {
		coremask += 2;
	}

	i = strlen(coremask);
	while ((i > 0) && isblank(coremask[i - 1])) {
		i--;
	}

	if (i == 0) {
		return 0;
	}

	return (uint64_t)strtol(coremask, NULL, 16);
}

static int
nvmf_fc_add_discovery_subsystem(void)
{
	struct nvmf_tgt_subsystem *app_subsys;

	app_subsys = spdk_nvmf_bcm_fc_tgt_create_subsystem(SPDK_NVMF_DISCOVERY_NQN,
			SPDK_NVMF_SUBTYPE_DISCOVERY,
			NVMF_SUBSYSTEM_MODE_DIRECT,
			spdk_env_get_master_lcore());
	if (app_subsys == NULL) {
		SPDK_ERRLOG("Failed creating discovery nvmf library subsystem\n");
		return -1;
	}

	spdk_nvmf_bcm_fc_tgt_start_subsystem(app_subsys);

	return 0;
}

static int
nvmf_fc_parse_nvmf_tgt(void)
{
	struct spdk_conf_section *sp;
	struct spdk_nvmf_tgt_opts opts;
	int intval;
	int rc;
	char *lcore_mask_str;

	sp = spdk_conf_find_section(NULL, "Nvmf");
	if (sp == NULL) {
		SPDK_ERRLOG("No Nvmf section in configuration file.\n");
		return -1;
	}

	intval = spdk_conf_section_get_intval(sp, "MaxQueueDepth");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_QUEUE_DEPTH_DEFAULT;
	}
	opts.max_queue_depth = spdk_max(intval, SPDK_NVMF_CONFIG_QUEUE_DEPTH_MIN);
	opts.max_queue_depth = spdk_min(intval, SPDK_NVMF_CONFIG_QUEUE_DEPTH_MAX);

	intval = spdk_conf_section_get_intval(sp, "MaxAssociations");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_ASSOC_DEFAULT;
	}
	opts.max_associations = spdk_max(intval, SPDK_NVMF_CONFIG_ASSOC_MIN);
	opts.max_associations = spdk_min(intval, SPDK_NVMF_CONFIG_ASSOC_MAX);

	intval = spdk_conf_section_get_intval(sp, "MaxAqDepth");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_AQ_DEPTH_DEFAULT;
	}
	opts.max_aq_depth = spdk_max(intval, SPDK_NVMF_CONFIG_AQ_DEPTH_MIN);
	opts.max_aq_depth = spdk_min(intval, SPDK_NVMF_CONFIG_AQ_DEPTH_MAX);

	intval = spdk_conf_section_get_intval(sp, "MaxQueuesPerSession");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_DEFAULT;
	}
	opts.max_queues_per_session = spdk_max(intval, SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_MIN);
	opts.max_queues_per_session = spdk_min(intval, SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_MAX);

	intval = spdk_conf_section_get_intval(sp, "MaxIOSize");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_MAX_IO_SIZE_DEFAULT;
	} else if ((intval % 4096) != 0) {
		SPDK_ERRLOG("MaxIOSize must be a multiple of 4096\n");
		return -1;
	}
	opts.max_io_size = spdk_max(intval, SPDK_NVMF_CONFIG_MAX_IO_SIZE_MIN);
	opts.max_io_size = spdk_min(intval, SPDK_NVMF_CONFIG_MAX_IO_SIZE_MAX);

	intval = spdk_conf_section_get_intval(sp, "ArbitrationBurst");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_ARBITRATION_BURST_DEFAULT;
	}
	opts.rab = intval;

	intval = spdk_conf_section_get_intval(sp, "IEEEOUI0");
	if (intval < 0) {
		intval = 0;
	}
	opts.ieee[0] = intval;

	intval = spdk_conf_section_get_intval(sp, "IEEEOUI1");
	if (intval < 0) {
		intval = 0;
	}
	opts.ieee[1] = intval;

	intval = spdk_conf_section_get_intval(sp, "IEEEOUI2");
	if (intval < 0) {
		intval = 0;
	}
	opts.ieee[2] = intval;


	intval = spdk_conf_section_get_intval(sp, "CMIC");
	if (intval < 0) {
		intval = 0;
	}
	opts.cmic = intval;

	intval = spdk_conf_section_get_intval(sp, "OptAerSupport");
	if (intval < 0) {
		intval = 0;
	}
	opts.oaes = intval;

	intval = spdk_conf_section_get_intval(sp, "AbortLimit");
	if (intval < 0) {
		intval = 0;
	}
	opts.acl = intval;

	intval = spdk_conf_section_get_intval(sp, "AerLimit");
	if (intval < 1) {
		intval = 1;
	}
	opts.aerl = intval;

	intval = spdk_conf_section_get_intval(sp, "ErrLogEntries");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_ERR_LOG_PAGE_ENTRIES_DEFAULT;
	}
	opts.elpe = intval;

	intval = spdk_conf_section_get_intval(sp, "NumPowerStates");
	if (intval < 0) {
		intval = 0;
	}
	opts.npss = intval;

	intval = spdk_conf_section_get_intval(sp, "KeepAliveInt");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_KEEP_ALIVE_INTERVAL_DEFAULT;
	}
	opts.kas = intval;

	intval = spdk_conf_section_get_intval(sp, "VolatileWriteCache");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_VOLATILE_WRITE_CACHE_DEFAULT;
	}
	opts.vwc = intval;

	intval = spdk_conf_section_get_intval(sp, "AtomicWriteNormal");
	if (intval < 0) {
		intval = 0;
	}
	opts.awun = intval;

	intval = spdk_conf_section_get_intval(sp, "AtomicWritePowerFail");
	if (intval < 0) {
		intval = 0;
	}
	opts.awupf = intval;

	intval = spdk_conf_section_get_intval(sp, "SGLSupport");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_SGL_SUPPORT_DEFAULT;
	}
	opts.sgls = intval;

	lcore_mask_str = spdk_conf_section_get_val(sp, "NvmfReactorMask");
	if (!lcore_mask_str) {
		SPDK_ERRLOG("NvmfReactorMask not specified. \n");
		return -1;
	}

	opts.lcore_mask = nvmf_fc_parse_nvmf_core_mask(lcore_mask_str);
	if (!opts.lcore_mask) {
		SPDK_ERRLOG("NvmfReactorMask value invalid. \n");
		return -1;
	}

	rc = spdk_nvmf_tgt_opts_init(&opts);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_nvmf_tgt_opts_init() failed\n");
		return rc;
	}

	rc = nvmf_fc_add_discovery_subsystem();
	if (rc != 0) {
		SPDK_ERRLOG("nvmf_add_discovery_subsystem failed\n");
		return rc;
	}

	return 0;
}

#ifndef SPDK_NO_NVMF_DIRECT
static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvmf_probe_ctx *ctx = cb_ctx;

	if (ctx->any && !ctx->found) {
		ctx->found = true;
		return true;
	}

	if (strcmp(trid->traddr, ctx->trid.traddr) == 0) {
		ctx->found = true;
		return true;
	}

	return false;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvmf_probe_ctx *ctx = cb_ctx;
	int rc;
	int numa_node = -1;
	struct spdk_pci_addr pci_addr;
	struct spdk_pci_device *pci_dev;

	spdk_pci_addr_parse(&pci_addr, trid->traddr);

	SPDK_NOTICELOG("Attaching NVMe device %p at %s to subsystem %s\n",
		       ctrlr,
		       trid->traddr,
		       spdk_nvmf_subsystem_get_nqn(ctx->app_subsystem->subsystem));

	pci_dev = spdk_pci_get_device(&pci_addr);
	if (pci_dev) {
		numa_node = spdk_pci_device_get_socket_id(pci_dev);
	}
	if (numa_node >= 0) {
		/* Running subsystem and NVMe device is on the same socket or not */
		if (spdk_env_get_socket_id(ctx->app_subsystem->lcore) != (unsigned)numa_node) {
			SPDK_WARNLOG("Subsystem %s is configured to run on a CPU core %u belonging "
				     "to a different NUMA node than the associated NVMe device. "
				     "This may result in reduced performance.\n",
				     spdk_nvmf_subsystem_get_nqn(ctx->app_subsystem->subsystem),
				     ctx->app_subsystem->lcore);
			SPDK_WARNLOG("The NVMe device is on socket %u\n", numa_node);
			SPDK_WARNLOG("The Subsystem is on socket %u\n",
				     spdk_env_get_socket_id(ctx->app_subsystem->lcore));
		}
	}

	rc = nvmf_subsystem_add_ctrlr(ctx->app_subsystem->subsystem, ctrlr, &pci_addr);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to add controller to subsystem\n");
	}
}
#endif

static int
nvmf_fc_validate_sn(const char *sn)
{
	size_t len;

	len = strlen(sn);
	if (len > MAX_SN_LEN) {
		SPDK_ERRLOG("Invalid sn \"%s\": length %zu > max %d\n", sn, len, MAX_SN_LEN);
		return -1;
	}

	return 0;
}

static int
nvmf_fc_allocate_lcore(uint64_t mask, uint32_t lcore)
{
	uint32_t end;

	if (lcore == 0) {
		end = 0;
	} else {
		end = lcore - 1;
	}

	do {
		if (((mask >> lcore) & 1U) == 1U) {
			break;
		}
		lcore = (lcore + 1) % 64;
	} while (lcore != end);

	return lcore;
}

static int
nvmf_fc_parse_subsystem(struct spdk_conf_section *sp)
{
	const char *nqn, *mode_str;
	int i, ret;
	int lcore;
	int num_listen_addrs = 0;
	struct rpc_listen_address listen_addrs[MAX_LISTEN_ADDRESSES];
	int num_hosts;
	char *hosts[MAX_HOSTS];
	const char *bdf;
	const char *sn;
	int num_devs;
	char *devs[MAX_VIRTUAL_NAMESPACE];

	nqn = spdk_conf_section_get_val(sp, "NQN");
	mode_str = spdk_conf_section_get_val(sp, "Mode");
	lcore = spdk_conf_section_get_intval(sp, "Core");

	/* Parse Host sections */
	for (i = 0; i < MAX_HOSTS; i++) {
		hosts[i] = spdk_conf_section_get_nval(sp, "Host", i);
		if (!hosts[i]) {
			break;
		}
	}
	num_hosts = i;

	bdf = spdk_conf_section_get_val(sp, "NVMe");
	sn = spdk_conf_section_get_val(sp, "SN");

	num_devs = 0;
	for (i = 0; i < MAX_VIRTUAL_NAMESPACE; i++) {
		devs[i] = spdk_conf_section_get_nmval(sp, "Namespace", i, 0);
		if (!devs[i]) {
			break;
		}

		num_devs++;
	}

	ret = spdk_nvmf_bcm_fc_construct_subsystem(nqn, mode_str, lcore,
			num_listen_addrs,
			listen_addrs, num_hosts,
			hosts, bdf, sn, num_devs,
			devs);

	return ret;
}

static int
nvmf_fc_parse_subsystems(void)
{
	int rc = 0;
	struct spdk_conf_section *sp;

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "Subsystem")) {
			rc = nvmf_fc_parse_subsystem(sp);
			if (rc < 0) {
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}
	return 0;
}

int
spdk_nvmf_bcm_fc_parse_conf(void)
{
	int rc;

	/* NVMf section */
	rc = nvmf_fc_parse_nvmf_tgt();
	if (rc < 0) {
		return rc;
	}

	/* Subsystem sections */
	rc = nvmf_fc_parse_subsystems();
	if (rc < 0) {
		return rc;
	}

	return 0;
}

int
spdk_nvmf_bcm_fc_construct_subsystem(const char *name,
				     const char *mode_str, int32_t lcore,
				     int num_listen_addresses,
				     struct rpc_listen_address *addresses,
				     int num_hosts, char *hosts[],
				     const char *bdf, const char *sn,
				     int num_devs, char *dev_list[])
{
	struct spdk_nvmf_subsystem *subsystem;
	struct nvmf_tgt_subsystem *app_subsys;
	enum spdk_nvmf_subsystem_mode mode;
	int i;
	uint64_t mask;

	if (name == NULL) {
		SPDK_ERRLOG("No NQN specified for subsystem\n");
		return -1;
	}

	if (num_hosts > MAX_HOSTS) {
		SPDK_ERRLOG("invalid hosts number\n");
		return -1;
	}

	if (lcore < 0) {
		lcore = ++g_last_core;
	}

	/* Determine which core to assign to the subsystem */
	mask = spdk_app_get_core_mask();
	lcore = nvmf_fc_allocate_lcore(mask, lcore);
	g_last_core = lcore;

	/* Determine the mode the subsysem will operate in */
	if (mode_str == NULL) {
		SPDK_ERRLOG("No Mode specified for Subsystem %s\n", name);
		return -1;
	}

	if (strcasecmp(mode_str, "Direct") == 0) {
		mode = NVMF_SUBSYSTEM_MODE_DIRECT;
	} else if (strcasecmp(mode_str, "Virtual") == 0) {
		mode = NVMF_SUBSYSTEM_MODE_VIRTUAL;
	} else {
		SPDK_ERRLOG("Invalid Subsystem mode: %s\n", mode_str);
		return -1;
	}

	app_subsys = spdk_nvmf_bcm_fc_tgt_create_subsystem(name,
			SPDK_NVMF_SUBTYPE_NVME,
			mode, lcore);
	if (app_subsys == NULL) {
		SPDK_ERRLOG("Subsystem creation failed\n");
		return -1;
	}
	subsystem = app_subsys->subsystem;

	/* Parse Host sections */
	for (i = 0; i < num_hosts; i++) {
		spdk_nvmf_subsystem_add_host(subsystem, hosts[i]);
	}

	if (mode == NVMF_SUBSYSTEM_MODE_DIRECT) {
#ifndef SPDK_NO_NVMF_DIRECT
		struct spdk_nvmf_probe_ctx ctx = { 0 };
		struct spdk_nvme_transport_id trid = {};
		struct spdk_pci_addr pci_addr = {};

		if (bdf == NULL) {
			SPDK_ERRLOG("Subsystem %s: missing NVMe directive\n", name);
			goto error;
		}

		if (num_devs != 0) {
			SPDK_ERRLOG("Subsystem %s: Namespaces not allowed for Direct mode\n", name);
			goto error;
		}

		trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
		ctx.app_subsystem = app_subsys;
		ctx.found = false;
		if (strcmp(bdf, "*") == 0) {
			ctx.any = true;
		} else {
			if (spdk_pci_addr_parse(&pci_addr, bdf) < 0) {
				SPDK_ERRLOG("Invalid format for NVMe BDF: %s\n", bdf);
				goto error;
			}
			ctx.any = false;
			spdk_pci_addr_fmt(trid.traddr, sizeof(trid.traddr), &pci_addr);
			ctx.trid = trid;
		}

		if (spdk_nvme_probe(&trid, &ctx, probe_cb, attach_cb, NULL)) {
			SPDK_ERRLOG("One or more controllers failed in spdk_nvme_probe()\n");
		}

		if (!ctx.found) {
			SPDK_ERRLOG("Could not find NVMe controller at PCI address %04x:%02x:%02x.%x\n",
				    pci_addr.domain, pci_addr.bus, pci_addr.dev, pci_addr.func);
			goto error;
		}
#else
		SPDK_ERRLOG("DIRECT controllers not supported\n");
#endif
	} else {
		struct spdk_bdev *bdev;
		const char *namespace;

		if (sn == NULL) {
			SPDK_ERRLOG("Subsystem %s: missing serial number\n", name);
			goto error;
		}
		if (nvmf_fc_validate_sn(sn) != 0) {
			goto error;
		}

		if (num_devs > MAX_VIRTUAL_NAMESPACE) {
			goto error;
		}

		subsystem->dev.virt.ns_count = 0;
		snprintf(subsystem->dev.virt.sn, MAX_SN_LEN, "%s", sn);

		for (i = 0; i < num_devs; i++) {
			namespace = dev_list[i];
			if (!namespace) {
				SPDK_ERRLOG("Namespace %d: missing block device\n", i);
				goto error;
			}
			bdev = spdk_bdev_get_by_name(namespace);
			if (bdev == NULL) {
				SPDK_ERRLOG("Could not find namespace bdev '%s'\n", namespace);
				goto error;
			}
			if (spdk_nvmf_subsystem_add_ns(subsystem, bdev, 0) <= 0) {
				goto error;
			}

			SPDK_NOTICELOG("Attaching block device %s to subsystem %s\n",
				       bdev->name, subsystem->subnqn);

		}
	}

	spdk_nvmf_bcm_fc_tgt_start_subsystem(app_subsys);

	return 0;

error:
	spdk_nvmf_delete_subsystem(app_subsys->subsystem);
	app_subsys->subsystem = NULL;
	return -1;
}
