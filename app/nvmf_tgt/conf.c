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

#include "nvmf_tgt.h"

#include "spdk/conf.h"
#include "spdk/log.h"
#include "spdk/bdev.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/nvme_spec.h"

#define MAX_LISTEN_ADDRESSES 255
#define PORTNUMSTRLEN 32
#define SPDK_NVMF_DEFAULT_SIN_PORT ((uint16_t)4420)

#define ACCEPT_TIMEOUT_US		10000 /* 10ms */

struct spdk_nvmf_probe_ctx {
	struct nvmf_tgt_subsystem	*app_subsystem;
	bool				any;
	bool				found;
	struct spdk_nvme_transport_id	trid;
};

#define MAX_STRING_LEN 255

#define SPDK_NVMF_CONFIG_AQ_DEPTH_DEFAULT 32
#define SPDK_NVMF_CONFIG_AQ_DEPTH_MIN 32
#define SPDK_NVMF_CONFIG_AQ_DEPTH_MAX 1024

#define SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_DEFAULT 4
#define SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_MIN 2
#define SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_MAX 1024

#define SPDK_NVMF_CONFIG_NVME_VERSION_DEFAULT 0x010201
#define SPDK_NVMF_CONFIG_IO_QUEUE_DEPTH_DEFAULT 128
#define SPDK_NVMF_CONFIG_IO_QUEUE_DEPTH_MIN 16
#define SPDK_NVMF_CONFIG_IO_QUEUE_DEPTH_MAX 1024

#define SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_DEFAULT 4096
#define SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_MIN 4096
#define SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_MAX 131072

#define SPDK_NVMF_CONFIG_MAX_IO_SIZE_DEFAULT 131072
#define SPDK_NVMF_CONFIG_MAX_IO_SIZE_MIN 4096
#define SPDK_NVMF_CONFIG_MAX_IO_SIZE_MAX 131072

#define SPDK_NVMF_CONFIG_ARBITRATION_BURST_DEFAULT 6
#define SPDK_NVMF_CONFIG_ERR_LOG_PAGE_ENTRIES_DEFAULT 127
#define SPDK_NVMF_CONFIG_KEEP_ALIVE_INTERVAL_DEFAULT 10
#define SPDK_NVMF_CONFIG_VOLATILE_WRITE_CACHE_DEFAULT 1
#define SPDK_NVMF_CONFIG_SGL_SUPPORT_DEFAULT 5
#define SPDK_NVMF_CONFIG_OPT_NVM_CMD_SUPPORT_DEFAULT 4
#define SPDK_NVMF_CONFIG_MODEL_NUMBER_DEFAULT "SPDK Virtual Controller"

#define SPDK_NVMF_CONFIG_ALLOW_ANY_HOST_DEFAULT false
#define SPDK_NVMF_CONFIG_ALLOW_ANY_LISTENER_DEFAULT false

struct spdk_nvmf_tgt_conf g_spdk_nvmf_tgt_conf;
static int32_t g_last_core = -1;
static struct spdk_nvmf_tgt_opts tgt_opts;

static int
spdk_get_numa_node_value(const char *path)
{
	FILE *fd;
	int numa_node = -1;
	char buf[MAX_STRING_LEN];

	fd = fopen(path, "r");
	if (!fd) {
		return -1;
	}

	if (fgets(buf, sizeof(buf), fd) != NULL) {
		numa_node = strtoul(buf, NULL, 10);
	}
	fclose(fd);

	return numa_node;
}

static int
spdk_get_ifaddr_numa_node(const char *if_addr)
{
	int ret;
	struct ifaddrs *ifaddrs, *ifa;
	struct sockaddr_in addr, addr_in;
	char path[MAX_STRING_LEN];
	int numa_node = -1;

	addr_in.sin_addr.s_addr = inet_addr(if_addr);

	ret = getifaddrs(&ifaddrs);
	if (ret < 0)
		return -1;

	for (ifa = ifaddrs; ifa != NULL; ifa = ifa->ifa_next) {
		addr = *(struct sockaddr_in *)ifa->ifa_addr;
		if ((uint32_t)addr_in.sin_addr.s_addr != (uint32_t)addr.sin_addr.s_addr) {
			continue;
		}
		snprintf(path, MAX_STRING_LEN, "/sys/class/net/%s/device/numa_node", ifa->ifa_name);
		numa_node = spdk_get_numa_node_value(path);
		break;
	}
	freeifaddrs(ifaddrs);

	return numa_node;
}

static int
spdk_add_nvmf_discovery_subsystem(void)
{
	struct nvmf_tgt_subsystem *app_subsys;

	app_subsys = nvmf_tgt_create_subsystem(SPDK_NVMF_DISCOVERY_NQN, SPDK_NVMF_SUBTYPE_DISCOVERY,
					       NVMF_SUBSYSTEM_MODE_DIRECT,
					       g_spdk_nvmf_tgt_conf.acceptor_lcore);
	if (app_subsys == NULL) {
		SPDK_ERRLOG("Failed creating discovery nvmf library subsystem\n");
		return -1;
	}

	spdk_nvmf_subsystem_set_allow_any_host(app_subsys->subsystem, true);
	spdk_nvmf_subsystem_set_allow_any_listener(app_subsys->subsystem, true);
	nvmf_tgt_start_subsystem(app_subsys);

	return 0;
}

static int
spdk_nvmf_parse_nvmf_tgt(void)
{
	struct spdk_conf_section *sp;
	int intval;
	int rc;
	char *model_number;

	sp = spdk_conf_find_section(NULL, "Nvmf");
	if (sp == NULL) {
		SPDK_ERRLOG("No Nvmf section in configuration file.\n");
		return -1;
	}

	intval = spdk_conf_section_get_intval(sp, "NVMeVersion");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_NVME_VERSION_DEFAULT;
	}
	tgt_opts.nvmever = intval;

	intval = spdk_conf_section_get_intval(sp, "MaxIOQueueDepth");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_IO_QUEUE_DEPTH_DEFAULT;
	}
	tgt_opts.max_io_queue_depth = spdk_max(intval, SPDK_NVMF_CONFIG_IO_QUEUE_DEPTH_MIN);
	tgt_opts.max_io_queue_depth = spdk_min(intval, SPDK_NVMF_CONFIG_IO_QUEUE_DEPTH_MAX);

	intval = spdk_conf_section_get_intval(sp, "MaxAqDepth");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_AQ_DEPTH_DEFAULT;
	}
	tgt_opts.max_aq_depth = spdk_max(intval, SPDK_NVMF_CONFIG_AQ_DEPTH_MIN);
	tgt_opts.max_aq_depth = spdk_min(intval, SPDK_NVMF_CONFIG_AQ_DEPTH_MAX);

	intval = spdk_conf_section_get_intval(sp, "MaxQueuesPerSession");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_DEFAULT;
	}
	tgt_opts.max_queues_per_session = spdk_max(intval, SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_MIN);
	tgt_opts.max_queues_per_session = spdk_min(intval, SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_MAX);

	intval = spdk_conf_section_get_intval(sp, "InCapsuleDataSize");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_DEFAULT;
	} else if ((intval % 16) != 0) {
		SPDK_ERRLOG("InCapsuleDataSize must be a multiple of 16\n");
		return -1;
	}
	tgt_opts.in_capsule_data_size = spdk_max(intval, SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_MIN);
	tgt_opts.in_capsule_data_size = spdk_min(intval, SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_MAX);

	intval = spdk_conf_section_get_intval(sp, "MaxIOSize");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_MAX_IO_SIZE_DEFAULT;
	} else if ((intval % 4096) != 0) {
		SPDK_ERRLOG("MaxIOSize must be a multiple of 4096\n");
		return -1;
	}
	tgt_opts.max_io_size = spdk_max(intval, SPDK_NVMF_CONFIG_MAX_IO_SIZE_MIN);
	tgt_opts.max_io_size = spdk_min(intval, SPDK_NVMF_CONFIG_MAX_IO_SIZE_MAX);

	intval = spdk_conf_section_get_intval(sp, "ArbitrationBurst");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_ARBITRATION_BURST_DEFAULT;
	}
	tgt_opts.rab = intval;

	intval = spdk_conf_section_get_intval(sp, "IEEEOUI0");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.ieee[0] = intval;

	intval = spdk_conf_section_get_intval(sp, "IEEEOUI1");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.ieee[1] = intval;

	intval = spdk_conf_section_get_intval(sp, "IEEEOUI2");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.ieee[2] = intval;


	intval = spdk_conf_section_get_intval(sp, "CMIC");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.cmic = intval;

	intval = spdk_conf_section_get_intval(sp, "NMIC");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.nmic = intval;

	intval = spdk_conf_section_get_intval(sp, "OptAerSupport");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.oaes = intval;

	intval = spdk_conf_section_get_intval(sp, "AbortLimit");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.acl = intval;

	intval = spdk_conf_section_get_intval(sp, "AerLimit");
	if (intval < 1) {
		intval = 1;
	}
	tgt_opts.aerl = intval;

	intval = spdk_conf_section_get_intval(sp, "AsyncEventConfig");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.async_event_config = intval;

	intval = spdk_conf_section_get_intval(sp, "ErrLogEntries");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_ERR_LOG_PAGE_ENTRIES_DEFAULT;
	}
	tgt_opts.elpe = intval;

	intval = spdk_conf_section_get_intval(sp, "NumPowerStates");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.npss = intval;

	intval = spdk_conf_section_get_intval(sp, "KeepAliveInt");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_KEEP_ALIVE_INTERVAL_DEFAULT;
	}
	tgt_opts.kas = intval;

	intval = spdk_conf_section_get_intval(sp, "VolatileWriteCache");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_VOLATILE_WRITE_CACHE_DEFAULT;
	}
	tgt_opts.vwc = intval;

	intval = spdk_conf_section_get_intval(sp, "AtomicWriteNormal");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.awun = intval;

	intval = spdk_conf_section_get_intval(sp, "AtomicWritePowerFail");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.awupf = intval;

	intval = spdk_conf_section_get_intval(sp, "SGLSupport");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_SGL_SUPPORT_DEFAULT;
	}
	tgt_opts.sgls = intval;

	intval = spdk_conf_section_get_intval(sp, "OptNVMCommandSupport");
	if (intval < 0) {
		intval = SPDK_NVMF_CONFIG_OPT_NVM_CMD_SUPPORT_DEFAULT;
	}
	tgt_opts.oncs = intval;

	model_number = spdk_conf_section_get_val(sp, "ModelNumber");
	if (model_number == NULL) {
		model_number = SPDK_NVMF_CONFIG_MODEL_NUMBER_DEFAULT;
	}
	strncpy(tgt_opts.mn, model_number, sizeof(tgt_opts.mn));

	tgt_opts.allow_any_listener = spdk_conf_section_get_boolval(sp, "AllowAnyListener",
				      SPDK_NVMF_CONFIG_ALLOW_ANY_LISTENER_DEFAULT);

	tgt_opts.allow_any_host = spdk_conf_section_get_boolval(sp, "AllowAnyHost",
				  SPDK_NVMF_CONFIG_ALLOW_ANY_HOST_DEFAULT);

	intval = spdk_conf_section_get_intval(sp, "AcceptorCore");
	if (intval < 0) {
		intval = spdk_env_get_current_core();
	}
	g_spdk_nvmf_tgt_conf.acceptor_lcore = intval;

	intval = spdk_conf_section_get_intval(sp, "AcceptorPollRate");
	if (intval < 0) {
		intval = ACCEPT_TIMEOUT_US;
	}
	g_spdk_nvmf_tgt_conf.acceptor_poll_rate = intval;

	intval = spdk_conf_section_get_intval(sp, "ANATransitionTime");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.anatt = intval;

	intval = spdk_conf_section_get_intval(sp, "ANACapability");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.anacap = intval;

	intval = spdk_conf_section_get_intval(sp, "ANAGroupIDMax");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.anagrpmax = intval;

	intval = spdk_conf_section_get_intval(sp, "NumANAGroupID");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.nanagrpid = intval;

	intval = spdk_conf_section_get_intval(sp, "MaxNumAllocedNS");
	if (intval < 0) {
		intval = 0;
	}
	tgt_opts.mnan = intval;

	rc = spdk_nvmf_tgt_opts_init(&tgt_opts);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_nvmf_tgt_opts_init() failed\n");
		return rc;
	}

	rc = spdk_add_nvmf_discovery_subsystem();
	if (rc != 0) {
		SPDK_ERRLOG("spdk_add_nvmf_discovery_subsystem failed\n");
		return rc;
	}

	return 0;
}

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
	if (ctx->trid.traddr[0] != '\0' && strcmp(trid->traddr, ctx->trid.traddr)) {
		SPDK_WARNLOG("Attached device is not expected\n");
		return;
	}
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
	ctx->found = true;
}

static int
spdk_nvmf_allocate_lcore(uint64_t mask, uint32_t lcore)
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
spdk_nvmf_parse_subsystem(struct spdk_conf_section *sp)
{
	const char *nqn, *mode_str, *queue_str, *ana_group_str;
	int i, ret;
	int lcore;
	int num_listen_addrs;
	struct rpc_listen_address listen_addrs[MAX_LISTEN_ADDRESSES];
	char *listen_addrs_str[MAX_LISTEN_ADDRESSES] = {};
	bool allow_any_listener;
	int num_hosts;
	struct spdk_host_conf hosts[MAX_HOSTS] = {};
	bool allow_any_host;
	const char *bdf;
	const char *sn;
	int num_devs;
	char *devs[MAX_VIRTUAL_NAMESPACE];
	char *devs_nidt[MAX_VIRTUAL_NAMESPACE];
	char *devs_nid[MAX_VIRTUAL_NAMESPACE];
	uint32_t anagrpids[MAX_VIRTUAL_NAMESPACE];

	nqn = spdk_conf_section_get_val(sp, "NQN");
	mode_str = spdk_conf_section_get_val(sp, "Mode");
	lcore = spdk_conf_section_get_intval(sp, "Core");

	/* Parse Listen sections */
	num_listen_addrs = 0;
	for (i = 0; i < MAX_LISTEN_ADDRESSES; i++) {
		listen_addrs[num_listen_addrs].adrfam = NULL;
		listen_addrs[num_listen_addrs].transport =
			spdk_conf_section_get_nmval(sp, "Listen", i, 0);
		if (!listen_addrs[num_listen_addrs].transport) {
			break;
		}

		listen_addrs_str[i] = spdk_conf_section_get_nmval(sp, "Listen", i, 1);
		if (!listen_addrs_str[i]) {
			break;
		}

		listen_addrs_str[i] = strdup(listen_addrs_str[i]);

		ret = spdk_parse_ip_addr(listen_addrs_str[i], &listen_addrs[num_listen_addrs].traddr,
					 &listen_addrs[num_listen_addrs].trsvcid);
		if (ret < 0) {
			SPDK_ERRLOG("Unable to parse listen address '%s'\n", listen_addrs_str[i]);
			free(listen_addrs_str[i]);
			listen_addrs_str[i] = NULL;
			continue;
		}

		num_listen_addrs++;
	}
	allow_any_listener = spdk_conf_section_get_boolval(sp, "AllowAnyListener",
			     tgt_opts.allow_any_listener);

	/* Parse Host sections */
	for (i = 0; i < MAX_HOSTS; i++) {

		if (!(hosts[i].hostnqn = spdk_conf_section_get_nmval(sp, "Host", i, 0))) {
			break;
		}

		if ((queue_str = spdk_conf_section_get_nmval(sp, "Host", i, 1))) {
			hosts[i].max_io_queue_depth = (uint16_t) atoi(queue_str);
		} else {
			SPDK_NOTICELOG("max_io_queue_depth not defined for Host %s\n", hosts[i].hostnqn);
			hosts[i].max_io_queue_depth = tgt_opts.max_io_queue_depth;
			hosts[i].max_io_queue_num = tgt_opts.max_queues_per_session - 1;
			continue;
		}

		if ((queue_str = spdk_conf_section_get_nmval(sp, "Host", i, 2))) {
			hosts[i].max_io_queue_num = (uint16_t) atoi(queue_str) - 1;
		} else {
			SPDK_NOTICELOG("max_io_queue_num not defined for Host %s\n", hosts[i].hostnqn);
			hosts[i].max_io_queue_num = tgt_opts.max_queues_per_session - 1;
		}
	}
	num_hosts = i;

	allow_any_host = spdk_conf_section_get_boolval(sp, "AllowAnyHost", tgt_opts.allow_any_host);

	bdf = spdk_conf_section_get_val(sp, "NVMe");

	sn = spdk_conf_section_get_val(sp, "SN");

	num_devs = 0;
	for (i = 0; i < MAX_VIRTUAL_NAMESPACE; i++) {
		devs[i] = spdk_conf_section_get_nmval(sp, "Namespace", i, 0);
		if (!devs[i]) {
			break;
		}
		devs_nidt[i] = spdk_conf_section_get_nmval(sp, "Namespace", i, 1);
		devs_nid[i] = spdk_conf_section_get_nmval(sp, "Namespace", i, 2);

		if ((ana_group_str = spdk_conf_section_get_nmval(sp, "Namespace", i, 3))) {
			anagrpids[i] = (uint32_t) atoi(ana_group_str);
		} else {
			SPDK_NOTICELOG("ANA group id not defined for namespace %s\n", devs[i]);
			anagrpids[i] = 0;
		}

		num_devs++;
	}

	ret = spdk_nvmf_construct_subsystem(nqn, mode_str, lcore,
					    num_listen_addrs, listen_addrs,
					    allow_any_listener,
					    num_hosts, hosts, allow_any_host,
					    bdf, sn,
					    num_devs, devs, devs_nidt, devs_nid,
					    anagrpids);

	for (i = 0; i < MAX_LISTEN_ADDRESSES; i++) {
		free(listen_addrs_str[i]);
	}

	return ret;
}

static int
spdk_nvmf_parse_subsystems(void)
{
	int rc = 0;
	struct spdk_conf_section *sp;

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "Subsystem")) {
			rc = spdk_nvmf_parse_subsystem(sp);
			if (rc < 0) {
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}
	return 0;
}

int
spdk_nvmf_parse_conf(void)
{
	int rc;

	/* NVMf section */
	rc = spdk_nvmf_parse_nvmf_tgt();
	if (rc < 0) {
		return rc;
	}

	/* Subsystem sections */
	rc = spdk_nvmf_parse_subsystems();
	if (rc < 0) {
		return rc;
	}

	return 0;
}

int
spdk_nvmf_construct_subsystem(const char *name,
			      const char *mode_str, int32_t lcore,
			      int num_listen_addresses, struct rpc_listen_address *addresses,
			      bool allow_any_listener, int num_hosts, struct spdk_host_conf *hosts,
			      bool allow_any_host, const char *bdf, const char *sn,
			      int num_devs, char *dev_list[], char *dev_nidt[], char *dev_nid[], uint32_t *anagrpids)
{
	struct spdk_nvmf_subsystem *subsystem;
	struct nvmf_tgt_subsystem *app_subsys;
	struct spdk_nvmf_listen_addr *listen_addr;
	enum spdk_nvmf_subsystem_mode mode;
	int i;
	uint64_t mask;
	uint8_t nidt, nid[16];
	uint32_t anagrpid;

	if (name == NULL) {
		SPDK_ERRLOG("No NQN specified for subsystem\n");
		return -1;
	}

	if (num_listen_addresses > MAX_LISTEN_ADDRESSES) {
		SPDK_ERRLOG("invalid listen adresses number\n");
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
	lcore = spdk_nvmf_allocate_lcore(mask, lcore);
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

	app_subsys = nvmf_tgt_create_subsystem(name, SPDK_NVMF_SUBTYPE_NVME,
					       mode, lcore);
	if (app_subsys == NULL) {
		SPDK_ERRLOG("Subsystem creation failed\n");
		return -1;
	}
	subsystem = app_subsys->subsystem;

	spdk_nvmf_subsystem_set_allow_any_listener(subsystem, allow_any_listener);

	/* Parse Listen sections */
	for (i = 0; i < num_listen_addresses; i++) {
		int nic_numa_node = spdk_get_ifaddr_numa_node(addresses[i].traddr);
		unsigned subsys_numa_node = spdk_env_get_socket_id(app_subsys->lcore);
		const char *adrfam_str;
		enum spdk_nvmf_adrfam adrfam;

		if (nic_numa_node >= 0) {
			if (subsys_numa_node != (unsigned)nic_numa_node) {
				SPDK_WARNLOG("Subsystem %s is configured to run on a CPU core %d belonging "
					     "to a different NUMA node than the associated NIC. "
					     "This may result in reduced performance.\n",
					     name, lcore);
				SPDK_WARNLOG("The NIC is on socket %d\n", nic_numa_node);
				SPDK_WARNLOG("The Subsystem is on socket %u\n",
					     subsys_numa_node);
			}
		}

		if (addresses[i].transport == NULL) {
			SPDK_ERRLOG("Missing listen address transport type\n");
			goto error;
		}

		adrfam_str = addresses[i].adrfam;
		if (adrfam_str == NULL) {
			adrfam_str = "IPv4";
		}

		if (spdk_nvme_transport_id_parse_adrfam(&adrfam, adrfam_str)) {
			SPDK_ERRLOG("Unknown address family '%s'\n", adrfam_str);
			goto error;
		}

		listen_addr = spdk_nvmf_tgt_listen(addresses[i].transport, adrfam,
						   addresses[i].traddr, addresses[i].trsvcid);
		if (listen_addr == NULL) {
			SPDK_ERRLOG("Failed to listen on transport %s, adrfam %s, traddr %s, trsvcid %s\n",
				    addresses[i].transport,
				    adrfam_str,
				    addresses[i].traddr,
				    addresses[i].trsvcid);
			goto error;
		}
		spdk_nvmf_subsystem_add_listener(subsystem, listen_addr);
	}

	/* Parse Host sections */
	for (i = 0; i < num_hosts; i++) {
		spdk_nvmf_subsystem_add_host(subsystem, hosts[i].hostnqn, hosts[i].max_io_queue_depth,
					     hosts[i].max_io_queue_num);
	}
	spdk_nvmf_subsystem_set_allow_any_host(subsystem, allow_any_host);

	if (mode == NVMF_SUBSYSTEM_MODE_DIRECT) {
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
	} else {
		struct spdk_bdev *bdev;
		const char *namespace;

		if (sn == NULL) {
			SPDK_ERRLOG("Subsystem %s: missing serial number\n", name);
			goto error;
		}

		if (spdk_nvmf_subsystem_set_sn(subsystem, sn)) {
			SPDK_ERRLOG("Subsystem %s: invalid serial number '%s'\n", name, sn);
			goto error;
		}

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
			if (dev_nidt && dev_nid) {
				nidt = 0;
				memset(nid, 0, 16);
				if (dev_nidt[i] && (strcasecmp(dev_nidt[i], "EUI64") == 0)) {
					nidt = SPDK_NVME_NIDT_EUI64;
					if (dev_nid[i] && (strlen(dev_nid[i]) == 16)) {
						sscanf(dev_nid[i], "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
						       &nid[0], &nid[1], &nid[2], &nid[3],
						       &nid[4], &nid[5], &nid[6], &nid[7]);
					}
				} else if (dev_nidt[i] && (strcasecmp(dev_nidt[i], "NGUID") == 0)) {
					nidt = SPDK_NVME_NIDT_NGUID;
					if (dev_nid[i] && (strlen(dev_nid[i]) == 32)) {
						sscanf(dev_nid[i], "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
						       &nid[0], &nid[1], &nid[2], &nid[3],
						       &nid[4], &nid[5], &nid[6], &nid[7]);
						sscanf(dev_nid[i] + 16, "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
						       &nid[8], &nid[9], &nid[10], &nid[11],
						       &nid[12], &nid[13], &nid[14], &nid[15]);
					}
				} else if (dev_nidt[i] && (strcasecmp(dev_nidt[i], "UUID") == 0)) {
					nidt = SPDK_NVME_NIDT_UUID;
					if (dev_nid[i] && (strlen(dev_nid[i]) == 32)) {
						sscanf(dev_nid[i], "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
						       &nid[0], &nid[1], &nid[2], &nid[3],
						       &nid[4], &nid[5], &nid[6], &nid[7]);
						sscanf(dev_nid[i] + 16, "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
						       &nid[8], &nid[9], &nid[10], &nid[11],
						       &nid[12], &nid[13], &nid[14], &nid[15]);
					}
				}
				bdev->id_desc = spdk_nvmf_get_ns_id_desc(nidt, nid);
			}

			anagrpid = anagrpids ? anagrpids[i] : 0;

			if (spdk_nvmf_subsystem_add_ns(subsystem, bdev, 0, anagrpid) == 0) {
				goto error;
			}

			SPDK_NOTICELOG("Attaching block device %s in ana group %u to subsystem %s\n",
				       bdev->name, anagrpid, subsystem->subnqn);

		}
	}

	nvmf_tgt_start_subsystem(app_subsys);

	return 0;

error:
	spdk_nvmf_delete_subsystem(app_subsys->subsystem);
	app_subsys->subsystem = NULL;
	return -1;
}
