/*
 *
 *   BSD LICENSE
 *
 *   Copyright (c) 2017 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Limited and/or its subsidiaries.
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
#include "spdk/assert.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/endian.h"
#include "spdk/event.h"
#include "spdk_internal/log.h"

#include "bcm_fc.h"
#include "bcm_sli_fc.h"

extern void spdk_post_event(void *context, struct spdk_event *event);

extern void
spdk_nvmf_bcm_fc_req_abort(struct spdk_nvmf_bcm_fc_request *fc_req, bool send_abts,
			   spdk_nvmf_bcm_fc_caller_cb cb, void *cb_args);
void
nvmf_fc_poller_queue_sync_done(void *arg1, void *arg2);

extern void spdk_nvmf_bcm_fc_req_abort_complete(void *arg1, void *arg2);

extern bool spdk_nvmf_bcm_fc_req_in_xfer(struct spdk_nvmf_bcm_fc_request *fc_req);

static void
nvmf_fc_poller_api_cb_event(void *arg1, void *arg2)
{
	SPDK_TRACELOG(SPDK_NVMF_BCM_FC_POLLER_API, "\n");
	if (arg1) {
		struct spdk_nvmf_bcm_fc_poller_api_cb_info *cb_info =
			(struct spdk_nvmf_bcm_fc_poller_api_cb_info *) arg1;

		cb_info->cb_func(cb_info->cb_data, cb_info->ret);
	}
}

static void
nvmf_fc_poller_api_perform_cb(struct spdk_nvmf_bcm_fc_poller_api_cb_info *cb_info,
			      spdk_nvmf_bcm_fc_poller_api_ret_t ret)
{
	SPDK_TRACELOG(SPDK_NVMF_BCM_FC_POLLER_API, "\n");

	if (cb_info->cb_func) {
		struct spdk_event *event = NULL;

		cb_info->ret = ret;

		/* callback to master thread */
		event = spdk_event_allocate(spdk_env_get_master_lcore(),
					    nvmf_fc_poller_api_cb_event,
					    (void *) cb_info, NULL);

		SPDK_TRACELOG(SPDK_NVMF_BCM_FC_POLLER_API, "\n");

		spdk_event_call(event);
	}
}

static void
nvmf_fc_poller_api_add_connection(void *arg1, void *arg2)
{
	spdk_nvmf_bcm_fc_poller_api_ret_t ret = SPDK_NVMF_BCM_FC_POLLER_API_SUCCESS;
	struct spdk_nvmf_bcm_fc_poller_api_add_connection_args *conn_args =
		(struct spdk_nvmf_bcm_fc_poller_api_add_connection_args *)arg1;
	struct spdk_nvmf_bcm_fc_conn *fc_conn;
	bool bfound = false;

	SPDK_TRACELOG(SPDK_NVMF_BCM_FC_POLLER_API, "Poller add connection, conn_id 0x%lx\n",
		      conn_args->fc_conn->conn_id);

	/* make sure connection is not already in poller's list */
	TAILQ_FOREACH(fc_conn, &conn_args->fc_conn->hwqp->connection_list,
		      link) {
		if (fc_conn->conn_id == conn_args->fc_conn->conn_id) {
			bfound = true;
			break;
		}
	}
	if (bfound) {
		SPDK_ERRLOG("duplicate connection found");
		ret = SPDK_NVMF_BCM_FC_POLLER_API_DUP_CONN_ID;
	} else {
		SPDK_TRACELOG(SPDK_NVMF_BCM_FC_POLLER_API,
			      "conn_id=%lx", conn_args->fc_conn->conn_id);
		TAILQ_INSERT_TAIL(&conn_args->fc_conn->hwqp->connection_list,
				  conn_args->fc_conn, link);
	}

	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, ret);
}

static void
nvmf_fc_poller_api_quiesce_queue(void *arg1, void *arg2)
{
	struct spdk_nvmf_bcm_fc_poller_api_quiesce_queue_args *q_args =
		(struct spdk_nvmf_bcm_fc_poller_api_quiesce_queue_args *) arg1;
	struct spdk_nvmf_bcm_fc_request *fc_req = NULL, *tmp;
	struct spdk_event *event = NULL;

	/* should be already, but make sure queue is quiesced */
	q_args->hwqp->state = SPDK_FC_HWQP_OFFLINE;

	/*
	 * Kill all the outstanding commands that are in the transfer state and
	 * in the process of being aborted.
	 * We can run into this situation if an adapter reset happens when an IT delete
	 * is in progress.
	 */
	TAILQ_FOREACH_SAFE(fc_req, &q_args->hwqp->in_use_reqs, link, tmp) {
		if (spdk_nvmf_bcm_fc_req_in_xfer(fc_req) && fc_req->is_aborted == true) {
			event = spdk_event_allocate(fc_req->poller_lcore,
						    spdk_nvmf_bcm_fc_req_abort_complete, (void *)fc_req, NULL);
			spdk_post_event(fc_req->hwqp->context, event);

		}
	}

	/*
	 * Reset the pending XRI list associated with the HWQP.
	 * NOTE: The XRIs will be freed and reallocated in the subsequent HW port init.
	 */
	TAILQ_INIT(&q_args->hwqp->pending_xri_list);

	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&q_args->cb_info, 0);
}

static void
nvmf_fc_poller_api_activate_queue(void *arg1, void *arg2)
{
	struct spdk_nvmf_bcm_fc_poller_api_quiesce_queue_args *q_args =
		(struct spdk_nvmf_bcm_fc_poller_api_quiesce_queue_args *) arg1;

	q_args->hwqp->state = SPDK_FC_HWQP_ONLINE;

	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&q_args->cb_info, 0);
}

static void
nvmf_fc_poller_conn_abort_done(void *hwqp, int32_t status, void *cb_args)
{
	struct spdk_nvmf_bcm_fc_poller_api_del_connection_args *conn_args = cb_args;
	spdk_nvmf_bcm_fc_poller_api_ret_t ret = SPDK_NVMF_BCM_FC_POLLER_API_SUCCESS;

	if (conn_args->fc_request_cnt) {
		conn_args->fc_request_cnt -= 1;
	}

	if (!conn_args->fc_request_cnt) {
		if (!TAILQ_EMPTY(&conn_args->hwqp->connection_list)) {
			/* All the requests for this connection are aborted. */
			TAILQ_REMOVE(&conn_args->hwqp->connection_list,	conn_args->fc_conn, link);

			SPDK_TRACELOG(SPDK_NVMF_BCM_FC_POLLER_API, "Connection deleted, conn_id 0x%lx\n",
				      conn_args->fc_conn->conn_id);
		} else {
			/*
			 * Duplicate connection delete can happen if one is
			 * coming in via an association disconnect and the other
			 * is initiated by a port reset.
			 */
			SPDK_TRACELOG(SPDK_NVMF_BCM_FC_POLLER_API, "Duplicate conn delete.");
		}

		/* perform callback */
		nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, ret);
	}
}

static void
nvmf_fc_poller_api_del_connection(void *arg1, void *arg2)
{
	spdk_nvmf_bcm_fc_poller_api_ret_t ret = SPDK_NVMF_BCM_FC_POLLER_API_SUCCESS;
	struct spdk_nvmf_bcm_fc_poller_api_del_connection_args *conn_args =
		(struct spdk_nvmf_bcm_fc_poller_api_del_connection_args *)arg1;
	struct spdk_nvmf_bcm_fc_conn *fc_conn;
	bool bfound = false;

	SPDK_TRACELOG(SPDK_NVMF_BCM_FC_POLLER_API, "Poller delete connection, conn_id 0x%lx\n",
		      conn_args->fc_conn->conn_id);

	/* find the connection in poller's list */
	TAILQ_FOREACH(fc_conn, &conn_args->hwqp->connection_list, link) {
		if (fc_conn->conn_id == conn_args->fc_conn->conn_id) {
			bfound = true;
			break;
		}
	}
	if (bfound) {
		struct spdk_nvmf_bcm_fc_request *fc_req = NULL, *tmp;
		struct spdk_nvmf_bcm_fc_hwqp *hwqp = conn_args->hwqp;

		conn_args->fc_request_cnt = 0;

		TAILQ_FOREACH_SAFE(fc_req, &hwqp->in_use_reqs, link, tmp) {
			if (fc_req->fc_conn->conn_id == fc_conn->conn_id) {
				conn_args->fc_request_cnt += 1;
				spdk_nvmf_bcm_fc_req_abort(fc_req, conn_args->send_abts,
							   nvmf_fc_poller_conn_abort_done,
							   conn_args);
			}
		}

		if (!conn_args->fc_request_cnt) {
			SPDK_TRACELOG(SPDK_NVMF_BCM_FC_POLLER_API, "Connection deleted.\n");
			TAILQ_REMOVE(&conn_args->hwqp->connection_list,	fc_conn, link);
		} else {
			/* Will be handled in req abort callback */
			return;
		}

	} else {
		ret = SPDK_NVMF_BCM_FC_POLLER_API_NO_CONN_ID;
	}

	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, ret);
}

static void
nvmf_fc_poller_abts_done(void *hwqp, int32_t status, void *cb_args)
{
	struct spdk_nvmf_bcm_fc_poller_api_abts_recvd_args *args = cb_args;

	SPDK_TRACELOG(SPDK_NVMF_BCM_FC_POLLER_API,
		      "ABTS poller done, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		      args->ctx->rpi, args->ctx->oxid, args->ctx->rxid);

	nvmf_fc_poller_api_perform_cb(&args->cb_info,
				      SPDK_NVMF_BCM_FC_POLLER_API_SUCCESS);
}

static void
nvmf_fc_poller_api_abts_received(void *arg1, void *arg2)
{
	spdk_nvmf_bcm_fc_poller_api_ret_t ret = SPDK_NVMF_BCM_FC_POLLER_API_OXID_NOT_FOUND;
	struct spdk_nvmf_bcm_fc_poller_api_abts_recvd_args *args = arg1;
	struct spdk_nvmf_bcm_fc_request *fc_req = NULL;
	struct spdk_nvmf_bcm_fc_hwqp *hwqp = args->hwqp;
	struct spdk_nvmf_bcm_fc_conn *fc_conn;

	TAILQ_FOREACH(fc_conn, &hwqp->connection_list, link) {
		if (fc_conn->rpi == args->ctx->rpi) {
			TAILQ_FOREACH(fc_req, &fc_conn->in_use_reqs, conn_link) {
				if (fc_req->oxid == args->ctx->oxid) {
					spdk_nvmf_bcm_fc_req_abort(fc_req, false,
								   nvmf_fc_poller_abts_done, args);
					return;
				}
			}
		}
	}

	nvmf_fc_poller_api_perform_cb(&args->cb_info, ret);
}

void
nvmf_fc_poller_queue_sync_done(void *arg1, void *arg2)
{
	struct spdk_nvmf_bcm_fc_hwqp *hwqp = arg1;
	uint64_t tag = (uint64_t)arg2;
	struct spdk_nvmf_bcm_fc_poller_api_queue_sync_args *args = NULL, *tmp = NULL;

	TAILQ_FOREACH_SAFE(args, &hwqp->sync_cbs, link, tmp) {
		if (args->u_id == tag) {
			/* Queue successfully synced. Remove from cb list */
			TAILQ_REMOVE(&hwqp->sync_cbs, args, link);

			SPDK_TRACELOG(SPDK_NVMF_BCM_FC_POLLER_API,
				      "HWQP sync done for u_id = 0x%lx\n", args->u_id);

			/* Return the status to poller */
			nvmf_fc_poller_api_perform_cb(&args->cb_info,
						      SPDK_NVMF_BCM_FC_POLLER_API_SUCCESS);
			return;
		}
	}
}

static void
nvmf_fc_poller_api_queue_sync(void *arg1, void *arg2)
{
	struct spdk_nvmf_bcm_fc_poller_api_queue_sync_args *args = arg1;

	SPDK_TRACELOG(SPDK_NVMF_BCM_FC_POLLER_API,
		      "HWQP sync requested for u_id = 0x%lx\n", args->u_id);

	/* Add this args to hwqp sync_cb list */
	TAILQ_INSERT_TAIL(&args->hwqp->sync_cbs, args, link);
}

spdk_nvmf_bcm_fc_poller_api_ret_t
spdk_nvmf_bcm_fc_poller_api(struct spdk_nvmf_bcm_fc_hwqp *hwqp, spdk_nvmf_bcm_fc_poller_api_t api,
			    void *api_args)
{
	struct spdk_event *event = NULL;
	uint32_t lcore = hwqp->lcore_id;

	switch (api) {
	case SPDK_NVMF_BCM_FC_POLLER_API_ADD_CONNECTION:
		event = spdk_event_allocate(lcore,
					    nvmf_fc_poller_api_add_connection,
					    api_args, NULL);
		break;
	case SPDK_NVMF_BCM_FC_POLLER_API_DEL_CONNECTION:
		event = spdk_event_allocate(lcore,
					    nvmf_fc_poller_api_del_connection,
					    api_args, NULL);
		break;
	case SPDK_NVMF_BCM_FC_POLLER_API_QUIESCE_QUEUE: {
		/* quiesce q polling now, don't wait for poller to do it */
		hwqp->state = SPDK_FC_HWQP_OFFLINE;

		event = spdk_event_allocate(lcore,
					    nvmf_fc_poller_api_quiesce_queue,
					    api_args, NULL);
	}
	break;
	case SPDK_NVMF_BCM_FC_POLLER_API_ACTIVATE_QUEUE:
		event = spdk_event_allocate(lcore,
					    nvmf_fc_poller_api_activate_queue,
					    api_args, NULL);
		break;
	case SPDK_NVMF_BCM_FC_POLLER_API_ABTS_RECEIVED:
		event = spdk_event_allocate(lcore,
					    nvmf_fc_poller_api_abts_received,
					    api_args, NULL);
		break;
	case SPDK_NVMF_BCM_FC_POLLER_API_QUEUE_SYNC:
		event = spdk_event_allocate(lcore,
					    nvmf_fc_poller_api_queue_sync,
					    api_args, NULL);
		break;
	case SPDK_NVMF_BCM_FC_POLLER_API_ADAPTER_EVENT:
	case SPDK_NVMF_BCM_FC_POLLER_API_AEN:
		break;

	default:
		SPDK_ERRLOG("BAD ARG!");
		return SPDK_NVMF_BCM_FC_POLLER_API_INVALID_ARG;

	}

	if (event) {
		spdk_post_event(hwqp->context, event);
		return SPDK_NVMF_BCM_FC_POLLER_API_SUCCESS;
	}

	SPDK_ERRLOG("event alloc failed");
	return SPDK_NVMF_BCM_FC_POLLER_API_ERROR;
}

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf_bcm_fc_poller_api", SPDK_NVMF_BCM_FC_POLLER_API)
