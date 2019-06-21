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
#include "request.h"
#include "session.h"
#include "subsystem.h"
#include "transport.h"

#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/trace.h"
#include "spdk_internal/event.h"

#include "spdk_internal/log.h"

void
spdk_nvmf_set_request_resp(struct spdk_nvmf_request *req, enum spdk_nvme_status_code_type sct,
			   uint16_t sc, bool dnr, bool more)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	response->status.sct = sct;
	response->status.sc = sc;
	response->status.dnr = dnr;
	response->status.m = more;

	if (response->status.sct == SPDK_NVME_SCT_GENERIC && response->status.sc == SPDK_NVME_SC_SUCCESS) {
		return;
	}

	SPDK_ERRLOG("NVM opc 0x%02X failed: sct: 0x%x sc: 0x%x dnr: %d more: %d\n",
		    cmd->opc, response->status.sct, response->status.sc, response->status.dnr, response->status.m);
}

int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	response->sqid = 0;
	response->status.p = 0;
	response->cid = req->cmd->nvme_cmd.cid;

	SPDK_TRACELOG(SPDK_TRACE_NVMF,
		      "cpl: cid=%u cdw0=0x%08x rsvd1=%u status=0x%04x\n",
		      response->cid, response->cdw0, response->rsvd1,
		      *(uint16_t *)&response->status);

	/* Check if this is fused command_1 and if it is the first one to fail */
	if (req->fused_partner &&
	    req->cmd->nvme_cmd.fuse == SPDK_NVME_FUSED_CMD1
	    && req->rsp->nvme_cpl.status.sc) {
		/* Let the fused write partner req know this */
		req->fused_partner->is_fused_partner_failed = true;
		if (req->fused_partner->fail_with_fused_aborted) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Setting fused partner status to aborted\n");
			spdk_nvmf_set_request_resp(req->fused_partner, SPDK_NVME_SCT_GENERIC,
						   SPDK_NVME_SC_ABORTED_FAILED_FUSED, req->rsp->nvme_cpl.status.dnr, 0);
		}
	}

	if (spdk_nvmf_is_fused_command(&req->cmd->nvme_cmd)) {
		/*
		 * We are completing a fused command, so uncouple
		 * both the commands, if they are coupled
		 */
		if (req->fused_partner) {
			req->fused_partner->fused_partner = NULL;
			req->fused_partner = NULL;
		}
	}

	if (req->conn->transport->req_complete(req)) {
		SPDK_ERRLOG("Transport request completion error!\n");
		return -1;
	}

	return 0;
}

static spdk_nvmf_request_exec_status
nvmf_process_property_get(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_prop_get_rsp *response;
	struct spdk_nvmf_fabric_prop_get_cmd *cmd;

	cmd = &req->cmd->prop_get_cmd;
	response = &req->rsp->prop_get_rsp;

	spdk_nvmf_property_get(req->conn->sess, cmd, response);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static spdk_nvmf_request_exec_status
nvmf_process_property_set(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_prop_set_cmd *cmd;

	cmd = &req->cmd->prop_set_cmd;

	spdk_nvmf_property_set(req->conn->sess, cmd, &req->rsp->nvme_cpl);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

void
spdk_nvmf_handle_connect(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_cmd *connect = &req->cmd->connect_cmd;
	struct spdk_nvmf_fabric_connect_data *connect_data = (struct spdk_nvmf_fabric_connect_data *)
			req->data;
	struct spdk_nvmf_fabric_connect_rsp *response = &req->rsp->connect_rsp;
	struct spdk_nvmf_conn *conn = req->conn;

	spdk_nvmf_session_connect(conn, connect, connect_data, response);

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "CONNECT capsule response: cntlid = 0x%04x\n",
		      response->status_code_specific.success.cntlid);

	spdk_nvmf_request_complete(req);
	return;
}

static void
invalid_connect_response(struct spdk_nvmf_fabric_connect_rsp *rsp, uint8_t iattr, uint16_t ipo)
{
	rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
	rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
	rsp->status_code_specific.invalid.iattr = iattr;
	rsp->status_code_specific.invalid.ipo = ipo;
}

#define INVALID_CONNECT_DATA(field) invalid_connect_response(rsp, 1, offsetof(struct spdk_nvmf_fabric_connect_data, field))

static void
nvmf_process_connect_master(void *arg1, void *arg2)
{
	struct spdk_nvmf_subsystem	*subsystem;
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)arg1;
	struct spdk_nvmf_fabric_connect_data *data = (struct spdk_nvmf_fabric_connect_data *)
			req->data;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;

	subsystem = spdk_nvmf_find_subsystem((const char *)data->subnqn);
	if (subsystem == NULL) {
		SPDK_ERRLOG("Could not find subsystem '%s'\n", data->subnqn);
		INVALID_CONNECT_DATA(subnqn);
		spdk_nvmf_request_complete(req);
		return;
	}

	subsystem->app_cbs->connect_cb(subsystem->cb_ctx, req);
}

static spdk_nvmf_request_exec_status
nvmf_process_connect(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_data *data = (struct spdk_nvmf_fabric_connect_data *)
			req->data;
	struct spdk_nvmf_fabric_connect_cmd *cmd = &req->cmd->connect_cmd;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	void *end;
	struct spdk_event *event;

	if (cmd->recfmt != 0) {
		SPDK_ERRLOG("Connect command unsupported RECFMT %u\n", cmd->recfmt);
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVMF_FABRIC_SC_INCOMPATIBLE_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (req->length < sizeof(struct spdk_nvmf_fabric_connect_data)) {
		SPDK_ERRLOG("Connect command data length 0x%x too small\n", req->length);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* Ensure that subnqn and hostnqn are null terminated */
	end = memchr(data->subnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1);
	if (!end) {
		SPDK_ERRLOG("Connect SUBNQN is not null terminated\n");
		INVALID_CONNECT_DATA(subnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	end = memchr(data->hostnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1);
	if (!end) {
		SPDK_ERRLOG("Connect HOSTNQN is not null terminated\n");
		INVALID_CONNECT_DATA(hostnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* Move to Master thread. Do not peek into subsystem or host lists from an IO poller thread.
	 * Those lists can change while we are trying to walk them in the IO poller context.
	 */
	event = spdk_event_allocate(spdk_env_get_master_lcore(), nvmf_process_connect_master, req, NULL);
	spdk_event_call(event);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static spdk_nvmf_request_exec_status
nvmf_process_fabrics_command(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_conn *conn = req->conn;
	struct spdk_nvmf_capsule_cmd *cap_hdr;

	cap_hdr = &req->cmd->nvmf_cmd;

	if (conn->sess == NULL) {
		/* No session established yet; the only valid command is Connect */
		if (cap_hdr->fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT) {
			return nvmf_process_connect(req);
		} else {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Got fctype 0x%x, expected Connect\n",
				      cap_hdr->fctype);
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else if (conn->type == CONN_TYPE_AQ) {
		/*
		 * Session is established, and this is an admin queue.
		 * Disallow Connect and allow other fabrics commands.
		 */
		switch (cap_hdr->fctype) {
		case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET:
			return nvmf_process_property_set(req);
		case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET:
			return nvmf_process_property_get(req);
		default:
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "recv capsule header type invalid [%x]!\n",
				      cap_hdr->fctype);
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else {
		/* Session is established, and this is an I/O queue */
		/* For now, no I/O-specific Fabrics commands are implemented (other than Connect) */
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Unexpected I/O fctype 0x%x\n", cap_hdr->fctype);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static void
nvmf_trace_command(union nvmf_h2c_msg *h2c_msg, enum conn_type conn_type)
{
	struct spdk_nvmf_capsule_cmd *cap_hdr = &h2c_msg->nvmf_cmd;
	struct spdk_nvme_cmd *cmd = &h2c_msg->nvme_cmd;
	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;
	uint8_t opc;

	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		opc = cap_hdr->fctype;
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "%s Fabrics cmd: %s fctype 0x%02x cid %u\n",
			      conn_type == CONN_TYPE_AQ ? "Admin" : "I/O",
			      spdk_print_fabric_cmd(cap_hdr->fctype), cap_hdr->fctype, cap_hdr->cid);
	} else {
		opc = cmd->opc;
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "%s cmd: opc 0x%02x fuse %u cid %u nsid %u cdw10 0x%08x\n",
			      conn_type == CONN_TYPE_AQ ? "Admin" : "I/O",
			      cmd->opc, cmd->fuse, cmd->cid, cmd->nsid, cmd->cdw10);
		if (cmd->mptr) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "mptr 0x%" PRIx64 "\n", cmd->mptr);
		}
		if (cmd->psdt != SPDK_NVME_PSDT_SGL_MPTR_CONTIG &&
		    cmd->psdt != SPDK_NVME_PSDT_SGL_MPTR_SGL) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "psdt %u\n", cmd->psdt);
		}
	}

	if (spdk_nvme_opc_get_data_transfer(opc) != SPDK_NVME_DATA_NONE) {
		if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF,
				      "SGL: Keyed%s: addr 0x%" PRIx64 " key 0x%x len 0x%x\n",
				      sgl->generic.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY ? " (Inv)" : "",
				      sgl->address, sgl->keyed.key, sgl->keyed.length);
		} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "SGL: Data block: %s 0x%" PRIx64 " len 0x%x\n",
				      sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET ? "offs" : "addr",
				      sgl->address, sgl->unkeyed.length);
		} else {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "SGL type 0x%x subtype 0x%x\n",
				      sgl->generic.type, sgl->generic.subtype);
		}
	}
}

void
spdk_nvmf_request_cleanup(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	if (session && (cmd->opc != SPDK_NVME_OPC_FABRIC)) {
		struct spdk_nvmf_subsystem *subsystem;

		subsystem = session->subsys;

		if ((req->conn->type != CONN_TYPE_AQ) && subsystem->ops->io_cleanup) {
			subsystem->ops->io_cleanup(req);
		}
	}
}

int
spdk_nvmf_request_abort(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	if (session && (cmd->opc != SPDK_NVME_OPC_FABRIC)) {
		struct spdk_nvmf_subsystem *subsystem;

		subsystem = session->subsys;

		if (subsystem->ops->io_abort) {
			subsystem->ops->io_abort(req);
		}
	}
	return 0;
}

static spdk_nvmf_request_exec_status
spdk_nvmf_request_setup_sgl(struct spdk_nvmf_request *req)
{
	size_t length = (size_t) req->length;
	caddr_t bp = (caddr_t) req->data;
	size_t plength, offset  = 0;
	uint64_t pa;
	int rc, i = 0;

	assert(req->set_sge != NULL);
	if (req->set_sge == NULL) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_ERROR;
	}

	req->iovcnt = 0;

	while (length > 0) {
		pa = spdk_vtophys_and_len(bp, length, &plength);

		if (pa == SPDK_VTOPHYS_ERROR) {
			SPDK_ERRLOG("spdk_vtophys_and_len returned SPDK_VTOPHYS_ERROR, VA %p, offset 0x%lx, length 0x%lx\n",
				    bp, offset, length);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_ERROR;
		}

		rc = req->set_sge(req, req->length, (uint32_t) offset, bp, plength, i);

		if (rc) {
			SPDK_ERRLOG("set_sge function returned error %d, VA %p, offset 0x%lx, length 0x%lx, index %d\n", rc,
				    bp, offset, length, i);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_ERROR;
		}

		offset += plength;
		length -= plength;
		bp += plength;
		i++;
	}

	assert(req->length == (uint32_t) offset);
	assert(req->sgl_filled == true);

	req->iovcnt = i;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_READY;
}

spdk_nvmf_request_exec_status
spdk_nvmf_request_setup_dma(struct spdk_nvmf_request *req, uint32_t max_io_size)
{
	spdk_nvmf_request_exec_status status;
	bool data_allocated = false;

	if (req->length == 0) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_READY;
	}

	/* Some transports don't pre-allocate a data buffer. */

	if (!req->data) {
		if ((req->data = spdk_dma_zmalloc(req->length, 4096, NULL)) == NULL) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Buffer allocation failed for command 0x%x, length %d\n",
				      req->cmd->nvme_cmd.opc,
				      req->length);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_PENDING;
		}
		data_allocated = true;
	}

	/* Some transports don't provide a set_sge function */

	/* XXX The max_io_size hack is temporary.  It will be removed after debugging. XXX */
	if (req->set_sge != NULL && max_io_size > 65536) {

		status = spdk_nvmf_request_setup_sgl(req);

		if (status == SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_ERROR) {

			SPDK_TRACELOG(SPDK_TRACE_NVMF, "SGL setup failed for command 0x%x, length %d\n",
				      req->cmd->nvme_cmd.opc, req->length);

			if (data_allocated) {
				spdk_dma_free(req->data);
				req->data = NULL;
			}
		}

		return status;
	}

	/* Do it the old fasion way. */
	req->iovcnt = spdk_dma_virt_to_iovec(req->data, req->length, req->iov, MAX_NUM_OF_IOVECTORS);

	if (req->iovcnt == 0) {

		SPDK_TRACELOG(SPDK_TRACE_NVMF, "SGL setup failed for command 0x%x, length %d\n",
			      req->cmd->nvme_cmd.opc, req->length);

		if (data_allocated) {
			spdk_dma_free(req->data);
			req->data = NULL;
		}

		return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_ERROR;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_READY;
}

spdk_nvmf_request_exec_status
spdk_nvmf_request_init(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	spdk_nvmf_request_exec_status status;
	struct spdk_nvmf_subsystem *subsystem;

	nvmf_trace_command(req->cmd, req->conn->type);

	/*
	 * Make sure xfer len is according to MDTS
	 * The host should not submit a command that exceeds this transfer size.
	 * If a command is submitted that exceeds the transfer size, then the
	 * command is aborted with a status of Invalid Field in Command.
	 */
	if (req->length > g_nvmf_tgt.opts.max_io_size) {
		SPDK_ERRLOG("IO length requested is greater than MDTS\n");
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* Only Fabric commands are allowed when the controller is disabled */
	if (cmd->opc != SPDK_NVME_OPC_FABRIC && (session == NULL || !session->vcprop.cc.bits.en)) {
		SPDK_ERRLOG("Non-Fabric command sent to disabled controller\n");
		rsp->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_FABRIC:
		status = spdk_nvmf_request_setup_dma(req, g_nvmf_tgt.opts.max_io_size);
		break;

	default:
		subsystem = session->subsys;
		assert(subsystem != NULL);

		if (subsystem->is_removed) {
			/* XXX do we want to set DNR here? */
			rsp->status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		switch (req->conn->type) {
		case CONN_TYPE_AQ:
			status = spdk_nvmf_request_setup_dma(req, g_nvmf_tgt.opts.max_io_size);
			break;
		case CONN_TYPE_IOQ:
			if (subsystem->ops->io_init) {
				status = subsystem->ops->io_init(req);
			} else {
				status = spdk_nvmf_request_setup_dma(req, g_nvmf_tgt.opts.max_io_size);
			}
			break;
		default:
			SPDK_ERRLOG("Invalid connection type 0x%x\n", req->conn->type);
			rsp->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
			status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}

	return status;
}

int
spdk_nvmf_request_exec(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	spdk_nvmf_request_exec_status status;

	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		status = nvmf_process_fabrics_command(req);
	} else if (session == NULL || !session->vcprop.cc.bits.en) {
		/* Only Fabric commands are allowed when the controller is disabled */
		SPDK_ERRLOG("Non-Fabric command sent to disabled controller\n");
		rsp->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	} else {
		struct spdk_nvmf_subsystem *subsystem;

		subsystem = session->subsys;
		assert(subsystem != NULL);

		if (subsystem->is_removed) {
			/* XXX do we want to set DNR here? */
			rsp->status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
			status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else if (req->conn->type == CONN_TYPE_AQ) {
			status = subsystem->ops->process_admin_cmd(req);
		} else {
			status = subsystem->ops->process_io_cmd(req);
		}
	}

	if (status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		spdk_nvmf_request_complete(req);
	}

	return status;
}
