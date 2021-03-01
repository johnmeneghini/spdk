/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NetApp, Inc. All rights reserved.
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

#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/nvme_ocssd.h"
#include "spdk/nvme_zns.h"
#include "spdk/env.h"
#include "spdk/nvme_intel.h"
#include "spdk/nvme_kv.h"
#include "spdk/nvmf_spec.h"
#include "spdk/pci_ids.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/uuid.h"

enum kv_cmd_type {
	KV_CMD_TYPE_STORE,
	KV_CMD_TYPE_RETRIEVE,
	KV_CMD_TYPE_EXIST,
	KV_CMD_TYPE_LIST,
	KV_CMD_TYPE_DELETE,
};

enum kv_cmd_type g_kv_cmd_type = 0;

static int outstanding_commands;

static int g_shm_id = -1;

static int g_dpdk_mem = 0;

static bool g_dpdk_mem_single_seg = false;

static int g_main_core = 0;

static char g_core_mask[16] = "0x1";

static struct spdk_nvme_transport_id g_trid;
static char g_hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];

static int g_controllers_found = 0;

static bool g_vmd = false;

static bool g_print_completion = false;

static struct spdk_nvme_kv_key_t g_key;

struct kv_op {
	enum kv_cmd_type cmd_type;
	struct spdk_nvme_kv_key_t key;
	void *buffer;
	size_t buffer_len;
};

static struct kv_op g_op;

static void
usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\n");
	printf("options:\n");
	printf(" -r trid    remote NVMe over Fabrics target address\n");
	printf("    Format: 'key:value [key:value] ...'\n");
	printf("    Keys:\n");
	printf("     trtype      Transport type (e.g. RDMA)\n");
	printf("     adrfam      Address family (e.g. IPv4, IPv6)\n");
	printf("     traddr      Transport address (e.g. 192.168.100.8)\n");
	printf("     trsvcid     Transport service identifier (e.g. 4420)\n");
	printf("     subnqn      Subsystem NQN (default: %s)\n", SPDK_NVMF_DISCOVERY_NQN);
	printf("     hostnqn     Host NQN\n");
	printf("    Example: -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420'\n");

	spdk_log_usage(stdout, "-L");

	printf(" -d         DPDK huge memory size in MB\n");
	printf(" -c         KV command (store, retrieve, exist, delete, list)\n");
	printf(" -C         print completion\n");
	printf(" -k         key (16-byte string)\n");
	printf(" -H         show this usage\n");
}

static int
parse_args(int argc, char **argv)
{
	int op;
	char *hostnqn;
	int rc;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "d:r:c:Ck:L:")) != -1) {
		switch (op) {
		case 'd':
			g_dpdk_mem = spdk_strtol(optarg, 10);
			if (g_dpdk_mem < 0) {
				fprintf(stderr, "Invalid DPDK memory size\n");
				return g_dpdk_mem;
			}
			break;
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport address\n");
				return 1;
			}

			hostnqn = strcasestr(optarg, "hostnqn:");
			if (hostnqn) {
				size_t len;

				hostnqn += strlen("hostnqn:");

				len = strcspn(hostnqn, " \t\n");
				if (len > (sizeof(g_hostnqn) - 1)) {
					fprintf(stderr, "Host NQN is too long\n");
					return 1;
				}

				memcpy(g_hostnqn, hostnqn, len);
				g_hostnqn[len] = '\0';
			}
			break;
		case 'c':
			if (strncmp(optarg, "store", strlen("store")) == 0) {
				g_kv_cmd_type = KV_CMD_TYPE_STORE;
			} else if (strncmp(optarg, "retrieve", strlen("retrieve")) == 0) {
				g_kv_cmd_type = KV_CMD_TYPE_RETRIEVE;
			} else if (strncmp(optarg, "exist", strlen("exist")) == 0) {
				g_kv_cmd_type = KV_CMD_TYPE_EXIST;
			} else if (strncmp(optarg, "list", strlen("list")) == 0) {
				g_kv_cmd_type = KV_CMD_TYPE_LIST;
			} else if (strncmp(optarg, "delete", strlen("delete")) == 0) {
				g_kv_cmd_type = KV_CMD_TYPE_DELETE;
			} else {
				fprintf(stderr, "Invalid command: %s\n", optarg);
				fprintf(stderr, "  Use: store, retrieve, exist, list, delete\n");
				return 1;
			}
			break;
		case 'C':
			g_print_completion = true;
			break;
		case 'k':
			spdk_kv_key_parse(optarg, &g_key);
			break;
		case 'L':
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				SPDK_ERRLOG("unknown flag\n");
				usage(argv[0]);
				return 1;
			}
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return 0;
}

static void
process_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct kv_op *op = (struct kv_op *)cb_arg;
	struct spdk_nvme_kv_ns_list_data *list_data;
	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("IO failed (sct=%d, sc=%d)\n", cpl->status.sct, cpl->status.sc);
	}

	if (g_print_completion) {
		printf("\nCompletion:\n");
		printf("  CID:  %d\n", cpl->cid);
		printf("  SCT:  %d\n", cpl->status.sct);
		printf("  SC:   %d\n", cpl->status.sc);
		printf("  SQHD: %d\n", cpl->sqhd);
		printf("  SQID: %d\n", cpl->sqid);
		printf("  CDW0: %d\n", cpl->cdw0);
		printf("  CDW1: %d\n", cpl->cdw1);
	}

	if (!spdk_nvme_cpl_is_error(cpl)) {
		switch (op->cmd_type) {
		case KV_CMD_TYPE_STORE:
			break;
		case KV_CMD_TYPE_RETRIEVE:
			break;
		case KV_CMD_TYPE_EXIST:
			break;
		case KV_CMD_TYPE_DELETE:
			break;
		case KV_CMD_TYPE_LIST:
			list_data = (struct spdk_nvme_kv_ns_list_data *)op->buffer;
			struct spdk_nvme_kv_key_t *key = &list_data->keys[0];
			for (uint16_t i = 0; i < list_data->nrk; i++) {
				assert(key->kl > 0 && key->kl <= KV_MAX_KEY_SIZE);
				char key_str[KV_KEY_STRING_LEN];
				spdk_kv_key_fmt_lower(key_str, sizeof(key_str), key->kl, key->key);
				printf("%04u: %s\n", i, key_str);
				uint32_t key_len = ((sizeof(key->kl) + key->kl + 3) / 4) * 4;
				key = (struct spdk_nvme_kv_key_t *)(((uint8_t *)key) + key_len);
			}
			break;
		default:
			fprintf(stderr, "Unsupported command\n");
		}
	}
	outstanding_commands--;
}

static void
run_kv_cmd(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, enum kv_cmd_type cmd_type,
	   struct spdk_nvme_kv_key_t key)
{
	/* Get qpair */
	struct spdk_nvme_qpair *qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (qpair == NULL) {
		printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		exit(1);
	}

	/* Get given ns or first active ns */
	uint32_t tmp_nsid = 0;
	bool found_ns = false;
	for (tmp_nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	     tmp_nsid != 0; tmp_nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, tmp_nsid)) {
		if (nsid == 0 || tmp_nsid == nsid) {
			found_ns = true;
			break;
		}
	}
	if (!found_ns) {
		if (nsid == 0) {
			printf("ERROR: No active namespaces found\n");
		} else {
			printf("ERROR: Namespace %d not found\n", nsid);
		}
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		exit(1);
	}

	struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, tmp_nsid);
	if (ns == NULL) {
		printf("ERROR: spdk_nvme_ctrlr_get_ns() failed\n");
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		exit(1);
	}

	if (spdk_nvme_ns_get_csi(ns) != SPDK_NVME_CSI_KV) {
		printf("ERROR: Not a KV namespace\n");
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		exit(1);
	}

	int buffer_size = 4096;
	char *buf = spdk_dma_zmalloc(buffer_size, 0x200, NULL);
	if (buf == NULL) {
		fprintf(stderr, "spdk_dma_zmalloc failed\n");
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		exit(1);
	}

	g_op.cmd_type = cmd_type;
	g_op.key = key;
	g_op.buffer = buf;
	g_op.buffer_len = buffer_size;

	switch (cmd_type) {
	case KV_CMD_TYPE_STORE:
		if (fgets(buf, sizeof buf, stdin) == NULL) {
			fprintf(stderr, "Unable to read from stdin\n");
			spdk_dma_free(buf);
			spdk_nvme_ctrlr_free_io_qpair(qpair);
			exit(1);
		}
		printf("Store buffer: %s\n", buf);
		++outstanding_commands;
		spdk_nvme_kv_cmd_store(ns, qpair, &key, buf, strlen(buf), 0, process_completion, &g_op, 0);
		break;
	case KV_CMD_TYPE_RETRIEVE:
		++outstanding_commands;
		spdk_nvme_kv_cmd_retrieve(ns, qpair, &key, buf, buffer_size, 0, process_completion, &g_op, 0);
		break;
	case KV_CMD_TYPE_EXIST:
		++outstanding_commands;
		spdk_nvme_kv_cmd_exist(ns, qpair, &key, process_completion, &g_op);
		break;
	case KV_CMD_TYPE_DELETE:
		++outstanding_commands;
		spdk_nvme_kv_cmd_delete(ns, qpair, &key, process_completion, &g_op);
		break;
	case KV_CMD_TYPE_LIST:
		++outstanding_commands;
		spdk_nvme_kv_cmd_list(ns, qpair, &key, buf, buffer_size, process_completion, &g_op);
		break;
	default:
		fprintf(stderr, "Unsupported command\n");
	}

	while (outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 0);
	}

	if (cmd_type == KV_CMD_TYPE_RETRIEVE) {
		printf("Retrieve buffer: %s\n", buf);
	}

	spdk_dma_free(buf);
	spdk_nvme_ctrlr_free_io_qpair(qpair);
}


int main(int argc, char **argv)
{
	int				rc;
	struct spdk_env_opts		opts;
	struct spdk_nvme_ctrlr		*ctrlr;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "kv_cmd";
	opts.shm_id = g_shm_id;
	opts.mem_size = g_dpdk_mem;
	opts.mem_channel = 1;
	opts.main_core = g_main_core;
	opts.core_mask = g_core_mask;
	opts.hugepage_single_segments = g_dpdk_mem_single_seg;
	if (g_trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		opts.no_pci = true;
	}
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	if (g_vmd && spdk_vmd_init()) {
		fprintf(stderr, "Failed to initialize VMD."
			" Some NVMe devices can be unavailable.\n");
	}

	if (g_key.kl == 0) {
		fprintf(stderr, "key (-k) required\n");
		return 1;
	}

	/* A specific trid is required. */
	if (strlen(g_trid.traddr) != 0) {
		struct spdk_nvme_ctrlr_opts opts;

		spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, sizeof(opts));
		memcpy(opts.hostnqn, g_hostnqn, sizeof(opts.hostnqn));
		ctrlr = spdk_nvme_connect(&g_trid, &opts, sizeof(opts));
		if (!ctrlr) {
			fprintf(stderr, "spdk_nvme_connect() failed\n");
			return 1;
		}

		g_controllers_found++;
		run_kv_cmd(ctrlr, 0, g_kv_cmd_type, g_key);
		spdk_nvme_detach(ctrlr);
	} else {
		fprintf(stderr, "traddr (-r) required\n");
		return 1;
	}

	if (g_controllers_found == 0) {
		fprintf(stderr, "No NVMe controllers found.\n");
	}

	if (g_vmd) {
		spdk_vmd_fini();
	}

	return 0;
}
