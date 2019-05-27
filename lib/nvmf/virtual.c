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
	session->vcdata.anatt = g_nvmf_tgt.opts.anatt;
	memcpy(&session->vcdata.anacap, &g_nvmf_tgt.opts.anacap, sizeof(uint8_t));
	session->vcdata.anagrpmax = g_nvmf_tgt.opts.anagrpmax;
	session->vcdata.nanagrpid = g_nvmf_tgt.opts.nanagrpid;
	session->vcdata.nn = MAX_VIRTUAL_NAMESPACE;
	session->vcdata.mnan = g_nvmf_tgt.opts.mnan;
	memcpy(&session->vcdata.nwpc, &g_nvmf_tgt.opts.nwpc, sizeof(uint8_t));
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


	/*
	 * The status for the CMD2 of a fused command will be explicitly
	 * set by CMD1 when the fused command fails.is_fused_partner_failed
	 * flag is set only on CMD2 when CMD1 fails
	 */
	if (!req->is_fused_partner_failed) {
		spdk_bdev_io_get_nvme_status(bdev_io, &sct, &sc, &dnr);
		response->status.sc = sc;
		response->status.sct = sct;
		response->status.dnr = dnr;
	}

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
nvmf_virtual_ctrlr_get_changed_ns_log_page(struct spdk_nvmf_request *req, uint64_t offset,
		uint32_t length)
{
	uint8_t *curr_ptr = req->data;
	struct spdk_nvmf_session *sess;
	uint32_t i, copy_len, nsid;
	uint16_t num_ns_changed;

	sess = req->conn->sess;

	num_ns_changed = spdk_nvmf_session_get_num_ns_changed(sess);
	if (!num_ns_changed) {
		goto success;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF,
		      "Subsystem: %s received changed ns log page request length %u and offset %lu on controller %u\n",
		      spdk_nvmf_subsystem_get_nqn(req->conn->sess->subsys),
		      length, offset, req->conn->sess->cntlid);

	/* zero fill */
	memset(curr_ptr, 0, length);

	if (sess->ns_changed_map.ns_changed_count > CHANGED_NS_MAX_NS_TO_REPORT) {
		*((uint32_t *)curr_ptr) = CHANGED_NS_LOG_TOO_MANY_NAME_SPACES;
		goto success;
	}

	for (i = 0; i < MAX_VIRTUAL_NAMESPACE; i++) {
		nsid = i + 1;
		if (spdk_nvmf_session_has_ns_changed(sess, nsid)) {
			copy_len = sizeof(uint32_t);
			if (offset >= copy_len) {
				/* No copy, skip ahead */
				offset -= copy_len;
			} else {
				copy_len -= offset;
				if (copy_len > length) {
					copy_len = length;
				}
				length -= copy_len;
				memcpy(curr_ptr, (char *)&nsid + offset, copy_len);
				if (!length) {
					goto success;
				}
				curr_ptr += copy_len;
				offset = 0;
			}
		}
	}

	goto success;
success:
	spdk_nvmf_session_reset_ns_changed_map(sess);
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_virtual_ctrlr_get_ana_log_page(struct spdk_nvmf_request *req, uint8_t lsp, uint64_t offset,
				    uint32_t length)
{
	struct spdk_nvmf_subsystem *subsys = NULL;
	uint32_t nsid, num_ns, copy_len;
	uint8_t *curr_ptr = req->data;
	struct spdk_nvmf_ana_log_page hdr = {0};
	struct spdk_nvmf_ana_group_desc_format group_desc = {0};
	struct spdk_nvmf_ana_group *ana_subsys_entry = NULL;
	struct spdk_nvmf_ana_group *ana_subsys_entry_tmp = NULL;
	uint8_t ana_state;
	bool rgo = (lsp & 0x01);

	subsys = req->conn->sess->subsys;
	num_ns = subsys->dev.virt.max_nsid;

	SPDK_TRACELOG(SPDK_TRACE_NVMF,
		      "Subsystem: %s received ana log page request with rgo %u, length %u and offset %lu on controller %u\n",
		      spdk_nvmf_subsystem_get_nqn(subsys), rgo, length, offset, req->conn->sess->cntlid);

	if (offset >= sizeof(struct spdk_nvmf_ana_log_page)) {
		/** Skip header */
		offset -= sizeof(struct spdk_nvmf_ana_log_page);
	} else {
		/** Copy (part of) header */
		hdr.change_count = req->conn->sess->ana_log_change_count;
		hdr.num_group_descs = subsys->num_ana_groups;
		copy_len = sizeof(hdr) - offset;
		if (copy_len > length) {
			copy_len = length;
		}
		length -= copy_len;
		memcpy(curr_ptr, (char *)&hdr + offset, copy_len);
		if (!length) {
			goto done;
		}
		curr_ptr += copy_len;
		offset = 0;
	}

	TAILQ_FOREACH_SAFE(ana_subsys_entry, &subsys->ana_groups, link, ana_subsys_entry_tmp) {
		assert(ana_subsys_entry->anagrpid != 0);
		if (offset >= sizeof(struct spdk_nvmf_ana_group_desc_format)) {
			/** Skip log page entry for this group */
			offset -= sizeof(struct spdk_nvmf_ana_group_desc_format);
		} else {
			/** Copy (part of) log page entry for this group */
			group_desc.anagrpid = ana_subsys_entry->anagrpid;
			group_desc.num_nsids = rgo ? 0 : ana_subsys_entry->num_nsids;
			/* We do not support group level change count */
			group_desc.change_count = 0;
			ana_state = spdk_nvmf_subsystem_get_ana_group_state(subsys, ana_subsys_entry->anagrpid,
					req->conn->sess->cntlid);
			memcpy(&group_desc.ana_state, &ana_state, sizeof(group_desc.ana_state));
			copy_len = sizeof(group_desc) - offset;
			if (copy_len > length) {
				copy_len = length;
			}
			length -= copy_len;
			memcpy(curr_ptr, (char *)&group_desc + offset, copy_len);
			if (!length) {
				goto done;
			}
			curr_ptr += copy_len;
			offset = 0;
		}

		if (!rgo) {
			if (offset >= (ana_subsys_entry->num_nsids * sizeof(nsid))) {
				/** Skip all nsids for this group */
				offset -= (ana_subsys_entry->num_nsids * sizeof(nsid));
			} else {
				/** Copy (part of) nsids for this group */
				for (nsid = 1; nsid <= num_ns; nsid++) {
					if (subsys->dev.virt.ns_list[nsid - 1] == NULL) {
						continue;
					}

					if (subsys->dev.virt.ns_list[nsid - 1]->anagrpid == ana_subsys_entry->anagrpid) {
						if (offset >= sizeof(nsid)) {
							/** Skip this nsid */
							offset -= sizeof(nsid);
						} else {
							/** Copy (part of) this nsid */
							copy_len = sizeof(nsid) - offset;
							if (copy_len > length) {
								copy_len = length;
							}
							length -= copy_len;
							memcpy(curr_ptr, (char *)&nsid + offset, copy_len);
							if (!length) {
								goto done;
							}
							curr_ptr += copy_len;
							offset = 0;
						}
					}
				}
				assert(offset == 0);
			}
		}
	}
	goto done;
done:
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_virtual_ctrlr_get_vendor_specific_log_page(struct spdk_nvmf_request *req,
		uint64_t offset)
{
	struct spdk_nvmf_subsystem *subsys = req->conn->sess->subsys;
	if (subsys->app_cbs->get_vs_log_page) {
		if (subsys->app_cbs->get_vs_log_page(req, offset) < 0) {
			/*
			 * In case of any error, since this is a vendor specific
			 * log page, it will be upto the application callback
			 * function i.e. get_vs_log_page(..) to set the appropriate
			 * status code to return in the command response.
			 */
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Vendor specific log page callback not defined\n");
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static inline uint32_t
nvmf_get_log_page_len(struct spdk_nvme_cmd *cmd)
{
	uint32_t numdl = (cmd->cdw10 >> 16) & 0xFFFFu;
	uint32_t numdu = (cmd->cdw11) & 0xFFFFu;
	return ((numdu << 16) + numdl + 1) * sizeof(uint32_t);
}

static int
nvmf_virtual_ctrlr_get_log_page(struct spdk_nvmf_request *req)
{
	uint8_t lid;
	uint8_t lsp = 0;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	uint32_t len;
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
	lsp = (cmd->cdw10 >> 8) & 0xF;
	len = nvmf_get_log_page_len(cmd);

	if (!len) {
		SPDK_ERRLOG("Get log page with 0 length\n");
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	if (len > req->length) {
		SPDK_ERRLOG("Get log page: len (%u) > buf size (%u)\n",
			    len, req->length);
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	switch (lid) {
	case SPDK_NVME_LOG_ERROR:
	case SPDK_NVME_LOG_HEALTH_INFORMATION:
	case SPDK_NVME_LOG_FIRMWARE_SLOT:
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	case SPDK_NVME_ANA_LOG:
		return nvmf_virtual_ctrlr_get_ana_log_page(req, lsp, log_page_offset, len);
		break;
	case SPDK_NVME_LOG_CHANGED_NS_LIST:
		return nvmf_virtual_ctrlr_get_changed_ns_log_page(req, log_page_offset, len);
		break;
	case SPDK_NVME_LOG_VENDOR_SPECIFIC:
		return nvmf_virtual_ctrlr_get_vendor_specific_log_page(req, log_page_offset);
		break;
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
	    struct spdk_nvme_ns_data *nsdata,
	    struct spdk_nvmf_session *session)
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

	if (spdk_nvmf_session_get_ana_status(session)) {
		nsdata->anagrpid = bdev->anagrpid;
		if ((spdk_bdev_get_ana_state(bdev, session->cntlid) ==
		     SPDK_NVME_ANA_INACCESSIBLE) ||
		    (spdk_bdev_get_ana_state(bdev, session->cntlid) ==
		     SPDK_NVME_ANA_PERSISTENT_LOSS)) {
			nsdata->nuse = 0;
		} else {
			nsdata->nuse = num_blocks;
		}
	} else {
		nsdata->anagrpid = 0;
		nsdata->nuse = num_blocks;
	}

	nsdata->nsze = num_blocks;
	nsdata->ncap = num_blocks;

	if (bdev->write_protect_flags.write_protect) {
		nsdata->nsattr.write_protect = 1;
	} else {
		nsdata->nsattr.write_protect = 0;
	}

	nsdata->nlbaf = 0;
	nsdata->flbas.format = 0;
	nsdata->nmic.can_share = g_nvmf_tgt.opts.nmic;
	nsdata->lbaf[0].lbads = spdk_u32log2(spdk_bdev_get_block_size(bdev));

	if (bdev->blocklen == 4096) {
		nsdata->nawun = 0;
		nsdata->nawupf = 0;
	} else {
		nsdata->nawun = (g_nvmf_tgt.opts.max_io_size / bdev->blocklen) - 1;
		nsdata->nawupf = (g_nvmf_tgt.opts.max_io_size / bdev->blocklen) - 1;
		nsdata->nsfeat.ns_atomic_write_unit = 1;
	}

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
		return identify_ns(subsystem, cmd, rsp, req->data, session);
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
		response->cdw0 = 0;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
		return spdk_nvmf_session_get_features_keep_alive_timer(req);
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
		return spdk_nvmf_session_get_features_async_event_configuration(req);
	case SPDK_NVME_FEAT_HOST_IDENTIFIER:
		return spdk_nvmf_session_get_features_host_identifier(req);
	case SPDK_NVME_FEAT_NAMESPACE_WRITE_PROTECT_CONFIG:
		return spdk_nvmf_session_get_features_ns_write_protection_config(req);
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
	case SPDK_NVME_FEAT_NAMESPACE_WRITE_PROTECT_CONFIG:
		return spdk_nvmf_session_set_features_ns_write_protection_config(req);
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
	default:
		SPDK_ERRLOG("Admin opc 0x%02X not supported, Invalid Opcode/n", cmd->opc);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

}

static int
nvmf_virtual_ctrlr_handle_bdev_rc(int rc, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	int status;

	switch (rc) {
	default:
		/* Catch all - returns an non retriable error that will result in an IO error */
		response->status.dnr = 1;
		response->status.sc = SPDK_NVME_SC_OPERATION_DENIED;
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		break;
	case -EFAULT:
		/* Unaligned: nbytes or length is not a multiple of bdev->blocklen */
		response->status.dnr = 1;
		response->status.sc = SPDK_NVME_SC_SGL_DATA_BLOCK_GRANULARITY_INVALID;
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		break;
	case -EDOM:
		/* Overflow: offset + nbytes is less than offset */
		response->status.dnr = 1;
		response->status.sc = SPDK_NVME_SC_INVALID_SGL_OFFSET;
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		break;
	case -ERANGE:
		/* offset + nbytes exceeds the size of the bdev */
		response->status.dnr = 1;
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		break;
	case -E2BIG:
		response->status.dnr = 1;
		response->status.sc = SPDK_NVME_SC_ATOMIC_WRITE_UNIT_EXCEEDED;
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		break;
	case -EINVAL:
		SPDK_ERRLOG("end of media\n");
		response->status.dnr = 1;
		response->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		break;
	case -EPERM:
		response->status.dnr = 1;
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		break;
	case -EBADF:
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_NAMESPACE_IS_WRITE_PROTECTED;
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		break;
	case -ENOMEM:
	/* fall through */
	case -EAGAIN:
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_PENDING;
		break;
	case 0:
		/* Success! */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_READY;
	}

	SPDK_ERRLOG("NVM opc 0x%02X failed: rc: %d sct: 0x%x sc: 0x%x, dnr: %d, status: %d\n",
		    cmd->opc, rc, response->status.sct, response->status.sc, response->status.dnr, status);

	return status;
}

static int
nvmf_virtual_ctrlr_rw_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			  struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t lba_address;
	uint64_t fused_lba_address;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cmd *fused_cmd = NULL;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvme_cpl *fused_response = NULL;
	struct nvme_read_cdw12 *cdw12 = (struct nvme_read_cdw12 *)&cmd->cdw12;
	struct nvme_read_cdw12 *fused_cdw12 = NULL;

	lba_address = cmd->cdw11;
	lba_address = (lba_address << 32) + cmd->cdw10;

	/* SPDK expects the transport to submit the fused
	   commands in order. The lba range validation checks
	   are performed when the first command is submitted
	 */
	if (cmd->fuse == SPDK_NVME_FUSED_CMD1) {
		assert(req->fused_partner);
		fused_cmd = &req->fused_partner->cmd->nvme_cmd;
		fused_lba_address = fused_cmd->cdw11;
		fused_lba_address = (fused_lba_address << 32) + fused_cmd->cdw10;
		fused_cdw12 = (struct nvme_read_cdw12 *)&fused_cmd->cdw12;
		if (cmd->nsid != fused_cmd->nsid || lba_address != fused_lba_address ||
		    cdw12->nlb != fused_cdw12->nlb) {
			SPDK_ERRLOG("Incorrect or mismatched Fused command NSID(%d:%d) LBA(%ld:%ld) NLB(%d:%d)\n",
				    cmd->nsid,
				    fused_cmd->nsid, lba_address,
				    fused_lba_address, cdw12->nlb, fused_cdw12->nlb);
			response->status.dnr = 1;
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			req->fused_partner->fail_with_fused_aborted = false;
			fused_response = &req->fused_partner->rsp->nvme_cpl;
			fused_response->status.dnr = 1;
			fused_response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		if (cdw12->nlb != req->conn->sess->vcdata.acwu) {
			SPDK_ERRLOG("Fused command larger than ACWU(%d:%d)\n", req->conn->sess->vcdata.acwu, cdw12->nlb);
			response->status.dnr = 1;
			response->status.sc = SPDK_NVME_SC_ATOMIC_WRITE_UNIT_EXCEEDED;
			req->fused_partner->fail_with_fused_aborted = false;
			fused_response = &req->fused_partner->rsp->nvme_cpl;
			fused_response->status.dnr = 1;
			fused_response->status.sc = SPDK_NVME_SC_ATOMIC_WRITE_UNIT_EXCEEDED;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}

	assert(req->bdev_io != NULL);
	if (req->bdev_io == NULL) {
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_WRITE:
	case SPDK_NVME_OPC_COMPARE:
		switch (spdk_bdev_submit_io(req->bdev_io)) {
		case 0: /* success! */
			break;
		default:
			/* XXX need to translate errno's returned from bdev here XXX */
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
		break;
	default:
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;

}

#ifndef NETAPP
static int
nvmf_virtual_ctrlr_flush_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			     struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	assert(req->bdev_io != NULL);
	if (req->bdev_io == NULL) {
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	switch (spdk_bdev_submit_io(req->bdev_io)) {
	case 0: /* success! */
		break;
	default:
		/* XXX need to translate errno's returned from bdev here XXX */
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

	assert(req->bdev_io != NULL);
	if (req->bdev_io == NULL) {
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	nr = ((cmd->cdw10 & 0x000000ff) + 1);
	attribute = cmd->cdw11 & 0x00000007;

	if (attribute & SPDK_NVME_DSM_ATTR_DEALLOCATE) {
		struct spdk_nvme_dsm_range *dsm_range = (struct spdk_nvme_dsm_range *)req->data;
		unmap = req->unmap_bdesc;
		for (i = 0; i < nr; i++) {
			to_be64(&unmap[i].lba, dsm_range[i].starting_lba);
			to_be32(&unmap[i].block_count, dsm_range[i].length);
		}
	}

	switch (spdk_bdev_submit_io(req->bdev_io)) {
	case 0: /* success! */
		break;
	default:
		/* XXX need to translate errno's returned from bdev here XXX */
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static int
nvmf_virtual_ctrlr_nvme_passthru_io(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				    struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	assert(req->bdev_io != NULL);
	if (req->bdev_io == NULL) {
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	switch (spdk_bdev_submit_io(req->bdev_io)) {
	case 0: /* success! */
		break;
	default:
		/* XXX need to translate errno's returned from bdev here XXX */
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}
#endif

static bool
nvmf_virtual_ctrlr_is_compare_supported(struct spdk_nvmf_request *req)
{
	if (spdk_nvmf_is_fused_command(&req->cmd->nvme_cmd)) {
		return (g_nvmf_tgt.opts.fuses & 0x1);
	} else {
		return (g_nvmf_tgt.opts.oncs & 0x1);
	}
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

	if (spdk_nvmf_is_fused_command(&req->cmd->nvme_cmd)) {
		/* Rules for fused command:
		   1) Whenever CMD1 fails, it will explicitly set
		   the status for CMD2 also.
		   2) CMD2 will be failed with a status of SPDK_NVME_SC_ABORTED_FAILED_FUSED
		   in most of the cases (whenever fail_with_fused_aborted flag is set).
		   3) There are a few scenarios where CMD2 will need to be failed with
		   a different status. In such cases fail_with_fused_aborted shall be unset
		   and CMD1 will set the status of CMD2 explicitly
		   */
		if (cmd->fuse == SPDK_NVME_FUSED_CMD1) {
			req->fused_partner->fail_with_fused_aborted = true;
		} else if (req->is_fused_partner_failed) {
			/* This is CMD2 and our partner CMD1 failed, fail CMD_2 also.
			   CMD1 would have set the right status for CMD2
			   during CMD1's completion */
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}

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
	case SPDK_NVME_OPC_COMPARE:
		if (nvmf_virtual_ctrlr_is_compare_supported(req)) {
			return nvmf_virtual_ctrlr_rw_cmd(bdev, desc, ch, req);
		} else {
			response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
#ifndef NETAPP
	case SPDK_NVME_OPC_FLUSH:
		return nvmf_virtual_ctrlr_flush_cmd(bdev, desc, ch, req);
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		return nvmf_virtual_ctrlr_dsm_cmd(bdev, desc, ch, req);
	default:
		return nvmf_virtual_ctrlr_nvme_passthru_io(bdev, desc, ch, req);
#else
	/*
	 * NetApp only supports the manditory opcodes Read, Write and Flush.
	 * Flush is a noop in the NetApp implementation, so return success here.
	 */
	case SPDK_NVME_OPC_FLUSH:
		response->status.sc = SPDK_NVME_SC_SUCCESS;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	default:
		SPDK_ERRLOG("NVM opc 0x%02X not allowed in NVMf\n", cmd->opc);
		response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
#endif
	}
}

static void
nvmf_virtual_ctrlr_queue_request_complete(void *arg1, void *arg2)
{
	struct spdk_nvmf_request *req = arg1;

	spdk_nvmf_request_complete(req);
}

static void
nvmf_virtual_ctrlr_io_abort(struct spdk_nvmf_request *req)
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
	} else if ((req->conn->type == CONN_TYPE_AQ) &&
		   (cmd->opc == SPDK_NVME_OPC_GET_LOG_PAGE) &&
		   ((cmd->cdw10 & 0xFF) == SPDK_NVME_LOG_VENDOR_SPECIFIC)) {

		if (!req->conn->sess->subsys->app_cbs->abort_vs_log_page_req) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF,
				      "Abort vendor specific log page request callback not defined\n");
			return;
		}
		struct spdk_event *event = NULL;
		event = spdk_event_allocate(spdk_env_get_master_lcore(),
					    req->conn->sess->subsys->app_cbs->abort_vs_log_page_req,
					    (void *)req, NULL);
		spdk_event_call(event);
	} else if (req->bdev_io) {
		spdk_bdev_io_abort(req->bdev_io);
	}
}

static int
nvmf_virtual_cntrl_get_bdev(struct spdk_nvmf_request *req, struct spdk_bdev **bdev,
			    struct spdk_bdev_desc **desc)
{
	uint32_t nsid;
	struct spdk_nvmf_subsystem *subsystem = req->conn->sess->subsys;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	nsid = cmd->nsid;

	if (nsid > subsystem->dev.virt.max_nsid || nsid == 0) {
		SPDK_ERRLOG("Unsuccessful query for nsid %u\n", cmd->nsid);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return EINVAL;
	}

	*bdev = subsystem->dev.virt.ns_list[nsid - 1];
	if (*bdev == NULL) {
		SPDK_ERRLOG("No namespace attached at nsid %u\n", cmd->nsid);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return EINVAL;
	}

	*desc = subsystem->dev.virt.desc[nsid - 1];

	return 0;
}

static int
nvmf_virtual_ctrlr_validate_rw_request(struct spdk_nvmf_request *req, struct spdk_bdev *bdev,
				       uint64_t *offset, spdk_nvmf_request_exec_status *status)
{
	uint64_t lba_address;
	uint64_t blockcnt;
	uint64_t io_bytes;
	uint64_t llen;
	uint32_t block_size;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct nvme_read_cdw12 *cdw12 = (struct nvme_read_cdw12 *)&cmd->cdw12;

	block_size = spdk_bdev_get_block_size(bdev);
	blockcnt = spdk_bdev_get_num_blocks(bdev);
	lba_address = cmd->cdw11;
	lba_address = (lba_address << 32) + cmd->cdw10;
	if (offset != NULL) {
		*offset = lba_address * block_size;
	}

	/*
	 * Note that cdw12 for read, write, and compare are slightly different.
	 * This code is safe because the number of locical blocks field is
	 * identical for these 3 commands.
	 */
	llen = cdw12->nlb + 1;

	if (lba_address >= blockcnt || llen > blockcnt || lba_address > (blockcnt - llen)) {
		SPDK_ERRLOG("end of media\n");
		response->status.dnr = 1;
		response->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		*status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		return -EINVAL;
	}

	io_bytes = llen * bdev->blocklen;

	if (io_bytes != req->length) {
		SPDK_ERRLOG("Read/Write/Compare NLB length(%ld) != SGL length(%d)\n", io_bytes, req->length);
		response->status.dnr = 1;
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		*status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		return -ERANGE;
	}

	return 0;
}

static int
nvmf_virtual_ctrlr_validate_dsm_request(struct spdk_nvmf_request *req, struct spdk_bdev *bdev,
					spdk_nvmf_request_exec_status *status)
{
	uint32_t attribute;
	uint16_t nr;
	struct spdk_scsi_unmap_bdesc *unmap;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	nr = ((cmd->cdw10 & 0x000000ff) + 1);
	if (nr * sizeof(struct spdk_nvme_dsm_range) > req->length) {
		SPDK_ERRLOG("Dataset Management number of ranges > SGL length\n");
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		*status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		return -EINVAL;
	}

	attribute = cmd->cdw11 & 0x00000007;
	if (attribute & SPDK_NVME_DSM_ATTR_DEALLOCATE) {
		unmap = calloc(nr, sizeof(*unmap));
		if (unmap == NULL) {
			SPDK_ERRLOG("memory allocation failure\n");
			*status = SPDK_NVMF_REQUEST_EXEC_STATUS_BUFF_PENDING;
			return -EAGAIN;
		}
		req->unmap_bdesc = unmap;
	}

	return 0;
}

static int
nvmf_virtual_ctrlr_io_init(struct spdk_nvmf_request *req)
{
	struct spdk_bdev *bdev = NULL;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ch = NULL;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	enum spdk_bdev_io_type io_type;
	spdk_nvmf_request_exec_status status;
	uint64_t offset;
	uint64_t nbytes;
	uint16_t nr;
	int rc;

	if (spdk_nvmf_is_fused_command(&req->cmd->nvme_cmd)) {
		/* Rules for fused command:
		   1) Whenever CMD1 fails, it will explicitly set
		   the status for CMD2 also.
		   2) CMD2 will be failed with a status of SPDK_NVME_SC_ABORTED_FAILED_FUSED
		   in most of the cases (whenever fail_with_fused_aborted flag is set).
		   3) There are a few scenarios where CMD2 will need to be failed with
		   a different status. In such cases fail_with_fused_aborted shall be unset
		   and CMD1 will set the status of CMD2 explicitly
		   */
		if (cmd->fuse == SPDK_NVME_FUSED_CMD1) {
			req->fused_partner->fail_with_fused_aborted = true;
		} else if (req->is_fused_partner_failed) {
			/* This is CMD2 and our partner CMD1 failed, fail CMD_2 also.
			   CMD1 would have set the right status for CMD2
			   during CMD1's completion */
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}

	/* pre-set response details for this command */
	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_SUCCESS;

	assert(req->conn->type == CONN_TYPE_IOQ);

	if (nvmf_virtual_cntrl_get_bdev(req, &bdev, &desc)) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if ((io_type = spdk_bdev_nvme_opcode_to_bdev_io_type(cmd->opc, bdev)) == SPDK_BDEV_IO_TYPE_NONE) {
		SPDK_ERRLOG("Invalid opcode or io_type not supported, \n");
		response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if ((rc = spdk_bdev_get_ioctx(io_type, desc, ch, req->io_rsrc_pool,
				      nvmf_virtual_ctrlr_complete_cmd, req,
				      req->set_sge, req, (void **) &req->bdev_io))) {

		return (nvmf_virtual_ctrlr_handle_bdev_rc(rc, req));
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_WRITE:
	case SPDK_NVME_OPC_COMPARE:
		if (nvmf_virtual_ctrlr_validate_rw_request(req, bdev, &offset, &status)) {
			spdk_bdev_put_ioctx((void **) &req->bdev_io);
			return status;
		}

		rc = spdk_bdev_init_ioctx((void **) &req->bdev_io, req->iov, &req->iovcnt, req->length, offset);

		break;
	case SPDK_NVME_OPC_FLUSH:
		nbytes = spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev);

		rc = spdk_bdev_init_ioctx((void **) &req->bdev_io, NULL, NULL, nbytes, 0);

		break;
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		nr = ((cmd->cdw10 & 0x000000ff) + 1);

		if (nvmf_virtual_ctrlr_validate_dsm_request(req, bdev, &status)) {
			spdk_bdev_put_ioctx((void **) &req->bdev_io);
			return status;
		}

		rc = spdk_bdev_init_ioctx((void **) &req->bdev_io, req->unmap_bdesc, &nr, 0, 0);

		break;
	default:
		if (cmd->opc >= SPDK_NVME_OPC_VENDOR_SPECIFIC) {
			rc = spdk_bdev_init_ioctx((void **) &req->bdev_io, &req->cmd->nvme_cmd, &req->data, req->length, 0);
		} else {
			spdk_bdev_put_ioctx((void **) &req->bdev_io);
			SPDK_ERRLOG("Invalid opcode or io_type not supported, \n");
			response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}

	return (nvmf_virtual_ctrlr_handle_bdev_rc(rc, req));
}

static void
nvmf_virtual_ctrlr_io_cleanup(struct spdk_nvmf_request *req)
{
	if (!req->bdev_io) {
		return;
	}

	/* Cleanup any buffers allocated by bdev. */
	spdk_bdev_fini_io(req->bdev_io);
	spdk_bdev_put_ioctx((void **) &req->bdev_io);
	req->iovcnt = 0;
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
			subsystem->dev.virt.ns_list[i] = NULL;
		}
	}
	subsystem->dev.virt.max_nsid = 0;
}

const struct spdk_nvmf_ctrlr_ops spdk_nvmf_virtual_ctrlr_ops = {
	.attach				= nvmf_virtual_ctrlr_attach,
	.ctrlr_get_data			= nvmf_virtual_ctrlr_get_data,
	.process_admin_cmd		= nvmf_virtual_ctrlr_process_admin_cmd,
	.io_init			= nvmf_virtual_ctrlr_io_init,
	.process_io_cmd			= nvmf_virtual_ctrlr_process_io_cmd,
	.io_cleanup			= nvmf_virtual_ctrlr_io_cleanup,
	.io_abort			= nvmf_virtual_ctrlr_io_abort,
	.poll_for_completions		= nvmf_virtual_ctrlr_poll_for_completions,
	.detach				= nvmf_virtual_ctrlr_detach,
};
