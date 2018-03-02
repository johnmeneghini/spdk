/*
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

#ifndef __BCM_FC_H__
#define __BCM_FC_H__

#include <nvmf/request.h>
#include <nvmf/session.h>
#include <spdk/nvmf.h>
#include "spdk/assert.h"
#include "spdk/nvme_spec.h"
#include "spdk/event.h"
#include "spdk/error.h"
#include "fc_nvme_spec.h"
#include "bcm_sli_fc.h"

#ifndef UINT16_MAX
#define UINT16_MAX  (65535U)
#endif
#ifndef UINT32_MAX
#define UINT32_MAX  (4294967295U)
#endif

#define NVMF_BCM_FC_TRANSPORT_NAME       "bcm_nvmf_fc"
// 5 byte string("nn-0x") + 8 byte node wwn + 6 byte string(":pn-0x")+ 8 byte port wwn
#define NVMF_TGT_FC_TR_ADDR_LENGTH 256

#define NVMF_FC_MAX_IO_QUEUES            16

/*
 * End of FC-NVMe Spec. Defintions
 */

/*
 * Misc defines
 */
#define BCM_RQ_BUFFER_SIZE 		2048 /* Driver needs to be in sync. */
#define BCM_MAX_LS_REQ_CMD_SIZE    	1536
#define BCM_MAX_RESP_BUFFER_SIZE 	64
#define BCM_MAX_IOVECS			(MAX_NUM_OF_IOVECTORS + 2) /* 2 for skips */

#define SPDK_NVMF_FC_BCM_MRQ_CONNID_QUEUE_MASK  0xff

/*
 * PRLI service parameters
 */
enum spdk_nvmf_bcm_fc_service_parameters {
	SPDK_NVMF_FC_FIRST_BURST_SUPPORTED                      = 0x0001,
	SPDK_NVMF_FC_DISCOVERY_SERVICE                          = 0x0008,
	SPDK_NVMF_FC_TARGET_FUNCTION                            = 0x0010,
	SPDK_NVMF_FC_INITIATOR_FUNCTION                         = 0x0020,
	SPDK_NVMF_FC_CONFIRMED_COMPLETION_SUPPORTED             = 0x0080,
};

/*
 * FC HW port states.
 */
typedef enum spdk_fc_port_state_e {
	SPDK_FC_PORT_OFFLINE = 0,
	SPDK_FC_PORT_ONLINE = 1,
	SPDK_FC_PORT_QUIESCED = 2,
} spdk_fc_port_state_t;

typedef enum spdk_fc_hwqp_state_e {
	SPDK_FC_HWQP_OFFLINE = 0,
	SPDK_FC_HWQP_ONLINE = 1,
} spdk_fc_hwqp_state_t;

/*
 * NVMF BCM FC Object state
 * Add all the generic states of the object here.
 * Specific object states can be added separately
 */
typedef enum spdk_nvmf_bcm_fc_object_state_e {
	SPDK_NVMF_BCM_FC_OBJECT_CREATED = 0,
	SPDK_NVMF_BCM_FC_OBJECT_TO_BE_DELETED = 1,
	SPDK_NVMF_BCM_FC_OBJECT_ZOMBIE = 2,      /* Partial Create or Delete  */
} spdk_nvmf_bcm_fc_object_state_t;

/*
 * Hardware queues structure.
 * Structure passed from master thread to poller thread.
 */
struct spdk_nvmf_bcm_fc_hw_queues {
	struct fc_eventq eq;
	struct fc_eventq cq_wq;
	struct fc_eventq cq_rq;
	struct fc_wrkq   wq;
	struct fc_rcvq   rq_hdr;
	struct fc_rcvq   rq_payload;
};

/*
 * NVME FC transport errors
 */
struct spdk_nvmf_bcm_fc_errors {
	uint32_t no_xri;
	uint32_t nport_invalid;
	uint32_t unknown_frame;
	uint32_t wqe_cmplt_err;
	uint32_t wqe_write_err;
	uint32_t rq_status_err;
	uint32_t rq_buf_len_err;
	uint32_t rq_id_err;
	uint32_t rq_index_err;
	uint32_t invalid_cq_type;
	uint32_t invalid_cq_id;
	uint32_t fc_req_buf_err;
	uint32_t aq_buf_alloc_err;
	uint32_t write_buf_alloc_err;
	uint32_t read_buf_alloc_err;
	uint32_t unexpected_err;
	uint32_t nvme_cmd_iu_err;
	uint32_t nvme_cmd_xfer_err;
	uint32_t queue_entry_invalid;
	uint32_t invalid_conn_err;
	uint32_t fcp_rsp_failure;
	uint32_t write_failed;
	uint32_t read_failed;
	uint32_t rport_invalid;
	uint32_t num_aborted;
	uint32_t num_abts_sent;
};

/*
 * Structure for maintaining the XRI's
 */
struct spdk_nvmf_bcm_fc_xri {
	uint32_t xri;   /* The actual xri value */
	/* Internal */
	TAILQ_ENTRY(spdk_nvmf_bcm_fc_xri) link;
	bool is_active;
};

/*
 *  HWQP poller structure passed from Master thread
 */
struct spdk_nvmf_bcm_fc_hwqp {
	uint32_t lcore_id;   /* core we are running on */
	uint32_t hwqp_id;    /* A unique id (per physical port) for a hwqp */
	struct spdk_nvmf_bcm_fc_hw_queues queues; /* poller queue set */
	struct spdk_nvmf_bcm_fc_port *fc_port; /* HW port structure for these queues */
	struct spdk_poller *poller;

	void *context;			/* Vendor Context */

	TAILQ_HEAD(, spdk_nvmf_bcm_fc_conn) connection_list;
	uint32_t num_conns; /* number of connections to queue */
	uint16_t cid_cnt;   /* used to generate unique conn. id for RQ */
	uint32_t free_q_slots; /* free q slots available for connections  */
	spdk_fc_hwqp_state_t state;  /* Poller state (e.g. online, offline) */

	/* Internal */
	struct spdk_mempool *fc_request_pool;
	TAILQ_HEAD(, spdk_nvmf_bcm_fc_request) in_use_reqs;

	TAILQ_HEAD(, spdk_nvmf_bcm_fc_xri) pending_xri_list;

	struct spdk_nvmf_bcm_fc_errors counters;
	uint32_t send_frame_xri;
	uint8_t send_frame_seqid;

	/* Pending LS request waiting for XRI. */
	TAILQ_HEAD(, spdk_nvmf_bcm_fc_ls_rqst) ls_pending_queue;

	/* Sync req list */
	TAILQ_HEAD(, spdk_nvmf_bcm_fc_poller_api_queue_sync_args) sync_cbs;
};

/*
 *  Send Single Request/Response Sequence.
 */
struct spdk_nvmf_bcm_fc_send_srsr {
	bcm_buffer_desc_t rqst;
	bcm_buffer_desc_t rsp;
	bcm_buffer_desc_t sgl; /* Note: Len = (2 * bcm_sge_t) */
	uint16_t rpi;
};

/*
 * NVMF FC Association
 */
struct spdk_nvmf_bcm_fc_association {
	uint64_t assoc_id;
	uint32_t s_id;
	struct spdk_nvmf_bcm_fc_nport *tgtport;
	struct spdk_nvmf_bcm_fc_remote_port_info *rport;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_host *host;
	spdk_nvmf_bcm_fc_object_state_t assoc_state;

	char host_id[SPDK_NVMF_FC_HOST_ID_LEN];
	char host_nqn[SPDK_NVMF_FC_NQN_MAX_LEN];
	char sub_nqn[SPDK_NVMF_FC_NQN_MAX_LEN];

	uint16_t conn_count;
	TAILQ_HEAD(, spdk_nvmf_bcm_fc_conn) fc_conns;

	TAILQ_ENTRY(spdk_nvmf_bcm_fc_association) link;

	/* for port's association free list */
	TAILQ_ENTRY(spdk_nvmf_bcm_fc_association) port_free_assoc_list_link;

	void *ls_del_op_ctx; /* delete assoc. callback list */

	/* req/resp buffers used to send disconnect to initiator */
	struct spdk_nvmf_bcm_fc_send_srsr snd_disconn_bufs;
};

struct spdk_nvmf_bcm_fc_ls_rsrc_pool {
	void *assocs_mptr;
	uint32_t assocs_count;
	TAILQ_HEAD(, spdk_nvmf_bcm_fc_association) assoc_free_list;

	void *conns_mptr;
	uint32_t conns_count;
	TAILQ_HEAD(, spdk_nvmf_bcm_fc_conn) fc_conn_free_list;
};

/*
 * FC HW port.
 */
struct spdk_nvmf_bcm_fc_port {
	uint8_t port_hdl;
	spdk_fc_port_state_t hw_port_status;
	uint32_t xri_base;
	uint32_t xri_count;
	uint16_t fcp_rq_id;
	struct spdk_ring *xri_ring;
	struct spdk_nvmf_bcm_fc_hwqp ls_queue;
	uint32_t max_io_queues;
	struct spdk_nvmf_bcm_fc_hwqp io_queues[NVMF_FC_MAX_IO_QUEUES];
	/*
	 * List of nports on this HW port.
	 */
	TAILQ_HEAD(, spdk_nvmf_bcm_fc_nport)nport_list;
	int	num_nports;
	TAILQ_ENTRY(spdk_nvmf_bcm_fc_port) link;

	struct spdk_nvmf_bcm_fc_ls_rsrc_pool ls_rsrc_pool;
	struct spdk_mempool *io_rsrc_pool; /* Pools to store bdev_io's for this port */
	void *port_ctx;
};

/*
 * FC WWN
 */
struct spdk_nvmf_bcm_fc_wwn {
	union {
		uint64_t wwn; /* World Wide Names consist of eight bytes */
		uint8_t octets[sizeof(uint64_t)];
	} u;
};

/*
 * FC Remote Port
 */
struct spdk_nvmf_bcm_fc_remote_port_info {
	uint32_t s_id;
	uint32_t rpi;
	uint32_t assoc_count;
	struct spdk_nvmf_bcm_fc_wwn fc_nodename;
	struct spdk_nvmf_bcm_fc_wwn fc_portname;
	spdk_nvmf_bcm_fc_object_state_t rport_state;
	TAILQ_ENTRY(spdk_nvmf_bcm_fc_remote_port_info) link;
};

/*
 * Struct representing a nport/lif.
 */
struct spdk_nvmf_bcm_fc_nport {

	uint16_t nport_hdl;
	uint8_t port_hdl;
	uint32_t d_id;
	spdk_nvmf_bcm_fc_object_state_t nport_state;
	struct spdk_nvmf_bcm_fc_wwn fc_nodename;
	struct spdk_nvmf_bcm_fc_wwn fc_portname;

	/* list of remote ports (i.e. initiators) connected to nport */
	TAILQ_HEAD(, spdk_nvmf_bcm_fc_remote_port_info) rem_port_list;
	uint32_t rport_count;

	void *vendor_data;	/* available for vendor use */

	/* list of associations to nport */
	TAILQ_HEAD(, spdk_nvmf_bcm_fc_association) fc_associations;
	uint32_t assoc_count;
	struct spdk_nvmf_bcm_fc_port *fc_port;
	TAILQ_ENTRY(spdk_nvmf_bcm_fc_nport) link; /* list of nports on a hw port. */
};

/*
 * FC request state
 */
typedef enum {
	SPDK_NVMF_BCM_FC_REQ_INIT = 0,
	SPDK_NVMF_BCM_FC_REQ_READ_BDEV,
	SPDK_NVMF_BCM_FC_REQ_READ_XFER,
	SPDK_NVMF_BCM_FC_REQ_READ_RSP,
	SPDK_NVMF_BCM_FC_REQ_WRITE_BUFFS,
	SPDK_NVMF_BCM_FC_REQ_WRITE_XFER,
	SPDK_NVMF_BCM_FC_REQ_WRITE_BDEV,
	SPDK_NVMF_BCM_FC_REQ_WRITE_RSP,
	SPDK_NVMF_BCM_FC_REQ_NONE_BDEV,
	SPDK_NVMF_BCM_FC_REQ_NONE_RSP,
	SPDK_NVMF_BCM_FC_REQ_SUCCESS,
	SPDK_NVMF_BCM_FC_REQ_FAILED,
	SPDK_NVMF_BCM_FC_REQ_ABORTED,
	SPDK_NVMF_BCM_FC_REQ_PENDING,
	SPDK_NVMF_BCM_FC_REQ_MAX_STATE,
} spdk_nvmf_bcm_fc_request_state_t;

SPDK_STATIC_ASSERT(MAX_REQ_STATES >= SPDK_NVMF_BCM_FC_REQ_MAX_STATE, "Too many request states");

/*
 * NVMF FC Request
 */
struct spdk_nvmf_bcm_fc_request {
	struct spdk_nvmf_request req;
	struct spdk_nvmf_fc_ersp_iu ersp;
	uint32_t poller_lcore;
	uint16_t buf_index;
	struct spdk_nvmf_bcm_fc_xri *xri;
	uint16_t oxid;
	uint16_t rpi;
	struct spdk_nvmf_bcm_fc_conn *fc_conn;
	struct spdk_nvmf_bcm_fc_hwqp *hwqp;
	int state;
	uint32_t transfered_len;
	bool is_aborted;
	uint32_t magic;
	TAILQ_ENTRY(spdk_nvmf_bcm_fc_request) link;
	TAILQ_ENTRY(spdk_nvmf_bcm_fc_request) pending_link;
	TAILQ_HEAD(, fc_caller_ctx) abort_cbs;
};

SPDK_STATIC_ASSERT(!offsetof(struct spdk_nvmf_bcm_fc_request, req),
		   "FC request and NVMF request address doesnt match.");

/*
 * NVMF FC Session
 */
struct spdk_nvmf_bcm_fc_session {
	struct spdk_nvmf_session session;
	struct spdk_nvmf_bcm_fc_association *fc_assoc;
};

/*
 * NVMF FC Connection
 */
struct spdk_nvmf_bcm_fc_conn {
	struct spdk_nvmf_conn conn;

	uint64_t conn_id;
	struct spdk_nvmf_bcm_fc_hwqp *hwqp;
	uint16_t esrp_ratio;
	uint16_t rsp_count;
	uint32_t rsn;

	/* The maximum number of I/O outstanding on this connection at one time */
	uint16_t max_queue_depth;
	uint16_t max_rw_depth;
	/* The current number of I/O outstanding on this connection. This number
	 * includes all I/O from the time the capsule is first received until it is
	 * completed.
	 */
	uint16_t cur_queue_depth;

	/* number of read/write requests that are outstanding */
	uint16_t cur_fc_rw_depth;

	/* requests that are waiting to obtain xri/buffer */
	TAILQ_HEAD(, spdk_nvmf_bcm_fc_request) pending_queue;

	struct spdk_nvmf_bcm_fc_association *fc_assoc;

	/* additional FC info here - TBD */
	uint16_t rpi;

	/* for association's connection list */
	TAILQ_ENTRY(spdk_nvmf_bcm_fc_conn) assoc_link;

	/* for port's free connection list */
	TAILQ_ENTRY(spdk_nvmf_bcm_fc_conn) port_free_conn_list_link;

	/* for hwqp's connection list */
	TAILQ_ENTRY(spdk_nvmf_bcm_fc_conn) link;
};

/*
 * Poller API error codes
 */
typedef enum {
	SPDK_NVMF_BCM_FC_POLLER_API_SUCCESS = 0,
	SPDK_NVMF_BCM_FC_POLLER_API_ERROR,
	SPDK_NVMF_BCM_FC_POLLER_API_INVALID_ARG,
	SPDK_NVMF_BCM_FC_POLLER_API_NO_CONN_ID,
	SPDK_NVMF_BCM_FC_POLLER_API_DUP_CONN_ID,
	SPDK_NVMF_BCM_FC_POLLER_API_OXID_NOT_FOUND,
} spdk_nvmf_bcm_fc_poller_api_ret_t;

/*
 * Poller API definitions
 */
typedef enum {
	SPDK_NVMF_BCM_FC_POLLER_API_ADD_CONNECTION,
	SPDK_NVMF_BCM_FC_POLLER_API_DEL_CONNECTION,
	SPDK_NVMF_BCM_FC_POLLER_API_QUIESCE_QUEUE,
	SPDK_NVMF_BCM_FC_POLLER_API_ACTIVATE_QUEUE,
	SPDK_NVMF_BCM_FC_POLLER_API_ABTS_RECEIVED,
	SPDK_NVMF_BCM_FC_POLLER_API_ADAPTER_EVENT,
	SPDK_NVMF_BCM_FC_POLLER_API_AEN,
	SPDK_NVMF_BCM_FC_POLLER_API_QUEUE_SYNC,
} spdk_nvmf_bcm_fc_poller_api_t;

/*
 * Poller API callback function proto
 */
typedef void (*spdk_nvmf_bcm_fc_poller_api_cb)(void *cb_data,
		spdk_nvmf_bcm_fc_poller_api_ret_t ret);

/*
 * Poller API callback data
 */
struct spdk_nvmf_bcm_fc_poller_api_cb_info {
	spdk_nvmf_bcm_fc_poller_api_cb cb_func;
	void *cb_data;
	spdk_nvmf_bcm_fc_poller_api_ret_t ret;
};

/*
 * Poller API structures
 */
struct spdk_nvmf_bcm_fc_poller_api_add_connection_args {
	struct spdk_nvmf_bcm_fc_conn *fc_conn;
	struct spdk_nvmf_bcm_fc_poller_api_cb_info cb_info;
};

struct spdk_nvmf_bcm_fc_poller_api_del_connection_args {
	struct spdk_nvmf_bcm_fc_conn *fc_conn;
	struct spdk_nvmf_bcm_fc_hwqp *hwqp;
	struct spdk_nvmf_bcm_fc_poller_api_cb_info cb_info;
	bool send_abts;
	/* internal */
	int fc_request_cnt;
};

struct spdk_nvmf_bcm_fc_poller_api_quiesce_queue_args {
	void   *ctx;
	struct spdk_nvmf_bcm_fc_hwqp *hwqp;
	struct spdk_nvmf_bcm_fc_poller_api_cb_info cb_info;
};

struct spdk_nvmf_bcm_fc_poller_api_activate_queue_args {
	struct spdk_nvmf_bcm_fc_hwqp *hwqp;
	struct spdk_nvmf_bcm_fc_poller_api_cb_info cb_info;
};

struct spdk_nvmf_bcm_fc_poller_api_abts_recvd_args {
	fc_abts_ctx_t *ctx;
	struct spdk_nvmf_bcm_fc_hwqp *hwqp;
	struct spdk_nvmf_bcm_fc_poller_api_cb_info cb_info;
};

struct spdk_nvmf_bcm_fc_poller_api_queue_sync_args {
	uint64_t u_id;
	struct spdk_nvmf_bcm_fc_hwqp *hwqp;
	struct spdk_nvmf_bcm_fc_poller_api_cb_info cb_info;

	/* Used internally by poller */
	TAILQ_ENTRY(spdk_nvmf_bcm_fc_poller_api_queue_sync_args) link;
};

/*
 * NVMF LS request structure
 */
struct spdk_nvmf_bcm_fc_ls_rqst {
	bcm_buffer_desc_t rqstbuf;
	bcm_buffer_desc_t rspbuf;
	uint32_t rqst_len;
	uint32_t rsp_len;
	uint32_t rpi;
	struct spdk_nvmf_bcm_fc_xri *xri;
	uint16_t oxid;
	void *private_data; /* for RQ handler only (LS does not touch) */
	TAILQ_ENTRY(spdk_nvmf_bcm_fc_ls_rqst) ls_pending_link;
	uint32_t s_id;
	uint32_t d_id;
	struct spdk_nvmf_bcm_fc_nport *nport;
	struct spdk_nvmf_bcm_fc_remote_port_info *rport;
};

/*
 * FC RQ buffer NVMe Command
 */
struct __attribute__((__packed__)) spdk_nvmf_bcm_fc_rq_buf_nvme_cmd {
	struct spdk_nvmf_fc_cmnd_iu cmd_iu;
	struct spdk_nvmf_fc_xfer_rdy_iu xfer_rdy;
	struct bcm_sge sge[BCM_MAX_IOVECS];
	uint8_t rsvd[BCM_RQ_BUFFER_SIZE - (sizeof(struct spdk_nvmf_fc_cmnd_iu)
					   + sizeof(struct spdk_nvmf_fc_xfer_rdy_iu)
					   + (sizeof(struct bcm_sge) * BCM_MAX_IOVECS))];
};

SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_bcm_fc_rq_buf_nvme_cmd) ==
		   BCM_RQ_BUFFER_SIZE, "RQ Buffer overflow");

/*
 * RQ Buffer LS Overlay Structure
 */
struct __attribute__((__packed__)) spdk_nvmf_bcm_fc_rq_buf_ls_request {
	uint8_t rqst[BCM_MAX_LS_REQ_CMD_SIZE];
	uint8_t resp[BCM_MAX_RESP_BUFFER_SIZE];
	struct spdk_nvmf_bcm_fc_ls_rqst ls_rqst;
	uint8_t rsvd[BCM_RQ_BUFFER_SIZE - (sizeof(struct spdk_nvmf_bcm_fc_ls_rqst) +
					   BCM_MAX_RESP_BUFFER_SIZE + BCM_MAX_LS_REQ_CMD_SIZE)];
};

SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_bcm_fc_rq_buf_ls_request) ==
		   BCM_RQ_BUFFER_SIZE, "RQ Buffer overflow");

/*
 * Function protos
 */

/* Poller API entry point function */
spdk_nvmf_bcm_fc_poller_api_ret_t spdk_nvmf_bcm_fc_poller_api(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
		spdk_nvmf_bcm_fc_poller_api_t api,
		void *api_args);

//void spdk_nvmf_bcm_fc_ls_init(void);
void spdk_nvmf_bcm_fc_ls_init(struct spdk_nvmf_bcm_fc_port *fc_port);

//void spdk_nvmf_bcm_fc_ls_fini(void);
void spdk_nvmf_bcm_fc_ls_fini(struct spdk_nvmf_bcm_fc_port *fc_port);

typedef void (*spdk_nvmf_fc_del_assoc_cb)(void *arg, uint32_t err);

struct spdk_nvmf_bcm_fc_hwqp *spdk_nvmf_bcm_fc_get_hwqp(struct spdk_nvmf_bcm_fc_nport *tgtport,
		uint64_t conn_id);

int spdk_nvmf_bcm_fc_delete_association(struct spdk_nvmf_bcm_fc_nport *tgtport,
					uint64_t assoc_id, bool send_abts,
					spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
					void *cb_data);
void spdk_nvmf_bcm_fc_handle_ls_rqst(struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst);

int spdk_nvmf_bcm_fc_xmt_ls_rsp(struct spdk_nvmf_bcm_fc_nport *tgtport,
				struct spdk_nvmf_bcm_fc_ls_rqst *ls_rqst);

void spdk_nvmf_bcm_fc_port_list_add(struct spdk_nvmf_bcm_fc_port *fc_port);

struct spdk_nvmf_bcm_fc_nport *spdk_nvmf_bcm_fc_nport_get(uint8_t port_hdl, uint16_t nport_hdl);

struct spdk_nvmf_bcm_fc_nport *spdk_nvmf_bcm_req_fc_nport_get(struct spdk_nvmf_request *req);

struct spdk_nvmf_bcm_fc_session *spdk_nvmf_bcm_fc_get_fc_session(struct spdk_nvmf_session *session);

struct spdk_nvmf_bcm_fc_port *spdk_nvmf_bcm_fc_port_list_get(uint8_t port_hdl);

void spdk_nvmf_bcm_fc_init_poller_queues(struct spdk_nvmf_bcm_fc_hwqp *hwqp);

void spdk_nvmf_bcm_fc_init_poller(struct spdk_nvmf_bcm_fc_port *fc_port,
				  struct spdk_nvmf_bcm_fc_hwqp *hwqp);

void spdk_nvmf_bcm_fc_add_poller(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
				 uint64_t period_microseconds);

void spdk_nvmf_bcm_fc_delete_poller(struct spdk_nvmf_bcm_fc_hwqp *hwqp);

uint32_t spdk_nvmf_bcm_fc_queue_poller(void *arg);

void spdk_nvmf_bcm_fc_handle_abts_frame(struct spdk_nvmf_bcm_fc_nport *nport,
					uint16_t rpi, uint16_t oxid,
					uint16_t rxid);

void spdk_nvmf_bcm_fc_subsys_connect_cb(void *cb_ctx,
					struct spdk_nvmf_request *req);

void spdk_nvmf_bcm_fc_subsys_disconnect_cb(void *cb_ctx,
		struct spdk_nvmf_conn *conn);

struct spdk_nvmf_bcm_fc_xri *
spdk_nvmf_bcm_fc_get_xri(struct spdk_nvmf_bcm_fc_hwqp *hwqp);

int spdk_nvmf_bcm_fc_put_xri(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
			     struct spdk_nvmf_bcm_fc_xri *xri);

spdk_err_t spdk_nvmf_bcm_fc_port_set_online(struct spdk_nvmf_bcm_fc_port *fc_port);

spdk_err_t spdk_nvmf_bcm_fc_port_set_offline(struct spdk_nvmf_bcm_fc_port *fc_port);

spdk_err_t spdk_nvmf_bcm_fc_port_add_nport(struct spdk_nvmf_bcm_fc_port *fc_port,
		struct spdk_nvmf_bcm_fc_nport *nport);

spdk_err_t spdk_nvmf_bcm_fc_port_remove_nport(struct spdk_nvmf_bcm_fc_port *fc_port,
		struct spdk_nvmf_bcm_fc_nport *nport);

bool spdk_nvmf_bcm_fc_port_is_offline(struct spdk_nvmf_bcm_fc_port *fc_port);

spdk_err_t spdk_nvmf_bcm_fc_hwqp_port_set_online(struct spdk_nvmf_bcm_fc_hwqp *hwqp);

spdk_err_t spdk_nvmf_bcm_fc_hwqp_port_set_offline(struct spdk_nvmf_bcm_fc_hwqp *hwqp);

uint32_t spdk_nvmf_bcm_fc_nport_get_association_count(struct spdk_nvmf_bcm_fc_nport *nport);

bool spdk_nvmf_bcm_fc_nport_is_association_empty(struct spdk_nvmf_bcm_fc_nport *nport);

bool spdk_nvmf_bcm_fc_nport_is_rport_empty(struct spdk_nvmf_bcm_fc_nport *nport);

spdk_err_t spdk_nvmf_bcm_fc_nport_set_state(struct spdk_nvmf_bcm_fc_nport *nport,
		spdk_nvmf_bcm_fc_object_state_t state);

spdk_err_t spdk_nvmf_bcm_fc_assoc_set_state(struct spdk_nvmf_bcm_fc_association *assoc,
		spdk_nvmf_bcm_fc_object_state_t state);

bool spdk_nvmf_bcm_fc_nport_add_rem_port(struct spdk_nvmf_bcm_fc_nport *nport,
		struct spdk_nvmf_bcm_fc_remote_port_info *rem_port);

bool spdk_nvmf_bcm_fc_nport_remove_rem_port(struct spdk_nvmf_bcm_fc_nport *nport,
		struct spdk_nvmf_bcm_fc_remote_port_info *rem_port);

uint32_t spdk_nvmf_bcm_fc_get_prli_service_params(void);

spdk_err_t spdk_nvmf_bcm_fc_rport_set_state(struct spdk_nvmf_bcm_fc_remote_port_info *rport,
		spdk_nvmf_bcm_fc_object_state_t state);

int spdk_nvmf_bcm_fc_xmt_srsr_req(struct spdk_nvmf_bcm_fc_hwqp *hwqp,
				  struct spdk_nvmf_bcm_fc_send_srsr *srsr,
				  spdk_nvmf_bcm_fc_caller_cb cb, void *cb_args);

uint32_t spdk_nvmf_bcm_fc_get_num_nport_sessions_in_subsystem(uint8_t port_hdl, uint16_t nport_hdl,
		struct spdk_nvmf_subsystem *subsys);

bool spdk_nvmf_bcm_fc_is_spdk_session_on_nport(uint8_t port_hdl, uint16_t nport_hdl,
		struct spdk_nvmf_session *session);

spdk_err_t spdk_nvmf_bcm_fc_get_sess_init_traddr(char *traddr, struct spdk_nvmf_session *session);

uint32_t spdk_nvmf_bcm_fc_get_hwqp_id(struct spdk_nvmf_request *req);

#endif
