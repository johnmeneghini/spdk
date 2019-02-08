/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2017 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Limited and/or its subsidiaries.
 *   All rights reserved.
 *
 *   Copyright (c) Intel Corporation.
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


#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/spdk_nvmf_fault_injects.h"

#include "nvmf/nvmf_internal.h"
#include "nvmf_fc_tgt.h"
#include "fc/ocs_tgt_api.h"
#include "nvmf_fc/fc_adm_api.h"

static TAILQ_HEAD(, nvmf_tgt_subsystem) g_subsystems = TAILQ_HEAD_INITIALIZER(g_subsystems);
static bool g_subsystems_shutdown;

static struct spdk_nvmf_bcm_fc_master_ops g_nvmf_fc_ops = {
	spdk_nvmf_bcm_fc_master_enqueue_event,
};

static void
shutdown_complete(void)
{
	spdk_app_stop(0);
}

static void
nvmf_fc_subsystem_delete_event(void *arg1, void *arg2)
{
	struct nvmf_tgt_subsystem *app_subsys = arg1;
	struct spdk_nvmf_subsystem *subsystem = app_subsys->subsystem;

	TAILQ_REMOVE(&g_subsystems, app_subsys, tailq);
	spdk_free(app_subsys);

	spdk_nvmf_delete_subsystem(subsystem);

	if (g_subsystems_shutdown && TAILQ_EMPTY(&g_subsystems)) {
		spdk_nvmf_tgt_fini();
		/* Finished shutting down all subsystems - continue the shutdown process. */
		shutdown_complete();
	}
}

static void
nvmf_fc_tgt_delete_subsystem(struct nvmf_tgt_subsystem *app_subsys)
{
	struct spdk_event *event;

	/*
	 * Unregister the poller - this starts a chain of events that will eventually free
	 * the subsystem's memory.
	 */
	event = spdk_event_allocate(spdk_env_get_current_core(), nvmf_fc_subsystem_delete_event,
				    app_subsys, NULL);
	spdk_poller_unregister(&app_subsys->poller, event);
}

static void
nvmf_fc_shutdown_subsystems(void)
{
	struct nvmf_tgt_subsystem *app_subsys, *tmp;

	g_subsystems_shutdown = true;
	TAILQ_FOREACH_SAFE(app_subsys, &g_subsystems, tailq, tmp) {
		nvmf_fc_tgt_delete_subsystem(app_subsys);
	}
}

static void
nvmf_fc_shutdown_cb(void)
{
	SPDK_NOTICELOG("\n=========================\n");
	SPDK_NOTICELOG("   NVMF FC shutdown signal\n");
	SPDK_NOTICELOG("=========================\n");

	nvmf_fc_shutdown_subsystems();
	spdk_fc_api_subsystem_exit();
}

/* Don't need subsystem poller unless running direct device or
 * for subsystem removal AEN support
static void
nvmf_fc_subsystem_poll(void *arg)
{
	struct nvmf_tgt_subsystem *app_subsys = arg;

	spdk_nvmf_subsystem_poll(app_subsys->subsystem);
} */

static void
nvmf_fc_tgt_start_subsystem(void *arg1, void *arg2)
{
	struct nvmf_tgt_subsystem *app_subsys = arg1;
	struct spdk_nvmf_subsystem *subsystem = app_subsys->subsystem;
	/* int lcore = spdk_env_get_current_core(); */

	spdk_nvmf_subsystem_start(subsystem);

	/* Don't need to start poller unless running direct device or
	* for subsystem removal AEN support */
	/* spdk_poller_register(&app_subsys->poller, subsystem_poll, app_subsys, lcore, 0); */
}

void
spdk_nvmf_bcm_fc_tgt_start_subsystem(struct nvmf_tgt_subsystem *app_subsys)
{
	struct spdk_event *event;

	event = spdk_event_allocate(app_subsys->lcore, nvmf_fc_tgt_start_subsystem,
				    app_subsys, NULL);
	spdk_event_call(event);
}

struct nvmf_tgt_subsystem *
spdk_nvmf_bcm_fc_tgt_create_subsystem(const char *name, enum spdk_nvmf_subtype subtype,
				      enum spdk_nvmf_subsystem_mode mode, uint32_t lcore)
{
	struct spdk_nvmf_subsystem *subsystem;
	struct nvmf_tgt_subsystem *app_subsys;

	if (spdk_nvmf_subsystem_exists(name)) {
		SPDK_ERRLOG("Subsystem already exist\n");
		return NULL;
	}

	app_subsys = spdk_calloc(1, sizeof(*app_subsys));
	if (app_subsys == NULL) {
		SPDK_ERRLOG("Subsystem allocation failed\n");
		return NULL;
	}

	subsystem = spdk_nvmf_create_subsystem(name, subtype, mode, app_subsys,
					       spdk_nvmf_bcm_fc_subsys_connect_cb,
					       spdk_nvmf_bcm_fc_subsys_disconnect_cb);

	if (subsystem == NULL) {
		SPDK_ERRLOG("Subsystem creation failed\n");
		spdk_free(app_subsys);
		return NULL;
	}

	app_subsys->subsystem = subsystem;
	app_subsys->lcore = lcore;

	SPDK_NOTICELOG("allocated subsystem %s on lcore %u on socket %u\n", name, lcore,
		       spdk_env_get_socket_id(lcore));

	TAILQ_INSERT_TAIL(&g_subsystems, app_subsys, tailq);

	return app_subsys;
}

/* This function can only be used before the pollers are started. */
static void
nvmf_fc_tgt_delete_subsystems(void)
{
	struct nvmf_tgt_subsystem *app_subsys, *tmp;
	struct spdk_nvmf_subsystem *subsystem;

	TAILQ_FOREACH_SAFE(app_subsys, &g_subsystems, tailq, tmp) {
		TAILQ_REMOVE(&g_subsystems, app_subsys, tailq);
		subsystem = app_subsys->subsystem;
		spdk_nvmf_delete_subsystem(subsystem);
		spdk_free(app_subsys);
	}
}

struct nvmf_tgt_subsystem *
spdk_nvmf_bcm_fc_tgt_subsystem_first(void)
{
	return TAILQ_FIRST(&g_subsystems);
}

struct nvmf_tgt_subsystem *
spdk_nvmf_bcm_fc_tgt_subsystem_next(struct nvmf_tgt_subsystem *subsystem)
{
	return TAILQ_NEXT(subsystem, tailq);
}

int
spdk_nvmf_bcm_fc_tgt_shutdown_subsystem_by_nqn(const char *nqn)
{
	struct nvmf_tgt_subsystem *tgt_subsystem, *subsys_tmp;

	TAILQ_FOREACH_SAFE(tgt_subsystem, &g_subsystems, tailq, subsys_tmp) {
		if (strcmp(tgt_subsystem->subsystem->subnqn, nqn) == 0) {
			nvmf_fc_tgt_delete_subsystem(tgt_subsystem);
			return 0;
		}
	}
	return -1;
}

static void
nvmf_fc_startup(void *arg1, void *arg2)
{
	int rc;

	spdk_nvmf_fault_init();
	rc = spdk_nvmf_bcm_fc_parse_conf();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_nvmf_parse_conf() failed\n");
		goto initialize_error;
	}

#ifdef stdout
	if (getenv("MEMZONE_DUMP") != NULL) {
		spdk_memzone_dump(stdout);
		fflush(stdout);
	}
#endif

	spdk_nvmf_fc_register_ops(&g_nvmf_fc_ops);

	if (spdk_fc_api_subsystem_init()) {
		SPDK_ERRLOG("Failed to init FC subsystem.\n");
		goto initialize_error;
	}

	return;

initialize_error:
	nvmf_fc_tgt_delete_subsystems();
	spdk_app_stop(rc);
}

int
spdk_nvmf_bcm_fc_tgt_start(struct spdk_app_opts *opts)
{
	int rc;

	opts->shutdown_cb = nvmf_fc_shutdown_cb;
	spdk_app_init(opts);

	printf("Total cores available: %d\n", spdk_env_get_core_count());
	/* Blocks until the application is exiting */
	rc = spdk_app_start(nvmf_fc_startup, NULL, NULL);

	spdk_app_fini();

	return rc;
}

spdk_err_t
spdk_nvmf_bcm_fc_tgt_add_port(const char *trname,
			      struct spdk_nvmf_bcm_fc_nport *nport)
{
	struct spdk_nvmf_listen_addr *fc_listen_addr = NULL;
	char traddr[64];
	struct spdk_nvmf_subsystem *subsystem = NULL;
	spdk_err_t err = SPDK_SUCCESS;

	/* add this nport to the subsystems list of allowed listeners */
	sprintf(traddr, "nn-0x%lx:pn-0x%lx",
		nport->fc_nodename.u.wwn, nport->fc_portname.u.wwn);

	fc_listen_addr = spdk_nvmf_tgt_listen(NVMF_BCM_FC_TRANSPORT_NAME,
					      traddr, "none");

	if (fc_listen_addr) {
		TAILQ_FOREACH(subsystem, &g_nvmf_tgt.subsystems, entries) {
			spdk_nvmf_subsystem_add_listener(subsystem,
							 fc_listen_addr);
		}
	} else {
		err = SPDK_ERR_NOMEM;
	}

	/* discovery log page needs to be updated. */
	g_nvmf_tgt.discovery_genctr++;

	return err;
}

spdk_err_t
spdk_nvmf_bcm_fc_tgt_remove_port(const char *trname,
				 struct spdk_nvmf_bcm_fc_nport *nport)
{
	char traddr[64];
	struct spdk_nvmf_subsystem *subsystem = NULL;
	spdk_err_t err = SPDK_SUCCESS;

	sprintf(traddr, "nn-0x%lx:pn-0x%lx",
		nport->fc_nodename.u.wwn, nport->fc_portname.u.wwn);

	/* find listener address in each subsystem and remove it */
	TAILQ_FOREACH(subsystem, &g_nvmf_tgt.subsystems, entries) {
		struct spdk_nvmf_subsystem_allowed_listener *al;
		TAILQ_FOREACH(al, &subsystem->allowed_listeners, link) {
			if ((strcmp(al->listen_addr->trname, trname) == 0) &&
			    (strcmp(al->listen_addr->traddr, traddr) == 0)) {
				TAILQ_REMOVE(&subsystem->allowed_listeners,
					     al, link);
				spdk_free(al);
				break;
			}
		}

	}

	/* discovery log page needs to be updated */
	g_nvmf_tgt.discovery_genctr++;

	return err;
}
