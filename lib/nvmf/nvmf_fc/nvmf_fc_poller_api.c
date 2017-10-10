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

extern int bcm_nvmf_fc_issue_abort(struct fc_hwqp *hwqp, fc_xri_t *xri, bool send_abts,
				   bcm_fc_caller_cb cb, void *cb_args);

static void
nvmf_fc_poller_api_cb_event(void *arg1, void *arg2)
{
	SPDK_TRACELOG(SPDK_TRACE_POLLER_API, " ");
	if (arg1) {
		struct nvmf_poller_api_cb_info *cb_info =
			(struct nvmf_poller_api_cb_info *) arg1;

		cb_info->cb_func(cb_info->cb_data, cb_info->ret);
		free(arg2);
	}
}

static void
nvmf_fc_poller_api_perform_cb(struct nvmf_poller_api_cb_info *cb_info,
			      nvmf_fc_poller_api_ret_t ret)
{
	SPDK_TRACELOG(SPDK_TRACE_POLLER_API, " ");

	if (cb_info->cb_func) {
		struct spdk_event *event = NULL;

		cb_info->ret = ret;

		/* callback to master thread */
		event = spdk_event_allocate(spdk_env_get_master_lcore(),
					    nvmf_fc_poller_api_cb_event,
					    (void *) cb_info, NULL);

		SPDK_TRACELOG(SPDK_TRACE_POLLER_API, " ");

		spdk_event_call(event);
	}
}

static void
nvmf_fc_poller_api_add_connection(void *arg1, void *arg2)
{
	nvmf_fc_poller_api_ret_t ret = NVMF_FC_POLLER_API_SUCCESS;
	struct nvmf_fc_poller_api_add_connection_args *conn_args =
		(struct nvmf_fc_poller_api_add_connection_args *)arg1;
	struct spdk_nvmf_fc_conn *fc_conn;
	bool bfound = false;

	SPDK_TRACELOG(SPDK_TRACE_POLLER_API, " ");

	/* make sure connection is not already in poller's list */
	TAILQ_FOREACH(fc_conn, &conn_args->hwqp->connection_list, link) {
		if (fc_conn->conn_id == conn_args->fc_conn->conn_id) {
			bfound = true;
			break;
		}
	}
	if (bfound) {
		SPDK_ERRLOG("duplicate connection found");
		ret = NVMF_FC_POLLER_API_DUP_CONN_ID;
	} else {
		SPDK_TRACELOG(SPDK_TRACE_POLLER_API,
			      "conn_id=%lx", conn_args->fc_conn->conn_id);
		TAILQ_INSERT_TAIL(&conn_args->hwqp->connection_list,
				  conn_args->fc_conn, link);
	}

	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, ret);
}

static void
nvmf_fc_poller_api_quiesce_queue(void *arg1, void *arg2)
{
	struct nvmf_fc_poller_api_quiesce_queue_args *q_args =
		(struct nvmf_fc_poller_api_quiesce_queue_args *) arg1;

	/* should be already, but make sure queue is quiesced */
	q_args->hwqp->state = SPDK_FC_HWQP_OFFLINE;
	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&q_args->cb_info, 0);
}

static void
nvmf_fc_poller_api_activate_queue(void *arg1, void *arg2)
{
	struct nvmf_fc_poller_api_quiesce_queue_args *q_args =
		(struct nvmf_fc_poller_api_quiesce_queue_args *) arg1;

	q_args->hwqp->state = SPDK_FC_HWQP_ONLINE;
	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&q_args->cb_info, 0);
}

static void
nvmf_fc_poller_api_del_connection(void *arg1, void *arg2)
{
	nvmf_fc_poller_api_ret_t ret = NVMF_FC_POLLER_API_SUCCESS;
	struct nvmf_fc_poller_api_del_connection_args *conn_args =
		(struct nvmf_fc_poller_api_del_connection_args *)arg1;
	struct spdk_nvmf_fc_conn *fc_conn;
	bool bfound = false;

	SPDK_TRACELOG(SPDK_TRACE_POLLER_API, " ");

	/* find the connection in poller's list */
	TAILQ_FOREACH(fc_conn, &conn_args->hwqp->connection_list, link) {
		if (fc_conn->conn_id == conn_args->fc_conn->conn_id) {
			bfound = true;
			break;
		}
	}
	if (bfound) {
		struct spdk_nvmf_fc_request *fc_req = NULL;
		struct fc_hwqp *hwqp = conn_args->hwqp;

		TAILQ_FOREACH(fc_req, &hwqp->in_use_reqs, link) {
			if (fc_req->fc_conn->conn_id == fc_conn->conn_id) {
				if (fc_req->is_aborted) {
					continue;
				}

				/* XXX Prior to issuing the ABTS-LS BDAL needs
				       to be called to abort Reads and Writes sent
				       to the Storage Layer XXX */

				fc_req->is_aborted = TRUE;
				if (!fc_req->xri_activated) {
					continue;
				}

				bcm_nvmf_fc_issue_abort(hwqp, fc_req->xri, TRUE,
							NULL, NULL);
			}
		}
		TAILQ_REMOVE(&conn_args->hwqp->connection_list,	fc_conn, link);
	} else {
		ret = NVMF_FC_POLLER_API_NO_CONN_ID;
	}

	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, ret);
}

static void
nvmf_fc_poller_abts_done(void *hwqp, int32_t status, void *cb_args)
{
	nvmf_fc_poller_api_ret_t ret = NVMF_FC_POLLER_API_SUCCESS;
	struct nvmf_fc_poller_api_abts_recvd_args *args = cb_args;

	if (status) {
		ret = NVMF_FC_POLLER_API_ERROR;
	}

	nvmf_fc_poller_api_perform_cb(&args->cb_info, ret);
}

static void
nvmf_fc_poller_api_abts_received(void *arg1, void *arg2)
{
	int rc;
	nvmf_fc_poller_api_ret_t ret = NVMF_FC_POLLER_API_OXID_NOT_FOUND;
	struct nvmf_fc_poller_api_abts_recvd_args *args = arg1;
	struct spdk_nvmf_fc_request *fc_req = NULL;
	struct fc_hwqp *hwqp = args->hwqp;

	TAILQ_FOREACH(fc_req, &hwqp->in_use_reqs, link) {
		if ((fc_req->rpi == args->ctx->rpi) &&
		    (fc_req->oxid == args->ctx->oxid)) {

			if (!fc_req->xri_activated) {
				fc_req->is_aborted = TRUE;
				ret = NVMF_FC_POLLER_API_SUCCESS;
				break;
			}

			rc = bcm_nvmf_fc_issue_abort(hwqp, fc_req->xri, FALSE,
						     nvmf_fc_poller_abts_done, args);
			if (!rc) {
				fc_req->is_aborted = TRUE;
				return;
			}

			/* Convert to API Error. */
			ret = NVMF_FC_POLLER_API_ERROR;
			break;
		}
	}

	nvmf_fc_poller_api_perform_cb(&args->cb_info, ret);
}

nvmf_fc_poller_api_ret_t
nvmf_fc_poller_api(uint32_t lcore, nvmf_fc_poller_api_t api, void *api_args)
{
	struct spdk_event *event = NULL;

	switch (api) {
	case NVMF_FC_POLLER_API_ADD_CONNECTION:
		event = spdk_event_allocate(lcore,
					    nvmf_fc_poller_api_add_connection,
					    api_args, NULL);
		break;
	case NVMF_FC_POLLER_API_DEL_CONNECTION:
		event = spdk_event_allocate(lcore,
					    nvmf_fc_poller_api_del_connection,
					    api_args, NULL);
		break;
	case NVMF_FC_POLLER_API_QUIESCE_QUEUE: {
		/* quiesce q polling now, don't wait for poller to do it */
		struct nvmf_fc_poller_api_quiesce_queue_args *q_args =
			(struct nvmf_fc_poller_api_quiesce_queue_args *) api_args;
		q_args->hwqp->state = SPDK_FC_HWQP_OFFLINE;

		event = spdk_event_allocate(lcore,
					    nvmf_fc_poller_api_quiesce_queue,
					    api_args, NULL);
	}
	break;
	case NVMF_FC_POLLER_API_ACTIVATE_QUEUE:
		event = spdk_event_allocate(lcore,
					    nvmf_fc_poller_api_activate_queue,
					    api_args, NULL);
		break;
	case NVMF_FC_POLLER_API_ABTS_RECEIVED:
		event = spdk_event_allocate(lcore,
					    nvmf_fc_poller_api_abts_received,
					    api_args, NULL);
		break;
	case NVMF_FC_POLLER_API_ADAPTER_EVENT:
	case NVMF_FC_POLLER_API_AEN:
		break;

	default:
		SPDK_ERRLOG("BAD ARG!");
		return NVMF_FC_POLLER_API_INVALID_ARG;

	}

	if (event) {
		spdk_event_call(event);
		return NVMF_FC_POLLER_API_SUCCESS;
	}

	SPDK_ERRLOG("event alloc failed");
	return NVMF_FC_POLLER_API_ERROR;
}

SPDK_LOG_REGISTER_TRACE_FLAG("nmvf_fc_poller_api", SPDK_TRACE_POLLER_API)
