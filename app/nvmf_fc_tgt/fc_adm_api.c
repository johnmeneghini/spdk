/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 1992-2017 NetApp, Inc.
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

#include "fc_adm_api.h"
#include <nvmf/transport.h>
#include <nvmf/nvmf_internal.h>
#include <spdk/trace.h>
#include <spdk_internal/log.h>

#include <spdk/nvmf_spec.h>
#include <spdk/log.h>
#include <spdk/string.h>

static void spdk_fc_hw_port_init(void *arg1, void *arg2);
static void spdk_fc_hw_port_online(void *arg1, void *arg2);
static void spdk_fc_nport_create(void *arg1, void *arg2);
static void spdk_fc_nport_delete(void *arg1, void *arg2);
static void spdk_fc_i_t_add(void *arg1, void *arg2);
static void spdk_fc_abts_recv(void *arg1, void *arg2);

/* PRIVATE HELPER FUNCTIONS FOR NETAPP - BEGIN */

/* Initializes the data for the creation of a FC-Port object in the SPDK library.
 * The spdk_nvmf_fc_port is a well defined structure that is part of the API to the
 * library. The contents added to this well defined structure is private to each
 * vendors implementation.
 */
static spdk_err_t
nvmf_tgt_fc_hw_port_data_init(struct spdk_nvmf_fc_port *fc_port, spdk_hw_port_init_args_t *args)
{
	/* Used a high number for the LS HWQP so that it does not clash with the
	 * IO HWQP's and immediately shows a LS queue during tracing.
	 */
#define FC_LS_HWQP_ID 0xff
	uint32_t                  poweroftwo = 1;
	char                     *poolname;
	struct fc_xri_s          *ring_xri     = NULL;
	struct fc_xri_s          *ring_xri_ptr = NULL;
	spdk_err_t                err        = SPDK_SUCCESS;
	int                       i, rc;
	uint32_t                  lcore_id   = 0;

	bzero(fc_port, sizeof(struct spdk_nvmf_fc_port));

	fc_port->port_hdl       = args->port_handle;
	fc_port->hw_port_status = SPDK_FC_PORT_OFFLINE;
	fc_port->xri_base       = args->xri_base;
	fc_port->xri_count      = args->xri_count;

	while (poweroftwo <= fc_port->xri_count) {
		poweroftwo *= 2;
	}

	/*
	 * Create a ring for the XRI's and store the XRI's in there.
	 * The ring size is set to count, which must be a power of two.
	 * The real usable ring size is count-1 instead of count to differentiate
	 * a free ring from an empty ring
	 */
	poolname          = spdk_sprintf_alloc("xri_ring:%d", args->port_handle);
	fc_port->xri_ring = spdk_ring_create(poolname, poweroftwo, SPDK_ENV_SOCKET_ID_ANY, 0);
	if (!fc_port->xri_ring) {
		SPDK_ERRLOG("XRI ring alloc failed for port = %d\n", args->port_handle);
		err = SPDK_ERR_NOMEM;
		goto err;
	}

	ring_xri = spdk_calloc(fc_port->xri_count, sizeof(struct fc_xri_s));
	if (!ring_xri) {
		SPDK_ERRLOG("XRI ring buffer alloc failed\n");
		err = SPDK_ERR_NOMEM;
		goto err;
	}

	/*
	 * Store all the XRI elements in this ring. Start from the base and
	 * work up.
	 */
	ring_xri_ptr = ring_xri;
	for (uint32_t count = (NVMF_FC_MAX_IO_QUEUES + 1); count < fc_port->xri_count; count++) {
		ring_xri_ptr->xri = fc_port->xri_base + count;

		/* Since we created the ring with NO flags, this means it is mp-mc safe */
		rc = spdk_ring_enqueue(fc_port->xri_ring, (void *)ring_xri_ptr);
		if (rc != 0) {
			SPDK_ERRLOG("XRI ring buffer enqueue failed at count: %d\n", count);
			err = SPDK_ERR_INTERNAL;
			goto err;
		}
		ring_xri_ptr++;
	}

	/*
	 * Initialize the LS queue wherever needed.
	 */
	fc_port->ls_queue.queues    = args->ls_queue;
	fc_port->ls_queue.lcore_id  = spdk_env_get_master_lcore();
	fc_port->ls_queue.hwqp_id   = FC_LS_HWQP_ID;
	fc_port->ls_queue.send_frame_xri  = fc_port->xri_base;
	TAILQ_INIT(&fc_port->ls_queue.pending_xri_list);

	/*
	 * Initialize the LS poller.
	 */
	spdk_nvmf_fc_init_poller(fc_port, &fc_port->ls_queue);

	/*
	 * Initialize the IO queues.
	 */
	for (i = 0; i < NVMF_FC_MAX_IO_QUEUES; i++) {
		fc_port->io_queues[i].queues = args->io_queues[i];

		/* Initialize the session list on this FC HW Q */
		TAILQ_INIT(&fc_port->io_queues[i].connection_list);
		TAILQ_INIT(&fc_port->io_queues[i].pending_xri_list);

		lcore_id = spdk_env_get_next_core(lcore_id);

		/* Skip master */
		if (lcore_id == spdk_env_get_master_lcore()) {
			lcore_id = spdk_env_get_next_core(lcore_id);
		}

		/* Wrap around */
		if (lcore_id == UINT32_MAX) {
			lcore_id = spdk_env_get_first_core();
		}

		fc_port->io_queues[i].lcore_id  = lcore_id;
		fc_port->io_queues[i].hwqp_id   = i;
		fc_port->io_queues[i].send_frame_xri = fc_port->xri_base + 1 + i;

		spdk_nvmf_fc_init_poller(fc_port, &fc_port->io_queues[i]);
	}
	fc_port->max_io_queues = NVMF_FC_MAX_IO_QUEUES;

	/*
	 * Initialize the list of nport on this HW port.
	 */
	TAILQ_INIT(&fc_port->nport_list);
	fc_port->num_nports = 0;

err:
	if (err != SPDK_SUCCESS) {
		if (ring_xri) {
			spdk_free(ring_xri);
		}
		if (fc_port->xri_ring) {
			spdk_ring_free(fc_port->xri_ring);
		}
	}

	return err;
}

/* Initializes the data for the creation of a Nport object in the Library.
 * The spdk_nvmf_fc_nport is a well defined structure that is part of the API to the
 * library. The contents added to this well defined structure is private to each
 * vendors implementation.
 */
static void
nvmf_tgt_fc_nport_data_init(struct spdk_nvmf_fc_nport *nport, spdk_nport_create_args_t *args)
{
	bzero(nport, sizeof(*nport));

	nport->nport_hdl = args->nport_handle;
	nport->port_hdl  = args->port_handle;
	nport->nport_status = false;
	nport->nport_state  = SPDK_FC_OBJECT_CREATED;
	nport->fc_nodename  = args->fc_nodename;
	nport->fc_portname  = args->fc_portname;
	nport->d_id         = args->d_id;
	nport->fc_port      = spdk_nvmf_fc_port_list_get(args->port_handle);

	TAILQ_INIT(&nport->rem_port_list);
	TAILQ_INIT(&nport->fc_associations);
	nport->assoc_count = 0;
}

/*
 * A private implementation that sets up the listening addresses in
 * various subsystems that have the right ACL's for this nport.
 */
static spdk_err_t
nvmf_tgt_fc_nport_add_listen_addr(struct spdk_nvmf_fc_nport *nport)
{
	return spdk_nvmf_fc_tgt_add_port(NVMF_BCM_FC_TRANSPORT_NAME, nport);
}

static spdk_err_t
nvmf_tgt_fc_nport_remove_listen_addr(struct spdk_nvmf_fc_nport *nport)
{
	return spdk_nvmf_fc_tgt_remove_port(NVMF_BCM_FC_TRANSPORT_NAME, nport);
}


static void
nvmf_tgt_fc_delete_nport_assoc_cb(void *args, uint32_t err)
{
	struct spdk_nvmf_fc_nport_del_cb_data *cb_data  = args;
	struct spdk_nvmf_fc_nport             *nport    = cb_data->nport;
	fc_callback                            cb_func  = cb_data->fc_cb_func;
	spdk_err_t                             spdk_err = SPDK_SUCCESS;

	/*
	 * Assert on any association delete failure. We continue to delete other
	 * associations in promoted builds.
	 */
	if (0 != err) {
		assert(0 != "Nport's association delete callback returned error");

		SPDK_ERRLOG("Nport's association delete callback returned error. FC Port: %d, Nport: %d\n",
			    nport->port_hdl, nport->nport_hdl);

		/* This breaks the app-library interface by morphing a library datastructure */
		nport->assoc_count--;
	}

	/*
	 * Free the nport if this is the last association being deleted and
	 * execute the callback(s).
	 */
	if (nport && spdk_nvmf_fc_nport_is_association_empty(nport)) {
		/* Free the nport */
		spdk_free(nport);

		if (err) {
			/* TODO: Fix error compatibility */
			spdk_err = SPDK_ERR_INTERNAL;
		}

		/* TODO: Execute callbacks from callback vector */
		if (cb_func != NULL) {
			(void)cb_func(cb_data->port_handle, SPDK_FC_NPORT_DELETE, cb_data->fc_cb_ctx, spdk_err);
		}
		spdk_free(args);
	}
}

static void
nvmf_tgt_fc_rport_data_init(struct spdk_nvmf_fc_rem_port_info *rport, spdk_hw_i_t_add_args_t *args)
{
	bzero(rport, sizeof(*rport));

	rport->s_id = args->s_id;
	rport->rpi  = args->rpi;
}

/* PRIVATE HELPER FUNCTIONS FOR NETAPP - END */




/* PUBLIC FUNCTIONS FOR DRIVER AND LIBRARY INTERACTIONS - BEGIN */
/*
 * Queue up an event in the SPDK masters event queue.
 * Used by the FC driver to notify the SPDK master of FC related events.
 */
spdk_err_t
spdk_master_enqueue_event(spdk_fc_event_t event_type, void *args, fc_callback cb_func)
{
	spdk_err_t         err   = SPDK_SUCCESS;
	struct spdk_event *event = NULL;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_FC_ADM, "Enqueue event %d.\n", event_type);

	if (event_type >= SPDK_FC_EVENT_MAX) {
		SPDK_ERRLOG("Invalid spdk_fc_event_t %d.\n", event_type);
		err = SPDK_ERR_INVALID_ARGS;
		goto done;
	}

	if (args == NULL) {
		SPDK_ERRLOG("Null args for event %d.\n", event_type);
		err = SPDK_ERR_INVALID_ARGS;
		goto done;
	}

	switch (event_type) {
	case SPDK_FC_HW_PORT_INIT:
		event = spdk_event_allocate(spdk_env_get_master_lcore(), spdk_fc_hw_port_init, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;

	case SPDK_FC_HW_PORT_ONLINE:
		event =
			spdk_event_allocate(spdk_env_get_master_lcore(), spdk_fc_hw_port_online, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;

	case SPDK_FC_HW_PORT_OFFLINE:
		break;

	case SPDK_FC_HW_PORT_RESET:
		break;

	case SPDK_FC_NPORT_CREATE:
		event = spdk_event_allocate(spdk_env_get_master_lcore(), spdk_fc_nport_create, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;

	case SPDK_FC_NPORT_DELETE:
		event = spdk_event_allocate(spdk_env_get_master_lcore(), spdk_fc_nport_delete, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;

	case SPDK_FC_IT_ADD:
		event = spdk_event_allocate(spdk_env_get_master_lcore(), spdk_fc_i_t_add, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;

	case SPDK_FC_IT_DELETE:
		break;

	case SPDK_FC_ABTS_RECV:
		event = spdk_event_allocate(spdk_env_get_master_lcore(), spdk_fc_abts_recv, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;

	case SPDK_FC_LINK_BREAK:
		break;
	case SPDK_FC_ADAPTER_ERR1: /* Firmware Dump */
		break;
	case SPDK_FC_ADAPTER_ERR2:
		break;
	case SPDK_FC_UNRECOVERABLE_ERR:
		break;

	default:
		SPDK_ERRLOG("Invalid spdk_fc_event_t: %d\n", event_type);
		err = SPDK_ERR_INVALID_ARGS;
		break;
	}

done:

	if (err == SPDK_SUCCESS) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_FC_ADM, "Enqueue event %d done successfully \n", event_type);
	} else {
		SPDK_ERRLOG("Enqueue event %d failed, rc = %d\n", event_type, err);
	}

	if (event) {
		spdk_event_call(event);
	}

	return err;
}



/*
 * Initialize and add a HW port entry to the global
 * HW port list.
 */
static void
spdk_fc_hw_port_init(void *arg1, void *arg2)
{
	struct spdk_nvmf_fc_port *fc_port      = NULL;
	spdk_hw_port_init_args_t *args         = (spdk_hw_port_init_args_t *)arg1;
	fc_callback               cb_func      = (fc_callback)arg2;
	spdk_err_t                err          = SPDK_SUCCESS;
	static bool               ioq_depth_adj_needed = true;

	/*
	 * 1. Check for duplicate initialization.
	 */
	fc_port = spdk_nvmf_fc_port_list_get(args->port_handle);
	if (fc_port != NULL) {
		SPDK_ERRLOG("SPDK FC port %d already initialized.\n", args->port_handle);
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * 2. Get the memory to instantiate a fc port.
	 */
	fc_port = spdk_malloc(sizeof(struct spdk_nvmf_fc_port));
	if (fc_port == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for fc_port %d.\n", args->port_handle);
		err = SPDK_ERR_NOMEM;
		goto err;
	}

	/*
	 * 3. Initialize the contents for the FC-port
	 */
	err = nvmf_tgt_fc_hw_port_data_init(fc_port, args);

	if (err != SPDK_SUCCESS) {
		SPDK_ERRLOG("Data initialization failed for fc_port %d.\n", args->port_handle);
		goto err;
	}


	/*
	 * 4. Add this port to the global fc port list in the library.
	 */
	spdk_nvmf_fc_port_list_add(fc_port);

	/*
	 * 5. Update the g_nvmf_tgt.max_queue_depth (if necessary)
	 *    max_queue_depth is used to set MQES property
	 */

	if (ioq_depth_adj_needed) {
		g_nvmf_tgt.max_queue_depth =
			spdk_nvmf_bcm_fc_calc_max_q_depth(fc_port->max_io_queues,
					fc_port->io_queues[0].queues.
					rq_payload.num_buffers,
					g_nvmf_tgt.max_associations,
					g_nvmf_tgt.max_queues_per_session,
					g_nvmf_tgt.max_aq_depth);

		ioq_depth_adj_needed = false;
		SPDK_TRACELOG(SPDK_TRACE_NVMF_FC_ADM, "MAX SQ size=%d.\n",
			      g_nvmf_tgt.max_queue_depth);
	}

	/* Success case. Go out */
	goto out;

err:

	if (fc_port) {
		spdk_free(fc_port);
	}
out:
	if (cb_func != NULL) {
		(void)cb_func(args->port_handle, SPDK_FC_HW_PORT_INIT, args->cb_ctx, err);
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF_FC_ADM, "HW port %d initialize done, rc = %d.\n", args->port_handle,
		      err);
}

/*
 * Online a HW port.
 */
static void
spdk_fc_hw_port_online(void *arg1, void *arg2)
{
	struct spdk_nvmf_fc_port   *fc_port = NULL;
	struct fc_hwqp             *hwqp    = NULL;
	spdk_hw_port_online_args_t *args    = (spdk_hw_port_online_args_t *)arg1;
	fc_callback                 cb_func = (fc_callback)arg2;
	int                         i   = 0;
	spdk_err_t                  err = SPDK_SUCCESS;

	/*
	 * 1. Get the port from the library
	 */
	fc_port = spdk_nvmf_fc_port_list_get(args->port_handle);
	if (fc_port) {
		/*
		 * 2. Set the port state to online
		 */
		err = spdk_nvmf_fc_port_set_online(fc_port);
		if (err != SPDK_SUCCESS) {
			SPDK_ERRLOG("Hw port %d online failed. rc = %d\n", fc_port->port_hdl, err);
			assert(0 != "HW port online failed");
		}

		/*
		 * 3. Register a poller function to poll the LS queue.
		 */
		hwqp                       = &fc_port->ls_queue;
		(void)spdk_nvmf_hwqp_port_set_online(hwqp);
		spdk_nvmf_fc_add_poller(hwqp);

		/*
		 * 4. Cycle through all the io queues and setup a
		 *    hwqp poller for each.
		 */
		for (i = 0; i < (int)fc_port->max_io_queues; i++) {
			hwqp          = &fc_port->io_queues[i];
			(void)spdk_nvmf_hwqp_port_set_online(hwqp);
			spdk_nvmf_fc_add_poller(hwqp);
		}
	} else {
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = SPDK_ERR_INVALID_ARGS;
	}

	if (cb_func != NULL) {
		(void)cb_func(args->port_handle, SPDK_FC_HW_PORT_ONLINE, args->cb_ctx, err);
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF_FC_ADM, "HW port %d online done, rc = %d.\n", args->port_handle, err);
}

/*
 * Create a Nport.
 */
static void
spdk_fc_nport_create(void *arg1, void *arg2)
{
	struct spdk_nvmf_fc_port  *fc_port = NULL;
	spdk_nport_create_args_t  *args    = (spdk_nport_create_args_t *)arg1;
	fc_callback                cb_func = (fc_callback)arg2;
	struct spdk_nvmf_fc_nport *nport   = NULL;
	spdk_err_t                 err     = SPDK_SUCCESS;
	spdk_err_t                 rc      = SPDK_SUCCESS;

	/*
	 * 1. Get the physical port.
	 */
	fc_port = spdk_nvmf_fc_port_list_get(args->port_handle);
	if (fc_port == NULL) {
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * 2. Check for duplicate initialization.
	 */
	nport = spdk_nvmf_fc_nport_get(args->port_handle, args->nport_handle);
	if (nport != NULL) {
		SPDK_ERRLOG("Duplicate SPDK FC nport %d exists for FC port:%d.\n", args->nport_handle,
			    args->port_handle);
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * 3. Get the memory to instantiate a fc nport.
	 */
	nport = spdk_malloc(sizeof(struct spdk_nvmf_fc_nport));
	if (nport == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for nport %d.\n", args->nport_handle);
		err = SPDK_ERR_NOMEM;
		goto fail;
	}

	/*
	 * 4. Initialize the contents for the nport
	 */
	nvmf_tgt_fc_nport_data_init(nport, args);

	/*
	 * 5. Populate the listening addresses for this nport in the right
	 * app-subsystems.
	 */
	rc = nvmf_tgt_fc_nport_add_listen_addr(nport);
	if (rc) {
		SPDK_ERRLOG("Unable to add the listen addr in the subsystems for nport %d.\n", nport->nport_hdl);
		err = SPDK_ERR_INTERNAL;
		goto fail;
	}

	/*
	 * 6. Add this port to the nport list (per FC port) in the library.
	 */
	(void)spdk_nvmf_fc_port_add_nport(fc_port, nport);

	goto out;

fail:
	if (nport) {
		spdk_free(nport);
	}

out:
	if (cb_func != NULL) {
		(void)cb_func(args->port_handle, SPDK_FC_NPORT_CREATE, args->cb_ctx, err);
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF_FC_ADM, "FC nport %d create done, rc = %d.\n", args->port_handle,
		      err);
}

/*
 * Delete Nport.
 */
static void
spdk_fc_nport_delete(void *arg1, void *arg2)
{
	struct spdk_nvmf_fc_port              *fc_port   = NULL;
	spdk_nport_delete_args_t              *args      = arg1;
	fc_callback                            cb_func   = arg2;
	struct spdk_nvmf_fc_nport             *nport     = NULL;
	struct spdk_nvmf_fc_nport_del_cb_data *cb_data   = NULL;
	struct spdk_nvmf_fc_association       *assoc     = NULL;
	struct spdk_nvmf_fc_rem_port_info     *rem_port  = NULL;
	spdk_err_t                             err       = SPDK_SUCCESS;
	spdk_err_t                             rc        = SPDK_SUCCESS;
	int                                    assoc_err = 0; /* Should move to SPDK error */


	/*
	 * 1. Make sure that the nport exists.
	 */
	nport = spdk_nvmf_fc_nport_get(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d for FC Port: %d.\n", args->nport_handle,
			    args->port_handle);
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * 2. Allocate memory for callback data.
	 */
	cb_data = spdk_malloc(sizeof(struct spdk_nvmf_fc_nport_del_cb_data));
	if (NULL == cb_data) {
		SPDK_ERRLOG("Failed to allocate memory for cb_data %d.\n", args->nport_handle);
		err = SPDK_ERR_NOMEM;
		goto out;
	}

	/*
	 * 3. Begin nport tear down
	 */
	if (nport->nport_state == SPDK_FC_OBJECT_CREATED) {
		err = spdk_nvmf_fc_nport_set_state(nport, SPDK_FC_OBJECT_TO_BE_DELETED);
	} else if (nport->nport_state == SPDK_FC_OBJECT_TO_BE_DELETED) {
		/*
		 * Deletion of this nport already in progress. Register callback
		 * and return.
		 */
		// TODO: Register callback in callback vector.
		goto out;
	} else {
		/* nport partially created/deleted */
		assert(nport->nport_state == SPDK_FC_OBJECT_ZOMBIE);
		assert(0 != "Nport in zombie state");
		err = SPDK_ERR_INTERNAL; /* Revisit this error */
		goto out;
	}

	/*
	 * 4. Remove this nport from the list of nports in HW port
	 */
	(void)spdk_nvmf_fc_port_remove_nport(fc_port, nport);

	/*
	 * 5. Remove this nport from listening addresses across subsystems
	 */
	rc = nvmf_tgt_fc_nport_remove_listen_addr(nport);

	if (SPDK_SUCCESS != rc) {
		err = spdk_nvmf_fc_nport_set_state(nport, SPDK_FC_OBJECT_ZOMBIE);
		SPDK_ERRLOG("Unable to remove the listen addr in the subsystems for nport %d.\n", nport->nport_hdl);
		goto out;
	}

	/*
	 * 6. Delete all the remote ports for the nport
	 */
	/* TODO - Need to do this with a "first" and a "next" accessor function
	 * for completeness. Look at app-subsystem as examples.
	 */
	TAILQ_FOREACH(rem_port, &nport->rem_port_list, link) {
		/* TODO err = spdk_fc_i_t_delete(); */
	}

	cb_data->nport       = nport;
	cb_data->fc_cb_func  = cb_func;
	cb_data->port_handle = args->port_handle;
	cb_data->fc_cb_ctx   = args->cb_ctx;

	/*
	 * 7. Delete the nport if there are no associations associated with it.
	 *    If not, then wait for the cb.
	 */
	if (spdk_nvmf_fc_nport_is_association_empty(nport)) {
		/* No associations to delete, complete the nport deletion */
		nvmf_tgt_fc_delete_nport_assoc_cb(cb_data, 0 /* error */);
		goto out;
	}

	/*
	 * 8. Start the cleanup of all the associations in the library.
	 */
	/* TODO - Need to do this with a "first" and a "next" accessor function
	 * for completeness. Look at app-subsystem as examples.
	 */
	TAILQ_FOREACH(assoc, &nport->fc_associations, link) {
		assoc_err = spdk_nvmf_fc_delete_association(nport, assoc->assoc_id,
				nvmf_tgt_fc_delete_nport_assoc_cb, cb_data);
		if (0 != assoc_err) {
			/*
			 * Mark this association as zombie and continue the
			 * nport delete.
			 */
			err = SPDK_ERR_INTERNAL;
			assert(0 != "Nport's association delete failed");
			(void)spdk_nvmf_fc_assoc_set_state(assoc, SPDK_FC_OBJECT_ZOMBIE);
		}
	}
out:
	/* On failure, execute the callback function now */
	if (err != SPDK_SUCCESS) {
		SPDK_ERRLOG("NPort %d delete failed, error = %d, fc port: %d.\n", args->nport_handle, err,
			    args->port_handle);
		if (cb_data) {
			spdk_free(cb_data);
		}
		if (cb_func != NULL) {
			(void)cb_func(args->port_handle, SPDK_FC_NPORT_DELETE, args->cb_ctx, err);
		}
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_FC_ADM, "NPort %d delete done succesfully, fc port: %d.\n",
			      args->nport_handle, args->port_handle);
	}
}

/*
 * Process an PRLI/IT add.
 */
void
spdk_fc_i_t_add(void *arg1, void *arg2)
{
	spdk_hw_i_t_add_args_t            *args    = arg1;
	fc_callback                        cb_func = (fc_callback)arg2;
	struct spdk_nvmf_fc_nport         *nport   = NULL;
	struct spdk_nvmf_fc_rem_port_info *rport   = NULL;
	spdk_err_t                         err     = SPDK_SUCCESS;

	/*
	 * 1. Make sure the nport port exists.
	 */
	nport = spdk_nvmf_fc_nport_get(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d\n", args->nport_handle);
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * TODO: 2. Check for duplicate i_t_add.
	 */

	/*
	 * 3. Get the memory to instantiate the remote port
	 */
	rport = spdk_malloc(sizeof(struct spdk_nvmf_fc_rem_port_info));
	if (rport == NULL) {
		SPDK_ERRLOG("Memory allocation for rem port failed.\n");
		err = SPDK_ERR_NOMEM;
		goto out;
	}

	/*
	 * 4. Initialize the contents for the rport
	 */
	nvmf_tgt_fc_rport_data_init(rport, args);

	/*
	 * 5. Add remote port to nport
	 */
	(void)spdk_nvmf_fc_nport_add_rem_port(nport, rport);

	/*
	 * TODO: 6. Do we validate the initiators service parameters?
	 */

	/*
	 * 7. Get the targets service parameters from the library
	 * to return back to the driver.
	 */
	args->target_prli_info = spdk_nvmf_fc_get_prli_service_params();

out:
	if (cb_func != NULL) {
		/*
		 * Passing pointer to the args struct as the first argument.
		 * The cb_func should handle this appropriately.
		 */
		(void)cb_func(args->port_handle, SPDK_FC_IT_ADD, args->cb_ctx, err);
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF_FC_ADM, "IT add on nport %d done, rc = %d.\n", args->nport_handle,
		      err);
}

static void
spdk_fc_abts_recv(void *arg1, void *arg2)
{
	spdk_abts_args_t          *args    = arg1;
	fc_callback                cb_func = (fc_callback)arg2;
	struct spdk_nvmf_fc_nport *nport   = NULL;
	spdk_err_t                 err     = SPDK_SUCCESS;

	/*
	 * 1. Make sure the nport port exists.
	 */
	nport = spdk_nvmf_fc_nport_get(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d\n", args->nport_handle);
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * 2. Pass the received ABTS-LS to the library for handling.
	 */
	spdk_nvmf_fc_handle_abts_frame(nport, args->rpi, args->oxid, args->rxid);

out:
	if (cb_func != NULL) {
		/*
		 * Passing pointer to the args struct as the first argument.
		 * The cb_func should handle this appropriately.
		 */
		(void)cb_func(args->port_handle, SPDK_FC_ABTS_RECV, args, err);
	}
	SPDK_TRACELOG(SPDK_TRACE_NVMF_FC_ADM, "FC ABTS received. RPI:%d, Oxid:%d, rxid:%d\n", args->rpi,
		      args->oxid, args->rxid);
}
/* PUBLIC FUNCTIONS FOR DRIVER AND LIBRARY INTERACTIONS - END */


SPDK_LOG_REGISTER_TRACE_FLAG("nvmf_fc_adm", SPDK_TRACE_NVMF_FC_ADM);
