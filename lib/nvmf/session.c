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

#include "session.h"
#include "nvmf_internal.h"
#include "request.h"
#include "subsystem.h"
#include "transport.h"

#include "spdk/trace.h"
#include "spdk/nvme_spec.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

#define MIN_KEEP_ALIVE_TIMEOUT 10000
#define SPDK_NVMF_BLOCK_SIZE 4096

static void
nvmf_init_discovery_session_properties(struct spdk_nvmf_session *session)
{
	session->vcdata.maxcmd = session->host->max_aq_depth;
	/* extended data for get log page supportted */
	session->vcdata.lpa.edlp = 1;
	session->vcdata.cntlid = session->cntlid;
	session->vcdata.nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
	session->vcdata.nvmf_specific.iorcsz = sizeof(struct spdk_nvme_cpl) / 16;
	session->vcdata.nvmf_specific.icdoff = 0; /* offset starts directly after SQE */
	session->vcdata.nvmf_specific.ctrattr.ctrlr_model = SPDK_NVMF_CTRLR_MODEL_DYNAMIC;
	session->vcdata.nvmf_specific.msdbd = 1; /* target supports single SGL in capsule */
	memcpy(&session->vcdata.sgls, &g_nvmf_tgt.opts.sgls, sizeof(uint32_t));
	session->vcdata.mdts = spdk_u32log2(g_nvmf_tgt.opts.max_io_size / SPDK_NVMF_BLOCK_SIZE);

	strncpy((char *)session->vcdata.subnqn, SPDK_NVMF_DISCOVERY_NQN, sizeof(session->vcdata.subnqn));

	/* Properties */
	session->vcprop.cap.raw = 0;
	session->vcprop.cap.bits.cqr = 1;	/* NVMF specification required */
	session->vcprop.cap.bits.mqes = (session->vcdata.maxcmd - 1);	/* max queue depth */
	session->vcprop.cap.bits.ams = 0;	/* optional arb mechanisms */
	session->vcprop.cap.bits.dstrd = 0;	/* fixed to 0 for NVMf */
	session->vcprop.cap.bits.css_nvm = 1; /* NVM command set */
	session->vcprop.cap.bits.mpsmin = 0; /* 2 ^ 12 + mpsmin == 4k */
	session->vcprop.cap.bits.mpsmax = 0; /* 2 ^ 12 + mpsmax == 4k */

	if (g_nvmf_tgt.opts.nvmever == SPDK_NVME_VERSION(1, 3, 0)) {
		/* Version Supported: 1.3.0 */
		session->vcprop.vs.bits.mjr = 1;
		session->vcprop.vs.bits.mnr = 3;
		session->vcprop.vs.bits.ter = 0;
	}
	/* Report at least version 1.2.1 */
	if (session->vcprop.vs.raw < SPDK_NVME_VERSION(1, 2, 1)) {
		session->vcprop.vs.bits.mjr = 1;
		session->vcprop.vs.bits.mnr = 2;
		session->vcprop.vs.bits.ter = 1;
	}
	session->vcdata.ver = session->vcprop.vs;

	session->vcprop.cc.raw = 0;

	session->vcprop.csts.raw = 0;
	session->vcprop.csts.bits.rdy = 0; /* Init controller as not ready */
}

static void
nvmf_init_nvme_session_properties(struct spdk_nvmf_session *session)
{
	uint16_t mqes;

	assert((g_nvmf_tgt.opts.max_io_size % SPDK_NVMF_BLOCK_SIZE) == 0);

	/* Init the controller details */
	session->subsys->ops->ctrlr_get_data(session);

	/* Adjust the aerl field for zero's based value */
	if (g_nvmf_tgt.opts.aerl > 0) {
		session->vcdata.aerl = (g_nvmf_tgt.opts.aerl - 1);
	} else {
		session->vcdata.aerl = g_nvmf_tgt.opts.aerl;
	}

	session->vcdata.cntlid = session->cntlid;
	session->vcdata.kas = g_nvmf_tgt.opts.kas;
	session->vcdata.maxcmd = session->host->max_io_queue_depth;
	session->vcdata.mdts = spdk_u32log2(g_nvmf_tgt.opts.max_io_size / SPDK_NVMF_BLOCK_SIZE);
	memcpy(&session->vcdata.sgls, &g_nvmf_tgt.opts.sgls, sizeof(uint32_t));

	session->vcdata.nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
	session->vcdata.nvmf_specific.iorcsz = sizeof(struct spdk_nvme_cpl) / 16;
	session->vcdata.nvmf_specific.icdoff = 0; /* offset starts directly after SQE */
	session->vcdata.nvmf_specific.ctrattr.ctrlr_model = SPDK_NVMF_CTRLR_MODEL_DYNAMIC;
	session->vcdata.nvmf_specific.msdbd = 1; /* target supports single SGL in capsule */

	/* TODO: this should be set by the transport */
	// session->vcdata.nvmf_specific.ioccsz += g_nvmf_tgt.opts.in_capsule_data_size / 16;

	strncpy((char *)session->vcdata.subnqn, session->subsys->subnqn, sizeof(session->vcdata.subnqn));

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	ctrlr data: maxcmd %u\n",
		      session->vcdata.maxcmd);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	ext ctrlr data: ioccsz %x\n",
		      session->vcdata.nvmf_specific.ioccsz);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	ext ctrlr data: iorcsz %x\n",
		      session->vcdata.nvmf_specific.iorcsz);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	ext ctrlr data: icdoff %x\n",
		      session->vcdata.nvmf_specific.icdoff);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	ext ctrlr data: ctrattr %x\n",
		      *(uint8_t *)&session->vcdata.nvmf_specific.ctrattr);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	ext ctrlr data: msdbd %x\n",
		      session->vcdata.nvmf_specific.msdbd);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	sgls data: 0x%x\n",
		      *(uint32_t *)&session->vcdata.sgls);

	mqes = (session->vcdata.maxcmd - 1);   /* max queue depth */

	session->vcprop.cap.raw = 0;
	session->vcprop.cap.bits.cqr = 1;
	session->vcprop.cap.bits.mqes = mqes;	/* max queue depth */
	session->vcprop.cap.bits.ams = 0;	/* optional arb mechanisms */
	session->vcprop.cap.bits.to = 1;	/* ready timeout - 500 msec units */
	session->vcprop.cap.bits.dstrd = 0;	/* fixed to 0 for NVMf */
	session->vcprop.cap.bits.css_nvm = 1; /* NVM command set */
	session->vcprop.cap.bits.mpsmin = 0; /* 2 ^ 12 + mpsmin == 4k */
	session->vcprop.cap.bits.mpsmax = 0; /* 2 ^ 12 + mpsmax == 4k */

	if (g_nvmf_tgt.opts.nvmever == SPDK_NVME_VERSION(1, 3, 0)) {
		/* Version Supported: 1.3.0 */
		session->vcprop.vs.bits.mjr = 1;
		session->vcprop.vs.bits.mnr = 3;
		session->vcprop.vs.bits.ter = 0;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "        properties cap data: %#lx\n",
		      *(uint64_t *)&session->vcprop.cap.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "        properties cap mqes: 0x%x (%u)\n", mqes, mqes);

	/* Report at least version 1.2.1 */
	if (session->vcprop.vs.raw < SPDK_NVME_VERSION(1, 2, 1)) {
		session->vcprop.vs.bits.mjr = 1;
		session->vcprop.vs.bits.mnr = 2;
		session->vcprop.vs.bits.ter = 1;
	}
	session->vcdata.ver = session->vcprop.vs;

	session->vcprop.cc.raw = 0;
	session->vcprop.cc.bits.en = 0; /* Init controller disabled */

	session->vcprop.csts.raw = 0;
	session->vcprop.csts.bits.rdy = 0; /* Init controller as not ready */

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	cap %" PRIx64 "\n",
		      session->vcprop.cap.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	vs %x\n", session->vcprop.vs.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	cc %x\n", session->vcprop.cc.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	csts %x\n",
		      session->vcprop.csts.raw);
}

static void session_destruct(struct spdk_nvmf_session *session)
{
	TAILQ_REMOVE(&session->subsys->sessions, session, link);
	session->transport->session_fini(session);
}

void
spdk_nvmf_session_destruct(struct spdk_nvmf_session *session)
{
	while (!TAILQ_EMPTY(&session->connections)) {
		struct spdk_nvmf_conn *conn = TAILQ_FIRST(&session->connections);

		TAILQ_REMOVE(&session->connections, conn, link);
		session->num_connections--;
		conn->transport->conn_fini(conn);
	}

	session_destruct(session);
}

static void
invalid_connect_response(struct spdk_nvmf_fabric_connect_rsp *rsp, uint8_t iattr, uint16_t ipo)
{
	rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
	rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
	rsp->status_code_specific.invalid.iattr = iattr;
	rsp->status_code_specific.invalid.ipo = ipo;
}

static uint16_t
spdk_nvmf_session_gen_cntlid(void)
{
	static uint16_t cntlid = 0; /* cntlid is static, so its value is preserved */
	struct spdk_nvmf_subsystem *subsystem;
	uint16_t count;

	count = UINT16_MAX - 1;
	do {
		/* cntlid is an unsigned 16-bit integer, so let it overflow
		 * back to 0 if necessary.
		 */
		cntlid++;
		if (cntlid == 0) {
			/* 0 is not a valid cntlid because it is the reserved value in the RDMA
			 * private data for cntlid. This is the value sent by pre-NVMe-oF 1.1
			 * initiators.
			 */
			cntlid++;
		}

		/* Check if a subsystem with this cntlid currently exists. This could
		 * happen for a very long-lived session on a target with many short-lived
		 * sessions, where cntlid wraps around.
		 */
		subsystem = spdk_nvmf_find_subsystem_with_cntlid(cntlid);

		count--;

	} while (subsystem != NULL && count > 0);

	if (count == 0) {
		return 0;
	}

	return cntlid;
}

bool
spdk_nvmf_validate_sqsize(struct spdk_nvmf_host *host,
			  uint16_t qid,
			  uint16_t sqsize,
			  const char *func)
{
	/*
	 * SQSIZE is a 0-based value, so it must be at least 1
	 * (minimum queue depth is 2) and strictly less than
	 * max_io_queue_depth.
	 */

	/*
	 * TODO: Currently the sqsize check is spec FC-NVMe 1.17 compliant.
	 * To make it 1.19 compliant we need to change the check below
	 * to '>=' max_io_queue_depth, not '>= max_io_queue_depth + 1.
	 *
	 * Testing with the Linux initiator shows that we see this
	 * off-by-one error only on the conection queues.  The admin
	 * queue sqsize doesn't display this bug. However other FC-NVMe
	 * intiators display this problem with both the admin and io queues.
	 */
	if (qid == 0) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "%s: Admin SQSIZE %u (max %u) qid %u\n", func, sqsize,
			      (host->max_aq_depth - 1), qid);
		if (sqsize == 0 || sqsize >= host->max_aq_depth + 1) {
			SPDK_ERRLOG("%s: Invalid Admin SQSIZE %u (min 1, max %u) qid %u\n", func,
				    sqsize, (host->max_aq_depth - 1), qid);
			return false;
		}
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "%s: IO SQSIZE %u (max %u) qid %u\n", func, sqsize,
			      (host->max_io_queue_depth - 1), qid);
		if (sqsize == 0 || sqsize >= (host->max_io_queue_depth + 1)) {
			SPDK_ERRLOG("%s: Invalid IO SQSIZE %u (min 1, max %u) qid %u\n", func,
				    sqsize, (host->max_io_queue_depth - 1), qid);
			return false;
		}
	}
	return true;
}

void
spdk_nvmf_session_connect(struct spdk_nvmf_conn *conn,
			  struct spdk_nvmf_fabric_connect_cmd *cmd,
			  struct spdk_nvmf_fabric_connect_data *data,
			  struct spdk_nvmf_fabric_connect_rsp *rsp)
{
	struct spdk_nvmf_session *session;
	struct spdk_nvmf_host *host;
	struct spdk_nvmf_subsystem *subsystem;

#define INVALID_CONNECT_CMD(field) invalid_connect_response(rsp, 0, offsetof(struct spdk_nvmf_fabric_connect_cmd, field))
#define INVALID_CONNECT_DATA(field) invalid_connect_response(rsp, 1, offsetof(struct spdk_nvmf_fabric_connect_data, field))

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "CONNECT recfmt 0x%x qid %u sqsize %u\n",
		      cmd->recfmt, cmd->qid, cmd->sqsize);

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Connect data:\n");
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "  cntlid:  0x%04x\n", data->cntlid);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "  hostid: %08x-%04x-%04x-%02x%02x-%04x%08x ***\n",
		      ntohl(*(uint32_t *)&data->hostid[0]),
		      ntohs(*(uint16_t *)&data->hostid[4]),
		      ntohs(*(uint16_t *)&data->hostid[6]),
		      data->hostid[8],
		      data->hostid[9],
		      ntohs(*(uint16_t *)&data->hostid[10]),
		      ntohl(*(uint32_t *)&data->hostid[12]));
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "  subnqn: \"%s\"\n", data->subnqn);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "  hostnqn: \"%s\"\n", data->hostnqn);

	if (!spdk_nvmf_valid_nqn((const char *) data->subnqn)) {
		SPDK_ERRLOG("Invalid subsystem NQN '%s'\n", data->subnqn);
		INVALID_CONNECT_DATA(subnqn);
		return;
	}

	subsystem = spdk_nvmf_find_subsystem(data->subnqn);

	if (subsystem == NULL) {
		SPDK_ERRLOG("Could not find subsystem '%s'\n", data->subnqn);
		INVALID_CONNECT_DATA(subnqn);
		return;
	}

	if ((host = spdk_nvmf_find_subsystem_host(subsystem, (const char *) data->hostnqn)) == NULL) {
		SPDK_ERRLOG("Could not find host '%s'\n", data->hostnqn);
		INVALID_CONNECT_DATA(hostnqn);
		return;
	}

	/*
	 * SQSIZE is a 0-based value, so it must be at least 1 (minimum queue depth is 2) and
	 *  strictly less than max_io_queue_depth.
	 */
	if (!(spdk_nvmf_validate_sqsize(host, cmd->qid, cmd->sqsize, __func__))) {
		INVALID_CONNECT_CMD(sqsize);
		return;
	}

	conn->sq_head_max = cmd->sqsize;
	conn->qid = cmd->qid;

	if (cmd->qid == 0) {
		conn->type = CONN_TYPE_AQ;

		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Connect Admin Queue for controller ID 0x%x\n", data->cntlid);

		if (data->cntlid != 0xFFFF) {
			/* This NVMf target only supports dynamic mode. */
			SPDK_ERRLOG("The NVMf target only supports dynamic mode (CNTLID = 0x%x).\n", data->cntlid);
			INVALID_CONNECT_DATA(cntlid);
			return;
		}

		/* Establish a new session */
		session = conn->transport->session_init();
		if (session == NULL) {
			SPDK_ERRLOG("Memory allocation failure\n");
			rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return;
		}

		TAILQ_INIT(&session->connections);

		session->cntlid = spdk_nvmf_session_gen_cntlid();
		if (session->cntlid == 0) {
			/* Unable to get a cntlid */
			SPDK_ERRLOG("Reached max simultaneous sessions\n");
			rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return;
		}
		session->host = host;
		session->kato = cmd->kato;
		session->async_event_config.raw = g_nvmf_tgt.opts.async_event_config;
		session->num_connections = 0;
		session->subsys = subsystem;
		session->max_connections_allowed = host->max_connections_allowed;
		session->aer_ctxt.is_aer_pending = false;
		session->aer_ctxt.aer_rsp_cdw0 = 0;
		session->aer_req = NULL;

		memcpy(session->hostid, data->hostid, sizeof(session->hostid));
		memcpy(session->hostnqn, data->hostnqn, sizeof(session->hostnqn));

		if (conn->transport->session_add_conn(session, conn)) {
			rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			conn->transport->session_fini(session);
			free(session);
			return;
		}

		if (subsystem->subtype == SPDK_NVMF_SUBTYPE_NVME) {
			nvmf_init_nvme_session_properties(session);
		} else {
			nvmf_init_discovery_session_properties(session);
		}

		TAILQ_INSERT_TAIL(&subsystem->sessions, session, link);
	} else {
		struct spdk_nvmf_session *tmp;

		conn->type = CONN_TYPE_IOQ;
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Connect I/O Queue for controller id 0x%x\n", data->cntlid);

		session = NULL;
		TAILQ_FOREACH(tmp, &subsystem->sessions, link) {
			if (tmp->cntlid == data->cntlid) {
				session = tmp;
				break;
			}
		}
		if (session == NULL) {
			SPDK_ERRLOG("Unknown controller ID 0x%x\n", data->cntlid);
			INVALID_CONNECT_DATA(cntlid);
			return;
		}

		if (!session->vcprop.cc.bits.en) {
			SPDK_ERRLOG("Got I/O connect before ctrlr was enabled\n");
			INVALID_CONNECT_CMD(qid);
			return;
		}

		if (1u << session->vcprop.cc.bits.iosqes != sizeof(struct spdk_nvme_cmd)) {
			SPDK_ERRLOG("Got I/O connect with invalid IOSQES %u\n",
				    session->vcprop.cc.bits.iosqes);
			INVALID_CONNECT_CMD(qid);
			return;
		}

		if (1u << session->vcprop.cc.bits.iocqes != sizeof(struct spdk_nvme_cpl)) {
			SPDK_ERRLOG("Got I/O connect with invalid IOCQES %u\n",
				    session->vcprop.cc.bits.iocqes);
			INVALID_CONNECT_CMD(qid);
			return;
		}

		/* check if we would exceed session connection limit */
		if (session->num_connections >= session->max_connections_allowed) {
			SPDK_ERRLOG("connection limit %d\n", session->num_connections);
			rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			rsp->status.sc = SPDK_NVMF_FABRIC_SC_CONTROLLER_BUSY;
			return;
		}

		if (conn->transport->session_add_conn(session, conn)) {
			INVALID_CONNECT_CMD(qid);
			return;
		}
	}

	session->num_connections++;
	TAILQ_INSERT_HEAD(&session->connections, conn, link);
	conn->sess = session;

	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	rsp->status_code_specific.success.cntlid = session->vcdata.cntlid;
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "connect capsule response: cntlid = 0x%04x\n",
		      rsp->status_code_specific.success.cntlid);
}

void
spdk_nvmf_session_disconnect(struct spdk_nvmf_conn *conn)
{
	struct spdk_nvmf_session *session = conn->sess;

	assert(session != NULL);
	session->num_connections--;
	TAILQ_REMOVE(&session->connections, conn, link);

	conn->transport->session_remove_conn(session, conn);
	conn->transport->conn_fini(conn);

	if (session->num_connections == 0) {
		session_destruct(session);
	}
}

struct spdk_nvmf_conn *
spdk_nvmf_session_get_conn(struct spdk_nvmf_session *session, uint16_t qid)
{
	struct spdk_nvmf_conn *conn;

	TAILQ_FOREACH(conn, &session->connections, link) {
		if (conn->qid == qid) {
			return conn;
		}
	}
	return NULL;
}

struct spdk_nvmf_request *
spdk_nvmf_conn_get_request(struct spdk_nvmf_conn *conn, uint16_t cid)
{
	/* TODO: track list of outstanding requests in conn? */
	return NULL;
}

static uint64_t
nvmf_prop_get_cap(struct spdk_nvmf_session *session)
{
	return session->vcprop.cap.raw;
}

static uint64_t
nvmf_prop_get_vs(struct spdk_nvmf_session *session)
{
	return session->vcprop.vs.raw;
}

static uint64_t
nvmf_prop_get_cc(struct spdk_nvmf_session *session)
{
	return session->vcprop.cc.raw;
}

static bool
nvmf_prop_set_cc(struct spdk_nvmf_session *session, uint64_t value)
{
	union spdk_nvme_cc_register cc, diff;

	cc.raw = (uint32_t)value;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "cur CC: 0x%08x\n", session->vcprop.cc.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "new CC: 0x%08x\n", cc.raw);

	/*
	 * Calculate which bits changed between the current and new CC.
	 * Mark each bit as 0 once it is handled to determine if any unhandled bits were changed.
	 */
	diff.raw = cc.raw ^ session->vcprop.cc.raw;

	if (diff.bits.en) {
		if (cc.bits.en) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Property Set CC Enable!\n");
			session->vcprop.cc.bits.en = 1;
			session->vcprop.csts.bits.rdy = 1;
		} else {
			/* SPDK_TRACE_NVMF	SPDK_ERRLOG("CC.EN transition from 1 to 0 (reset) not implemented!\n"); */
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "CC.EN transition from 1 to 0 (reset)\n");
			cc.bits.en = 0;
			session->vcprop.csts.bits.rdy = 0;
		}
		diff.bits.en = 0;
	}

	if (diff.bits.shn) {
		if (cc.bits.shn == SPDK_NVME_SHN_NORMAL ||
		    cc.bits.shn == SPDK_NVME_SHN_ABRUPT) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Property Set CC Shutdown %u%ub!\n",
				      cc.bits.shn >> 1, cc.bits.shn & 1);
			session->vcprop.cc.bits.shn = cc.bits.shn;
			session->vcprop.cc.bits.en = 0;
			session->vcprop.csts.bits.rdy = 0;
			session->vcprop.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
		} else if (cc.bits.shn == 0) {
			session->vcprop.cc.bits.shn = 0;
		} else {
			SPDK_ERRLOG("Prop Set CC: Invalid SHN value %u%ub\n",
				    cc.bits.shn >> 1, cc.bits.shn & 1);
			return false;
		}
		diff.bits.shn = 0;
	}

	if (diff.bits.iosqes) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Prop Set IOSQES = %u (%u bytes)\n",
			      cc.bits.iosqes, 1u << cc.bits.iosqes);
		session->vcprop.cc.bits.iosqes = cc.bits.iosqes;
		diff.bits.iosqes = 0;
	}

	if (diff.bits.iocqes) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Prop Set IOCQES = %u (%u bytes)\n",
			      cc.bits.iocqes, 1u << cc.bits.iocqes);
		session->vcprop.cc.bits.iocqes = cc.bits.iocqes;
		diff.bits.iocqes = 0;
	}

	if (diff.raw != 0) {
		SPDK_ERRLOG("Prop Set CC toggled reserved bits 0x%x!\n", diff.raw);
		return false;
	}

	return true;
}

static uint64_t
nvmf_prop_get_csts(struct spdk_nvmf_session *session)
{
	return session->vcprop.csts.raw;
}

struct nvmf_prop {
	uint32_t ofst;
	uint8_t size;
	char name[11];
	uint64_t (*get_cb)(struct spdk_nvmf_session *session);
	bool (*set_cb)(struct spdk_nvmf_session *session, uint64_t value);
};

#define PROP(field, size, get_cb, set_cb) \
	{ \
		offsetof(struct spdk_nvme_registers, field), \
		SPDK_NVMF_PROP_SIZE_##size, \
		#field, \
		get_cb, set_cb \
	}

static const struct nvmf_prop nvmf_props[] = {
	PROP(cap,  8, nvmf_prop_get_cap,  NULL),
	PROP(vs,   4, nvmf_prop_get_vs,   NULL),
	PROP(cc,   4, nvmf_prop_get_cc,   nvmf_prop_set_cc),
	PROP(csts, 4, nvmf_prop_get_csts, NULL),
};

static const struct nvmf_prop *
find_prop(uint32_t ofst)
{
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(nvmf_props); i++) {
		const struct nvmf_prop *prop = &nvmf_props[i];

		if (prop->ofst == ofst) {
			return prop;
		}
	}

	return NULL;
}

void
spdk_nvmf_property_get(struct spdk_nvmf_session *session,
		       struct spdk_nvmf_fabric_prop_get_cmd *cmd,
		       struct spdk_nvmf_fabric_prop_get_rsp *response)
{
	const struct nvmf_prop *prop;

	response->status.sc = 0;
	response->value.u64 = 0;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "PROPERTY_GET size %d, offset 0x%x\n",
		      cmd->attrib.size, cmd->ofst);

	if (cmd->attrib.size != SPDK_NVMF_PROP_SIZE_4 &&
	    cmd->attrib.size != SPDK_NVMF_PROP_SIZE_8) {
		SPDK_ERRLOG("Invalid size value %d\n", cmd->attrib.size);
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return;
	}

	prop = find_prop(cmd->ofst);
	if (prop == NULL || prop->get_cb == NULL) {
		/* Reserved properties return 0 when read */
		return;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "PROPERTY_GET name: %s\n", prop->name);
	if (cmd->attrib.size != prop->size) {
		SPDK_ERRLOG("offset 0x%x size mismatch: cmd %u, prop %u\n",
			    cmd->ofst, cmd->attrib.size, prop->size);
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return;
	}

	response->value.u64 = prop->get_cb(session);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "PROPERTY_GET response value: 0x%" PRIx64 "\n", response->value.u64);
}

void
spdk_nvmf_property_set(struct spdk_nvmf_session *session,
		       struct spdk_nvmf_fabric_prop_set_cmd *cmd,
		       struct spdk_nvme_cpl *response)
{
	const struct nvmf_prop *prop;
	uint64_t value;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "PROPERTY_SET size %d, offset 0x%x, value 0x%" PRIx64 "\n",
		      cmd->attrib.size, cmd->ofst, cmd->value.u64);

	prop = find_prop(cmd->ofst);
	if (prop == NULL || prop->set_cb == NULL) {
		SPDK_ERRLOG("Invalid offset 0x%x\n", cmd->ofst);
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "PROPERTY_SET name: %s\n", prop->name);
	if (cmd->attrib.size != prop->size) {
		SPDK_ERRLOG("offset 0x%x size mismatch: cmd %u, prop %u\n",
			    cmd->ofst, cmd->attrib.size, prop->size);
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return;
	}

	value = cmd->value.u64;
	if (prop->size == SPDK_NVMF_PROP_SIZE_4) {
		value = (uint32_t)value;
	}

	if (!prop->set_cb(session, value)) {
		SPDK_ERRLOG("PROPERTY_SET prop set_cb failed\n");
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return;
	}
}

int
spdk_nvmf_session_poll(struct spdk_nvmf_session *session)
{
	struct spdk_nvmf_conn	*conn, *tmp;
	struct spdk_nvmf_subsystem 	*subsys = session->subsys;

	if (subsys->is_removed && subsys->mode == NVMF_SUBSYSTEM_MODE_VIRTUAL) {
		if (session->aer_req) {
			struct spdk_nvmf_request *aer = session->aer_req;

			aer->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			aer->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
			aer->rsp->nvme_cpl.status.dnr = 0;
			spdk_nvmf_request_complete(aer);
			session->aer_req = NULL;
		}
	}

	TAILQ_FOREACH_SAFE(conn, &session->connections, link, tmp) {
		if (conn->transport->conn_poll && conn->transport->conn_poll(conn) < 0) {
			SPDK_ERRLOG("Transport poll failed for conn %p; closing connection\n", conn);
			spdk_nvmf_session_disconnect(conn);
		}
	}

	return 0;
}

int
spdk_nvmf_session_set_features_host_identifier(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	SPDK_ERRLOG("Set Features - Host Identifier not allowed\n");
	response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_session_get_features_host_identifier(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Get Features - Host Identifier\n");
	if (!(cmd->cdw11 & 1)) {
		/* NVMe over Fabrics requires EXHID=1 (128-bit/16-byte host ID) */
		SPDK_ERRLOG("Get Features - Host Identifier with EXHID=0 not allowed\n");
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (req->data == NULL || req->length < sizeof(session->hostid)) {
		SPDK_ERRLOG("Invalid data buffer for Get Features - Host Identifier\n");
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	memcpy(req->data, session->hostid, sizeof(session->hostid));
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_session_set_features_keep_alive_timer(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint32_t remainder;
	uint16_t cdw11 = cmd->cdw11;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Set Features - Keep Alive Timer (%u ms)\n", cmd->cdw11);

	if (cdw11 == 0) {
		rsp->status.sc = SPDK_NVME_SC_KEEP_ALIVE_INVALID;
	} else if (cdw11 < MIN_KEEP_ALIVE_TIMEOUT) {
		session->kato = MIN_KEEP_ALIVE_TIMEOUT;
	} else {
		remainder = cdw11 % (SPDK_NVME_KAS_GRANULARITY_IN_MSECS * session->vcdata.kas);
		/* We are ceiling kato value up to the second */
		if (remainder) {
			cdw11 += ((SPDK_NVME_KAS_GRANULARITY_IN_MSECS * session->vcdata.kas) - remainder);
		}
		session->kato = cdw11;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Set Features - Keep Alive Timer (KATO) set to %u ms, KAS is %u\n",
		      session->kato, session->vcdata.kas);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_session_get_features_keep_alive_timer(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Get Features - Keep Alive Timer\n");
	rsp->cdw0 = session->kato;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_session_set_features_number_of_queues(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint32_t nsqr, ncqr;

	/* Get the requested number of completion queues and submission queues.
	   Both are zero based values */
	nsqr = (req->cmd->nvme_cmd.cdw11 & 0xffff) + 1;
	ncqr = (req->cmd->nvme_cmd.cdw11 >> 16) + 1;

	/* Number of queues requested are zero based values */
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Set Features - Number of Queues Requested, sub:%u, compl:%u\n",
		      nsqr - 1, ncqr - 1);

	/* 1 completion queue is mapped to 1 submission queue in nvmf */
	if (nsqr != ncqr) {
		SPDK_ERRLOG("Number of submission queues requested not the same as completion queues requested\n");
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* verify that the contoller is ready to process commands */
	if (session->num_connections > 1) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Queue pairs already active!\n");
		rsp->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
	} else {
		/* We cannot configure more than what we support */
		if (nsqr > (uint32_t)(session->max_connections_allowed - 1)) {
			nsqr = session->max_connections_allowed - 1;
		}

		/* Extra 1 connection for Admin queue */
		session->max_connections_allowed = nsqr + 1;

		/* Number of IO queues has a zero based value */
		rsp->cdw0 = ((nsqr - 1) << 16) |
			    (nsqr - 1);
		SPDK_TRACELOG(SPDK_TRACE_NVMF,
			      "Set Features - Number of Queues Configured, cdw0 sub:%u, compl:%u\n",
			      rsp->cdw0 & 0xffff, rsp->cdw0 >> 16);
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_session_get_features_number_of_queues(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint32_t nr_io_queues;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Get Features - Number of Queues\n");

	nr_io_queues = session->max_connections_allowed - 1;

	/* Number of IO queues has a zero based value */
	rsp->cdw0 = ((nr_io_queues - 1) << 16) |
		    (nr_io_queues - 1);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_session_set_features_async_event_configuration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Set Features - Async Event Configuration, cdw11 0x%08x\n",
		      cmd->cdw11);
	/*
	 * If we support all the async events that are being requested to be set,
	 * then set them, otherwise fail the request
	 */
	if ((cmd->cdw11 & g_nvmf_tgt.opts.async_event_config) == cmd->cdw11) {
		session->async_event_config.raw = cmd->cdw11;
	} else {
		SPDK_ERRLOG("Failed setting async event config %d supported %d\n",
			    cmd->cdw11, g_nvmf_tgt.opts.async_event_config);
		rsp->status.sc = SPDK_NVME_SC_FEATURE_NOT_CHANGEABLE;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_session_get_features_async_event_configuration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Get Features - Async Event Configuration\n");
	rsp->cdw0 = session->async_event_config.raw;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_session_async_event_request(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Async Event Request\n");

	/* aerl is zero's based value */
	assert(session->vcdata.aerl == 0);
	if (session->aer_req != NULL) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "AERL exceeded\n");
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	session->aer_req = req;

	/* Check if an AER is pending and if so respond right away */
	if (session->aer_ctxt.is_aer_pending) {
		rsp->cdw0 = session->aer_ctxt.aer_rsp_cdw0;
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_SUCCESS;

		/* reset the flag & other fields */
		session->aer_req = NULL;
		session->aer_ctxt.is_aer_pending = false;
		session->aer_ctxt.aer_rsp_cdw0 = 0;

		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Responding for pending AER, cdw0 0x%08x\n",
			      rsp->cdw0);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static struct spdk_nvmf_conn *
spdk_nvmf_get_connection(struct spdk_nvmf_session *session, uint16_t qid)
{
	struct spdk_nvmf_conn *conn = NULL, *tmp;
	TAILQ_FOREACH_SAFE(conn, &session->connections, link, tmp) {
		if (conn->qid == qid) {
			return conn;
		}
	}
	return NULL;
}

uint16_t
spdk_nvmf_session_get_max_queue_depth(struct spdk_nvmf_session *session, uint16_t qid)
{
	struct spdk_nvmf_conn *conn = spdk_nvmf_get_connection(session, qid);
	if (conn) {
		/* sq_head_max has 0's based depth */
		return conn->sq_head_max + 1;
	}
	return 0;
}

uint16_t
spdk_nvmf_session_get_num_io_connections(struct spdk_nvmf_session *session)
{
	uint16_t num_io_conns = 0;
	struct spdk_nvmf_conn *conn = NULL, *tmp;

	if (!session) {
		SPDK_ERRLOG("Invalid session passed\n");
		assert(session != NULL);
		return 0;
	}

	TAILQ_FOREACH_SAFE(conn, &session->connections, link, tmp) {
		if (conn->type == CONN_TYPE_IOQ) {
			num_io_conns++;
		}
	}
	return num_io_conns;
}

void
spdk_nvmf_session_populate_io_queue_depths(struct spdk_nvmf_session *session,
		uint16_t *max_io_queue_depths, uint16_t num_io_queues)
{
	struct spdk_nvmf_conn *conn = NULL, *tmp;
	uint16_t curr_q_index = 0;

	if (!session) {
		SPDK_ERRLOG("Invalid session passed\n");
		assert(session != NULL);
		return;
	}

	if (!max_io_queue_depths) {
		SPDK_ERRLOG("NULL container passed to populate queue depths\n");
		assert(max_io_queue_depths != NULL);
		return;
	}

	if (!num_io_queues || num_io_queues > session->num_connections) {
		SPDK_ERRLOG("Invalid number of IO queues data requested (%d:%d)", num_io_queues,
			    session->num_connections);
		return;
	}

	TAILQ_FOREACH_SAFE(conn, &session->connections, link, tmp) {
		if (conn->qid == CONN_TYPE_AQ) {
			continue;
		}

		if (curr_q_index >= num_io_queues) {
			break;
		}

		/* sq_head_max has 0's based depth */
		max_io_queue_depths[curr_q_index] = conn->sq_head_max + 1;
		curr_q_index++;
	}
}

void
spdk_nvmf_queue_aer_rsp(struct spdk_nvmf_subsystem *subsystem,
			enum aer_type aer_type,
			uint8_t aer_info)
{
	struct spdk_nvmf_session *session, *session_tmp;
	uint32_t aer_rsp_cdw0 = aer_info;

	if (!subsystem) {
		return ;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "AER RSP for Subsystem NQN %s, AER type = 0x%x\n",
		      subsystem->subnqn, aer_type);

	/* Check if the aer_type is supported */
	if (aer_type == AER_TYPE_NOTICE) {
		/* set the AER info as appropriate
		 * set the log page when supported
		 */
		aer_rsp_cdw0 = (aer_rsp_cdw0 << 8) | aer_type;
	} else {
		/* unsupported AER type */
		SPDK_ERRLOG("Unsupported AER type: %d\n", aer_type);
		return;
	}

	TAILQ_FOREACH_SAFE(session, &subsystem->sessions, link, session_tmp) {
		/* Issue AER if available for each session */
		if (session->aer_req) {
			struct spdk_nvmf_request *aer = session->aer_req;
			struct spdk_nvme_cpl *rsp = &aer->rsp->nvme_cpl;
			rsp->cdw0 = aer_rsp_cdw0;
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc  = SPDK_NVME_SC_SUCCESS;
			(void)spdk_nvmf_request_complete(aer);
			session->aer_req = NULL;
		} else if (!session->aer_ctxt.is_aer_pending) {
			/*
			 * Set the pending flag if unset
			 * Currently only NS attributes changed AER is
			 * supported ;
			 * coagulating different AER types will come in
			 * when other AER types are supported
			 */
			session->aer_ctxt.is_aer_pending = true;
			session->aer_ctxt.aer_rsp_cdw0 = aer_rsp_cdw0;
		}
	}
}
