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

#include "nvmf_internal.h"
#include "session.h"
#include "subsystem.h"
#include "transport.h"

#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf_spec.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

struct spdk_nvmf_subsystem *
spdk_nvmf_find_subsystem(const char *subnqn)
{
	struct spdk_nvmf_subsystem	*subsystem;

	if (!subnqn) {
		return NULL;
	}

	TAILQ_FOREACH(subsystem, &g_nvmf_tgt.subsystems, entries) {
		if (strcmp(subnqn, subsystem->subnqn) == 0) {
			return subsystem;
		}
	}

	return NULL;
}

void
spdk_nvmf_subsystem_set_ns_changed(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_nvmf_session 	*session;

	if (nsid == 0 || nsid > MAX_VIRTUAL_NAMESPACE) {
		SPDK_ERRLOG("Invalid nsid %d passed for setting ns change mask on subsystem\n", nsid);
		return;
	}

	TAILQ_FOREACH(session, &subsystem->sessions, link) {
		spdk_nvmf_session_set_ns_changed(session, nsid);
	}
}

struct spdk_nvmf_session *
spdk_nvmf_subsystem_get_ctrlr(struct spdk_nvmf_subsystem *subsystem, uint16_t cntlid)
{
	struct spdk_nvmf_session 	*session;

	TAILQ_FOREACH(session, &subsystem->sessions, link) {
		if (session->cntlid == cntlid) {
			return session;
		}
	}

	return NULL;
}

struct spdk_nvmf_host *
spdk_nvmf_find_subsystem_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{

	struct spdk_nvmf_host *host;

	if (!hostnqn) {
		SPDK_ERRLOG("hostnqn is NULL\n");
		return NULL;
	}

	if (!spdk_nvmf_valid_nqn(hostnqn)) {
		return NULL;
	}

	if (TAILQ_EMPTY(&subsystem->hosts)) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "No Hosts defined for Subsystem %s\n", subsystem->subnqn);
		if (spdk_nvmf_subsystem_get_allow_any_host(subsystem)) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "AllowAnyHost: Hostnqn %s\n", hostnqn);
			return &subsystem->host0;
		} else {
			/* No hosts are defined and allow_any_host is false, no one can connect */
			return NULL;
		}
	}

	TAILQ_FOREACH(host, &subsystem->hosts, link) {
		if (strcmp(hostnqn, host->nqn) == 0) {
			return host;
		}
	}

	if (spdk_nvmf_subsystem_get_allow_any_host(subsystem)) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "AllowAnyHost: Hostnqn %s\n", hostnqn);
		return &subsystem->host0;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Hostnqn %s not found on Subsystem %s\n", hostnqn,
		      subsystem->subnqn);

	return NULL;
}

int
spdk_nvmf_subsystem_start(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->ops->attach(subsystem);
}

static bool
nvmf_subsystem_removable(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_session *session;
	struct spdk_nvmf_conn	*conn;

	if (subsystem->is_removed) {
		TAILQ_FOREACH(session, &subsystem->sessions, link) {
			TAILQ_FOREACH(conn, &session->connections, link) {
				if (!conn->transport->conn_is_idle(conn)) {
					return false;
				}
			}
		}
		return true;
	}
	return false;
}

void
spdk_nvmf_subsystem_poll(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_session *session;

	/* Check the backing physical device for completions. */
	if (subsystem->ops->poll_for_completions) {
		subsystem->ops->poll_for_completions(subsystem);
	}

	TAILQ_FOREACH(session, &subsystem->sessions, link) {
		/* For each connection in the session, check for completions */
		spdk_nvmf_session_poll(session);
	}

	if (nvmf_subsystem_removable(subsystem)) {
		if (subsystem->ops->detach) {
			subsystem->ops->detach(subsystem);
		}
	}
}

bool
spdk_nvmf_valid_nqn(const char *nqn)
{
	size_t len;

	if (!memchr(nqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1)) {
		SPDK_ERRLOG("Invalid NQN length > max %d\n", SPDK_NVMF_NQN_MAX_LEN - 1);
		return false;
	}

	len = strlen(nqn);
	if (len >= SPDK_NVMF_NQN_MAX_LEN) {
		SPDK_ERRLOG("Invalid NQN \"%s\": length %zu > max %d\n", nqn, len, SPDK_NVMF_NQN_MAX_LEN - 1);
	}

	if (strncmp(nqn, "nqn.", 4) != 0) {
		SPDK_ERRLOG("Invalid NQN \"%s\": NQN must begin with \"nqn.\".\n", nqn);
		return false;
	}

	/* yyyy-mm. */
	if (!(isdigit(nqn[4]) && isdigit(nqn[5]) && isdigit(nqn[6]) && isdigit(nqn[7]) &&
	      nqn[8] == '-' && isdigit(nqn[9]) && isdigit(nqn[10]) && nqn[11] == '.')) {
		SPDK_ERRLOG("Invalid date code in NQN \"%s\"\n", nqn);
		return false;
	}

	return true;
}

/* XXX: Throwaway with spdk17.10. Leverage spdk17.10 containers */
void
spdk_nvmf_add_discovery_log_allowed_fn(void *fn)
{
	g_nvmf_tgt.disc_log_allowed_fn = (disc_log_allowed_fn_t) fn;
}

static const char *host0_nqn = "nqn.2017-11.Any.Host";

struct spdk_nvmf_subsystem *
spdk_nvmf_create_subsystem(const char *nqn,
			   enum spdk_nvmf_subtype type,
			   enum spdk_nvmf_subsystem_mode mode,
			   void *cb_ctx,
			   struct spdk_nvmf_subsystem_app_cbs *app_cbs)
{
	struct spdk_nvmf_subsystem	*subsystem;

	if (!spdk_nvmf_valid_nqn(nqn)) {
		return NULL;
	}

	subsystem = calloc(1, sizeof(struct spdk_nvmf_subsystem));
	if (subsystem == NULL) {
		return NULL;
	}

	g_nvmf_tgt.current_subsystem_id++;

	subsystem->id = g_nvmf_tgt.current_subsystem_id;
	subsystem->subtype = type;
	subsystem->mode = mode;
	subsystem->cb_ctx = cb_ctx;
	subsystem->app_cbs = app_cbs;
	snprintf(subsystem->subnqn, sizeof(subsystem->subnqn), "%s", nqn);

	subsystem->host0.max_io_queue_depth = g_nvmf_tgt.opts.max_io_queue_depth;
	subsystem->host0.max_connections_allowed = g_nvmf_tgt.opts.max_queues_per_session;
	subsystem->host0.max_aq_depth = g_nvmf_tgt.opts.max_aq_depth;
	if (!(subsystem->host0.nqn = strdup(host0_nqn))) {
		free(subsystem);
		return NULL;
	}

	TAILQ_INIT(&subsystem->ana_groups);
	TAILQ_INIT(&subsystem->allowed_listeners);
	TAILQ_INIT(&subsystem->hosts);
	TAILQ_INIT(&subsystem->sessions);

	if (type == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		subsystem->ops = &spdk_nvmf_discovery_ctrlr_ops;
	} else if (mode == NVMF_SUBSYSTEM_MODE_DIRECT) {
		subsystem->ops = &spdk_nvmf_direct_ctrlr_ops;
		subsystem->dev.direct.outstanding_admin_cmd_count = 0;
	} else {
		subsystem->ops = &spdk_nvmf_virtual_ctrlr_ops;
	}

	TAILQ_INSERT_TAIL(&g_nvmf_tgt.subsystems, subsystem, entries);
	g_nvmf_tgt.discovery_genctr++;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Created Subsystem NQN %s\n", nqn);

	return subsystem;
}

void
spdk_nvmf_delete_subsystem(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_subsystem_allowed_listener	*allowed_listener, *allowed_listener_tmp;
	struct spdk_nvmf_host		*host, *host_tmp;
	struct spdk_nvmf_session	*session, *session_tmp;
	struct spdk_nvmf_ana_group *ana_group = NULL, *ana_group_tmp = NULL;

	if (!subsystem) {
		return;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Destroy Subsystem NQN %s\n",
		      spdk_nvmf_subsystem_get_nqn(subsystem));
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "subsystem is %p\n", subsystem);

	TAILQ_FOREACH_SAFE(allowed_listener,
			   &subsystem->allowed_listeners, link, allowed_listener_tmp) {
		TAILQ_REMOVE(&subsystem->allowed_listeners, allowed_listener, link);

		free(allowed_listener);
	}

	TAILQ_FOREACH_SAFE(host, &subsystem->hosts, link, host_tmp) {
		TAILQ_REMOVE(&subsystem->hosts, host, link);
		free(host->nqn);
		free(host);
	}

	TAILQ_FOREACH_SAFE(session, &subsystem->sessions, link, session_tmp) {
		spdk_nvmf_session_destruct(session);
	}

	TAILQ_FOREACH_SAFE(ana_group, &subsystem->ana_groups, link, ana_group_tmp) {
		TAILQ_REMOVE(&subsystem->ana_groups, ana_group, link);
		free(ana_group);
		subsystem->num_ana_groups--;
	}

	if (subsystem->ops->detach) {
		subsystem->ops->detach(subsystem);
	}

	TAILQ_REMOVE(&g_nvmf_tgt.subsystems, subsystem, entries);
	g_nvmf_tgt.discovery_genctr++;

	free(subsystem->host0.nqn);
	free(subsystem);
}

struct spdk_nvmf_listen_addr *
spdk_nvmf_tgt_listen(const char *trname, enum spdk_nvmf_adrfam adrfam, const char *traddr,
		     const char *trsvcid)
{
	struct spdk_nvmf_listen_addr *listen_addr;
	const struct spdk_nvmf_transport *transport;
	int rc;

	TAILQ_FOREACH(listen_addr, &g_nvmf_tgt.listen_addrs, link) {
		if ((strcmp(listen_addr->trname, trname) == 0) &&
		    (listen_addr->adrfam == adrfam) &&
		    (strcmp(listen_addr->traddr, traddr) == 0) &&
		    (strcmp(listen_addr->trsvcid, trsvcid) == 0)) {
			return listen_addr;
		}
	}

	transport = spdk_nvmf_transport_get(trname);
	if (!transport) {
		SPDK_ERRLOG("Unknown transport '%s'\n", trname);
		return NULL;
	}

	listen_addr = spdk_nvmf_listen_addr_create(trname, adrfam, traddr, trsvcid);
	if (!listen_addr) {
		return NULL;
	}

	rc = transport->listen_addr_add(listen_addr);
	if (rc < 0) {
		spdk_nvmf_listen_addr_cleanup(listen_addr);
		SPDK_ERRLOG("Unable to listen on address '%s'\n", traddr);
		return NULL;
	}

	TAILQ_INSERT_HEAD(&g_nvmf_tgt.listen_addrs, listen_addr, link);

	return listen_addr;
}

void
spdk_nvmf_subsystem_set_allow_any_host(struct spdk_nvmf_subsystem *subsystem, bool allow_any_host)
{
	subsystem->allow_any_host = allow_any_host;
}

bool
spdk_nvmf_subsystem_get_allow_any_host(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->allow_any_host;
}

bool
spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	struct spdk_nvmf_host *host;

	if (!hostnqn) {
		return false;
	}

	if (spdk_nvmf_subsystem_get_allow_any_host(subsystem)) {
		return true;
	}

	if (TAILQ_EMPTY(&subsystem->hosts)) {
		/* No hosts are defined and allow_any_host is false, no one can connect */
		return false;
	}

	TAILQ_FOREACH(host, &subsystem->hosts, link) {
		if (strcmp(hostnqn, host->nqn) == 0) {
			return true;
		}
	}

	return false;
}

int
spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem, const char *host_nqn,
			     uint16_t max_io_queue_depth, uint16_t max_io_connections_allowed)
{
	struct spdk_nvmf_host *host;

	if (!spdk_nvmf_valid_nqn(host_nqn)) {
		return -1;
	}

	/* Check if the host is already present and return */
	TAILQ_FOREACH(host, &subsystem->hosts, link) {
		if (strcmp(host_nqn, host->nqn) == 0) {
			return 0;
		}
	}

	host = calloc(1, sizeof(*host));
	if (!host) {
		return -1;
	}
	host->nqn = strdup(host_nqn);
	if (!host->nqn) {
		free(host);
		return -1;
	}

	host->max_io_queue_depth = max_io_queue_depth;
	host->max_connections_allowed = max_io_connections_allowed + 1;
	host->max_aq_depth = g_nvmf_tgt.opts.max_aq_depth;

	TAILQ_INSERT_HEAD(&subsystem->hosts, host, link);
	g_nvmf_tgt.discovery_genctr++;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Host NQN %s (%u/%u/%u) added to Subsystem %s\n",
		      host->nqn, host->max_aq_depth, host->max_io_queue_depth, host->max_connections_allowed,
		      spdk_nvmf_subsystem_get_nqn(subsystem));

	return 0;
}

void
spdk_nvmf_subsystem_remove_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	struct spdk_nvmf_host *host, *host_tmp;

	TAILQ_FOREACH_SAFE(host, &subsystem->hosts, link, host_tmp) {
		if (strcmp(hostnqn, host->nqn) == 0) {
			TAILQ_REMOVE(&subsystem->hosts, host, link);
			free(host->nqn);
			free(host);
			g_nvmf_tgt.discovery_genctr++;

			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Host NQN %s removed from Subsystem %s\n",
				      hostnqn, spdk_nvmf_subsystem_get_nqn(subsystem));

			return;
		}
	}
}

struct spdk_nvmf_listen_addr *
spdk_nvmf_find_subsystem_listener(struct spdk_nvmf_subsystem *subsystem,
				  struct spdk_nvmf_listen_addr *listen_addr)
{
	if (!listen_addr) {
		SPDK_ERRLOG("listen_addr is NULL\n");
		return NULL;
	}

	if (spdk_nvmf_subsystem_listener_allowed(subsystem, listen_addr)) {
		return listen_addr;
	}

	return NULL;
}

void
spdk_nvmf_subsystem_set_allow_any_listener(struct spdk_nvmf_subsystem *subsystem,
		bool allow_any_listener)
{
	subsystem->allow_any_listener = allow_any_listener;
}

bool
spdk_nvmf_subsystem_get_allow_any_listener(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->allow_any_listener;
}

int
spdk_nvmf_subsystem_remove_listener(struct spdk_nvmf_subsystem *subsystem,
				    struct spdk_nvmf_listen_addr *listen_addr)
{
	struct spdk_nvmf_subsystem_allowed_listener	*allowed_listener, *allowed_listener_tmp;

	TAILQ_FOREACH_SAFE(allowed_listener,
			   &subsystem->allowed_listeners, link, allowed_listener_tmp) {
		if (allowed_listener->listen_addr == listen_addr) {
			TAILQ_REMOVE(&subsystem->allowed_listeners, allowed_listener, link);
			free(allowed_listener);
			g_nvmf_tgt.discovery_genctr++;
			return 0;
		}
	}
	return -1;
}

int
spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				 struct spdk_nvmf_listen_addr *listen_addr)
{
	struct spdk_nvmf_subsystem_allowed_listener *allowed_listener;

	TAILQ_FOREACH(allowed_listener, &subsystem->allowed_listeners, link) {
		if (spdk_nvmf_listen_addr_compare(allowed_listener->listen_addr, listen_addr)) {
			return 0;
		}
	}
	allowed_listener = calloc(1, sizeof(*allowed_listener));
	if (!allowed_listener) {
		return -1;
	}

	allowed_listener->listen_addr = listen_addr;

	TAILQ_INSERT_HEAD(&subsystem->allowed_listeners, allowed_listener, link);

	g_nvmf_tgt.discovery_genctr++;
	return 0;
}

/*
 * Returns true if the listener is allowed on the subsystem.
 */
bool
spdk_nvmf_subsystem_listener_allowed(struct spdk_nvmf_subsystem *subsystem,
				     struct spdk_nvmf_listen_addr *listen_addr)
{
	struct spdk_nvmf_subsystem_allowed_listener *allowed_listener;

	if (spdk_nvmf_subsystem_get_allow_any_listener(subsystem)) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "AllowAnyListener: Subsystem %s\n", subsystem->subnqn);
		return true;
	} else {
		TAILQ_FOREACH(allowed_listener, &subsystem->allowed_listeners, link) {
			if (spdk_nvmf_listen_addr_compare(allowed_listener->listen_addr, listen_addr)) {

				return true;
			}
		}
	}

	return false;
}

int
nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem,
			 struct spdk_nvme_ctrlr *ctrlr, const struct spdk_pci_addr *pci_addr)
{
	subsystem->dev.direct.ctrlr = ctrlr;
	subsystem->dev.direct.pci_addr = *pci_addr;

	return 0;
}

struct spdk_nvme_ns_id_desc *
spdk_nvmf_get_ns_id_desc(uint8_t nidt, uint8_t nid[])
{
	struct spdk_nvme_ns_id_desc *desc;
	uint8_t nidl = SPDK_NVME_NIDT_EUI64;

	assert((nidt >= SPDK_NVME_NIDT_EUI64) && (nidt <= SPDK_NVME_NIDT_UUID));
	if ((nidt < SPDK_NVME_NIDT_EUI64) || (nidt > SPDK_NVME_NIDT_UUID)) {
		SPDK_ERRLOG("Invalid value for nidt\n");
		return NULL;
	}

	if (nidt == SPDK_NVME_NIDT_EUI64) {
		nidl = SPDK_NVME_NIDL_EUI64;
	} else if (nidt == SPDK_NVME_NIDT_NGUID) {
		nidl = SPDK_NVME_NIDL_NGUID;
	} else {
		nidl = SPDK_NVME_NIDL_UUID;
	}

	desc = calloc(1, sizeof(struct spdk_nvme_ns_id_desc) + nidl);
	desc->nidt = nidt;
	desc->nidl = nidl;
	memcpy(desc->nid, nid, nidl);

	return desc;
}

uint8_t
spdk_nvmf_subsystem_get_ana_group_state(struct spdk_nvmf_subsystem *subsystem,
					uint32_t anagrpid, uint16_t cntlid)
{
	struct spdk_bdev *bdev = NULL;

	if (!subsystem) {
		SPDK_ERRLOG("Invalid subsystem passed during get_ana_group_state\n");
		assert(false);
		return SPDK_NVME_ANA_INACCESSIBLE;
	}

	for (uint32_t i = 0; i < MAX_VIRTUAL_NAMESPACE; i++) {
		bdev = subsystem->dev.virt.ns_list[i];
		if (bdev && bdev->anagrpid == anagrpid) {
			return spdk_bdev_get_ana_state(bdev, cntlid);
		}
	}

	/*
	 * There are no namespace belonging to a particular
	 * ana group.
	 */
	SPDK_ERRLOG("Subsystem %s: no namespaces found for ana_group %u\n",
		    spdk_nvmf_subsystem_get_nqn(subsystem), anagrpid);
	return SPDK_NVME_ANA_INACCESSIBLE;
}

struct spdk_nvmf_ana_group *
spdk_nvmf_subsystem_find_ana_group(struct spdk_nvmf_subsystem *subsystem, uint32_t anagrpid)
{
	struct spdk_nvmf_ana_group *ana_group = NULL;

	if (!subsystem) {
		SPDK_ERRLOG("Invalid subsystem passed during find_ana_group\n");
		assert(false);
		return NULL;
	}

	TAILQ_FOREACH(ana_group, &subsystem->ana_groups, link) {
		if (ana_group->anagrpid == anagrpid) {
			return ana_group;
		}
	}
	return NULL;
}

int
spdk_nvmf_subsystem_remove_ana_group(struct spdk_nvmf_subsystem *subsystem, uint32_t anagrpid)
{
	struct spdk_nvmf_ana_group *ana_group = NULL;

	if (!subsystem) {
		SPDK_ERRLOG("Invalid subsystem passed during remove_ana_group\n");
		assert(false);
		return -1;
	}

	ana_group = spdk_nvmf_subsystem_find_ana_group(subsystem, anagrpid);
	if (!ana_group) {
		SPDK_ERRLOG("Subsystem %s: attempt to remove invalid ana_group %u\n",
			    spdk_nvmf_subsystem_get_nqn(subsystem), anagrpid);
		return -1;
	}

	if (ana_group->num_nsids) {
		SPDK_ERRLOG("Subsystem %s: attempt to remove ana_group %u with namespaces %u\n",
			    spdk_nvmf_subsystem_get_nqn(subsystem), anagrpid, ana_group->num_nsids);
		return -1;
	}

	TAILQ_REMOVE(&subsystem->ana_groups, ana_group, link);
	free(ana_group);
	subsystem->num_ana_groups--;
	/*
	 * Note that we are not publishing an AEN OR incrementing change_count
	 * here deliberately. Applications are expected to increment the change count
	 * and publish the AEN when they deem appropriate
	 */
	return 0;
}

int
spdk_nvmf_subsystem_add_ana_group(struct spdk_nvmf_subsystem *subsystem, uint32_t anagrpid,
				  void *app_ctxt)
{
	struct spdk_nvmf_ana_group *ana_group = NULL;
	struct spdk_nvmf_ana_group *ana_group_tmp = NULL;
	bool found_successor = false;

	if (!subsystem) {
		SPDK_ERRLOG("Invalid subsystem passed during add_ana_group\n");
		assert(false);
		return -1;
	}

	ana_group = spdk_nvmf_subsystem_find_ana_group(subsystem, anagrpid);
	if (ana_group) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Subsystem %s: attempt to add an existent ana_group %u\n",
			      spdk_nvmf_subsystem_get_nqn(subsystem), anagrpid);
		return 0;
	}

	ana_group = calloc(1, sizeof(struct spdk_nvmf_ana_group));
	if (ana_group == NULL) {
		SPDK_ERRLOG("Subsystem %s: failed while allocating ana group %u \n",
			    spdk_nvmf_subsystem_get_nqn(subsystem), anagrpid);
		return -1;
	}

	ana_group->anagrpid = anagrpid;
	ana_group->num_nsids = 0;
	ana_group->app_ctxt = app_ctxt;

	TAILQ_FOREACH(ana_group_tmp, &subsystem->ana_groups, link) {
		if (ana_group_tmp->anagrpid > anagrpid) {
			found_successor = true;
			break;
		}
	}

	if (found_successor) {
		TAILQ_INSERT_BEFORE(ana_group_tmp, ana_group, link);
	} else {
		TAILQ_INSERT_TAIL(&subsystem->ana_groups, ana_group, link);
	}
	subsystem->num_ana_groups++;
	/*
	 * Note that we are not publishing an AEN OR incrementing change_count
	 * here deliberately. Applications are expected to increment the change count
	 * and publish the AEN when they deem appropriate
	 */
	return 0;
}

static void
spdk_nvmf_subsystem_adjust_max_nsid(struct spdk_nvmf_subsystem *subsystem)
{
	int32_t i;

	if (!subsystem) {
		SPDK_ERRLOG("Invalid subsystem passed during adjust_max_nsid\n");
		assert(false);
		return;
	}

	subsystem->dev.virt.max_nsid = 0;
	for (i = MAX_VIRTUAL_NAMESPACE - 1; i >= 0 ; --i) {
		if (subsystem->dev.virt.ns_list[i] != NULL) {
			subsystem->dev.virt.max_nsid = i + 1;
			break;
		}
	}
}

/*
 * This API removes the namespace (bdev) from ns_list but does not free it.
 * spdk_bdev_close api has to be called explicitly by the applications to
 * free the bdev and the associated resources.
 */
int
spdk_nvmf_subsystem_remove_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_bdev *bdev = NULL;
	struct spdk_nvmf_ana_group *ana_group = NULL;

	if (!subsystem) {
		SPDK_ERRLOG("Invalid subsystem passed during remove_ns\n");
		assert(false);
		return -1;
	}

	if (nsid > subsystem->dev.virt.max_nsid || nsid == 0) {
		SPDK_ERRLOG("Subsystem %s: Remove namespace for invalid nsid %u\n",
			    spdk_nvmf_subsystem_get_nqn(subsystem), nsid);
		return -1;
	}


	bdev = subsystem->dev.virt.ns_list[nsid - 1];
	if (bdev == NULL) {
		SPDK_ERRLOG("Subsytem %s: Remove namespace with invalid bdev nsid %u\n",
			    spdk_nvmf_subsystem_get_nqn(subsystem), nsid);
		return -1;
	}

	if (bdev->anagrpid) {
		ana_group = spdk_nvmf_subsystem_find_ana_group(subsystem, bdev->anagrpid);
		if (!ana_group) {
			SPDK_ERRLOG("Subsystem %s: Unable to find ana group %u for nsid %u\n",
				    spdk_nvmf_subsystem_get_nqn(subsystem), bdev->anagrpid, nsid);
			return -1;
		}
		ana_group->num_nsids--;
		bdev->anagrpid = 0;
		/* It is possible that this is  the last ns  in the ana group
		 * but we dont remove the ana group and  expect the application
		 * to remove the group  explicitly
		 */
	}

	subsystem->dev.virt.ns_list[nsid - 1] = NULL;
	spdk_nvmf_subsystem_set_ns_changed(subsystem, nsid);

	if (nsid == subsystem->dev.virt.max_nsid) {
		spdk_nvmf_subsystem_adjust_max_nsid(subsystem);
	}

	return 0;
}

uint32_t
spdk_nvmf_subsystem_add_ns(struct spdk_nvmf_subsystem *subsystem, struct spdk_bdev *bdev,
			   uint32_t nsid, uint32_t anagrpid)
{
	uint32_t i;
	int rc;
	struct spdk_nvmf_ana_group *ana_group = NULL;

	assert(subsystem->mode == NVMF_SUBSYSTEM_MODE_VIRTUAL);

	if (nsid == 0) {
		/* NSID not specified - find a free index */
		for (i = 0; i < MAX_VIRTUAL_NAMESPACE; i++) {
			if (subsystem->dev.virt.ns_list[i] == NULL) {
				nsid = i + 1;
				break;
			}
		}
		if (nsid == 0) {
			SPDK_ERRLOG("All available NSIDs in use\n");
			return 0;
		}
	} else {
		/* Specific NSID requested */
		i = nsid - 1;
		if (i >= MAX_VIRTUAL_NAMESPACE) {
			SPDK_ERRLOG("Requested NSID %" PRIu32 " out of range\n", nsid);
			return 0;
		}

		if (subsystem->dev.virt.ns_list[i]) {
			SPDK_ERRLOG("Requested NSID %" PRIu32 " already in use\n", nsid);
			return 0;
		}
	}

	if (anagrpid) {
		/** We expect the application to create ana group before adding a namespace */
		ana_group = spdk_nvmf_subsystem_find_ana_group(subsystem, anagrpid);
		if (!ana_group) {
			SPDK_ERRLOG("Subsystem %s: namespace add on invalid ana group %u\n", subsystem->subnqn,
				    anagrpid);
			return 0;
		}
	}

	rc = spdk_bdev_open(bdev, true, NULL, NULL,
			    &subsystem->dev.virt.desc[i]);
	if (rc != 0) {
		SPDK_ERRLOG("Subsystem %s: bdev %s cannot be opened, error=%d\n",
			    subsystem->subnqn, spdk_bdev_get_name(bdev), rc);
		return rc;

	}

	if (ana_group) {
		ana_group->num_nsids++;
		bdev->anagrpid = anagrpid;
	}

	subsystem->dev.virt.ns_list[i] = bdev;
	subsystem->dev.virt.max_nsid =  spdk_max(subsystem->dev.virt.max_nsid, nsid);
	spdk_nvmf_subsystem_set_ns_changed(subsystem, nsid);

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Subsystem %s: bdev %s assigned nsid %" PRIu32 " anagrpid %u\n",
		      spdk_nvmf_subsystem_get_nqn(subsystem),
		      spdk_bdev_get_name(bdev),
		      nsid,
		      anagrpid);

	return nsid;
}

int
spdk_nvmf_update_ns_attr_write_protect(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid,
				       struct nwpc wp_flags)
{
	struct spdk_bdev *bdev = NULL;
	bool changed = false;

	if (!subsystem) {
		SPDK_ERRLOG("Invalid subsystem passed during update_ns_attr_ro\n");
		return -1;
	}

	if (nsid > subsystem->dev.virt.max_nsid || nsid == 0) {
		SPDK_ERRLOG("Subsystem %s: Update ns attr for invalid nsid %u\n",
			    spdk_nvmf_subsystem_get_nqn(subsystem), nsid);
		return -1;
	}

	bdev = subsystem->dev.virt.ns_list[nsid - 1];
	if (bdev == NULL) {
		SPDK_ERRLOG("Subsytem %s: Update ns attr with invalid bdev nsid %u\n",
			    spdk_nvmf_subsystem_get_nqn(subsystem), nsid);
		return -1;
	}

	changed = (bdev->write_protect_flags.write_protect != wp_flags.write_protect);
	bdev->write_protect_flags = wp_flags;
	/* Set an AEN only if write_protect flag have been changed */
	if (changed) {
		spdk_nvmf_subsystem_set_ns_changed(subsystem, nsid);
	}

	return 0;
}

const char *
spdk_nvmf_subsystem_get_sn(const struct spdk_nvmf_subsystem *subsystem)
{
	if (subsystem->mode != NVMF_SUBSYSTEM_MODE_VIRTUAL) {
		return NULL;
	}

	return subsystem->dev.virt.sn;
}

int
spdk_nvmf_subsystem_set_sn(struct spdk_nvmf_subsystem *subsystem, const char *sn)
{
	size_t len, max_len;

	if (subsystem->mode != NVMF_SUBSYSTEM_MODE_VIRTUAL) {
		return -1;
	}

	max_len = sizeof(subsystem->dev.virt.sn) - 1;
	len = strlen(sn);
	if (len > max_len) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Invalid sn \"%s\": length %zu > max %zu\n",
			      sn, len, max_len);
		return -1;
	}

	snprintf(subsystem->dev.virt.sn, sizeof(subsystem->dev.virt.sn), "%s", sn);

	return 0;
}

int
spdk_nvmf_subsystem_set_pci_id(struct spdk_nvmf_subsystem *subsystem,
			       struct spdk_pci_id *sub_pci_id)
{
	if (sub_pci_id == NULL) {
		return -1;
	}

	if (subsystem->mode != NVMF_SUBSYSTEM_MODE_VIRTUAL) {
		return -1;
	}

	memcpy(&subsystem->dev.virt.sub_pci_id, sub_pci_id, sizeof(struct spdk_pci_id));

	return 0;
}

const char *
spdk_nvmf_subsystem_get_nqn(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->subnqn;
}

/* Workaround for astyle formatting bug */
typedef enum spdk_nvmf_subtype nvmf_subtype_t;

nvmf_subtype_t
spdk_nvmf_subsystem_get_type(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->subtype;
}

/* Workaround for astyle formatting bug */
typedef enum spdk_nvmf_subsystem_mode nvmf_mode_t;

nvmf_mode_t
spdk_nvmf_subsystem_get_mode(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->mode;
}
