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

#include "nvme_internal.h"

struct nvme_string {
	uint16_t	value;
	const char 	*str;
};

static const struct nvme_string admin_opcode[] = {
	{ NVME_OPC_DELETE_IO_SQ, "DELETE IO SQ" },
	{ NVME_OPC_CREATE_IO_SQ, "CREATE IO SQ" },
	{ NVME_OPC_GET_LOG_PAGE, "GET LOG PAGE" },
	{ NVME_OPC_DELETE_IO_CQ, "DELETE IO CQ" },
	{ NVME_OPC_CREATE_IO_CQ, "CREATE IO CQ" },
	{ NVME_OPC_IDENTIFY, "IDENTIFY" },
	{ NVME_OPC_ABORT, "ABORT" },
	{ NVME_OPC_SET_FEATURES, "SET FEATURES" },
	{ NVME_OPC_GET_FEATURES, "GET FEATURES" },
	{ NVME_OPC_ASYNC_EVENT_REQUEST, "ASYNC EVENT REQUEST" },
	{ NVME_OPC_NS_MANAGEMENT, "NAMESPACE MANAGEMENT" },
	{ NVME_OPC_FIRMWARE_COMMIT, "FIRMWARE COMMIT" },
	{ NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD, "FIRMWARE IMAGE DOWNLOAD" },
	{ NVME_OPC_NS_ATTACHMENT, "NAMESPACE ATTACHMENT" },
	{ NVME_OPC_KEEP_ALIVE, "KEEP ALIVE" },
	{ NVME_OPC_FORMAT_NVM, "FORMAT NVM" },
	{ NVME_OPC_SECURITY_SEND, "SECURITY SEND" },
	{ NVME_OPC_SECURITY_RECEIVE, "SECURITY RECEIVE" },
	{ 0xFFFF, "ADMIN COMMAND" }
};

static const struct nvme_string io_opcode[] = {
	{ NVME_OPC_FLUSH, "FLUSH" },
	{ NVME_OPC_WRITE, "WRITE" },
	{ NVME_OPC_READ, "READ" },
	{ NVME_OPC_WRITE_UNCORRECTABLE, "WRITE UNCORRECTABLE" },
	{ NVME_OPC_COMPARE, "COMPARE" },
	{ NVME_OPC_WRITE_ZEROES, "WRITE ZEROES" },
	{ NVME_OPC_DATASET_MANAGEMENT, "DATASET MANAGEMENT" },
	{ NVME_OPC_RESERVATION_REGISTER, "RESERVATION REGISTER" },
	{ NVME_OPC_RESERVATION_REPORT, "RESERVATION REPORT" },
	{ NVME_OPC_RESERVATION_ACQUIRE, "RESERVATION ACQUIRE" },
	{ NVME_OPC_RESERVATION_RELEASE, "RESERVATION RELEASE" },
	{ 0xFFFF, "IO COMMAND" }
};

static const char *
nvme_get_string(const struct nvme_string *strings, uint16_t value)
{
	const struct nvme_string *entry;

	entry = strings;

	while (entry->value != 0xFFFF) {
		if (entry->value == value) {
			return entry->str;
		}
		entry++;
	}
	return entry->str;
}

static void
nvme_admin_qpair_print_command(struct spdk_nvme_qpair *qpair,
			       struct nvme_command *cmd)
{

	SPDK_NOTICELOG("%s (%02x) sqid:%d cid:%d nsid:%x "
		       "cdw10:%08x cdw11:%08x\n",
		       nvme_get_string(admin_opcode, cmd->opc), cmd->opc, qpair->id, cmd->cid,
		       cmd->nsid, cmd->cdw10, cmd->cdw11);
}

static void
nvme_io_qpair_print_command(struct spdk_nvme_qpair *qpair,
			    struct nvme_command *cmd)
{
	assert(qpair != NULL);
	assert(cmd != NULL);
	switch ((int)cmd->opc) {
	case NVME_OPC_WRITE:
	case NVME_OPC_READ:
	case NVME_OPC_WRITE_UNCORRECTABLE:
	case NVME_OPC_COMPARE:
		SPDK_NOTICELOG("%s sqid:%d cid:%d nsid:%d "
			       "lba:%llu len:%d\n",
			       nvme_get_string(io_opcode, cmd->opc), qpair->id, cmd->cid,
			       cmd->nsid,
			       ((unsigned long long)cmd->cdw11 << 32) + cmd->cdw10,
			       (cmd->cdw12 & 0xFFFF) + 1);
		break;
	case NVME_OPC_FLUSH:
	case NVME_OPC_DATASET_MANAGEMENT:
		SPDK_NOTICELOG("%s sqid:%d cid:%d nsid:%d\n",
			       nvme_get_string(io_opcode, cmd->opc), qpair->id, cmd->cid,
			       cmd->nsid);
		break;
	default:
		SPDK_NOTICELOG("%s (%02x) sqid:%d cid:%d nsid:%d\n",
			       nvme_get_string(io_opcode, cmd->opc), cmd->opc, qpair->id,
			       cmd->cid, cmd->nsid);
		break;
	}
}

void
nvme_qpair_print_command(struct spdk_nvme_qpair *qpair, struct nvme_command *cmd)
{
	assert(qpair != NULL);
	assert(cmd != NULL);

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_admin_qpair_print_command(qpair, cmd);
	} else {
		nvme_io_qpair_print_command(qpair, cmd);
	}
}

static const struct nvme_string generic_status[] = {
	{ NVME_SC_SUCCESS, "SUCCESS" },
	{ NVME_SC_INVALID_OPCODE, "INVALID OPCODE" },
	{ NVME_SC_INVALID_FIELD, "INVALID FIELD" },
	{ NVME_SC_COMMAND_ID_CONFLICT, "COMMAND ID CONFLICT" },
	{ NVME_SC_DATA_TRANSFER_ERROR, "DATA TRANSFER ERROR" },
	{ NVME_SC_ABORTED_POWER_LOSS, "ABORTED - POWER LOSS" },
	{ NVME_SC_INTERNAL_DEVICE_ERROR, "INTERNAL DEVICE ERROR" },
	{ NVME_SC_ABORTED_BY_REQUEST, "ABORTED - BY REQUEST" },
	{ NVME_SC_ABORTED_SQ_DELETION, "ABORTED - SQ DELETION" },
	{ NVME_SC_ABORTED_FAILED_FUSED, "ABORTED - FAILED FUSED" },
	{ NVME_SC_ABORTED_MISSING_FUSED, "ABORTED - MISSING FUSED" },
	{ NVME_SC_INVALID_NAMESPACE_OR_FORMAT, "INVALID NAMESPACE OR FORMAT" },
	{ NVME_SC_COMMAND_SEQUENCE_ERROR, "COMMAND SEQUENCE ERROR" },
	{ NVME_SC_INVALID_SGL_SEG_DESCRIPTOR, "INVALID SGL SEGMENT DESCRIPTOR" },
	{ NVME_SC_INVALID_NUM_SGL_DESCIRPTORS, "INVALID NUMBER OF SGL DESCRIPTORS" },
	{ NVME_SC_DATA_SGL_LENGTH_INVALID, "DATA SGL LENGTH INVALID" },
	{ NVME_SC_METADATA_SGL_LENGTH_INVALID, "METADATA SGL LENGTH INVALID" },
	{ NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID, "SGL DESCRIPTOR TYPE INVALID" },
	{ NVME_SC_INVALID_CONTROLLER_MEM_BUF, "INVALID CONTROLLER MEMORY BUFFER" },
	{ NVME_SC_INVALID_PRP_OFFSET, "INVALID PRP OFFSET" },
	{ NVME_SC_ATOMIC_WRITE_UNIT_EXCEEDED, "ATOMIC WRITE UNIT EXCEEDED" },
	{ NVME_SC_INVALID_SGL_OFFSET, "INVALID SGL OFFSET" },
	{ NVME_SC_INVALID_SGL_SUBTYPE, "INVALID SGL SUBTYPE" },
	{ NVME_SC_HOSTID_INCONSISTENT_FORMAT, "HOSTID INCONSISTENT FORMAT" },
	{ NVME_SC_KEEP_ALIVE_EXPIRED, "KEEP ALIVE EXPIRED" },
	{ NVME_SC_KEEP_ALIVE_INVALID, "KEEP ALIVE INVALID" },
	{ NVME_SC_LBA_OUT_OF_RANGE, "LBA OUT OF RANGE" },
	{ NVME_SC_CAPACITY_EXCEEDED, "CAPACITY EXCEEDED" },
	{ NVME_SC_NAMESPACE_NOT_READY, "NAMESPACE NOT READY" },
	{ NVME_SC_RESERVATION_CONFLICT, "RESERVATION CONFLICT" },
	{ NVME_SC_FORMAT_IN_PROGRESS, "FORMAT IN PROGRESS" },
	{ 0xFFFF, "GENERIC" }
};

static const struct nvme_string command_specific_status[] = {
	{ NVME_SC_COMPLETION_QUEUE_INVALID, "INVALID COMPLETION QUEUE" },
	{ NVME_SC_INVALID_QUEUE_IDENTIFIER, "INVALID QUEUE IDENTIFIER" },
	{ NVME_SC_MAXIMUM_QUEUE_SIZE_EXCEEDED, "MAX QUEUE SIZE EXCEEDED" },
	{ NVME_SC_ABORT_COMMAND_LIMIT_EXCEEDED, "ABORT CMD LIMIT EXCEEDED" },
	{ NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED, "ASYNC LIMIT EXCEEDED" },
	{ NVME_SC_INVALID_FIRMWARE_SLOT, "INVALID FIRMWARE SLOT" },
	{ NVME_SC_INVALID_FIRMWARE_IMAGE, "INVALID FIRMWARE IMAGE" },
	{ NVME_SC_INVALID_INTERRUPT_VECTOR, "INVALID INTERRUPT VECTOR" },
	{ NVME_SC_INVALID_LOG_PAGE, "INVALID LOG PAGE" },
	{ NVME_SC_INVALID_FORMAT, "INVALID FORMAT" },
	{ NVME_SC_FIRMWARE_REQ_RESET, "FIRMWARE REQUIRES CONVENTIONAL RESET" },
	{ NVME_SC_INVALID_QUEUE_DELETION, "INVALID QUEUE DELETION" },
	{ NVME_SC_FEATURE_ID_NOT_SAVEABLE, "FEATURE ID NOT SAVEABLE" },
	{ NVME_SC_FEATURE_NOT_CHANGEABLE, "FEATURE NOT CHANGEABLE" },
	{ NVME_SC_FEATURE_NOT_NAMESPACE_SPECIFIC, "FEATURE NOT NAMESPACE SPECIFIC" },
	{ NVME_SC_FIRMWARE_REQ_NVM_RESET, "FIRMWARE REQUIRES NVM RESET" },
	{ NVME_SC_FIRMWARE_REQ_RESET, "FIRMWARE REQUIRES RESET" },
	{ NVME_SC_FIRMWARE_REQ_MAX_TIME_VIOLATION, "FIRMWARE REQUIRES MAX TIME VIOLATION" },
	{ NVME_SC_FIRMWARE_ACTIVATION_PROHIBITED, "FIRMWARE ACTIVATION PROHIBITED" },
	{ NVME_SC_OVERLAPPING_RANGE, "OVERLAPPING RANGE" },
	{ NVME_SC_NAMESPACE_INSUFFICIENT_CAPACITY, "NAMESPACE INSUFFICIENT CAPACITY" },
	{ NVME_SC_NAMESPACE_ID_UNAVAILABLE, "NAMESPACE ID UNAVAILABLE" },
	{ NVME_SC_NAMESPACE_ALREADY_ATTACHED, "NAMESPACE ALREADY ATTACHED" },
	{ NVME_SC_NAMESPACE_IS_PRIVATE, "NAMESPACE IS PRIVATE" },
	{ NVME_SC_NAMESPACE_NOT_ATTACHED, "NAMESPACE NOT ATTACHED" },
	{ NVME_SC_THINPROVISIONING_NOT_SUPPORTED, "THINPROVISIONING NOT SUPPORTED" },
	{ NVME_SC_CONTROLLER_LIST_INVALID, "CONTROLLER LIST INVALID" },
	{ NVME_SC_CONFLICTING_ATTRIBUTES, "CONFLICTING ATTRIBUTES" },
	{ NVME_SC_INVALID_PROTECTION_INFO, "INVALID PROTECTION INFO" },
	{ NVME_SC_ATTEMPTED_WRITE_TO_RO_PAGE, "WRITE TO RO PAGE" },
	{ 0xFFFF, "COMMAND SPECIFIC" }
};

static const struct nvme_string media_error_status[] = {
	{ NVME_SC_WRITE_FAULTS, "WRITE FAULTS" },
	{ NVME_SC_UNRECOVERED_READ_ERROR, "UNRECOVERED READ ERROR" },
	{ NVME_SC_GUARD_CHECK_ERROR, "GUARD CHECK ERROR" },
	{ NVME_SC_APPLICATION_TAG_CHECK_ERROR, "APPLICATION TAG CHECK ERROR" },
	{ NVME_SC_REFERENCE_TAG_CHECK_ERROR, "REFERENCE TAG CHECK ERROR" },
	{ NVME_SC_COMPARE_FAILURE, "COMPARE FAILURE" },
	{ NVME_SC_ACCESS_DENIED, "ACCESS DENIED" },
	{ NVME_SC_DEALLOCATED_OR_UNWRITTEN_BLOCK, "DEALLOCATED OR UNWRITTEN BLOCK" },
	{ 0xFFFF, "MEDIA ERROR" }
};

static const char *
get_status_string(uint16_t sct, uint16_t sc)
{
	const struct nvme_string *entry;

	switch (sct) {
	case NVME_SCT_GENERIC:
		entry = generic_status;
		break;
	case NVME_SCT_COMMAND_SPECIFIC:
		entry = command_specific_status;
		break;
	case NVME_SCT_MEDIA_ERROR:
		entry = media_error_status;
		break;
	case NVME_SCT_VENDOR_SPECIFIC:
		return "VENDOR SPECIFIC";
	default:
		return "RESERVED";
	}

	return nvme_get_string(entry, sc);
}

void
nvme_qpair_print_completion(struct spdk_nvme_qpair *qpair,
			    struct nvme_completion *cpl)
{
	SPDK_NOTICELOG("%s (%02x/%02x) sqid:%d cid:%d cdw0:%x sqhd:%04x p:%x m:%x dnr:%x\n",
		       get_status_string(cpl->status.sct, cpl->status.sc),
		       cpl->status.sct, cpl->status.sc, cpl->sqid, cpl->cid, cpl->cdw0,
		       cpl->sqhd, cpl->status.p, cpl->status.m, cpl->status.dnr);
}

bool
nvme_completion_is_retry(const struct nvme_completion *cpl)
{
	/*
	 * TODO: spec is not clear how commands that are aborted due
	 *  to TLER will be marked.  So for now, it seems
	 *  NAMESPACE_NOT_READY is the only case where we should
	 *  look at the DNR bit.
	 */
	switch ((int)cpl->status.sct) {
	case NVME_SCT_GENERIC:
		switch ((int)cpl->status.sc) {
		case NVME_SC_NAMESPACE_NOT_READY:
		case NVME_SC_FORMAT_IN_PROGRESS:
			if (cpl->status.dnr) {
				return false;
			} else {
				return true;
			}
		case NVME_SC_INVALID_OPCODE:
		case NVME_SC_INVALID_FIELD:
		case NVME_SC_COMMAND_ID_CONFLICT:
		case NVME_SC_DATA_TRANSFER_ERROR:
		case NVME_SC_ABORTED_POWER_LOSS:
		case NVME_SC_INTERNAL_DEVICE_ERROR:
		case NVME_SC_ABORTED_BY_REQUEST:
		case NVME_SC_ABORTED_SQ_DELETION:
		case NVME_SC_ABORTED_FAILED_FUSED:
		case NVME_SC_ABORTED_MISSING_FUSED:
		case NVME_SC_INVALID_NAMESPACE_OR_FORMAT:
		case NVME_SC_COMMAND_SEQUENCE_ERROR:
		case NVME_SC_LBA_OUT_OF_RANGE:
		case NVME_SC_CAPACITY_EXCEEDED:
		default:
			return false;
		}
	case NVME_SCT_COMMAND_SPECIFIC:
	case NVME_SCT_MEDIA_ERROR:
	case NVME_SCT_VENDOR_SPECIFIC:
	default:
		return false;
	}
}

static void
nvme_qpair_manual_complete_request(struct spdk_nvme_qpair *qpair,
				   struct nvme_request *req, uint32_t sct, uint32_t sc,
				   bool print_on_error)
{
	struct nvme_completion	cpl;
	bool			error;

	memset(&cpl, 0, sizeof(cpl));
	cpl.sqid = qpair->id;
	cpl.status.sct = sct;
	cpl.status.sc = sc;

	error = spdk_nvme_cpl_is_error(&cpl);

	if (error && print_on_error) {
		nvme_qpair_print_command(qpair, &req->cmd);
		nvme_qpair_print_completion(qpair, &cpl);
	}

	if (req->cb_fn) {
		req->cb_fn(req->cb_arg, &cpl);
	}

	nvme_free_request(req);
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	return nvme_transport_qpair_process_completions(qpair, max_completions);
}

int
nvme_qpair_init(struct spdk_nvme_qpair *qpair, uint16_t id,
		uint16_t num_entries,
		struct spdk_nvme_ctrlr *ctrlr,
		enum nvme_qprio qprio)
{
	assert(num_entries != 0);

	qpair->id = id;
	qpair->num_entries = num_entries;
	qpair->qprio = qprio;

	qpair->ctrlr = ctrlr;
	qpair->trtype = ctrlr->trid.trtype;

	STAILQ_INIT(&qpair->queued_req);

	return 0;
}

int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	int			rc = 0;
	struct nvme_request	*child_req, *tmp;
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	bool			child_req_failed = false;

	if (ctrlr->is_failed) {
		nvme_free_request(req);
		return -ENXIO;
	}

	if (req->num_children) {
		/*
		 * This is a split (parent) request. Submit all of the children but not the parent
		 * request itself, since the parent is the original unsplit request.
		 */
		TAILQ_FOREACH_SAFE(child_req, &req->children, child_tailq, tmp) {
			if (!child_req_failed) {
				rc = nvme_qpair_submit_request(qpair, child_req);
				if (rc != 0)
					child_req_failed = true;
			} else { /* free remaining child_reqs since one child_req fails */
				nvme_request_remove_child(req, child_req);
				nvme_free_request(child_req);
			}
		}

		return rc;
	}

	return nvme_transport_qpair_submit_request(qpair, req);
}

static void
_nvme_io_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	struct nvme_request		*req;

	/* Manually abort each queued I/O. */
	while (!STAILQ_EMPTY(&qpair->queued_req)) {
		req = STAILQ_FIRST(&qpair->queued_req);
		STAILQ_REMOVE_HEAD(&qpair->queued_req, stailq);
		SPDK_ERRLOG("aborting queued i/o\n");
		nvme_qpair_manual_complete_request(qpair, req, NVME_SCT_GENERIC,
						   NVME_SC_ABORTED_BY_REQUEST, true);
	}
}

void
nvme_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	if (nvme_qpair_is_io_queue(qpair)) {
		_nvme_io_qpair_enable(qpair);
	}

	nvme_transport_qpair_enable(qpair);
}

void
nvme_qpair_disable(struct spdk_nvme_qpair *qpair)
{
	nvme_transport_qpair_disable(qpair);
}

void
nvme_qpair_fail(struct spdk_nvme_qpair *qpair)
{
	struct nvme_request		*req;

	while (!STAILQ_EMPTY(&qpair->queued_req)) {
		req = STAILQ_FIRST(&qpair->queued_req);
		STAILQ_REMOVE_HEAD(&qpair->queued_req, stailq);
		SPDK_ERRLOG("failing queued i/o\n");
		nvme_qpair_manual_complete_request(qpair, req, NVME_SCT_GENERIC,
						   NVME_SC_ABORTED_BY_REQUEST, true);
	}

	nvme_transport_qpair_fail(qpair);
}
