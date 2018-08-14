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

#include "subsystem.h"
#include "session.h"
#include "request.h"

#include "spdk/bdev.h"
#include "spdk/endian.h"
#include "spdk/io_channel.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/trace.h"
#include "spdk/scsi_spec.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"
#include "spdk_internal/bdev.h"
#include "nvmf_internal.h"
#include "spdk_internal/event.h"
#include "nvmf/nvmf_internal.h"

#define FW_VERSION "FFFFFFFF"

/* read command dword 12 */
struct __attribute__((packed)) nvme_read_cdw12 {
	uint16_t	nlb;		/* number of logical blocks */
	uint16_t	rsvd	: 10;
	uint8_t		prinfo	: 4;	/* protection information field */
	uint8_t		fua	: 1;	/* force unit access */
	uint8_t		lr	: 1;	/* limited retry */
};

static void nvmf_virtual_verify_dsm(struct spdk_nvmf_session *session)
{
	uint32_t i;

	for (i = 0; i < session->subsys->dev.virt.max_nsid; i++) {
		struct spdk_bdev *bdev = session->subsys->dev.virt.ns_list[i];

		if (bdev == NULL) {
			continue;
		}

		if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF,
				      "Namespace %s does not support unmap\n",
				      spdk_bdev_get_name(bdev));
			session->vcdata.oncs.dsm = 0;
			return;
		}
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "DSM is %s in Subsystem %s\n",
		      (session->vcdata.oncs.dsm == 1) ? "enabled" : "disabled",
		      spdk_nvmf_subsystem_get_nqn(session->subsys));
}

static void
nvmf_virtual_ctrlr_get_data(struct spdk_nvmf_session *session)
{
	struct spdk_nvmf_subsystem *subsys = session->subsys;

	memset(&session->vcdata, 0, sizeof(struct spdk_nvme_ctrlr_data));
	session->vcdata.vid = subsys->dev.virt.sub_pci_id.vendor_id;
	session->vcdata.ssvid = subsys->dev.virt.sub_pci_id.subvendor_id;
	spdk_strcpy_pad(session->vcdata.fr, FW_VERSION, sizeof(session->vcdata.fr), ' ');
	spdk_strcpy_pad(session->vcdata.mn, g_nvmf_tgt.opts.mn, sizeof(session->vcdata.mn), ' ');
	spdk_strcpy_pad(session->vcdata.sn, subsys->dev.virt.sn, sizeof(session->vcdata.sn), ' ');
	session->vcdata.rab = g_nvmf_tgt.opts.rab;
	memcpy(session->vcdata.ieee, g_nvmf_tgt.opts.ieee, 3 * sizeof(uint8_t));
	memcpy(&session->vcdata.cmic, &g_nvmf_tgt.opts.cmic, sizeof(uint8_t));
	memcpy(&session->vcdata.oaes, &g_nvmf_tgt.opts.oaes, sizeof(uint32_t));

	/* Set appropriate values for 0's based fields */
	if (g_nvmf_tgt.opts.acl > 0) {
		session->vcdata.acl = (g_nvmf_tgt.opts.acl - 1);
	} else {
		session->vcdata.acl = g_nvmf_tgt.opts.acl;
	}

	if (g_nvmf_tgt.opts.aerl > 0) {
		session->vcdata.aerl = (g_nvmf_tgt.opts.aerl - 1);
	} else {
		session->vcdata.aerl = g_nvmf_tgt.opts.aerl;
	}

	if (g_nvmf_tgt.opts.elpe > 0) {
		session->vcdata.elpe = (g_nvmf_tgt.opts.elpe - 1);
	} else {
		session->vcdata.elpe = g_nvmf_tgt.opts.elpe;
	}

	if (g_nvmf_tgt.opts.npss > 0) {
		session->vcdata.npss = (g_nvmf_tgt.opts.npss - 1);
	} else {
		session->vcdata.npss = g_nvmf_tgt.opts.npss;
	}

	if (g_nvmf_tgt.opts.awun > 0) {
		session->vcdata.awun = (g_nvmf_tgt.opts.awun - 1);
	} else {
		session->vcdata.awun = g_nvmf_tgt.opts.awun;
	}

	if (g_nvmf_tgt.opts.awupf > 0) {
		session->vcdata.awupf = (g_nvmf_tgt.opts.awupf - 1);
	} else {
		session->vcdata.awupf = g_nvmf_tgt.opts.awupf;
	}

	session->vcdata.kas = g_nvmf_tgt.opts.kas;
	memcpy(&session->vcdata.vwc, &g_nvmf_tgt.opts.vwc, sizeof(uint8_t));
	memcpy(&session->vcdata.sgls, &g_nvmf_tgt.opts.sgls, sizeof(uint32_t));

	if (g_nvmf_tgt.opts.nvmever == SPDK_NVME_VERSION(1, 3, 0)) {
		/* Version Supported: 1.3.0 */
		session->vcdata.ver.bits.mjr = 1;
		session->vcdata.ver.bits.mnr = 3;
		session->vcdata.ver.bits.ter = 0;
	} else {
		/* default Version : 1.2.1 */
		session->vcdata.ver.bits.mjr = 1;
		session->vcdata.ver.bits.mnr = 2;
		session->vcdata.ver.bits.ter = 1;
	}

	session->vcdata.ctratt.host_id_exhid_supported = 1;
	session->vcdata.frmw.slot1_ro = 1;
	session->vcdata.frmw.num_slots = 1;
	session->vcdata.lpa.edlp = 1;
	session->vcdata.sqes.min = 0x06;
	session->vcdata.sqes.max = 0x06;
	session->vcdata.cqes.min = 0x04;
	session->vcdata.cqes.max = 0x04;
	session->vcdata.maxcmd = g_nvmf_tgt.opts.max_io_queue_depth;
	session->vcdata.nn = MAX_VIRTUAL_NAMESPACE;
	/* TODO: Madhu - Check if the next two lines are Ok. in our env. */
	session->vcdata.vwc.present = 1;
	session->vcdata.sgls.supported = 1;
	strncpy((char *)session->vcdata.subnqn, session->subsys->subnqn, sizeof(session->vcdata.subnqn));
	memcpy(&session->vcdata.oncs, &g_nvmf_tgt.opts.oncs, sizeof(uint16_t));
	nvmf_virtual_verify_dsm(session);
}

static void
nvmf_virtual_ctrlr_poll_for_completions(struct spdk_nvmf_subsystem *subsystem)
{
	return;
}

static void
nvmf_virtual_ctrlr_complete_cmd(struct spdk_bdev_io *bdev_io, bool success,
				void *cb_arg)
{
	struct spdk_nvmf_request 	*req = cb_arg;
	struct spdk_nvme_cpl 		*response = &req->rsp->nvme_cpl;
	struct spdk_nvme_cmd            *cmd = &req->cmd->nvme_cmd;
	int				sc, sct, dnr;

	if (cmd->opc == SPDK_NVME_OPC_DATASET_MANAGEMENT) {
		free(req->unmap_bdesc);
	}

	spdk_bdev_io_get_nvme_status(bdev_io, &sct, &sc, &dnr);
	response->status.sc = sc;
	response->status.sct = sct;
	response->status.dnr = dnr;


	/*
	 * BDEV IO is freed as part of request cleanup function call.
	 */
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		req->iovcnt = bdev_io->u.read.iovcnt;
	} else {
		req->iovcnt = bdev_io->u.write.iovcnt;
	}
	req->bdev_io = bdev_io;

	spdk_nvmf_request_complete(req);
}

static int
nvmf_virtual_ctrlr_get_log_page(struct spdk_nvmf_request *req)
{
	uint8_t lid;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	uint64_t log_page_offset;

	if (req->data == NULL) {
		SPDK_ERRLOG("get log command with no buffer\n");
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	memset(req->data, 0, req->length);

	log_page_offset = (uint64_t)cmd->cdw12 | ((uint64_t)cmd->cdw13 << 32);
	if (log_page_offset & 3) {
		SPDK_ERRLOG("Invalid log page offset 0x%" PRIx64 "\n", log_page_offset);
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	lid = cmd->cdw10 & 0xFF;
	switch (lid) {
	case SPDK_NVME_LOG_ERROR:
	case SPDK_NVME_LOG_HEALTH_INFORMATION:
	case SPDK_NVME_LOG_FIRMWARE_SLOT:
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	default:
		SPDK_ERRLOG("Unsupported Get Log Page 0x%02X\n", lid);
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVME_SC_INVALID_LOG_PAGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static int
identify_ns(struct spdk_nvmf_subsystem *subsystem,
	    struct spdk_nvme_cmd *cmd,
	    struct spdk_nvme_cpl *rsp,
	    struct spdk_nvme_ns_data *nsdata)
{
	struct spdk_bdev *bdev;
	uint64_t num_blocks;

	if (cmd->nsid > subsystem->dev.virt.max_nsid || cmd->nsid == 0) {
		SPDK_ERRLOG("Identify Namespace for invalid NSID %u\n", cmd->nsid);
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bdev = subsystem->dev.virt.ns_list[cmd->nsid - 1];

	if (bdev == NULL) {
		memset(nsdata, 0, sizeof(*nsdata));
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	num_blocks = spdk_bdev_get_num_blocks(bdev);

	nsdata->nsze = num_blocks;
	nsdata->ncap = num_blocks;
	nsdata->nuse = num_blocks;
	nsdata->nlbaf = 0;
	nsdata->flbas.format = 0;
	nsdata->nmic.can_share = g_nvmf_tgt.opts.nmic;
	nsdata->lbaf[0].lbads = spdk_u32log2(spdk_bdev_get_block_size(bdev));

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
identify_ctrlr(struct spdk_nvmf_session *session, struct spdk_nvme_ctrlr_data *cdata)
{
	*cdata = session->vcdata;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
identify_active_ns_list(struct spdk_nvmf_subsystem *subsystem,
			struct spdk_nvme_cmd *cmd,
			struct spdk_nvme_cpl *rsp,
			struct spdk_nvme_ns_list *ns_list)
{
	uint32_t i, num_ns, count = 0;

	if (cmd->nsid >= 0xfffffffeUL) {
		SPDK_ERRLOG("Identify Active Namespace List with invalid NSID %u\n", cmd->nsid);
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	num_ns = subsystem->dev.virt.max_nsid;

	for (i = 1; i <= num_ns; i++) {
		if (i <= cmd->nsid) {
			continue;
		}
		if (subsystem->dev.virt.ns_list[i - 1] == NULL) {
			continue;
		}
		ns_list->ns_list[count++] = i;
		if (count == SPDK_COUNTOF(ns_list->ns_list)) {
			break;
		}
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
identify_ns_id_desc_list(struct spdk_nvmf_subsystem *subsystem,
			 struct spdk_nvme_cmd *cmd,
			 struct spdk_nvme_cpl *rsp,
			 void *id_desc_list)
{
	struct spdk_bdev *bdev;

	if (cmd->nsid > subsystem->dev.virt.max_nsid || cmd->nsid == 0) {
		SPDK_ERRLOG("Identify Namespace ID descriptor for invalid NSID %u\n", cmd->nsid);
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bdev = subsystem->dev.virt.ns_list[cmd->nsid - 1];
	/*
	 * The NVMe1.3 spec is not clear if NSID is not found in active ns_list.
	 * Just following the CNS 00h way of handling. Just zero fill and return.
	 * req->data is already zero filled so just complete
	 */
	if (bdev == NULL) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (bdev->id_desc) {
		memcpy(id_desc_list, (char *)bdev->id_desc, sizeof(*bdev->id_desc) + bdev->id_desc->nidl);
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_virtual_ctrlr_identify(struct spdk_nvmf_request *req)
{
	uint8_t cns;
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_subsystem *subsystem = session->subsys;

	if (req->data == NULL || req->length < 4096) {
		SPDK_ERRLOG("identify command with invalid buffer\n");
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	memset(req->data, 0, req->length);

	cns = cmd->cdw10 & 0xFF;
	switch (cns) {
	case SPDK_NVME_IDENTIFY_NS:
		return identify_ns(subsystem, cmd, rsp, req->data);
	case SPDK_NVME_IDENTIFY_CTRLR:
		return identify_ctrlr(session, req->data);
	case SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST:
		return identify_active_ns_list(subsystem, cmd, rsp, req->data);
	case SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST:
		return identify_ns_id_desc_list(subsystem, cmd, rsp, req->data);
	default:
		SPDK_ERRLOG("Identify command with unsupported CNS 0x%02x\n", cns);
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static int
nvmf_virtual_ctrlr_abort(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	uint32_t cdw10 = cmd->cdw10;
	uint16_t cid = cdw10 >> 16;
	uint16_t sqid = cdw10 & 0xFFFFu;
	struct spdk_nvmf_conn *conn;
	struct spdk_nvmf_request *req_to_abort;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "abort sqid=%u cid=%u\n", sqid, cid);

	rsp->cdw0 = 1; /* Command not aborted */

	conn = spdk_nvmf_session_get_conn(session, sqid);
	if (conn == NULL) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "sqid %u not found\n", sqid);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/*
	 * NOTE: This relies on the assumption that all connections for a session will be handled
	 * on the same thread.  If this assumption becomes untrue, this will need to pass a message
	 * to the thread handling conn, and the abort will need to be asynchronous.
	 */
	req_to_abort = spdk_nvmf_conn_get_request(conn, cid);
	if (req_to_abort == NULL) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "cid %u not found\n", cid);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_nvmf_request_abort(req_to_abort) == 0) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "abort session=%p req=%p sqid=%u cid=%u successful\n",
			      session, req_to_abort, sqid, cid);
		rsp->cdw0 = 0; /* Command successfully aborted */
	}
	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_virtual_ctrlr_get_features(struct spdk_nvmf_request *req)
{
	uint8_t feature;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	feature = cmd->cdw10 & 0xff; /* mask out the FID value */
	switch (feature) {
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
		return spdk_nvmf_session_get_features_number_of_queues(req);
	case SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE:
		response->cdw0 = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
		return spdk_nvmf_session_get_features_keep_alive_timer(req);
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
		return spdk_nvmf_session_get_features_async_event_configuration(req);
	case SPDK_NVME_FEAT_HOST_IDENTIFIER:
		return spdk_nvmf_session_get_features_host_identifier(req);
	default:
		SPDK_ERRLOG("Get Features command with unsupported feature ID 0x%02x\n", feature);
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static int
nvmf_virtual_ctrlr_set_features(struct spdk_nvmf_request *req)
{
	uint8_t feature;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	feature = cmd->cdw10 & 0xff; /* mask out the FID value */
	switch (feature) {
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
		return spdk_nvmf_session_set_features_number_of_queues(req);
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
		return spdk_nvmf_session_set_features_keep_alive_timer(req);
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
		return spdk_nvmf_session_set_features_async_event_configuration(req);
	case SPDK_NVME_FEAT_HOST_IDENTIFIER:
		return spdk_nvmf_session_set_features_host_identifier(req);
	default:
		SPDK_ERRLOG("Set Features command with unsupported feature ID 0x%02x\n", feature);
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static int
nvmf_virtual_ctrlr_process_admin_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	/* pre-set response details for this command */
	response->status.sc = SPDK_NVME_SC_SUCCESS;

	switch (cmd->opc) {
	case SPDK_NVME_OPC_GET_LOG_PAGE:
		return nvmf_virtual_ctrlr_get_log_page(req);
	case SPDK_NVME_OPC_IDENTIFY:
		return nvmf_virtual_ctrlr_identify(req);
	case SPDK_NVME_OPC_ABORT:
		return nvmf_virtual_ctrlr_abort(req);
	case SPDK_NVME_OPC_GET_FEATURES:
		return nvmf_virtual_ctrlr_get_features(req);
	case SPDK_NVME_OPC_SET_FEATURES:
		return nvmf_virtual_ctrlr_set_features(req);
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
		return spdk_nvmf_session_async_event_request(req);
	case SPDK_NVME_OPC_KEEP_ALIVE:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Keep Alive\n");
		/*
		 * To handle keep alive just clear or reset the
		 * session based keep alive duration counter.
		 * When added, a separate timer based process
		 * will monitor if the time since last recorded
		 * keep alive has exceeded the max duration and
		 * take appropriate action.
		 */
		//session->keep_alive_timestamp = ;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;

	case SPDK_NVME_OPC_CREATE_IO_SQ:
	case SPDK_NVME_OPC_CREATE_IO_CQ:
	case SPDK_NVME_OPC_DELETE_IO_SQ:
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		SPDK_ERRLOG("Admin opc 0x%02X not allowed in NVMf\n", cmd->opc);
		response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	default:
		SPDK_ERRLOG("Unsupported admin command\n");
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

}

static int
nvmf_virtual_ctrlr_rw_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			  struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t lba_address;
	uint64_t blockcnt;
	uint64_t io_bytes;
	uint64_t offset;
	uint64_t llen;
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct nvme_read_cdw12 *cdw12 = (struct nvme_read_cdw12 *)&cmd->cdw12;

	blockcnt = spdk_bdev_get_num_blocks(bdev);
	lba_address = cmd->cdw11;
	lba_address = (lba_address << 32) + cmd->cdw10;
	offset = lba_address * block_size;
	llen = cdw12->nlb + 1;

	if (lba_address >= blockcnt || llen > blockcnt || lba_address > (blockcnt - llen)) {
		SPDK_ERRLOG("end of media\n");
		response->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	io_bytes = llen * bdev->blocklen;
	if (io_bytes != req->length) {
		SPDK_ERRLOG("Read/Write NLB length(%ld) != SGL length(%d)\n", io_bytes, req->length);
		response->status.dnr = 1;
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->opc == SPDK_NVME_OPC_READ) {
		/* modified to for SGL iovs */
		if (req->data) {
			if (spdk_bdev_read(desc, req->io_rsrc_pool, ch, req->data, offset, req->length,
					   nvmf_virtual_ctrlr_complete_cmd, req, &req->bdev_io)) {
				response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			}
		} else {
			/* acquire IOV buffers from backend */
			req->bdev_io = spdk_bdev_read_init(desc, ch, req->io_rsrc_pool, nvmf_virtual_ctrlr_complete_cmd,
							   req, req->iov, &req->iovcnt, req->length, offset);
			if (req->bdev_io) {
				if (spdk_bdev_readv(req->bdev_io) < 0) {
					response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
					return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
				}
			} else {
				return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_PENDING;
			}
		}
	} else { /* SPDK_NVME_OPC_WRITE */
		if (req->data) {
			if (spdk_bdev_write(desc, req->io_rsrc_pool, ch, req->data, offset, req->length,
					    nvmf_virtual_ctrlr_complete_cmd, req, &req->bdev_io)) {
				response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			}
		} else if (req->iovcnt) {
			if (spdk_bdev_writev(req->bdev_io) < 0) {
				response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			}
		} else {
			/* Acquire IOV buffers from backend */
			req->bdev_io = spdk_bdev_write_init(desc, ch, req->io_rsrc_pool, nvmf_virtual_ctrlr_complete_cmd,
							    req, req->iov, &req->iovcnt, req->length, offset);
			if (req->bdev_io) {
				return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_READY;
			} else {
				return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_PENDING;
			}
		}
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;

}

static int
nvmf_virtual_ctrlr_flush_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			     struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t nbytes;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	nbytes = spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev);
	if (spdk_bdev_flush(desc, req->io_rsrc_pool, ch, 0, nbytes, nvmf_virtual_ctrlr_complete_cmd, req)) {
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static int
nvmf_virtual_ctrlr_dsm_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			   struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	int i;
	uint32_t attribute;
	uint16_t nr;
	struct spdk_scsi_unmap_bdesc *unmap;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	bool async = false;

	nr = ((cmd->cdw10 & 0x000000ff) + 1);
	if (nr * sizeof(struct spdk_nvme_dsm_range) > req->length) {
		SPDK_ERRLOG("Dataset Management number of ranges > SGL length\n");
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	attribute = cmd->cdw11 & 0x00000007;
	if (attribute & SPDK_NVME_DSM_ATTR_DEALLOCATE) {
		struct spdk_nvme_dsm_range *dsm_range = (struct spdk_nvme_dsm_range *)req->data;
		unmap = calloc(nr, sizeof(*unmap));
		if (unmap == NULL) {
			SPDK_ERRLOG("memory allocation failure\n");
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		for (i = 0; i < nr; i++) {
			to_be64(&unmap[i].lba, dsm_range[i].starting_lba);
			to_be32(&unmap[i].block_count, dsm_range[i].length);
		}
		if (spdk_bdev_unmap(desc, ch, unmap, nr, nvmf_virtual_ctrlr_complete_cmd, req)) {
			free(unmap);
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
		req->unmap_bdesc = unmap;
		async = true;
	}

	if (async) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_virtual_ctrlr_nvme_passthru_io(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				    struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	if (spdk_bdev_nvme_io_passthru(desc, ch, &req->cmd->nvme_cmd, req->data, req->length,
				       nvmf_virtual_ctrlr_complete_cmd, req)) {
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static int
nvmf_virtual_ctrlr_process_io_cmd(struct spdk_nvmf_request *req)
{
	uint32_t nsid;
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch = NULL;
	struct spdk_nvmf_subsystem *subsystem = req->conn->sess->subsys;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	/* pre-set response details for this command */
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	nsid = cmd->nsid;

	if (nsid > subsystem->dev.virt.max_nsid || nsid == 0) {
		SPDK_ERRLOG("Unsuccessful query for nsid %u\n", cmd->nsid);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bdev = subsystem->dev.virt.ns_list[nsid - 1];
	if (bdev == NULL) {
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	desc = subsystem->dev.virt.desc[nsid - 1];

	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_WRITE:
		return nvmf_virtual_ctrlr_rw_cmd(bdev, desc, ch, req);
	case SPDK_NVME_OPC_FLUSH:
		return nvmf_virtual_ctrlr_flush_cmd(bdev, desc, ch, req);
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		return nvmf_virtual_ctrlr_dsm_cmd(bdev, desc, ch, req);
	default:
		return nvmf_virtual_ctrlr_nvme_passthru_io(bdev, desc, ch, req);
	}
}

static void
nvmf_virtual_ctrlr_queue_request_complete(void *arg1, void *arg2)
{
	struct spdk_nvmf_request *req = arg1;

	spdk_nvmf_request_complete(req);
}

static void
nvmf_virtual_ctrlr_process_io_abort(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_session *session = req->conn->sess;

	/* For now we only support aborting AEN and others NOP */
	if ((req->conn->type == CONN_TYPE_AQ) &&
	    (cmd->opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST)) {
		struct spdk_event *event = NULL;

		session->aer_req = NULL;

		/* Dont call in this context. Schedule for later */
		event = spdk_event_allocate(spdk_env_get_master_lcore(),
					    nvmf_virtual_ctrlr_queue_request_complete,
					    (void *)req, NULL);
		spdk_event_call(event);
	} else if (req->bdev_io) {
		spdk_bdev_io_abort(req->bdev_io, NULL);
	}
}

static void
nvmf_virtual_ctrlr_process_io_cleanup(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	if (!req->bdev_io) {
		return;
	}

	/* Cleanup any buffers allocated by bdev. */
	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
		spdk_bdev_read_fini(req->bdev_io);
		break;
	case SPDK_NVME_OPC_WRITE:
		spdk_bdev_write_fini(req->bdev_io);
	default:
		break; /* Do nothing. */
	}

	spdk_bdev_free_io(req->bdev_io);

	req->iovcnt = 0;
	req->bdev_io = NULL;
	return;
}

static int
nvmf_virtual_ctrlr_attach(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_bdev *bdev;
	uint32_t i;

	for (i = 0; i < subsystem->dev.virt.max_nsid; i++) {
		bdev = subsystem->dev.virt.ns_list[i];
		if (bdev == NULL) {
			continue;
		}
	}

	return 0;
}

static void
nvmf_virtual_ctrlr_detach(struct spdk_nvmf_subsystem *subsystem)
{
	uint32_t i;

	for (i = 0; i < subsystem->dev.virt.max_nsid; i++) {
		if (subsystem->dev.virt.ns_list[i]) {
			spdk_bdev_close(subsystem->dev.virt.desc[i]);
			subsystem->dev.virt.ch[i] = NULL;
			subsystem->dev.virt.ns_list[i] = NULL;
		}
	}
	subsystem->dev.virt.max_nsid = 0;
}

const struct spdk_nvmf_ctrlr_ops spdk_nvmf_virtual_ctrlr_ops = {
	.attach				= nvmf_virtual_ctrlr_attach,
	.ctrlr_get_data			= nvmf_virtual_ctrlr_get_data,
	.process_admin_cmd		= nvmf_virtual_ctrlr_process_admin_cmd,
	.process_io_cmd			= nvmf_virtual_ctrlr_process_io_cmd,
	.io_cleanup			= nvmf_virtual_ctrlr_process_io_cleanup,
	.io_abort			= nvmf_virtual_ctrlr_process_io_abort,
	.poll_for_completions		= nvmf_virtual_ctrlr_poll_for_completions,
	.detach				= nvmf_virtual_ctrlr_detach,
};
