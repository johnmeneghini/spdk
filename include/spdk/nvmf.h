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

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf_spec.h"
#include "spdk/queue.h"

#define SPDK_UUID_LEN 16
#define MAX_VIRTUAL_NAMESPACE 512
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

int spdk_nvmf_tgt_opts_update(struct spdk_nvmf_tgt_opts *opts);

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
	enum spdk_nvmf_adrfam			adrfam;
	TAILQ_ENTRY(spdk_nvmf_listen_addr)	link;
};

struct spdk_nvmf_host {
	char				*nqn;
	/*
	 * The below values are duplicates of the
	 * the values found in struct spdk_nvme_ctrlr_opts
	 */
	uint16_t			max_aq_depth;
	uint16_t			max_io_queue_depth;
	uint16_t			max_connections_allowed;
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
	 * Initialize IO command.
	 */
	int (*io_init)(struct spdk_nvmf_request *req);

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
	struct spdk_nvmf_listen_addr *listen_addr;
	TAILQ_ENTRY(spdk_nvmf_subsystem_allowed_listener)	link;
};

struct spdk_nvmf_ana_group {
	uint32_t    anagrpid;
	uint32_t    num_nsids;
	void        *app_ctxt;
	/*
	 * Intentionally not adding state here
	 * as state is associated with
	 * <ana_group, session> tuple
	 */
	TAILQ_ENTRY(spdk_nvmf_ana_group)    link;
};

/*
 * Function table for spdk_nvmf_subystem provides a set of APIs to
 * allow communication with the target application.
 */

struct spdk_nvmf_subsystem_app_cbs {
	void (*connect_cb)(void *cb_ctx, struct spdk_nvmf_request *req);
	void (*disconnect_cb)(void *cb_ctx, struct spdk_nvmf_conn *conn);
	int (*get_vs_log_page)(struct spdk_nvmf_request *req, uint64_t offset);
	void (*abort_vs_log_page_req)(void *arg1, void *arg2);
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
	char subnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
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
			struct spdk_bdev_desc *desc[MAX_VIRTUAL_NAMESPACE];
			uint32_t max_nsid;
			struct spdk_pci_id sub_pci_id;
		} virt;
	} dev;
	uint16_t next_cntlid;
	/*
	 * ana_groups should be present in the increasing order
	 * of ana group id
	 */
	TAILQ_HEAD(, spdk_nvmf_ana_group)       ana_groups;
	uint16_t                                num_ana_groups;

	const struct spdk_nvmf_ctrlr_ops *ops;

	struct spdk_nvmf_subsystem_app_cbs *app_cbs;
	void					*cb_ctx;

	TAILQ_HEAD(, spdk_nvmf_session)		sessions;

	TAILQ_HEAD(, spdk_nvmf_host)		hosts;
	bool					allow_any_host;

	/*
	 * Host 0 contains the default host/controller configuration.
	 * These values are used when the num_hosts == 0.
	 */
	struct spdk_nvmf_host                   host0;

	TAILQ_HEAD(, spdk_nvmf_subsystem_allowed_listener)	allowed_listeners;
	bool					allow_any_listener;

	TAILQ_ENTRY(spdk_nvmf_subsystem)	entries;
};

struct spdk_nvmf_tgt_opts {
	uint32_t				nvmever;
	uint16_t				max_io_queue_depth;
	uint16_t                                max_aq_depth;
	uint16_t				max_queues_per_session;
	uint32_t				in_capsule_data_size;
	uint32_t				max_io_size;
	uint64_t				lcore_mask;
	uint8_t					rab;
	uint8_t					ieee[SPDK_NVME_SPEC_IEEE_OUI_SIZE];
	uint8_t					cmic;
	uint8_t					nmic;
	uint8_t					acl;
	uint32_t				oaes;
	uint8_t					aerl;
	uint32_t                                async_event_config;
	uint8_t					elpe;
	uint8_t					npss;
	uint16_t				kas;
	uint8_t					vwc;
	uint16_t				awun;
	uint16_t				awupf;
	uint32_t				sgls;
	uint16_t				oncs;
	uint8_t                                 mn[SPDK_NVME_CTRLR_MN_LEN];
	bool                                    allow_any_host;
	bool                                    allow_any_listener;
	uint8_t					anatt;
	uint8_t					anacap;
	uint32_t				anagrpmax;
	uint32_t				nanagrpid;
	uint8_t					nwpc;
	uint32_t				mnan;
	uint8_t                                 tgt_instance_id;
	uint16_t                                fuses;
};

struct spdk_nvmf_subsystem *spdk_nvmf_create_subsystem(const char *nqn,
		enum spdk_nvmf_subtype type,
		enum spdk_nvmf_subsystem_mode mode,
		void *cb_ctx,
		struct spdk_nvmf_subsystem_app_cbs *app_cbs);

/**
 * Initialize the subsystem on the thread that will be used to poll it.
 *
 * \param subsystem Subsystem that will be polled on this core.
 */
int spdk_nvmf_subsystem_start(struct spdk_nvmf_subsystem *subsystem);

void spdk_nvmf_delete_subsystem(struct spdk_nvmf_subsystem *subsystem);

struct spdk_nvmf_subsystem *spdk_nvmf_find_subsystem(const char *subnqn);

/**
 * Set whether a subsystem should allow any host or only hosts in the allowed list.
 *
 * \param subsystem Subsystem to modify.
 * \param allow_any_host true to allow any host to connect to this subsystem, or false to enforce
 *                       the whitelist configured with spdk_nvmf_subsystem_add_host().
 */
void spdk_nvmf_subsystem_set_allow_any_host(struct spdk_nvmf_subsystem *subsystem,
		bool allow_any_host);

/**
 * Check whether a subsystem should allow any host or only hosts in the allowed list.
 *
 * \param subsystem Subsystem to modify.
 * \return true if any host is allowed to connect to this subsystem, or false if connecting hosts
 *         must be in the whitelist configured with spdk_nvmf_subsystem_add_host().
 */
bool spdk_nvmf_subsystem_get_allow_any_host(const struct spdk_nvmf_subsystem *subsystem);

/**
 * Check if the given host is allowed to connect to the subsystem.
 *
 * \param subsystem The subsystem to query
 * \param hostnqn The NQN of the host
 * \return true if allowed, false if not.
 */
bool spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn);

struct spdk_nvmf_listen_addr *spdk_nvmf_tgt_listen(const char *trname, enum spdk_nvmf_adrfam adrfam,
		const char *traddr, const char *trsvcid);

/**
 * Destroy and free up a listen addr
 *
 * \param addr The addr to free
 * \return true if listen addr found
 */
bool
spdk_nvmf_listen_addr_delete(struct spdk_nvmf_listen_addr *addr);

int
spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				 struct spdk_nvmf_listen_addr *listen_addr);

int
spdk_nvmf_subsystem_remove_listener(struct spdk_nvmf_subsystem *subsystem,
				    struct spdk_nvmf_listen_addr *listen_addr);

/**
 * Set whether a subsystem should allow any listener or only listeners in the allowed list.
 *
 * \param subsystem Subsystem to modify.
 * \param allow_any_listener true to allow any listener to connect to this subsystem, or false to enforce
 *                       the whitelist configured with spdk_nvmf_subsystem_add_listener().
 */
void
spdk_nvmf_subsystem_set_allow_any_listener(struct spdk_nvmf_subsystem *subsystem,
		bool allow_any_listener);

/**
 * Check whether a subsystem should allow any listener or only listeners in the allowed list.
 *
 * \param subsystem Subsystem to check.
 * \return true if any listener is allowed to connect to this subsystem, or false if connecting listeners
 *         must be in the whitelist configured with spdk_nvmf_subsystem_add_listener().
 */
bool
spdk_nvmf_subsystem_get_allow_any_listener(const struct spdk_nvmf_subsystem *subsystem);

bool
spdk_nvmf_subsystem_listener_allowed(struct spdk_nvmf_subsystem *subsystem,
				     struct spdk_nvmf_listen_addr *listen_addr);

int
spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem,
			     const char *host_nqn, uint16_t max_io_queue_depth, uint16_t max_io_connections_allowed);

int nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem,
			     struct spdk_nvme_ctrlr *ctrlr, const struct spdk_pci_addr *pci_addr);

void
spdk_nvmf_subsystem_remove_host(struct spdk_nvmf_subsystem *subsystem,
				const char *host_nqn);

void spdk_nvmf_subsystem_poll(struct spdk_nvmf_subsystem *subsystem);

struct spdk_nvme_ns_id_desc *spdk_nvmf_get_ns_id_desc(uint8_t nidt, uint8_t nid[]);

/**
 * Add a namespace to a subsytem.
 *
 * \param subsystem Subsystem to add namespace to.
 * \param bdev Block device to add as a namespace.
 * \param nsid Namespace ID to assign to the new namespace, or 0 to automatically use an available NSID.
 * \param anagrpid  ID of the ANA group that this namespace belongs to, or 0 if the namespace does not belong to any ANA group
 *
 * \return Newly added NSID on success or 0 on failure.
 */
uint32_t spdk_nvmf_subsystem_add_ns(struct spdk_nvmf_subsystem *subsystem, struct spdk_bdev *bdev,
				    uint32_t nsid, uint32_t anagrpid);

/**
 * Remove a namespace from a subsytem.
 *
 * \param subsystem Subsystem to remove namespace from.
 * \param nsid Namespace ID to assign to the new namespace, or 0 to automatically use an available NSID.
 *
 * \return A status <0 on failure and 0 on success
 */
int spdk_nvmf_subsystem_remove_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid);

/**
 * Update a namespace write protect attribute
 *
 * \param subsystem Subsystem to update namespace from
 * \param nsid Namespace ID to assign to the new namespace, or 0 to automatically use an available NSID
 * \param write_protect write_protect flags, true denotes ns is write protected false otherwise
 *
 * \return A status <0 on failure and 0 on success
 */
int spdk_nvmf_update_ns_attr_write_protect(struct spdk_nvmf_subsystem *subsystem,
		uint32_t  nsid, struct nwpc wp_flags);

/**
 * Find an ANA group in a subsystem with a specified group id
 *
 * \param subsystem Subsystem to find ANA group in
 * \param anagrpid ID of the ANA group to look for
 *
 * \return A pointer to the ANA group if one is found, or NULL if no ANA group is found
 */
struct spdk_nvmf_ana_group *spdk_nvmf_subsystem_find_ana_group(struct spdk_nvmf_subsystem
		*subsystem,
		uint32_t anagrpid);

/**
 * Add an ANA group to a subsystem with a specified group id
 *
 * \param subsystem Subsystem to add ANA group to
 * \param anagrpid ID of the ANA group to add
 * \param app_ctxt An application specific context for this ANA group
 *
 * \return A status <0 on failure and 0 on success
 */
int spdk_nvmf_subsystem_add_ana_group(struct spdk_nvmf_subsystem *subsystem, uint32_t anagrpid,
				      void *app_ctxt);

/**
 * Remove ANA group corresponding to group id specified from the subsystem
 *
 * \param subsystem Subsystem to remove ANA group from
 * \param anagrpid ID of the ANA group to remove
 *
 * \return A status <0 on failure and 0 on success
 */
int spdk_nvmf_subsystem_remove_ana_group(struct spdk_nvmf_subsystem *subsystem,
		uint32_t anagrpid);

/**
 * Get the ANA state, on the specified controller, for the group corresponding
 * to the specified group id
 *
 * \param subsystem Subsystem to which the ANA group belongs
 * \param anagrpid ID of the ANA group
 * \param cntlid Controller ID for which ANA group state has to be obtained
 *
 * \return ANA state of the group over the specified controller
 */
uint8_t spdk_nvmf_subsystem_get_ana_group_state(struct spdk_nvmf_subsystem *subsystem,
		uint32_t anagrpid, uint16_t cntlid);


int spdk_nvmf_subsystem_set_pci_id(struct spdk_nvmf_subsystem *subsystem,
				   struct spdk_pci_id *sub_pci_id);
const char *spdk_nvmf_subsystem_get_sn(const struct spdk_nvmf_subsystem *subsystem);
int spdk_nvmf_subsystem_set_sn(struct spdk_nvmf_subsystem *subsystem, const char *sn);

const char *spdk_nvmf_subsystem_get_nqn(struct spdk_nvmf_subsystem *subsystem);
enum spdk_nvmf_subtype spdk_nvmf_subsystem_get_type(struct spdk_nvmf_subsystem *subsystem);
enum spdk_nvmf_subsystem_mode spdk_nvmf_subsystem_get_mode(struct spdk_nvmf_subsystem *subsystem);

/* XXX: Throwaway with spdk17.10. Leverage spdk17.10 containers */
void spdk_nvmf_add_discovery_log_allowed_fn(void *fn);

void spdk_nvmf_acceptor_poll(void);

void spdk_nvmf_handle_connect(struct spdk_nvmf_request *req);

void spdk_nvmf_session_disconnect(struct spdk_nvmf_conn *conn);

void spdk_nvmf_queue_aer_rsp(struct spdk_nvmf_subsystem *subsystem,
			     enum aer_type aer_type,
			     uint8_t aer_info);

enum spdk_nvmf_qslot_update {
	NVMF_QSLOTS_ADDED	= 0,
	NVMF_QSLOTS_REMOVED	= 1,
};

/**
 * This allows the app to notify/warn when the number of command slots
 * on a specific hardware port starts crossing different thresholds.
 *
 * \param qslot_update Are the command slots being added or removed
 * \param qslots Number of command slots changing
 * \param port_ctx A context pointer per port that is provided by the app during port init.
 *
 * \return void
 */
void spdk_nvmf_qslots_update(enum spdk_nvmf_qslot_update qslot_update, uint32_t qslots,
			     void *port_ctx);
#endif
