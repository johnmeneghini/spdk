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

#include "nvmf/nvmf_internal.h"
#include "nvmf/request.h"
#include "nvmf/session.h"
#include "nvmf/subsystem.h"
#include "nvmf/transport.h"

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

#ifdef DEBUG
#define DEBUG_FC_POLLER_API_TRACE 1
#endif

static void
nvmf_fc_poller_api_cb_event(void *arg1, void *arg2)
{
	SPDK_TRACELOG(SPDK_TRACE_POLLER_API, "nvmf_fc_poller_api_cb_event");
	if (arg1) {
		struct nvmf_poller_api_cb_info *cb_info =
			(struct nvmf_poller_api_cb_info *) arg1;

		/* arg2 is the poller api return code */
		cb_info->cb_func(cb_info->cb_data,
				 *(nvmf_fc_poller_api_ret_t *)arg2);
	}
}

static void
nvmf_fc_poller_api_perform_cb(struct nvmf_poller_api_cb_info *cb_info,
			      nvmf_fc_poller_api_ret_t ret)
{
	SPDK_TRACELOG(SPDK_TRACE_POLLER_API, "nvmf_fc_poller_api_perform_cb");

	if (cb_info->cb_func) {
		struct spdk_event *event = NULL;

		/* callback to master thread */
		event = spdk_event_allocate(spdk_env_get_master_lcore(),
					    nvmf_fc_poller_api_cb_event,
					    (void *) cb_info, (void *) &ret);

		SPDK_TRACELOG(SPDK_TRACE_POLLER_API, "nvmf_fc_poller_api_perform_cb");

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

	SPDK_TRACELOG(SPDK_TRACE_POLLER_API, "nvmf_fc_poller_api_add_connection");

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

	q_args->hwqp->state = false;
	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&q_args->cb_info, 0);
}

static void
nvmf_fc_poller_api_activate_queue(void *arg1, void *arg2)
{
	struct nvmf_fc_poller_api_quiesce_queue_args *q_args =
		(struct nvmf_fc_poller_api_quiesce_queue_args *) arg1;

	q_args->hwqp->state = true;
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

	SPDK_TRACELOG(SPDK_TRACE_POLLER_API, "nvmf_fc_poller_api_del_connection");

	/* find the connection in poller's list */
	TAILQ_FOREACH(fc_conn, &conn_args->hwqp->connection_list, link) {
		if (fc_conn->conn_id == conn_args->fc_conn->conn_id) {
			bfound = true;
			break;
		}
	}
	if (bfound) {
		TAILQ_REMOVE(&conn_args->hwqp->connection_list,	fc_conn, link);
	} else {
		ret = NVMF_FC_POLLER_API_NO_CONN_ID;
	}

	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, ret);
}

nvmf_fc_poller_api_ret_t
nvmf_fc_poller_api(uint32_t lcore, nvmf_fc_poller_api_t api, void *api_args)
{
	struct spdk_event *event = NULL;
#ifdef DEBUG_FC_POLLER_API_TRACE
	extern struct spdk_trace_flag SPDK_TRACE_POLLER_API;
	SPDK_TRACE_POLLER_API.enabled = true;
#endif

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
	case NVMF_FC_POLLER_API_QUIESCE_QUEUE:
		event = spdk_event_allocate(lcore,
					    nvmf_fc_poller_api_quiesce_queue,
					    api_args, NULL);
		break;
	case NVMF_FC_POLLER_API_ACTIVATE_QUEUE:
		event = spdk_event_allocate(lcore,
					    nvmf_fc_poller_api_activate_queue,
					    api_args, NULL);
		break;
	case NVMF_FC_POLLER_API_ABTS_RECEIVED:
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

SPDK_LOG_REGISTER_TRACE_FLAG("fc_poller_api", SPDK_TRACE_POLLER_API)
