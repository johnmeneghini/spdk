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
#include "nvmf_fc_tgt.h"
#include <nvmf/transport.h>
#include <nvmf/nvmf_internal.h>
#include <spdk/trace.h>
#include <spdk_internal/log.h>
#include <spdk/nvmf_spec.h>
#include <spdk/log.h>
#include <spdk/string.h>

#ifndef DEV_VERIFY
#define DEV_VERIFY assert
#endif

#define SPDK_NVMF_FC_LOG_STR_SIZE 255

static void nvmf_fc_hw_port_init(void *arg1, void *arg2);
static void nvmf_fc_hw_port_link_break(void *arg1, void *arg2);
static void nvmf_fc_hw_port_online(void *arg1, void *arg2);
static void nvmf_fc_hw_port_offline(void *arg1, void *arg2);
static void nvmf_fc_nport_create(void *arg1, void *arg2);
static void nvmf_fc_nport_delete(void *arg1, void *arg2);
static void nvmf_fc_i_t_add(void *arg1, void *arg2);
static void nvmf_fc_i_t_delete(void *arg1, void *arg2);
static void nvmf_fc_abts_recv(void *arg1, void *arg2);
static void nvmf_fc_hw_port_dump(void *arg1, void *arg2);
static void nvmf_fc_hw_port_quiesce_dump_cb(void *ctx, spdk_err_t err);

/* ******************* PRIVATE HELPER FUNCTIONS - BEGIN ************** */

static uint32_t
nvmf_fc_tgt_get_next_lcore(uint32_t prev_core)
{
	int retries		= spdk_env_get_core_count() + 1;
	uint32_t lcore_id	= 0;
	uint64_t lcore_mask	= g_nvmf_tgt.opts.lcore_mask;

	while (retries) {
		retries --;
		lcore_id = spdk_env_get_next_core(prev_core);
		if ((lcore_id == spdk_env_get_master_lcore()) ||
		    (lcore_id == UINT32_MAX) ||
		    (lcore_mask && (!((lcore_mask >> lcore_id) & 0x1)))) {
			prev_core = lcore_id;
			continue;
		}
		return lcore_id;
	}

	return UINT32_MAX;
}

/*
 * Re-initialize the FC-Port after an offline event.
 * Only the queue information needs to be populated. XRI, lcore and other hwqp information remains
 * unchanged after the first initialization.
 *
 */
static spdk_err_t
nvmf_fc_tgt_hw_port_reinit_validate(struct spdk_nvmf_bcm_fc_port *fc_port,
				    spdk_nvmf_bcm_fc_hw_port_init_args_t *args)
{
	spdk_err_t err = SPDK_SUCCESS;
	int        i;

	/* Verify that the port was previously in offline state */
	if (!spdk_nvmf_bcm_fc_port_is_offline(fc_port)) {
		SPDK_ERRLOG("SPDK FC port %d already initialized.\n", args->port_handle);
		err = SPDK_ERR_INVALID_ARGS;
		goto err;
	}

	/* Initialize the LS queue */
	fc_port->ls_queue.queues = args->ls_queue;
	spdk_nvmf_bcm_fc_init_poller_queues(&fc_port->ls_queue);

	/* Initialize the IO queues */
	for (i = 0; i < NVMF_FC_MAX_IO_QUEUES; i++) {
		fc_port->io_queues[i].queues = args->io_queues[i];
		spdk_nvmf_bcm_fc_init_poller_queues(&fc_port->io_queues[i]);
	}

	/* Validate the port information */
	DEV_VERIFY(fc_port->xri_ring);
	DEV_VERIFY(TAILQ_EMPTY(&fc_port->nport_list));
	DEV_VERIFY(fc_port->num_nports == 0);
	if ((fc_port->xri_ring == NULL) || !TAILQ_EMPTY(&fc_port->nport_list) ||
	    (fc_port->num_nports != 0)) {
		err = SPDK_ERR_INTERNAL;
	}
err:
	return err;
}

/* Initializes the data for the creation of a FC-Port object in the SPDK
 * library. The spdk_nvmf_fc_port is a well defined structure that is part of
 * the API to the library. The contents added to this well defined structure
 * is private to each vendors implementation.
 */
static spdk_err_t
nvmf_fc_tgt_hw_port_data_init(struct spdk_nvmf_bcm_fc_port *fc_port,
			      spdk_nvmf_bcm_fc_hw_port_init_args_t *args)
{
	/* Used a high number for the LS HWQP so that it does not clash with the
	 * IO HWQP's and immediately shows a LS queue during tracing.
	 */
#define FC_LS_HWQP_ID 0xff
	uint32_t                    poweroftwo = 1;
	char                        poolname[32];
	struct spdk_nvmf_bcm_fc_xri *ring_xri     = NULL;
	struct spdk_nvmf_bcm_fc_xri *ring_xri_ptr = NULL;
	spdk_err_t                  err        = SPDK_SUCCESS;
	int                         i, rc;
	uint32_t                    lcore_id = UINT32_MAX ;

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
	 * The real usable ring size is count-1 instead of count to
	 * differentiate a free ring from an empty ring
	 */
	snprintf(poolname, sizeof(poolname), "xri_ring:%d", args->port_handle);
	fc_port->xri_ring = spdk_ring_create(poolname, poweroftwo, SPDK_ENV_SOCKET_ID_ANY, 0);
	if (!fc_port->xri_ring) {
		SPDK_ERRLOG("XRI ring alloc failed for port = %d\n", args->port_handle);
		err = SPDK_ERR_NOMEM;
		goto err;
	}

	ring_xri = spdk_calloc(fc_port->xri_count, sizeof(struct spdk_nvmf_bcm_fc_xri));
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
	for (uint32_t count = (NVMF_FC_MAX_IO_QUEUES + 1); count <
	     fc_port->xri_count; count++) {
		ring_xri_ptr->xri = fc_port->xri_base + count;

		/* Since we created the ring with NO flags, this means it is mp-mc safe */
		rc = spdk_ring_enqueue(fc_port->xri_ring, (void *)ring_xri_ptr);
		if (rc == 0) {
			SPDK_ERRLOG("XRI ring buffer enqueue failed at count: %d\n",
				    count);
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
	spdk_nvmf_bcm_fc_init_poller(fc_port, &fc_port->ls_queue);

	/*
	 * Initialize the IO queues.
	 */
	for (i = 0; i < NVMF_FC_MAX_IO_QUEUES; i++) {
		fc_port->io_queues[i].queues = args->io_queues[i];

		/* Initialize the session list on this FC HW Q */
		TAILQ_INIT(&fc_port->io_queues[i].connection_list);
		TAILQ_INIT(&fc_port->io_queues[i].pending_xri_list);

		lcore_id = nvmf_fc_tgt_get_next_lcore(lcore_id);
		if (lcore_id == UINT32_MAX) {
			goto err;
		}

		fc_port->io_queues[i].lcore_id  = lcore_id;
		fc_port->io_queues[i].hwqp_id   = i;
		fc_port->io_queues[i].send_frame_xri = fc_port->xri_base + 1 + i;

		spdk_nvmf_bcm_fc_init_poller(fc_port, &fc_port->io_queues[i]);
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

/*
 * Callback function for HW port link break operation.
 *
 * Notice that this callback is being triggered when spdk_fc_nport_delete()
 * completes, if that spdk_fc_nport_delete() called is issued by
 * nvmf_fc_hw_port_link_break().
 *
 * Since nvmf_fc_hw_port_link_break() can invoke spdk_fc_nport_delete() multiple
 * times (one per nport in the HW port's nport_list), a single call to
 * nvmf_fc_hw_port_link_break() can result in multiple calls to this callback function.
 *
 * As a result, this function only invokes a callback to the caller of
 * nvmf_fc_hw_port_link_break() only when the HW port's nport_list is empty.
 */
static void
nvmf_fc_tgt_hw_port_link_break_cb(uint8_t port_handle,
				  spdk_fc_event_t event_type, void *cb_args, spdk_err_t spdk_err)
{
	spdk_nvmf_bcm_fc_port_link_break_cb_data_t   *offline_cb_args = cb_args;
	spdk_nvmf_bcm_hw_port_link_break_args_t      *offline_args = NULL;
	spdk_nvmf_bcm_fc_callback                     cb_func      = NULL;
	spdk_err_t                                    err          = SPDK_SUCCESS;
	struct spdk_nvmf_bcm_fc_port                 *fc_port      = NULL;
	int                                           num_nports   = 0;
	char                                          log_str[SPDK_NVMF_FC_LOG_STR_SIZE];

	if (SPDK_SUCCESS != spdk_err) {
		DEV_VERIFY(!"port link break cb: spdk_err not success.");
		SPDK_ERRLOG("port link break cb: spdk_err:%d.\n", spdk_err);
		goto out;
	}

	if (!offline_cb_args) {
		DEV_VERIFY(!"port link break cb: port_offline_args is NULL.");
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	offline_args = offline_cb_args->args;
	if (!offline_args) {
		DEV_VERIFY(!"port link break cb: offline_args is NULL.");
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	if (port_handle != offline_args->port_handle) {
		DEV_VERIFY(!"port link break cb: port_handle mismatch.");
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	cb_func = offline_cb_args->cb_func;
	if (!cb_func) {
		DEV_VERIFY(!"port link break cb: cb_func is NULL.");
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	fc_port = spdk_nvmf_bcm_fc_port_list_get(port_handle);
	if (!fc_port) {
		DEV_VERIFY(!"port link break cb: fc_port is NULL.");
		SPDK_ERRLOG("port link break cb: Unable to find port:%d\n",
			    offline_args->port_handle);
		err =  SPDK_ERR_INTERNAL;
		goto out;
	}

	num_nports = fc_port->num_nports;
	if (!TAILQ_EMPTY(&fc_port->nport_list)) {
		/*
		 * Don't call the callback unless all nports have been deleted.
		 */
		goto out;
	}

	if (num_nports != 0) {
		DEV_VERIFY(!"port link break cb: num_nports in non-zero.");
		SPDK_ERRLOG("port link break cb: # of ports should be 0. Instead, num_nports:%d\n",
			    num_nports);
		err =  SPDK_ERR_INTERNAL;
	}

	/*
	 * Since there are no more nports, execute the callback(s).
	 */
	(void)cb_func(port_handle, SPDK_FC_LINK_BREAK,
		      (void *)offline_args->cb_ctx, spdk_err);

out:
	spdk_free(offline_cb_args);

	snprintf(log_str, SPDK_NVMF_FC_LOG_STR_SIZE,
		 "port link break cb: port:%d evt_type:%d num_nports:%d err:%d spdk_err:%d.\n",
		 port_handle, event_type, num_nports, err, spdk_err);

	if (err != SPDK_SUCCESS) {
		SPDK_ERRLOG("%s", log_str);
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "%s", log_str);
	}
	return;
}

/*
 * FC port must have all its nports deleted before transitioning to offline state.
 */
static void
nvmf_fc_tgt_hw_port_offline_nport_delete(struct spdk_nvmf_bcm_fc_port *fc_port)
{
	struct spdk_nvmf_bcm_fc_nport *nport = NULL;
	/* All nports must have been deleted at this point for this fc port */
	DEV_VERIFY(fc_port && TAILQ_EMPTY(&fc_port->nport_list));
	DEV_VERIFY(fc_port->num_nports == 0);
	/* Mark the nport states to be zombie, if they exist */
	if (fc_port && !TAILQ_EMPTY(&fc_port->nport_list)) {
		TAILQ_FOREACH(nport, &fc_port->nport_list, link) {
			(void)spdk_nvmf_bcm_fc_nport_set_state(nport, SPDK_NVMF_BCM_FC_OBJECT_ZOMBIE);
		}
	}
}

/* Initializes the data for the creation of a Nport object in the Library.
 * The spdk_nvmf_bcm_fc_nport is a well defined structure that is part of the API
 * to the library. The contents added to this well defined structure is private
 * to each vendors implementation.
 */
static void
nvmf_fc_tgt_nport_data_init(struct spdk_nvmf_bcm_fc_nport *nport,
			    spdk_nvmf_bcm_fc_nport_create_args_t *args)
{
	nport->nport_hdl = args->nport_handle;
	nport->port_hdl  = args->port_handle;
	nport->nport_state  = SPDK_NVMF_BCM_FC_OBJECT_CREATED;
	nport->fc_nodename  = args->fc_nodename;
	nport->fc_portname  = args->fc_portname;
	nport->d_id         = args->d_id;
	nport->fc_port      = spdk_nvmf_bcm_fc_port_list_get(args->port_handle);

	(void)spdk_nvmf_bcm_fc_nport_set_state(nport, SPDK_NVMF_BCM_FC_OBJECT_CREATED);
	TAILQ_INIT(&nport->rem_port_list);
	nport->rport_count = 0;
	TAILQ_INIT(&nport->fc_associations);
	nport->assoc_count = 0;
}

/*
 * A private implementation that sets up the listening addresses in
 * various subsystems that have the right ACL's for this nport.
 */
static spdk_err_t
nvmf_fc_tgt_nport_add_listen_addr(struct spdk_nvmf_bcm_fc_nport *nport)
{
	return spdk_nvmf_bcm_fc_tgt_add_port(NVMF_BCM_FC_TRANSPORT_NAME, nport);
}

static spdk_err_t
nvmf_fc_tgt_nport_remove_listen_addr(struct spdk_nvmf_bcm_fc_nport *nport)
{
	return spdk_nvmf_bcm_fc_tgt_remove_port(NVMF_BCM_FC_TRANSPORT_NAME, nport);
}

static void
nvmf_fc_tgt_i_t_delete_cb(void *args, uint32_t err)
{
	struct spdk_nvmf_bcm_fc_i_t_del_cb_data  *cb_data     = args;
	struct spdk_nvmf_bcm_fc_nport            *nport       = cb_data->nport;
	struct spdk_nvmf_bcm_fc_remote_port_info *rport       = cb_data->rport;
	spdk_nvmf_bcm_fc_callback                 cb_func     = cb_data->fc_cb_func;
	spdk_err_t                                spdk_err    = SPDK_SUCCESS;
	uint8_t                                   port_handle = cb_data->port_handle;
	uint32_t                                  s_id        = rport->s_id;
	uint32_t                                  rpi         = rport->rpi;
	uint32_t                                  assoc_count = rport->assoc_count;
	uint32_t                                  nport_hdl   = nport->nport_hdl;
	uint32_t                                  d_id        = nport->d_id;
	char                                      log_str[SPDK_NVMF_FC_LOG_STR_SIZE];

	/*
	 * Assert on any delete failure.
	 */
	if (SPDK_SUCCESS != err) {
		DEV_VERIFY(!"Error in IT Delete callback.");
		goto out;
	}

	/* TODO: Execute callbacks from callback vector */
	if (cb_func != NULL) {
		(void)cb_func(port_handle, SPDK_FC_IT_DELETE, cb_data->fc_cb_ctx, spdk_err);
	}

out:
	spdk_free(cb_data);

	snprintf(log_str, SPDK_NVMF_FC_LOG_STR_SIZE,
		 "IT delete assoc_cb on nport %d done, port_handle:%d s_id:%d d_id:%d rpi:%d rport_assoc_count:%d rc = %d.\n",
		 nport_hdl, port_handle, s_id, d_id, rpi, assoc_count, err);

	if (err != SPDK_SUCCESS) {
		SPDK_ERRLOG("%s", log_str);
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "%s", log_str);
	}
}

static void
nvmf_fc_tgt_i_t_delete_assoc_cb(void *args, uint32_t err)
{
	struct spdk_nvmf_bcm_fc_i_t_del_assoc_cb_data *cb_data     = args;
	struct spdk_nvmf_bcm_fc_nport                 *nport       = cb_data->nport;
	struct spdk_nvmf_bcm_fc_remote_port_info      *rport       = cb_data->rport;
	spdk_nvmf_bcm_fc_i_t_delete_assoc_cb_fn        cb_func     = cb_data->cb_func;
	uint32_t                                       s_id        = rport->s_id;
	uint32_t                                       rpi         = rport->rpi;
	uint32_t                                       assoc_count = rport->assoc_count;
	uint32_t                                       nport_hdl   = nport->nport_hdl;
	uint32_t                                       d_id        = nport->d_id;
	char                                       log_str[SPDK_NVMF_FC_LOG_STR_SIZE];

	/*
	 * Assert on any association delete failure. We continue to delete other
	 * associations in promoted builds.
	 */
	if (SPDK_SUCCESS != err) {
		DEV_VERIFY(!"Nport's association delete callback returned error");
		if (nport->assoc_count > 0) {
			nport->assoc_count--;
		}
		if (rport->assoc_count > 0) {
			rport->assoc_count--;
		}
	}

	/*
	 * If this is the last association being deleted for the ITN,
	 * execute the callback(s).
	 */
	if (0 == rport->assoc_count) {
		/* Remove the rport from the remote port list. */
		if (spdk_nvmf_bcm_fc_nport_remove_rem_port(nport, rport) != SPDK_SUCCESS) {
			SPDK_ERRLOG("Error while removing rport from list.\n");
			DEV_VERIFY(!"Error while removing rport from list.");
		}

		/* TODO: Execute callbacks from callback vector */
		if (cb_func != NULL) {
			/*
			 * Callback function is provided by the caller
			 * of nvmf_fc_tgt_i_t_delete_assoc().
			 */
			(void)cb_func(cb_data->cb_ctx, SPDK_SUCCESS);
		}
		spdk_free(rport);
		spdk_free(args);
	}

	snprintf(log_str, SPDK_NVMF_FC_LOG_STR_SIZE,
		 "IT delete assoc_cb on nport %d done, s_id:%d d_id:%d rpi:%d rport_assoc_count:%d err = %d.\n",
		 nport_hdl, s_id, d_id, rpi, assoc_count, err);

	if (err != SPDK_SUCCESS) {
		SPDK_ERRLOG("%s", log_str);
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "%s", log_str);
	}
}

/**
 * Process a IT delete.
 */
static void
nvmf_fc_tgt_i_t_delete_assoc(struct spdk_nvmf_bcm_fc_nport *nport,
			     struct spdk_nvmf_bcm_fc_remote_port_info *rport,
			     spdk_nvmf_bcm_fc_i_t_delete_assoc_cb_fn cb_func,
			     void *cb_ctx)
{
	spdk_err_t                                 err                     = SPDK_SUCCESS;
	struct spdk_nvmf_bcm_fc_association       *assoc                   = NULL;
	int                                        assoc_err               = 0;
	uint32_t                                   num_assoc               = 0;
	uint32_t                                   num_assoc_del_scheduled = 0;
	struct spdk_nvmf_bcm_fc_i_t_del_assoc_cb_data *cb_data             = NULL;
	uint8_t                                    port_hdl                = nport->port_hdl;
	uint32_t                                   s_id                    = rport->s_id;
	uint32_t                                   rpi                     = rport->rpi;
	uint32_t                                   assoc_count             = rport->assoc_count;
	char                                       log_str[SPDK_NVMF_FC_LOG_STR_SIZE];

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "IT delete associations on nport:%d begin.\n",
		      nport->nport_hdl);

	/*
	 * Allocate memory for callback data.
	 * This memory will be freed by the callback function.
	 */
	cb_data = spdk_calloc(1, sizeof(struct spdk_nvmf_bcm_fc_i_t_del_assoc_cb_data));
	if (NULL == cb_data) {
		SPDK_ERRLOG("Failed to allocate memory for cb_data on nport:%d.\n", nport->nport_hdl);
		err = SPDK_ERR_NOMEM;
		goto out;
	}
	cb_data->nport       = nport;
	cb_data->rport       = rport;
	cb_data->port_handle = port_hdl;
	cb_data->cb_func     = cb_func;
	cb_data->cb_ctx      = cb_ctx;

	/*
	 * Delete all associations, if any, related with this ITN/remote_port.
	 */
	TAILQ_FOREACH(assoc, &nport->fc_associations, link) {
		num_assoc++;
		if (assoc->s_id == s_id) {
			assoc_err = spdk_nvmf_bcm_fc_delete_association(nport,
					assoc->assoc_id,
					true /* send discon */,
					false /* send abts */,
					nvmf_fc_tgt_i_t_delete_assoc_cb, cb_data);
			if (SPDK_SUCCESS != assoc_err) {
				/*
				 * Mark this association as zombie.
				 */
				err = SPDK_ERR_INTERNAL;
				DEV_VERIFY(!"Error while deleting association");
				(void)spdk_nvmf_bcm_fc_assoc_set_state(assoc, SPDK_NVMF_BCM_FC_OBJECT_ZOMBIE);
			} else {
				num_assoc_del_scheduled++;
			}
		}
	}

out:
	if ((cb_data) && (num_assoc_del_scheduled == 0)) {
		/*
		 * Since there are no association_delete calls
		 * successfully scheduled, the association_delete
		 * callback function will never be called.
		 * In this case, call the callback function now.
		 */
		nvmf_fc_tgt_i_t_delete_assoc_cb(cb_data, SPDK_SUCCESS);
	}

	snprintf(log_str, SPDK_NVMF_FC_LOG_STR_SIZE,
		 "IT delete associations on nport:%d end. "
		 "s_id:%d rpi:%d assoc_count:%d assoc:%d assoc_del_scheduled:%d rc:%d.\n",
		 port_hdl, s_id, rpi, assoc_count, num_assoc, num_assoc_del_scheduled, err);

	if (err == SPDK_SUCCESS) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "%s", log_str);
	} else {
		SPDK_ERRLOG("%s", log_str);
	}
}

static void
nvmf_fc_tgt_rport_data_init(struct spdk_nvmf_bcm_fc_remote_port_info *rport,
			    spdk_nvmf_bcm_fc_hw_i_t_add_args_t *args)
{
	(void)spdk_nvmf_bcm_fc_rport_set_state(rport, SPDK_NVMF_BCM_FC_OBJECT_CREATED);
	rport->s_id = args->s_id;
	rport->rpi  = args->rpi;
}

static void
nvmf_tgt_fc_queue_quiesce_cb(void *cb_data, spdk_nvmf_bcm_fc_poller_api_ret_t ret)
{
	struct spdk_nvmf_bcm_fc_poller_api_quiesce_queue_args *quiesce_api_data = NULL;
	spdk_nvmf_bcm_fc_hw_port_quiesce_ctx_t                     *port_quiesce_ctx = NULL;
	struct spdk_nvmf_bcm_fc_hwqp                          *hwqp             = NULL;
	struct spdk_nvmf_bcm_fc_port                          *fc_port          = NULL;
	spdk_err_t                                             err              = SPDK_SUCCESS;

	quiesce_api_data           = (struct spdk_nvmf_bcm_fc_poller_api_quiesce_queue_args *)cb_data;
	hwqp                       = quiesce_api_data->hwqp;
	fc_port                    = hwqp->fc_port;
	port_quiesce_ctx           = (spdk_nvmf_bcm_fc_hw_port_quiesce_ctx_t *)quiesce_api_data->ctx;
	spdk_nvmf_bcm_fc_hw_port_quiesce_cb_fn cb_func = port_quiesce_ctx->cb_func;

	/*
	 * Decrement the callback/quiesced queue count.
	 */
	port_quiesce_ctx->quiesce_count--;
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "Queue%d Quiesced\n", quiesce_api_data->hwqp->hwqp_id);

	spdk_free(quiesce_api_data);
	/*
	 * Wait for 17 call backs i.e. NVMF_FC_MAX_IO_QUEUES + LS QUEUE.
	 */
	if (port_quiesce_ctx->quiesce_count > 0) {
		return;
	}

	if (fc_port->hw_port_status == SPDK_FC_PORT_QUIESCED) {
		SPDK_ERRLOG("Port %d already in quiesced state.\n", fc_port->port_hdl);
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "HW port %d  quiesced.\n", fc_port->port_hdl);
		fc_port->hw_port_status = SPDK_FC_PORT_QUIESCED;
	}

	if (cb_func) {
		/*
		 * Callback function for the called of quiesce.
		 */
		cb_func(port_quiesce_ctx->ctx, err);
	}

	/*
	 * Free the context structure.
	 */
	spdk_free(port_quiesce_ctx);

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "HW port %d quiesce done, rc = %d.\n", fc_port->port_hdl,
		      err);
}

static spdk_err_t
nvmf_tgt_fc_hw_queue_quiesce(struct spdk_nvmf_bcm_fc_hwqp *fc_hwqp, void *ctx,
			     spdk_nvmf_bcm_fc_poller_api_cb cb_func)
{
	struct spdk_nvmf_bcm_fc_poller_api_quiesce_queue_args *args;
	spdk_nvmf_bcm_fc_poller_api_ret_t                      rc = SPDK_NVMF_BCM_FC_POLLER_API_SUCCESS;
	spdk_err_t                                             err = SPDK_SUCCESS;

	args = spdk_calloc(1, sizeof(struct spdk_nvmf_bcm_fc_poller_api_quiesce_queue_args));

	if (args == NULL) {
		err = SPDK_ERR_NOMEM;
		SPDK_ERRLOG("Failed to allocate memory for poller quiesce args, hwqp:%d\n", fc_hwqp->hwqp_id);
		goto done;
	}
	args->hwqp            = fc_hwqp;
	args->ctx             = ctx;
	args->cb_info.cb_func = cb_func;
	args->cb_info.cb_data = args;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "Quiesce queue %d\n", fc_hwqp->hwqp_id);
	rc = spdk_nvmf_bcm_fc_poller_api(fc_hwqp->lcore_id, SPDK_NVMF_BCM_FC_POLLER_API_QUIESCE_QUEUE,
					 args);
	if (rc) {
		spdk_free(args);
		err = SPDK_ERR_INTERNAL;
	}

done:
	return err;
}

/*
 * Hw port Quiesce
 */
static spdk_err_t
nvmf_tgt_fc_hw_port_quiesce(struct spdk_nvmf_bcm_fc_port *fc_port, void *ctx,
			    spdk_nvmf_bcm_fc_hw_port_quiesce_cb_fn cb_func)
{
	struct spdk_nvmf_bcm_fc_hw_port_quiesce_ctx *port_quiesce_ctx = NULL;
	int                                     i                = 0;
	spdk_err_t                              err              = SPDK_SUCCESS;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "HW port:%d is being quiesced.\n", fc_port->port_hdl);

	if (fc_port->hw_port_status == SPDK_FC_PORT_QUIESCED) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "Port %d already in quiesced state.\n",
			      fc_port->port_hdl);
		/*
		 * Execute the callback function directly.
		 */
		cb_func(ctx, err);
		goto fail;
	}

	port_quiesce_ctx = spdk_calloc(1, sizeof(spdk_nvmf_bcm_fc_hw_port_quiesce_ctx_t));

	if (port_quiesce_ctx == NULL) {
		err = SPDK_ERR_NOMEM;
		SPDK_ERRLOG("Failed to allocate memory for LS queue quiesce ctx, port:%d\n",
			    fc_port->port_hdl);
		goto fail;
	}

	port_quiesce_ctx->quiesce_count = 0;
	port_quiesce_ctx->ctx           = ctx;
	port_quiesce_ctx->cb_func       = cb_func;

	/*
	 * Quiesce the LS queue.
	 */
	err = nvmf_tgt_fc_hw_queue_quiesce(&fc_port->ls_queue, port_quiesce_ctx,
					   nvmf_tgt_fc_queue_quiesce_cb);
	if (err != SPDK_SUCCESS) {
		SPDK_ERRLOG("Failed to quiesce the LS queue.\n");
		goto fail;
	}
	port_quiesce_ctx->quiesce_count++;

	/*
	 * Quiesce the IO queues.
	 */
	for (i = 0; i < NVMF_FC_MAX_IO_QUEUES; i++) {
		err = nvmf_tgt_fc_hw_queue_quiesce(&fc_port->io_queues[i],
						   port_quiesce_ctx,
						   nvmf_tgt_fc_queue_quiesce_cb);
		if (err != SPDK_SUCCESS) {
			DEV_VERIFY(0);
			SPDK_ERRLOG("Failed to quiesce the IO  queue:%d.\n", fc_port->io_queues[i].hwqp_id);
		}
		port_quiesce_ctx->quiesce_count++;
	}

fail:
	if (port_quiesce_ctx && err != SPDK_SUCCESS) {
		spdk_free(port_quiesce_ctx);
	}
	return err;
}

/*
 * Print content to a text buffer.
 */
static void
nvmf_tgt_fc_dump_buf_print(spdk_nvmf_bcm_fc_queue_dump_info_t *dump_info, char *fmt, ...)
{
	va_list ap;
	int32_t avail;
	int32_t written;
	va_start(ap, fmt);
	uint64_t buffer_size = SPDK_FC_HW_DUMP_BUF_SIZE;

	avail = (int32_t)(buffer_size - dump_info->offset);

	if (avail <= 0) {
		va_end(ap);
		return;
	}
	written = vsnprintf(dump_info->buffer + dump_info->offset, avail, fmt, ap);
	if (written >= avail) {
		dump_info->offset += avail;
	} else {
		dump_info->offset += written;
	}
	va_end(ap);
}

/*
 * Dump queue entry
 */
static void
nvmf_tgt_fc_dump_buffer(spdk_nvmf_bcm_fc_queue_dump_info_t *dump_info, const char *name,
			void *buffer,
			uint32_t size)
{
	uint32_t *dword;
	uint32_t  i;
	uint32_t  count;

	/*
	 * Print a max of 8 dwords per buffer line.
	 */
#define NVMF_TGT_FC_NEWLINE_MOD 8

	/*
	 * Make sure the print data  size is non-zero.
	 */
	count = size / sizeof(uint32_t);
	if (count == 0) {
		return;
	}

	nvmf_tgt_fc_dump_buf_print(dump_info, "%s type=buffer:", (char *)name);
	dword = buffer;

	for (i = 0; i < count; i++) {
		nvmf_tgt_fc_dump_buf_print(dump_info, "%08x", *dword++);
		if ((i % NVMF_TGT_FC_NEWLINE_MOD) == (NVMF_TGT_FC_NEWLINE_MOD - 1)) {
			nvmf_tgt_fc_dump_buf_print(dump_info, "\n");
		}
	}
}

/*
 * Dump queue entries.
 */
static void
nvmf_tgt_fc_dump_queue_entries(spdk_nvmf_bcm_fc_queue_dump_info_t *dump_info, bcm_sli_queue_t *q)
{
#define NVMF_TGT_FC_QDUMP_RADIUS 1
	char     name[64];
	int32_t  index = 0;
	uint8_t *entry = NULL;
	uint32_t i;

	index = q->tail;

	index -= NVMF_TGT_FC_QDUMP_RADIUS;
	if (index < 0) {
		index += q->max_entries;
	}

	/*
	 * Print the NVMF_TGT_FC_QDUMP_RADIUS number of entries before and
	 * the tail index.
	 */
	for (i = 0; i < 2 * NVMF_TGT_FC_QDUMP_RADIUS + 1; i++) {
		bzero(name, sizeof(name));
		(void)snprintf(name, sizeof(name), "\nentry:%d ", index);
		entry = q->address;
		entry += index * q->size;
		nvmf_tgt_fc_dump_buffer(dump_info, name, entry, q->size);
		index++;
		if (index >= q->max_entries) {
			index = 0;
		}
	}
	nvmf_tgt_fc_dump_buf_print(dump_info, "\n");
}

/*
 * Dump the contents of Event Q.
 */
static void
nvmf_tgt_fc_dump_sli_queue(spdk_nvmf_bcm_fc_queue_dump_info_t *dump_info, char *name,
			   bcm_sli_queue_t *q)
{
	nvmf_tgt_fc_dump_buf_print(dump_info,
				   "\nname:%s, head:%" PRIu16 ", tail:%" PRIu16 ", used:%" PRIu16 ", "
				   "posted_limit:%" PRIu32 ", processed_limit:%" PRIu32 ", "
				   "type:%" PRIu16 ", qid:%" PRIu16 ", size:%" PRIu16 ", "
				   "max_entries:%" PRIu16 ", address:%p",
				   name, q->head, q->tail, q->used, q->posted_limit, q->processed_limit,
				   q->type, q->qid, q->size, q->max_entries, q->address);

	nvmf_tgt_fc_dump_queue_entries(dump_info, q);
}

/*
 * Dump the contents of Event Q.
 */
static void
nvmf_tgt_fc_dump_eventq(spdk_nvmf_bcm_fc_queue_dump_info_t *dump_info, char *name, fc_eventq_t *eq)
{
	nvmf_tgt_fc_dump_sli_queue(dump_info, name, &eq->q);
}

/*
 * Dump the contents of Work Q.
 */
static void
nvmf_tgt_fc_dump_wrkq(spdk_nvmf_bcm_fc_queue_dump_info_t *dump_info, char *name, fc_wrkq_t *wq)
{
	nvmf_tgt_fc_dump_sli_queue(dump_info, name, &wq->q);
}

/*
 * Dump the contents of recv Q.
 */
static void
nvmf_tgt_fc_dump_rcvq(spdk_nvmf_bcm_fc_queue_dump_info_t *dump_info, char *name, fc_rcvq_t *rq)
{
	nvmf_tgt_fc_dump_sli_queue(dump_info, name, &rq->q);
}

/*
 * Dump the contents of fc_hwqp.
 */
static void
nvmf_tgt_fc_dump_hwqp(spdk_nvmf_bcm_fc_queue_dump_info_t         *dump_info,
		      struct spdk_nvmf_bcm_fc_hw_queues *hw_queue)
{
	/*
	 * Dump the EQ.
	 */
	nvmf_tgt_fc_dump_eventq(dump_info, "eq", &hw_queue->eq);

	/*
	 * Dump the CQ-WQ.
	 */
	nvmf_tgt_fc_dump_eventq(dump_info, "cq_wq", &hw_queue->cq_wq);

	/*
	 * Dump the CQ-RQ.
	 */
	nvmf_tgt_fc_dump_eventq(dump_info, "cq_rq", &hw_queue->cq_rq);

	/*
	 * Dump the WQ.
	 */
	nvmf_tgt_fc_dump_wrkq(dump_info, "wq", &hw_queue->wq);

	/*
	 * Dump the RQ-HDR.
	 */
	nvmf_tgt_fc_dump_rcvq(dump_info, "rq_hdr", &hw_queue->rq_hdr);

	/*
	 * Dump the RQ-PAYLOAD.
	 */
	nvmf_tgt_fc_dump_rcvq(dump_info, "rq_payload", &hw_queue->rq_payload);
}

/*
 * Dump the hwqps.
 */
static void
nvmf_tgt_fc_dump_all_queues(struct spdk_nvmf_bcm_fc_port *fc_port,
			    spdk_nvmf_bcm_fc_queue_dump_info_t    *dump_info)
{
	struct spdk_nvmf_bcm_fc_hwqp *ls_queue;
	struct spdk_nvmf_bcm_fc_hwqp *io_queue;
	int                           i = 0;

	/*
	 * Dump the LS queue.
	 */
	ls_queue = &fc_port->ls_queue;
	nvmf_tgt_fc_dump_buf_print(dump_info, "\nHW Queue type: LS, HW Queue ID:%d", ls_queue->hwqp_id);
	nvmf_tgt_fc_dump_hwqp(dump_info, &ls_queue->queues);

	/*
	 * Dump the IO queues.
	 */
	for (i = 0; i < NVMF_FC_MAX_IO_QUEUES; i++) {
		io_queue = &fc_port->io_queues[i];
		nvmf_tgt_fc_dump_buf_print(dump_info, "\nHW Queue type: IO, HW Queue ID:%d",
					   io_queue->hwqp_id);
		nvmf_tgt_fc_dump_hwqp(dump_info, &io_queue->queues);
	}
}

/* ******************* PRIVATE HELPER FUNCTIONS - END ************** */

/* ******************* PUBLIC FUNCTIONS FOR DRIVER AND LIBRARY INTERACTIONS - BEGIN ************** */

/*
 * Queue up an event in the SPDK masters event queue.
 * Used by the FC driver to notify the SPDK master of FC related events.
 */
spdk_err_t
spdk_nvmf_bcm_fc_master_enqueue_event(spdk_fc_event_t event_type, void *args,
				      spdk_nvmf_bcm_fc_callback cb_func)
{
	spdk_err_t         err   = SPDK_SUCCESS;
	struct spdk_event *event = NULL;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "Enqueue event %d.\n", event_type);

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
		event = spdk_event_allocate(spdk_env_get_master_lcore(), nvmf_fc_hw_port_init, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;

	case SPDK_FC_HW_PORT_ONLINE:
		event = spdk_event_allocate(spdk_env_get_master_lcore(), nvmf_fc_hw_port_online, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;

	case SPDK_FC_HW_PORT_OFFLINE:
		event = spdk_event_allocate(spdk_env_get_master_lcore(), nvmf_fc_hw_port_offline, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;

	case SPDK_FC_HW_PORT_RESET:
		break;

	case SPDK_FC_NPORT_CREATE:
		event = spdk_event_allocate(spdk_env_get_master_lcore(), nvmf_fc_nport_create, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;

	case SPDK_FC_NPORT_DELETE:
		event = spdk_event_allocate(spdk_env_get_master_lcore(), nvmf_fc_nport_delete, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;

	case SPDK_FC_IT_ADD:
		event = spdk_event_allocate(spdk_env_get_master_lcore(), nvmf_fc_i_t_add, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;

	case SPDK_FC_IT_DELETE:
		event = spdk_event_allocate(spdk_env_get_master_lcore(), nvmf_fc_i_t_delete, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;

	case SPDK_FC_ABTS_RECV:
		event = spdk_event_allocate(spdk_env_get_master_lcore(), nvmf_fc_abts_recv, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;

	case SPDK_FC_LINK_BREAK:
		event = spdk_event_allocate(spdk_env_get_master_lcore(), nvmf_fc_hw_port_link_break, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
		break;
	case SPDK_FC_HW_PORT_DUMP: /* Firmware Dump */
		event = spdk_event_allocate(spdk_env_get_master_lcore(), nvmf_fc_hw_port_dump, args, cb_func);
		if (event == NULL) {
			err = SPDK_ERR_NOMEM;
		}
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
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "Enqueue event %d done successfully \n", event_type);
	} else {
		SPDK_ERRLOG("Enqueue event %d failed, err = %d\n", event_type, err);
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
nvmf_fc_hw_port_init(void *arg1, void *arg2)
{
	struct spdk_nvmf_bcm_fc_port         *fc_port  = NULL;
	spdk_nvmf_bcm_fc_hw_port_init_args_t *args     = (spdk_nvmf_bcm_fc_hw_port_init_args_t *)arg1;
	spdk_nvmf_bcm_fc_callback             cb_func  = (spdk_nvmf_bcm_fc_callback)arg2;
	spdk_err_t                            err      = SPDK_SUCCESS;
	static bool                           ioq_depth_adj_needed = true;

	/*
	 * 1. Check for duplicate initialization.
	 */
	fc_port = spdk_nvmf_bcm_fc_port_list_get(args->port_handle);
	if (fc_port != NULL) {
		/* Port already exists, check if it has to be re-initialized */
		err = nvmf_fc_tgt_hw_port_reinit_validate(fc_port, args);
		goto err;
	}

	/*
	 * 2. Get the memory to instantiate a fc port.
	 */
	fc_port = spdk_calloc(1, sizeof(struct spdk_nvmf_bcm_fc_port));
	if (fc_port == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for fc_port %d.\n", args->port_handle);
		err = SPDK_ERR_NOMEM;
		goto err;
	}

	/*
	 * 3. Initialize the contents for the FC-port
	 */
	err = nvmf_fc_tgt_hw_port_data_init(fc_port, args);

	if (err != SPDK_SUCCESS) {
		SPDK_ERRLOG("Data initialization failed for fc_port %d.\n", args->port_handle);
		DEV_VERIFY(!"Data initialization failed for fc_port");
		goto err;
	}

	/*
	 * 4. Add this port to the global fc port list in the library.
	 */
	spdk_nvmf_bcm_fc_port_list_add(fc_port);

	/*
	 * 5. Update the g_nvmf_tgt.max_queue_depth (if necessary)
	 *    max_queue_depth is used to set MQES property
	 */

	if (ioq_depth_adj_needed) {
		g_nvmf_tgt.opts.max_queue_depth =
			spdk_nvmf_bcm_fc_calc_max_q_depth(
				fc_port->max_io_queues,
				fc_port->io_queues[0].queues.
				rq_payload.num_buffers,
				g_nvmf_tgt.opts.max_associations,
				g_nvmf_tgt.opts.max_queues_per_session,
				g_nvmf_tgt.opts.max_aq_depth);

		ioq_depth_adj_needed = false;
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "MAX SQ size=%d.\n",
			      g_nvmf_tgt.opts.max_queue_depth);
	}

err:
	if (err && fc_port) {
		spdk_free(fc_port);
	}
	if (cb_func != NULL) {
		(void)cb_func(args->port_handle, SPDK_FC_HW_PORT_INIT, args->cb_ctx, err);
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "HW port %d initialize done, rc = %d.\n",
		      args->port_handle, err);
}

/*
 * Online a HW port.
 */
static void
nvmf_fc_hw_port_online(void *arg1, void *arg2)
{
	struct spdk_nvmf_bcm_fc_port           *fc_port = NULL;
	struct spdk_nvmf_bcm_fc_hwqp           *hwqp    = NULL;
	spdk_nvmf_bcm_fc_hw_port_online_args_t *args    = (spdk_nvmf_bcm_fc_hw_port_online_args_t *) arg1;
	spdk_nvmf_bcm_fc_callback               cb_func = (spdk_nvmf_bcm_fc_callback)arg2;
	int                                     i       = 0;
	spdk_err_t                              err     = SPDK_SUCCESS;

	/*
	 * 1. Get the port from the library
	 */
	fc_port = spdk_nvmf_bcm_fc_port_list_get(args->port_handle);
	if (fc_port) {
		/*
		 * 2. Set the port state to online
		 */
		err = spdk_nvmf_bcm_fc_port_set_online(fc_port);
		if (err != SPDK_SUCCESS) {
			SPDK_ERRLOG("Hw port %d online failed. err = %d\n", fc_port->port_hdl, err);
			DEV_VERIFY(!"Hw port online failed");
			goto out;
		}

		/*
		 * 3. Register a poller function to poll the LS queue.
		 */
		hwqp = &fc_port->ls_queue;
		(void)spdk_nvmf_bcm_fc_hwqp_port_set_online(hwqp);
		spdk_nvmf_bcm_fc_add_poller(hwqp);

		/*
		 * 4. Cycle through all the io queues and setup a
		 *    hwqp poller for each.
		 */
		for (i = 0; i < (int)fc_port->max_io_queues; i++) {
			hwqp = &fc_port->io_queues[i];
			(void)spdk_nvmf_bcm_fc_hwqp_port_set_online(hwqp);
			spdk_nvmf_bcm_fc_add_poller(hwqp);
		}
	} else {
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = SPDK_ERR_INVALID_ARGS;
	}

out:
	if (cb_func != NULL) {
		(void)cb_func(args->port_handle, SPDK_FC_HW_PORT_ONLINE, args->cb_ctx, err);
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "HW port %d online done, rc = %d.\n", args->port_handle,
		      err);
}

/*
 * Offline a HW port.
 */
static void
nvmf_fc_hw_port_offline(void *arg1, void *arg2)
{
	struct spdk_nvmf_bcm_fc_port *fc_port = NULL;
	struct spdk_nvmf_bcm_fc_hwqp *hwqp    = NULL;
	spdk_nvmf_bcm_fc_hw_port_offline_args_t *args    = (spdk_nvmf_bcm_fc_hw_port_offline_args_t *)arg1;
	spdk_nvmf_bcm_fc_callback     cb_func = (spdk_nvmf_bcm_fc_callback)arg2;
	int                           i       = 0;
	spdk_err_t                    err     = SPDK_SUCCESS;

	/*
	 * 1. Get the fc port using the port handle.
	 */
	fc_port = spdk_nvmf_bcm_fc_port_list_get(args->port_handle);
	if (fc_port) {
		/*
		 * 2. Set the port state to offline, if it is not already.
		 */
		err = spdk_nvmf_bcm_fc_port_set_offline(fc_port);
		if (err != SPDK_SUCCESS) {
			SPDK_ERRLOG("Hw port %d already offline. err = %d\n", fc_port->port_hdl, err);
			err = SPDK_SUCCESS;
			goto out;
		}

		/*
		 * 3. Remove poller for the LS queue.
		 */
		hwqp = &fc_port->ls_queue;
		(void)spdk_nvmf_bcm_fc_hwqp_port_set_offline(hwqp);
		spdk_nvmf_bcm_fc_delete_poller(hwqp);

		/*
		 * 4. Remove poller for all the io queues.
		 */
		for (i = 0; i < (int)fc_port->max_io_queues; i++) {
			hwqp = &fc_port->io_queues[i];
			(void)spdk_nvmf_bcm_fc_hwqp_port_set_offline(hwqp);
			spdk_nvmf_bcm_fc_delete_poller(hwqp);
		}

		/*
		 * 5. Delete all the nports. Ideally, the nports should have
		 * been purged before the offline event, in which case,
		 * only a validation is required.
		 */
		nvmf_fc_tgt_hw_port_offline_nport_delete(fc_port);
	} else {
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = SPDK_ERR_INVALID_ARGS;
	}
out:
	if (cb_func != NULL) {
		(void)cb_func(args->port_handle, SPDK_FC_HW_PORT_OFFLINE, args->cb_ctx, err);
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "HW port %d offline done, rc = %d.\n", args->port_handle,
		      err);
}

/*
 * Create a Nport.
 */
static void
nvmf_fc_nport_create(void *arg1, void *arg2)
{
	struct spdk_nvmf_bcm_fc_port         *fc_port = NULL;
	spdk_nvmf_bcm_fc_nport_create_args_t *args    = (spdk_nvmf_bcm_fc_nport_create_args_t *)arg1;
	spdk_nvmf_bcm_fc_callback             cb_func = (spdk_nvmf_bcm_fc_callback)arg2;
	struct spdk_nvmf_bcm_fc_nport         *nport   = NULL;
	spdk_err_t                             err     = SPDK_SUCCESS;
	spdk_err_t                            rc      = SPDK_SUCCESS;

	/*
	 * 1. Get the physical port.
	 */
	fc_port = spdk_nvmf_bcm_fc_port_list_get(args->port_handle);
	if (fc_port == NULL) {
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * 2. Check for duplicate initialization.
	 */
	nport = spdk_nvmf_bcm_fc_nport_get(args->port_handle, args->nport_handle);
	if (nport != NULL) {
		SPDK_ERRLOG("Duplicate SPDK FC nport %d exists for FC port:%d.\n", args->nport_handle,
			    args->port_handle);
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * 3. Get the memory to instantiate a fc nport.
	 */
	nport = spdk_calloc(1, sizeof(struct spdk_nvmf_bcm_fc_nport));
	if (nport == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for nport %d.\n",
			    args->nport_handle);
		err = SPDK_ERR_NOMEM;
		goto out;
	}

	/*
	 * 4. Initialize the contents for the nport
	 */
	nvmf_fc_tgt_nport_data_init(nport, args);

	/*
	 * 5. Populate the listening addresses for this nport in the right
	 * app-subsystems.
	 */
	rc = nvmf_fc_tgt_nport_add_listen_addr(nport);
	if (rc) {
		SPDK_ERRLOG("Unable to add the listen addr in the subsystems for nport %d.\n",
			    nport->nport_hdl);
		err = SPDK_ERR_INTERNAL;
		goto out;
	}

	/*
	 * 6. Add this port to the nport list (per FC port) in the library.
	 */
	(void)spdk_nvmf_bcm_fc_port_add_nport(fc_port, nport);

out:
	if (err && nport) {
		spdk_free(nport);
	}

	if (cb_func != NULL) {
		(void)cb_func(args->port_handle, SPDK_FC_NPORT_CREATE, args->cb_ctx, err);
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "FC nport %d create done, rc = %d.\n", args->port_handle,
		      err);
}

static void
nvmf_fc_tgt_delete_nport_cb(uint8_t port_handle, spdk_fc_event_t event_type,
			    void *cb_args, spdk_err_t spdk_err)
{
	struct spdk_nvmf_bcm_fc_nport_del_cb_data *cb_data     = cb_args;
	struct spdk_nvmf_bcm_fc_nport             *nport       = cb_data->nport;
	spdk_nvmf_bcm_fc_callback                  cb_func     = cb_data->fc_cb_func;
	uint32_t                                   assoc_count = 0;
	spdk_err_t                                 err         = SPDK_SUCCESS;
	uint16_t                                   nport_hdl   = 0;
	char                                       log_str[SPDK_NVMF_FC_LOG_STR_SIZE];

	/*
	 * Assert on any delete failure.
	 */
	if (nport == NULL) {
		SPDK_ERRLOG("Nport delete callback returned null nport");
		DEV_VERIFY(!"nport is null.");
		goto out;
	}

	nport_hdl = nport->nport_hdl;
	if (SPDK_SUCCESS != spdk_err) {
		SPDK_ERRLOG("Nport delete callback returned error. FC Port: "
			    "%d, Nport: %d\n",
			    nport->port_hdl, nport->nport_hdl);
		DEV_VERIFY(!"nport delete callback error.");
	}

	/*
	 * Free the nport if this is the last rport being deleted and
	 * execute the callback(s).
	 */
	if (spdk_nvmf_bcm_fc_nport_is_rport_empty(nport)) {
		assoc_count = spdk_nvmf_bcm_fc_nport_get_association_count(nport);
		if (0 != assoc_count) {
			SPDK_ERRLOG("association count != 0\n");
			DEV_VERIFY(!"association count != 0");
		}

		err = spdk_nvmf_bcm_fc_port_remove_nport(nport->fc_port, nport);
		if (SPDK_SUCCESS != err) {
			SPDK_ERRLOG("Nport delete callback: Failed to remove "
				    "nport from nport list. FC Port:%d Nport:%d\n",
				    nport->port_hdl, nport->nport_hdl);
		}
		/* Free the nport */
		spdk_free(nport);

		/* TODO: Execute callbacks from callback vector */
		if (cb_func != NULL) {
			(void)cb_func(cb_data->port_handle, SPDK_FC_NPORT_DELETE, cb_data->fc_cb_ctx, spdk_err);
		}
		spdk_free(cb_data);
	}
out:
	snprintf(log_str, SPDK_NVMF_FC_LOG_STR_SIZE,
		 "port:%d nport:%d delete cb exit, evt_type:%d rc:%d.\n",
		 port_handle, nport_hdl, event_type, spdk_err);

	if (err != SPDK_SUCCESS) {
		SPDK_ERRLOG("%s", log_str);
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "%s", log_str);
	}
}

/*
 * Delete Nport.
 */
static void
nvmf_fc_nport_delete(void *arg1, void *arg2)
{
	spdk_nvmf_bcm_fc_nport_delete_args_t      *args        = arg1;
	spdk_nvmf_bcm_fc_callback                  cb_func     = arg2;
	struct spdk_nvmf_bcm_fc_nport             *nport       = NULL;
	struct spdk_nvmf_bcm_fc_nport_del_cb_data *cb_data     = NULL;
	struct spdk_nvmf_bcm_fc_remote_port_info *rport_iter  = NULL;
	spdk_err_t                                err         = SPDK_SUCCESS;
	uint32_t                                  rport_cnt   = 0;
	spdk_nvmf_bcm_fc_hw_i_t_delete_args_t    *it_del_args = NULL;
	struct spdk_event                        *spdk_evt    = NULL;
	spdk_err_t                                 rc          = SPDK_SUCCESS;

	/*
	 * 1. Make sure that the nport exists.
	 */
	nport = spdk_nvmf_bcm_fc_nport_get(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d for FC Port: %d.\n", args->nport_handle,
			    args->port_handle);
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * 2. Allocate memory for callback data.
	 */
	cb_data = spdk_calloc(1, sizeof(struct spdk_nvmf_bcm_fc_nport_del_cb_data));
	if (NULL == cb_data) {
		SPDK_ERRLOG("Failed to allocate memory for cb_data %d.\n", args->nport_handle);
		err = SPDK_ERR_NOMEM;
		goto out;
	}

	cb_data->nport       = nport;
	cb_data->port_handle = args->port_handle;
	cb_data->fc_cb_func  = cb_func;
	cb_data->fc_cb_ctx   = args->cb_ctx;

	/*
	 * 3. Begin nport tear down
	 */
	if (nport->nport_state == SPDK_NVMF_BCM_FC_OBJECT_CREATED) {
		(void)spdk_nvmf_bcm_fc_nport_set_state(nport, SPDK_NVMF_BCM_FC_OBJECT_TO_BE_DELETED);
	} else if (nport->nport_state == SPDK_NVMF_BCM_FC_OBJECT_TO_BE_DELETED) {
		/*
		 * Deletion of this nport already in progress. Register callback
		 * and return.
		 */
		// TODO: Register callback in callback vector. For now, set the error and return.
		err = SPDK_ERR_INTERNAL;
		goto out;
	} else {
		/* nport partially created/deleted */
		DEV_VERIFY(nport->nport_state == SPDK_NVMF_BCM_FC_OBJECT_ZOMBIE);
		DEV_VERIFY(0 != "Nport in zombie state");
		err = SPDK_ERR_INTERNAL; /* Revisit this error */
		goto out;
	}

	/*
	 * 4. Remove this nport from listening addresses across subsystems
	 */
	rc = nvmf_fc_tgt_nport_remove_listen_addr(nport);

	if (SPDK_SUCCESS != rc) {
		err = spdk_nvmf_bcm_fc_nport_set_state(nport, SPDK_NVMF_BCM_FC_OBJECT_ZOMBIE);
		SPDK_ERRLOG("Unable to remove the listen addr in the subsystems for nport %d.\n",
			    nport->nport_hdl);
		goto out;
	}

	/*
	 * 5. Delete all the remote ports (if any) for the nport
	 */
	/* TODO - Need to do this with a "first" and a "next" accessor function
	 * for completeness. Look at app-subsystem as examples.
	 */
	if (spdk_nvmf_bcm_fc_nport_is_rport_empty(nport)) {
		/* No rports to delete. Complete the nport deletion. */
		nvmf_fc_tgt_delete_nport_cb(nport->port_hdl, SPDK_FC_NPORT_DELETE, cb_data, SPDK_SUCCESS);
		goto out;
	}

	TAILQ_FOREACH(rport_iter, &nport->rem_port_list, link) {
		rport_cnt++;
		it_del_args               = spdk_calloc(1, sizeof(spdk_nvmf_bcm_fc_hw_i_t_delete_args_t));

		if (it_del_args == NULL) {
			err = SPDK_ERR_NOMEM;
			SPDK_ERRLOG("SPDK_FC_IT_DELETE failed for rport with rpi:%d s_id:%d.\n",
				    rport_iter->rpi, rport_iter->s_id);
			DEV_VERIFY(!"SPDK_FC_IT_DELETE failed, cannot allocate memory");
			continue;
		}

		it_del_args->port_handle  = nport->port_hdl;
		it_del_args->nport_handle = nport->nport_hdl;
		it_del_args->cb_ctx       = (void *)cb_data;
		it_del_args->rpi          = rport_iter->rpi;
		it_del_args->s_id         = rport_iter->s_id;

		spdk_evt = spdk_event_allocate(spdk_env_get_master_lcore(), nvmf_fc_i_t_delete,
					       (void *)it_del_args, nvmf_fc_tgt_delete_nport_cb);
		if (spdk_evt == NULL) {
			err = SPDK_ERR_NOMEM;
			SPDK_ERRLOG("SPDK_FC_IT_DELETE failed for rport with "
				    "rpi:%d s_id:%d.\n",
				    it_del_args->rpi, it_del_args->s_id);
			spdk_free(it_del_args);
			DEV_VERIFY(!"SPDK_FC_IT_DELETE failed");
		} else {
			spdk_event_call(spdk_evt);
		}
	}

out:
	/* On failure, execute the callback function now */
	if ((err != SPDK_SUCCESS) || (rc != SPDK_SUCCESS)) {
		SPDK_ERRLOG("NPort %d delete failed, error:%d, fc port:%d, "
			    "rport_cnt:%d rc:%d.\n",
			    args->nport_handle, err, args->port_handle,
			    rport_cnt, rc);
		if (cb_data) {
			spdk_free(cb_data);
		}
		if (cb_func != NULL) {
			(void)cb_func(args->port_handle, SPDK_FC_NPORT_DELETE, args->cb_ctx, err);
		}
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM,
			      "NPort %d delete done succesfully, fc port:%d. "
			      "rport_cnt:%d\n",
			      args->nport_handle, args->port_handle, rport_cnt);
	}
}

/*
 * Process an PRLI/IT add.
 */
static void
nvmf_fc_i_t_add(void *arg1, void *arg2)
{
	spdk_nvmf_bcm_fc_hw_i_t_add_args_t       *args       = arg1;
	spdk_nvmf_bcm_fc_callback                 cb_func    = (spdk_nvmf_bcm_fc_callback) arg2;
	struct spdk_nvmf_bcm_fc_nport            *nport      = NULL;
	struct spdk_nvmf_bcm_fc_remote_port_info *rport_iter = NULL;
	struct spdk_nvmf_bcm_fc_remote_port_info *rport      = NULL;
	spdk_err_t                                err        = SPDK_SUCCESS;

	/*
	 * 1. Make sure the nport port exists.
	 */
	nport = spdk_nvmf_bcm_fc_nport_get(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d\n", args->nport_handle);
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * TODO: 2. Check for duplicate i_t_add.
	 */
	TAILQ_FOREACH(rport_iter, &nport->rem_port_list, link) {
		if ((rport_iter->s_id == args->s_id) && (rport_iter->rpi == args->rpi)) {
			SPDK_ERRLOG("Duplicate rport found for FC nport %d: sid:%d rpi:%d\n",
				    args->nport_handle, rport_iter->s_id, rport_iter->rpi);
			err = SPDK_ERR_INTERNAL;
			goto out;
		}
	}

	/*
	 * 3. Get the memory to instantiate the remote port
	 */
	rport = spdk_calloc(1, sizeof(struct spdk_nvmf_bcm_fc_remote_port_info));
	if (rport == NULL) {
		SPDK_ERRLOG("Memory allocation for rem port failed.\n");
		err = SPDK_ERR_NOMEM;
		goto out;
	}

	/*
	 * 4. Initialize the contents for the rport
	 */
	nvmf_fc_tgt_rport_data_init(rport, args);

	/*
	 * 5. Add remote port to nport
	 */
	if (spdk_nvmf_bcm_fc_nport_add_rem_port(nport, rport) != SPDK_SUCCESS) {
		DEV_VERIFY(!"Error while adding rport to list");
	};

	/*
	 * TODO: 6. Do we validate the initiators service parameters?
	 */

	/*
	 * 7. Get the targets service parameters from the library
	 * to return back to the driver.
	 */
	args->target_prli_info = spdk_nvmf_bcm_fc_get_prli_service_params();

out:
	if (cb_func != NULL) {
		/*
		 * Passing pointer to the args struct as the first argument.
		 * The cb_func should handle this appropriately.
		 */
		(void)cb_func(args->port_handle, SPDK_FC_IT_ADD, args->cb_ctx, err);
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM,
		      "IT add on nport %d done, rc = %d.\n",
		      args->nport_handle, err);
}

/**
 * Process a IT delete.
 */
static void
nvmf_fc_i_t_delete(void *arg1, void *arg2)
{
	spdk_err_t                                 rc        = SPDK_SUCCESS;
	spdk_nvmf_bcm_fc_hw_i_t_delete_args_t     *args      = NULL;
	spdk_nvmf_bcm_fc_callback                  cb_func   = NULL;
	struct spdk_nvmf_bcm_fc_nport             *nport     = NULL;
	struct spdk_nvmf_bcm_fc_i_t_del_cb_data  *cb_data    = NULL;
	struct spdk_nvmf_bcm_fc_remote_port_info *rport_iter = NULL;
	struct spdk_nvmf_bcm_fc_remote_port_info *rport      = NULL;
	uint32_t                                  num_rport  = 0;
	char                                      log_str[SPDK_NVMF_FC_LOG_STR_SIZE];

	args    = (spdk_nvmf_bcm_fc_hw_i_t_delete_args_t *)arg1;
	cb_func = (spdk_nvmf_bcm_fc_callback)arg2;
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "IT delete on nport:%d begin.\n", args->nport_handle);

	/*
	 * Make sure the nport port exists. If it does not, error out.
	 */
	nport = spdk_nvmf_bcm_fc_nport_get(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport:%d\n", args->nport_handle);
		rc = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * Find this ITN / rport (remote port).
	 */
	TAILQ_FOREACH(rport_iter, &nport->rem_port_list, link) {
		num_rport++;
		if ((rport_iter->s_id == args->s_id) &&
		    (rport_iter->rpi == args->rpi) &&
		    (rport_iter->rport_state == SPDK_NVMF_BCM_FC_OBJECT_CREATED)) {
			rport = rport_iter;
			break;
		}
	}

	/*
	 * We should find either zero or exactly one rport.
	 *
	 * If we find zero rports, that means that a previous request has
	 * removed the rport by the time we reached here. In this case,
	 * simply return out.
	 */
	if (rport == NULL) {
		rc = SPDK_ERR_INTERNAL;
		goto out;
	}

	/*
	 * We have found exactly one rport. Allocate memory for callback data.
	 */
	cb_data = spdk_calloc(1, sizeof(struct spdk_nvmf_bcm_fc_i_t_del_cb_data));
	if (NULL == cb_data) {
		SPDK_ERRLOG("Failed to allocate memory for cb_data for nport:%d.\n", args->nport_handle);
		rc = SPDK_ERR_NOMEM;
		goto out;
	}

	cb_data->nport       = nport;
	cb_data->rport       = rport;
	cb_data->port_handle = args->port_handle;
	cb_data->fc_cb_func  = cb_func;
	cb_data->fc_cb_ctx   = args->cb_ctx;

	/*
	 * Validate rport object state.
	 */
	if (rport->rport_state == SPDK_NVMF_BCM_FC_OBJECT_CREATED) {
		(void)spdk_nvmf_bcm_fc_rport_set_state(rport, SPDK_NVMF_BCM_FC_OBJECT_TO_BE_DELETED);
	} else if (rport->rport_state == SPDK_NVMF_BCM_FC_OBJECT_TO_BE_DELETED) {
		/*
		 * Deletion of this rport already in progress. Register callback
		 * and return.
		 */
		// TODO: Register callback in callback vector. For now, set the error and return.
		rc = SPDK_ERR_INTERNAL;
		goto out;
	} else {
		/* rport partially created/deleted */
		DEV_VERIFY(rport->rport_state == SPDK_NVMF_BCM_FC_OBJECT_ZOMBIE);
		DEV_VERIFY(!"Invalid rport_state");
		rc = SPDK_ERR_INTERNAL; /* Revisit this error */
		goto out;
	}

	/*
	 * We have successfully found a rport to delete. Call
	 * nvmf_fc_tgt_i_t_delete_assoc(), which will perform further
	 * IT-delete processing as well as free the cb_data.
	 */
	nvmf_fc_tgt_i_t_delete_assoc(nport, rport, nvmf_fc_tgt_i_t_delete_cb,
				     (void *)cb_data);

out:
	if (rc != SPDK_SUCCESS) {
		/*
		 * We have entered here because either we encountered an
		 * error, or we did not find a rport to delete.
		 * As a result, we will not call the function
		 * nvmf_fc_tgt_i_t_delete_assoc() for further IT-delete
		 * processing. Therefore, execute the callback function now.
		 */
		if (cb_data) {
			spdk_free(cb_data);
		}
		if (cb_func != NULL) {
			(void)cb_func(args->port_handle, SPDK_FC_IT_DELETE, args->cb_ctx, rc);
		}
	}

	snprintf(log_str, SPDK_NVMF_FC_LOG_STR_SIZE,
		 "IT delete on nport:%d end. num_rport:%d rc = %d.\n",
		 args->nport_handle, num_rport, rc);

	if (rc != SPDK_SUCCESS) {
		SPDK_ERRLOG("%s", log_str);
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "%s", log_str);
	}
}

/*
 * Process ABTS received
 */
static void
nvmf_fc_abts_recv(void *arg1, void *arg2)
{
	spdk_nvmf_bcm_fc_abts_args_t  *args    = arg1;
	spdk_nvmf_bcm_fc_callback              cb_func = (spdk_nvmf_bcm_fc_callback)arg2;
	struct spdk_nvmf_bcm_fc_nport *nport   = NULL;
	spdk_err_t                     err     = SPDK_SUCCESS;

	/*
	 * 1. Make sure the nport port exists.
	 */
	nport = spdk_nvmf_bcm_fc_nport_get(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d\n", args->nport_handle);
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * 2. Pass the received ABTS-LS to the library for handling.
	 */
	spdk_nvmf_bcm_fc_handle_abts_frame(nport, args->rpi, args->oxid, args->rxid);

out:
	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "FC ABTS received. RPI:%d, oxid:%d, rxid:%d\n", args->rpi,
		      args->oxid, args->rxid);
	if (cb_func != NULL) {
		/*
		 * Passing pointer to the args struct as the first argument.
		 * The cb_func should handle this appropriately.
		 */
		(void)cb_func(args->port_handle, SPDK_FC_ABTS_RECV, args, err);
	} else {
		/* No callback set, free the args */
		spdk_free(args);
	}
}

/*
 * Callback function for hw port quiesce.
 */
static void
nvmf_fc_hw_port_quiesce_dump_cb(void *ctx, spdk_err_t err)
{
	spdk_nvmf_bcm_fc_hw_port_dump_ctx_t   *dump_ctx = (spdk_nvmf_bcm_fc_hw_port_dump_ctx_t *)ctx;
	spdk_nvmf_bcm_fc_hw_port_dump_args_t     *args     = dump_ctx->dump_args;
	spdk_nvmf_bcm_fc_callback                   cb_func  = dump_ctx->dump_cb_func;
	spdk_nvmf_bcm_fc_queue_dump_info_t     dump_info;
	struct spdk_nvmf_bcm_fc_port *fc_port       = NULL;
	char                         *dump_buf      = NULL;
	uint32_t                      dump_buf_size = SPDK_FC_HW_DUMP_BUF_SIZE;

	/*
	 * Free the callback context struct.
	 */
	spdk_free(ctx);

	if (err != SPDK_SUCCESS) {
		SPDK_ERRLOG("Port %d  quiesce operation failed.\n", args->port_handle);
		goto out;
	}

	/*
	 * Get the fc port.
	 */
	fc_port = spdk_nvmf_bcm_fc_port_list_get(args->port_handle);
	if (fc_port == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * Allocate memory for the dump buffer.
	 * This memory will be freed by FCT.
	 */
	dump_buf = (char *)spdk_calloc(1, dump_buf_size);
	if (dump_buf == NULL) {
		err = SPDK_ERR_NOMEM;
		SPDK_ERRLOG("Memory allocation for dump buffer failed, SPDK FC port %d\n", args->port_handle);
		goto out;
	}
	*args->dump_buf  = (uint32_t *)dump_buf;
	dump_info.buffer = dump_buf;
	dump_info.offset = 0;

	/*
	 * Add the dump reason to the top of the buffer.
	 */
	nvmf_tgt_fc_dump_buf_print(&dump_info, "%s\n", args->reason);

	/*
	 * Dump the hwqp.
	 */
	nvmf_tgt_fc_dump_all_queues(fc_port, &dump_info);

out:
	if (cb_func != NULL) {
		(void)cb_func(args->port_handle, SPDK_FC_HW_PORT_DUMP, args->cb_ctx, err);
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "HW port %d dump done, rc = %d.\n", args->port_handle,
		      err);
}

/*
 * HW port dump
 */
static void
nvmf_fc_hw_port_dump(void *arg1, void *arg2)
{
	struct spdk_nvmf_bcm_fc_port *fc_port = NULL;
	spdk_nvmf_bcm_fc_hw_port_dump_args_t     *args    = (spdk_nvmf_bcm_fc_hw_port_dump_args_t *)arg1;
	spdk_nvmf_bcm_fc_callback     cb_func = (spdk_nvmf_bcm_fc_callback)arg2;
	spdk_nvmf_bcm_fc_hw_port_dump_ctx_t   *ctx     = NULL;
	spdk_err_t                    err     = SPDK_SUCCESS;

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "HW port %d dump\n", args->port_handle);

	/*
	 * Make sure the physical port exists.
	 */
	fc_port = spdk_nvmf_bcm_fc_port_list_get(args->port_handle);
	if (fc_port == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * Save the dump event args and the callback in a context struct.
	 */
	ctx = spdk_calloc(1, sizeof(spdk_nvmf_bcm_fc_hw_port_dump_ctx_t));

	if (ctx == NULL) {
		err = SPDK_ERR_NOMEM;
		SPDK_ERRLOG("Memory allocation for dump ctx failed, SPDK FC port %d\n", args->port_handle);
		goto fail;
	}

	bzero(ctx, sizeof(spdk_nvmf_bcm_fc_hw_port_dump_ctx_t));
	ctx->dump_args    = (void *)arg1;
	ctx->dump_cb_func = cb_func;

	/*
	 * Quiesce the hw port.
	 */
	err = nvmf_tgt_fc_hw_port_quiesce(fc_port, ctx, nvmf_fc_hw_port_quiesce_dump_cb);
	if (err != SPDK_SUCCESS) {
		goto fail;
	}

	/*
	 * Once the ports are successfully quiesced the dump processing
	 * will continue in the callback function: spdk_fc_port_quiesce_dump_cb
	 */
	return;
fail:
	if (ctx) {
		spdk_free(ctx);
	}

out:
	if (cb_func != NULL) {
		(void)cb_func(args->port_handle, SPDK_FC_HW_PORT_DUMP, args->cb_ctx, err);
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "HW port %d dump done, rc = %d.\n", args->port_handle,
		      err);
}

/*
 * Process a link break event on a HW port.
 */
static void
nvmf_fc_hw_port_link_break(void *arg1, void *arg2)
{
	struct spdk_nvmf_bcm_fc_port   *fc_port = NULL;
	struct spdk_nvmf_bcm_fc_hwqp   *hwqp    = NULL;
	spdk_nvmf_bcm_hw_port_link_break_args_t *args    = (spdk_nvmf_bcm_hw_port_link_break_args_t *)arg1;
	spdk_nvmf_bcm_fc_callback       cb_func = (spdk_nvmf_bcm_fc_callback)arg2;
	int                             i       = 0;
	spdk_err_t                      err     = SPDK_SUCCESS;
	struct spdk_nvmf_bcm_fc_nport *nport   = NULL;
	uint32_t                        nport_deletes_sent = 0;
	uint32_t                        nport_deletes_skipped = 0;
	struct spdk_event              *spdk_evt = NULL;
	spdk_nvmf_bcm_fc_nport_delete_args_t       *nport_del_args = NULL;
	spdk_nvmf_bcm_fc_port_link_break_cb_data_t *cb_data = NULL;
	char                            log_str[SPDK_NVMF_FC_LOG_STR_SIZE];

	/*
	 * 1. Get the fc port using the port handle.
	 */
	fc_port = spdk_nvmf_bcm_fc_port_list_get(args->port_handle);
	if (!fc_port) {
		SPDK_ERRLOG("port link break: Unable to find the SPDK FC port %d\n",
			    args->port_handle);
		err = SPDK_ERR_INVALID_ARGS;
		goto out;
	}

	/*
	 * 2. Set the port state to offline, if it is not already.
	 */
	err = spdk_nvmf_bcm_fc_port_set_offline(fc_port);
	if (err != SPDK_SUCCESS) {
		SPDK_ERRLOG("port link break: HW port %d already offline. rc = %d\n",
			    fc_port->port_hdl, err);
		err = SPDK_SUCCESS;
		goto out;
	}

	/*
	 * 3. Remove poller for the LS queue.
	 */
	hwqp = &fc_port->ls_queue;
	(void)spdk_nvmf_bcm_fc_hwqp_port_set_offline(hwqp);
	spdk_nvmf_bcm_fc_delete_poller(hwqp);

	/*
	 * 4. Remove poller for all the io queues.
	 */
	for (i = 0; i < (int)fc_port->max_io_queues; i++) {
		hwqp = &fc_port->io_queues[i];
		(void)spdk_nvmf_bcm_fc_hwqp_port_set_offline(hwqp);
		spdk_nvmf_bcm_fc_delete_poller(hwqp);
	}

	/*
	 * 5. Delete all the nports, if any.
	 */
	if (TAILQ_EMPTY(&fc_port->nport_list)) {
		goto out;
	}


	TAILQ_FOREACH(nport, &fc_port->nport_list, link) {
		/* Skipped the nports that are not in CREATED state */
		if (nport->nport_state != SPDK_NVMF_BCM_FC_OBJECT_CREATED) {
			nport_deletes_skipped++;
			continue;
		}

		/* Allocate memory for callback data. */
		cb_data = spdk_calloc(1, sizeof(spdk_nvmf_bcm_fc_port_link_break_cb_data_t));
		if (NULL == cb_data) {
			SPDK_ERRLOG("port link break: Failed to allocate memory for cb_data %d.\n",
				    args->port_handle);
			err = SPDK_ERR_NOMEM;
			goto out;
		}
		cb_data->args = args;
		cb_data->cb_func = cb_func;
		nport_del_args = &cb_data->nport_del_args;
		nport_del_args->port_handle = args->port_handle;
		nport_del_args->nport_handle = nport->nport_hdl;
		nport_del_args->cb_ctx = cb_data;

		spdk_evt = spdk_event_allocate(spdk_env_get_master_lcore(),
					       nvmf_fc_nport_delete,
					       (void *)nport_del_args, nvmf_fc_tgt_hw_port_link_break_cb);
		if (spdk_evt == NULL) {
			err = SPDK_ERR_NOMEM;
			SPDK_ERRLOG("port link break: event allocate failed for nport \
with port_handle:%d.\n", nport_del_args->port_handle);
			spdk_free(cb_data);
			DEV_VERIFY(!"port link break: event allocate failed.");
			(void)spdk_nvmf_bcm_fc_nport_set_state(
				nport, SPDK_NVMF_BCM_FC_OBJECT_ZOMBIE);
		} else {
			spdk_event_call(spdk_evt);
			nport_deletes_sent++;
		}
	}
out:
	if ((cb_func != NULL) && (nport_deletes_sent == 0)) {
		/*
		 * No nport_deletes are sent, which would have eventually
		 * called the port_link_break callback. Therefore, call the
		 * port_link_break callback here.
		 */
		(void)cb_func(args->port_handle, SPDK_FC_LINK_BREAK, args->cb_ctx, err);
	}

	snprintf(log_str, SPDK_NVMF_FC_LOG_STR_SIZE,
		 "port link break done: port:%d nport_deletes_sent:%d nport_deletes_skipped:%d rc:%d.\n",
		 args->port_handle, nport_deletes_sent, nport_deletes_skipped, err);

	if (err != SPDK_SUCCESS) {
		SPDK_ERRLOG("%s", log_str);
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF_BCM_FC_ADM, "%s", log_str);
	}
}

/* ******************* PUBLIC FUNCTIONS FOR DRIVER AND LIBRARY INTERACTIONS - END ************** */

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf_bcm_fc_adm", SPDK_TRACE_NVMF_BCM_FC_ADM);
