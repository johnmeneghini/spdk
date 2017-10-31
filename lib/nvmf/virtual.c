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

#include "spdk/env.h"

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
#include "spdk_internal/event.h"
#include "nvmf/nvmf_internal.h"

#define MODEL_NUMBER "SPDK Virtual Controller"
#define FW_VERSION "FFFFFFFF"

/* read command dword 12 */
struct __attribute__((packed)) nvme_read_cdw12 {
	uint16_t	nlb;		/* number of logical blocks */
	uint16_t	rsvd	: 10;
	uint8_t		prinfo	: 4;	/* protection information field */
	uint8_t		fua	: 1;	/* force unit access */
	uint8_t		lr	: 1;	/* limited retry */
};

static void nvmf_virtual_set_dsm(struct spdk_nvmf_session *session)
{
	int i;

	for (i = 0; i < session->subsys->dev.virt.ns_count; i++) {
		struct spdk_bdev *bdev = session->subsys->dev.virt.ns_list[i];

		if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF,
				      "Subsystem%d Namespace %s does not support unmap - not enabling DSM\n",
				      i, bdev->name);
			return;
		}
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "All devices in Subsystem %s support unmap - enabling DSM\n",
		      spdk_nvmf_subsystem_get_nqn(session->subsys));
	session->vcdata.oncs.dsm = 1;
}

static void
nvmf_virtual_ctrlr_get_data(struct spdk_nvmf_session *session)
{
	struct spdk_nvmf_subsystem *subsys = session->subsys;

	memset(&session->vcdata, 0, sizeof(struct spdk_nvme_ctrlr_data));
	session->vcdata.vid = subsys->dev.virt.sub_pci_id.vendor_id;
	session->vcdata.ssvid = subsys->dev.virt.sub_pci_id.subvendor_id;
	spdk_strcpy_pad(session->vcdata.fr, FW_VERSION, sizeof(session->vcdata.fr), ' ');
	spdk_strcpy_pad(session->vcdata.mn, MODEL_NUMBER, sizeof(session->vcdata.mn), ' ');
	spdk_strcpy_pad(session->vcdata.sn, subsys->dev.virt.sn, sizeof(session->vcdata.sn), ' ');
	session->vcdata.rab = g_nvmf_tgt.opts.rab;
	memcpy(session->vcdata.ieee, g_nvmf_tgt.opts.ieee, 3 * sizeof(uint8_t));
	memcpy(&session->vcdata.cmic, &g_nvmf_tgt.opts.cmic, sizeof(uint32_t));
	memcpy(&session->vcdata.oaes, &g_nvmf_tgt.opts.oaes, sizeof(uint32_t));
	session->vcdata.acl = g_nvmf_tgt.opts.acl;
	session->vcdata.aerl = g_nvmf_tgt.opts.aerl;
	session->vcdata.elpe = g_nvmf_tgt.opts.elpe;
	session->vcdata.npss = g_nvmf_tgt.opts.npss;
	session->vcdata.kas = g_nvmf_tgt.opts.kas;
	memcpy(&session->vcdata.vwc, &g_nvmf_tgt.opts.vwc, sizeof(uint8_t));
	session->vcdata.awun = g_nvmf_tgt.opts.awun;
	session->vcdata.awupf = g_nvmf_tgt.opts.awupf;
	memcpy(&session->vcdata.sgls, &g_nvmf_tgt.opts.sgls, sizeof(uint32_t));
	session->vcdata.ver.bits.mjr = 1;
	session->vcdata.ver.bits.mnr = 2;
	session->vcdata.ver.bits.ter = 1;
	session->vcdata.ctratt.host_id_exhid_supported = 1;
	session->vcdata.frmw.slot1_ro = 1;
	session->vcdata.frmw.num_slots = 1;
	session->vcdata.lpa.edlp = 1;
	session->vcdata.sqes.min = 0x06;
	session->vcdata.sqes.max = 0x06;
	session->vcdata.cqes.min = 0x04;
	session->vcdata.cqes.max = 0x04;
	session->vcdata.maxcmd = g_nvmf_tgt.opts.max_queue_depth;
	session->vcdata.nn = subsys->dev.virt.ns_count;
	strncpy((char *)session->vcdata.subnqn, session->subsys->subnqn, sizeof(session->vcdata.subnqn));
	nvmf_virtual_set_dsm(session);
}

static void
nvmf_virtual_ctrlr_poll_for_completions(struct spdk_nvmf_subsystem *subsystem)
{
	return;
}

static void
nvmf_virtual_ctrlr_complete_cmd(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status,
				void *cb_arg)
{
	struct spdk_nvmf_request 	*req = cb_arg;
	struct spdk_nvme_cpl 		*response = &req->rsp->nvme_cpl;
	struct spdk_nvme_cmd            *cmd = &req->cmd->nvme_cmd;
	int				sc, sct, dnr;

	if (cmd->opc == SPDK_NVME_OPC_DATASET_MANAGEMENT) {
		spdk_free(req->unmap_bdesc);
	}

	spdk_bdev_io_get_nvme_status(bdev_io, &sc, &sct, &dnr);
	response->status.sc = sc;
	response->status.sct = sct;
	response->status.dnr = dnr;


	/*
	 * BDEV IO is freed as part of request cleanup function call.
	 */
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		req->bdev_io = bdev_io;
		req->iovcnt = bdev_io->u.read.iovcnt;
	} else {
		req->bdev_io = bdev_io;
		req->iovcnt = bdev_io->u.write.iovcnt;
	}
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

	if (cmd->nsid > subsystem->dev.virt.ns_count || cmd->nsid == 0) {
		SPDK_ERRLOG("Identify Namespace for invalid NSID %u\n", cmd->nsid);
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bdev = subsystem->dev.virt.ns_list[cmd->nsid - 1];

	nsdata->nsze = bdev->blockcnt;
	nsdata->ncap = bdev->blockcnt;
	nsdata->nuse = bdev->blockcnt;
	nsdata->nlbaf = 0;
	nsdata->flbas.format = 0;
	nsdata->lbaf[0].lbads = spdk_u32log2(bdev->blocklen);

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

	num_ns = subsystem->dev.virt.ns_count;

	for (i = 1; i <= num_ns; i++) {
		if (i <= cmd->nsid) {
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
	default:
		SPDK_ERRLOG("Identify command with unsupported CNS 0x%02x\n", cns);
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
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
nvmf_virtual_ctrlr_rw_cmd(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			  struct spdk_nvmf_request *req)
{
	uint64_t lba_address;
	uint64_t blockcnt;
	uint64_t io_bytes;
	uint64_t offset;
	uint64_t llen;
	int error;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct nvme_read_cdw12 *cdw12 = (struct nvme_read_cdw12 *)&cmd->cdw12;

	blockcnt = bdev->blockcnt;
	lba_address = cmd->cdw11;
	lba_address = (lba_address << 32) + cmd->cdw10;
	offset = lba_address * bdev->blocklen;
	llen = cdw12->nlb + 1;

	if (lba_address >= blockcnt || llen > blockcnt || lba_address > (blockcnt - llen)) {
		SPDK_ERRLOG("end of media\n");
		response->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	io_bytes = llen * bdev->blocklen;
	if (io_bytes > req->length) {
		SPDK_ERRLOG("Read/Write NLB > SGL length\n");
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->opc == SPDK_NVME_OPC_READ) {
		/* modified to for SGL iovs */
		if (req->data) {
			req->iovcnt = 1;
			req->iov[0].iov_base = req->data;
			req->iov[0].iov_len  = req->length;

			if (spdk_bdev_read(bdev, ch, req->data, offset, req->length,
					   nvmf_virtual_ctrlr_complete_cmd, req) == NULL) {
				response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			}
		} else {
			/* acquire IOV buffers from backend */
			error = spdk_bdev_read_init(bdev, req->length, req->iov, &req->iovcnt);
			if (error < 0) {
				return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_PENDING;
			} else if (error == 0) {
				if (spdk_bdev_readv(bdev, ch, req->iov, req->iovcnt, offset, req->length,
						    nvmf_virtual_ctrlr_complete_cmd, req) == NULL) {
					response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
					return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
				}
			} else {
				return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_ERROR;
			}
		}
	} else { /* SPDK_NVME_OPC_WRITE */
		if (req->data) {
			spdk_bdev_write_init(bdev, req->length, req->iov, &req->iovcnt, &req->iovctx);
			bcopy(req->data, req->iov[0].iov_base, req->length); // TODO: bcopy each elements
			if (spdk_bdev_writev(bdev, ch, req->iov, req->iovcnt, offset, req->length,
					     nvmf_virtual_ctrlr_complete_cmd, req) == NULL) {
				response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			}
		} else if (req->iovcnt) {
			if (spdk_bdev_writev(bdev, ch, req->iov, req->iovcnt, offset, req->length,
					     nvmf_virtual_ctrlr_complete_cmd, req) == NULL) {
				response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			}
		} else {
			/* Acquire IOV buffers from backend */
			error = spdk_bdev_write_init(bdev, req->length, req->iov, &req->iovcnt, &req->iovctx);
			if (error < 0) {
				return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_PENDING;
			} else if (error == 0) {
				return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_READY;
			} else {
				return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_ERROR;
			}
		}
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;

}

static int
nvmf_virtual_ctrlr_flush_cmd(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			     struct spdk_nvmf_request *req)
{

	uint64_t nbytes;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	nbytes = bdev->blockcnt * bdev->blocklen;
	if (spdk_bdev_flush(bdev, ch, 0, nbytes, nvmf_virtual_ctrlr_complete_cmd, req) == NULL) {
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static int
nvmf_virtual_ctrlr_dsm_cmd(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			   struct spdk_nvmf_request *req)
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
		unmap = spdk_calloc(nr, sizeof(*unmap));
		if (unmap == NULL) {
			SPDK_ERRLOG("memory allocation failure\n");
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		for (i = 0; i < nr; i++) {
			to_be64(&unmap[i].lba, dsm_range[i].starting_lba);
			to_be32(&unmap[i].block_count, dsm_range[i].length);
		}
		if (spdk_bdev_unmap(bdev, ch, unmap, nr, nvmf_virtual_ctrlr_complete_cmd, req) == NULL) {
			spdk_free(unmap);
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
nvmf_virtual_ctrlr_process_io_cmd(struct spdk_nvmf_request *req)
{
	uint32_t nsid;
	struct spdk_bdev *bdev;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_subsystem *subsystem = req->conn->sess->subsys;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	/* pre-set response details for this command */
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	nsid = cmd->nsid;

	if (nsid > subsystem->dev.virt.ns_count || nsid == 0) {
		SPDK_ERRLOG("Unsuccessful query for nsid %u\n", cmd->nsid);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bdev = subsystem->dev.virt.ns_list[nsid - 1];
	ch = subsystem->dev.virt.ch[nsid - 1];
	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_WRITE:
		return nvmf_virtual_ctrlr_rw_cmd(bdev, ch, req);
	case SPDK_NVME_OPC_FLUSH:
		return nvmf_virtual_ctrlr_flush_cmd(bdev, ch, req);
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		return nvmf_virtual_ctrlr_dsm_cmd(bdev, ch, req);
	default:
		SPDK_ERRLOG("Unsupported IO command opc: %x\n", cmd->opc);
		response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
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
	}

	/*
	 * NOTE: bdev ios abort handlers should not call request complete
	 * in this context. They need to shedule for later.
	 */
}

static void
nvmf_virtual_ctrlr_process_io_cleanup(struct spdk_nvmf_request *req)
{
	uint32_t nsid;
	struct spdk_bdev *bdev;
	struct spdk_nvmf_subsystem *subsystem = req->conn->sess->subsys;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	nsid = cmd->nsid;
	if (nsid > subsystem->dev.virt.ns_count || nsid == 0) {
		return;
	}

	bdev = subsystem->dev.virt.ns_list[nsid - 1];
	if (!bdev) {
		return;
	}

	/* Cleanup any buffers allocated by bdev. */
	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
		if (req->iovcnt) {
			spdk_bdev_read_fini(bdev, req->iov, req->iovcnt, req->iovctx);
		}
		break;
	case SPDK_NVME_OPC_WRITE:
		if (req->iovcnt) {
			spdk_bdev_write_fini(bdev, req->iov, req->iovcnt, req->iovctx);
		}
	default:
		break; /* Do nothing. */
	}

	if (req->bdev_io) {
		spdk_bdev_free_io(req->bdev_io);
	}

	req->iovcnt = 0;
	req->iovctx = NULL;
	req->bdev_io = NULL;
	return;
}

static int
nvmf_virtual_ctrlr_attach(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_bdev *bdev;
	struct spdk_io_channel *ch;
	uint32_t i;

	for (i = 0; i < subsystem->dev.virt.ns_count; i++) {
		bdev = subsystem->dev.virt.ns_list[i];
		ch = spdk_bdev_get_io_channel(bdev, SPDK_IO_PRIORITY_DEFAULT);
		if (ch == NULL) {
			SPDK_ERRLOG("io_channel allocation failed\n");
			return -1;
		}
		subsystem->dev.virt.ch[i] = ch;
	}

	return 0;
}

static void
nvmf_virtual_ctrlr_detach(struct spdk_nvmf_subsystem *subsystem)
{
	uint32_t i;

	for (i = 0; i < subsystem->dev.virt.ns_count; i++) {
		if (subsystem->dev.virt.ns_list[i]) {
			if (subsystem->dev.virt.ch[i]) {
				spdk_put_io_channel(subsystem->dev.virt.ch[i]);
			}
			spdk_bdev_unclaim(subsystem->dev.virt.ns_list[i]);
			subsystem->dev.virt.ch[i] = NULL;
			subsystem->dev.virt.ns_list[i] = NULL;
		}
	}
	subsystem->dev.virt.ns_count = 0;
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
