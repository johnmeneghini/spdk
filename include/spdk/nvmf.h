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

/** \file
 * NVMe over Fabrics target public API
 */

#ifndef SPDK_NVMF_H
#define SPDK_NVMF_H

#include "spdk/env.h"
#include "spdk/nvmf_spec.h"
#include "spdk/queue.h"

#define SPDK_UUID_LEN 16
#define MAX_VIRTUAL_NAMESPACE 16
#define MAX_SN_LEN 20

struct spdk_nvmf_subsystem;
struct spdk_nvmf_session;
struct spdk_nvmf_conn;
struct spdk_nvmf_request;
struct spdk_bdev;
struct spdk_nvme_ctrlr;
struct spdk_nvmf_request;
struct spdk_nvmf_conn;
struct spdk_nvmf_tgt_opts;

int spdk_nvmf_tgt_opts_init(struct spdk_nvmf_tgt_opts *opts);

int spdk_nvmf_tgt_fini(void);

int spdk_nvmf_check_pools(void);

typedef void (*spdk_nvmf_subsystem_connect_fn)(void *cb_ctx, struct spdk_nvmf_request *req);
typedef void (*spdk_nvmf_subsystem_disconnect_fn)(void *cb_ctx, struct spdk_nvmf_conn *conn);

enum spdk_nvmf_subsystem_mode {
	NVMF_SUBSYSTEM_MODE_DIRECT	= 0,
	NVMF_SUBSYSTEM_MODE_VIRTUAL	= 1,
};

struct spdk_uuid {
	uint8_t bytes[SPDK_UUID_LEN];
};

struct spdk_nvmf_listen_addr {
	char					*traddr;
	char					*trsvcid;
	char					*trname;
	TAILQ_ENTRY(spdk_nvmf_listen_addr)	link;
};

struct spdk_nvmf_host {
	char				*nqn;
	TAILQ_ENTRY(spdk_nvmf_host)	link;
};

struct spdk_nvmf_ctrlr_ops {
	/**
	 * Initialize the controller.
	 */
	int (*attach)(struct spdk_nvmf_subsystem *subsystem);

	/**
	 * Get NVMe identify controller data.
	 */
	void (*ctrlr_get_data)(struct spdk_nvmf_session *session);

	/**
	 * Process admin command.
	 */
	int (*process_admin_cmd)(struct spdk_nvmf_request *req);

	/**
	 * Process IO command.
	 */
	int (*process_io_cmd)(struct spdk_nvmf_request *req);

	/**
	 * Cleanup IO command.
	 */
	void (*io_cleanup)(struct spdk_nvmf_request *req);

	/**
	 * Abort IO command.
	 */
	void (*io_abort)(struct spdk_nvmf_request *req);

	/**
	 * Poll for completions.
	 */
	void (*poll_for_completions)(struct spdk_nvmf_subsystem *subsystem);

	/**
	 * Detach the controller.
	 */
	void (*detach)(struct spdk_nvmf_subsystem *subsystem);
};

struct spdk_nvmf_subsystem_allowed_listener {
	union {
		struct spdk_nvmf_listen_addr *listen_addr;
#ifdef SPDK_CONFIG_BCM_FC
		struct spdk_nvmf_bcm_fc_nport *fc_nport;
#endif
	};
	TAILQ_ENTRY(spdk_nvmf_subsystem_allowed_listener)	link;
};

/*
 * The NVMf subsystem, as indicated in the specification, is a collection
 * of virtual controller sessions.  Any individual controller session has
 * access to all the NVMe device/namespaces maintained by the subsystem.
 */
struct spdk_nvmf_subsystem {
	uint32_t id;
	uint32_t lcore;
	struct spdk_uuid container_uuid; /* UUID of the container of the subsystem */
	char subnqn[SPDK_NVMF_NQN_MAX_LEN];
	enum spdk_nvmf_subsystem_mode mode;
	enum spdk_nvmf_subtype subtype;
	bool is_removed;
	union {
		struct {
			struct spdk_nvme_ctrlr	*ctrlr;
			struct spdk_nvme_qpair	*io_qpair;
			struct spdk_pci_addr	pci_addr;
			struct spdk_poller	*admin_poller;
			int32_t			outstanding_admin_cmd_count;
		} direct;

		struct {
			char	sn[MAX_SN_LEN + 1];
			struct spdk_bdev *ns_list[MAX_VIRTUAL_NAMESPACE];
			struct spdk_io_channel *ch[MAX_VIRTUAL_NAMESPACE];
			uint16_t max_nsid;
			struct spdk_pci_id sub_pci_id;
		} virt;
	} dev;

	const struct spdk_nvmf_ctrlr_ops *ops;

	void					*cb_ctx;
	spdk_nvmf_subsystem_connect_fn		connect_cb;
	spdk_nvmf_subsystem_disconnect_fn	disconnect_cb;

	TAILQ_HEAD(, spdk_nvmf_session)		sessions;

	TAILQ_HEAD(, spdk_nvmf_host)		hosts;
	uint32_t				num_hosts;

	TAILQ_HEAD(, spdk_nvmf_subsystem_allowed_listener)	allowed_listeners;

	TAILQ_ENTRY(spdk_nvmf_subsystem)	entries;
};

struct spdk_nvmf_tgt_opts {
	uint16_t                                max_associations;
	uint16_t				max_queue_depth;
	uint16_t                                max_aq_depth;
	uint16_t				max_queues_per_session;
	uint32_t				in_capsule_data_size;
	uint32_t				max_io_size;
	uint64_t                                lcore_mask;
	uint8_t                                 rab;
	uint8_t                                 ieee[3];
	uint8_t                                 cmic;
	uint32_t                                oaes;
	uint8_t                                 acl;
	uint8_t                                 aerl;
	uint8_t                                 elpe;
	uint8_t                                 npss;
	uint16_t                                kas;
	uint8_t                                 vwc;
	uint16_t                                awun;
	uint16_t                                awupf;
	uint32_t                                sgls;
};

struct spdk_nvmf_subsystem *spdk_nvmf_create_subsystem(const char *nqn,
		enum spdk_nvmf_subtype type,
		enum spdk_nvmf_subsystem_mode mode,
		void *cb_ctx,
		spdk_nvmf_subsystem_connect_fn connect_cb,
		spdk_nvmf_subsystem_disconnect_fn disconnect_cb);

/**
 * Initialize the subsystem on the thread that will be used to poll it.
 *
 * \param subsystem Subsystem that will be polled on this core.
 */
int spdk_nvmf_subsystem_start(struct spdk_nvmf_subsystem *subsystem);

void spdk_nvmf_delete_subsystem(struct spdk_nvmf_subsystem *subsystem);

struct spdk_nvmf_subsystem *
nvmf_find_subsystem(const char *subnqn);

bool spdk_nvmf_subsystem_exists(const char *subnqn);

bool spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn);

struct spdk_nvmf_listen_addr *
spdk_nvmf_tgt_listen(const char *trname, const char *traddr, const char *trsvcid);

int
spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				 struct spdk_nvmf_listen_addr *listen_addr);

bool
spdk_nvmf_subsystem_listener_allowed(struct spdk_nvmf_subsystem *subsystem,
				     struct spdk_nvmf_listen_addr *listen_addr);

int
spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem,
			     const char *host_nqn);

int
nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem,
			 struct spdk_nvme_ctrlr *ctrlr, const struct spdk_pci_addr *pci_addr);

void spdk_nvmf_subsystem_poll(struct spdk_nvmf_subsystem *subsystem);

/**
 * Add a namespace to a subsytem.
 *
 * \param subsystem Subsystem to add namespace to.
 * \param bdev Block device to add as a namespace.
 * \param nsid Namespace ID to assign to the new namespace, or 0 to automatically use an available
 *             NSID.
 *
 * \return Newly added NSID on success or 0 on failure.
 */
uint32_t spdk_nvmf_subsystem_add_ns(struct spdk_nvmf_subsystem *subsystem, struct spdk_bdev *bdev,
				    uint32_t nsid);

int spdk_nvmf_subsystem_set_pci_id(struct spdk_nvmf_subsystem *subsystem,
				   struct spdk_pci_id *sub_pci_id);
int spdk_nvmf_subsystem_set_sn(struct spdk_nvmf_subsystem *subsystem, const char *sn);

const char *spdk_nvmf_subsystem_get_nqn(struct spdk_nvmf_subsystem *subsystem);
enum spdk_nvmf_subtype spdk_nvmf_subsystem_get_type(struct spdk_nvmf_subsystem *subsystem);
enum spdk_nvmf_subsystem_mode spdk_nvmf_subsystem_get_mode(struct spdk_nvmf_subsystem *subsystem);

/* XXX: Throwaway with spdk17.10. Leverage spdk17.10 containers */
void spdk_nvmf_add_discovery_log_allowed_fn(void *fn);

void spdk_nvmf_acceptor_poll(void);

void spdk_nvmf_handle_connect(struct spdk_nvmf_request *req);

void
spdk_nvmf_session_disconnect(struct spdk_nvmf_conn *conn);

#endif
