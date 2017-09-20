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

/*
 *  FC Connection Resource Defaults
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
#include "bcm_sli_fc.h"

#define TRUE  1
#define FALSE 0

#ifndef UINT16_MAX
#define UINT16_MAX  (65535U)
#endif
#ifndef UINT32_MAX
#define UINT32_MAX  (4294967295U)
#endif

#define NVMF_FC_MAX_IO_QUEUES       	16
#define HWQP_POLLER_TIMER_MSEC      	0
#define MAX_IOVS                    	128

#define NVME_FC_R_CTL_CMD_REQ	    	0x06
#define NVME_FC_R_CTL_DATA_OUT	    	0x01
#define NVME_FC_R_CTL_CONFIRM	    	0x03

#define NVME_FC_R_CTL_LS_REQUEST    	0x32
#define NVME_FC_R_CTL_LS_RESPONSE   	0x33

#define NVME_FC_R_CTL_BA_ABTS		0x81

#define NVME_FC_F_CTL_END_SEQ		0x080000
#define NVME_FC_F_CTL_SEQ_INIT		0x010000

#define NVME_FC_TYPE_BLS		0x0
#define NVME_FC_TYPE_FC_EXCHANGE    	0x08
#define NVME_FC_TYPE_NVMF_DATA	    	0x28

#define NVME_CMND_IU_FC_ID	    	0x28
#define NVME_CMND_IU_SCSI_ID	    	0xFD

#define NVME_CMND_IU_NODATA	    	0x00
#define NVME_CMND_IU_READ	    	0x10
#define NVME_CMND_IU_WRITE	    	0x01

#define NVME_ADMIN_QUEUE_ID	    	0

#define NVMF_BCM_FC_TRANSPORT_NAME "bcm_nvmf_fc"

/*
 * PRLI service parameters
 */
enum spdk_nvmf_fc_service_parameters {
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

} spdk_fc_port_state_t;

typedef enum spdk_fc_hwqp_state_e {

	SPDK_FC_HWQP_OFFLINE = 0,
	SPDK_FC_HWQP_ONLINE = 1,

} spdk_fc_hwqp_state_t;

/*
 * FC NPORT status.
 */
typedef enum spdk_fc_nport_state_e {

	SPDK_FC_NPORT_DOWN = 0,
	SPDK_FC_NPORT_UP = 1,

} spdk_fc_nport_state_t;

/*
 * Add all the generic states of the object here.
 * Specific object states can be added separately
 */
typedef enum spdk_fc_object_state {

	SPDK_FC_OBJECT_CREATED = 0,
	SPDK_FC_OBJECT_TO_BE_DELETED = 1,
	SPDK_FC_OBJECT_ZOMBIE = 2,      /* Partial Create or Delete  */
} spdk_fc_object_state_t;

/*
 * NVME FC ERSP status codes.
 */
typedef enum nvme_ersp_status_codes_e {

	NVME_ERSP_STATUS_SUCCESS = 0,
	NVME_ERSP_STATUS_INVALID_FIELD,
	NVME_ERSP_STATUS_INVALID_CONN_ID,

} nvme_ersp_status_codes_t;

/*
 * Hardware queues structure.
 * Structure passed from master thread to poller thread.
 */
struct fc_hw_queues {
	struct fc_eventq eq;
	struct fc_eventq cq_wq;
	struct fc_eventq cq_rq;
	struct fc_wrkq   wq;
	struct fc_rcvq   rq_hdr;
	struct fc_rcvq   rq_payload;
};

/*
 * Hardware queue port structure.
 * Structure passed from driver to master thread.
 */
struct fc_hw_port_queues {

	uint8_t port_handle;
	uint32_t xri_base;
	uint32_t xri_count;
	struct fc_hw_queues	ls_queue;
	uint32_t io_queue_cnt;   /* actual io queue count */
	struct fc_hw_queues	io_queues[NVMF_FC_MAX_IO_QUEUES];
};

/* NVME FC transport errors */
struct nvmf_fc_errors {
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
	uint32_t nvme_cmd_iu_err;
	uint32_t nvme_cmd_xfer_err;
	uint32_t queue_entry_invalid;
	uint32_t invalid_conn_err;
	uint32_t fcp_rsp_failure;
	uint32_t write_failed;
	uint32_t read_failed;
};

/*
 * Structure for maintaining the XRI's
 */
typedef struct fc_xri_s {
	uint32_t xri;   /* The actual xri value */
	/* Internal */
	TAILQ_ENTRY(fc_xri_s) link;
} fc_xri_t;

/*
 *  HWQP poller structure passed from Master thread
 */
struct fc_hwqp {

	uint32_t lcore_id;   /* core we are running on */
	uint32_t hwqp_id;    /* A unique id (per physical port) for a hwqp */
	struct fc_hw_queues	queues; /* poller queue set */
	struct spdk_nvmf_fc_port *fc_port; /* HW port structure for these queues */
	struct spdk_poller *poller;

	TAILQ_HEAD(, spdk_nvmf_fc_conn) connection_list;
	/* num_conns and cid_cnt used by master to generate (unique) connection IDs */
	uint32_t num_conns;
	uint16_t cid_cnt;
	spdk_fc_hwqp_state_t state;  /* Poller state SPDK_FC_HWQP_OFFLINE, SPDK_FC_HWQP_ONLINE  */

	/* Internal */
	struct spdk_mempool *fc_request_pool;
	TAILQ_HEAD(, spdk_nvmf_fc_request) in_use_reqs;

	TAILQ_HEAD(, fc_xri_s) pending_xri_list;

	struct nvmf_fc_errors counters;
	uint32_t send_frame_xri;
	uint8_t send_frame_seqid;
};

/*
 * FC HW port.
 */
struct spdk_nvmf_fc_port {
	uint8_t port_hdl;
	spdk_fc_port_state_t hw_port_status;
	uint32_t xri_base;
	uint32_t xri_count;
	struct spdk_ring *xri_ring;
	struct fc_hwqp ls_queue;
	uint32_t max_io_queues;
	struct fc_hwqp io_queues[NVMF_FC_MAX_IO_QUEUES];
	/*
	 * List of nports on this HW port.
	 */
	TAILQ_HEAD(, spdk_nvmf_fc_nport)nport_list;
	int	num_nports;
	TAILQ_ENTRY(spdk_nvmf_fc_port) link;
};

struct spdk_fc_wwn {
	union {
		uint64_t wwn; /* World Wide Names consist of eight bytes */
		uint8_t octets[sizeof(uint64_t)];
	} u;
};

struct spdk_nvmf_fc_rem_port_info {
	uint32_t s_id;
	uint32_t rpi;
	uint32_t assoc_count;  /* # of associations related to this port. Used by remote_port removal code */
	spdk_fc_object_state_t rport_state;
	TAILQ_ENTRY(spdk_nvmf_fc_rem_port_info) link;
};

/*
 * Struct representing a nport/lif.
 */
struct spdk_nvmf_fc_nport {

	uint16_t nport_hdl;
	uint8_t port_hdl;
	uint32_t d_id;
	bool nport_status;
	spdk_fc_object_state_t nport_state;
	struct spdk_fc_wwn fc_nodename;
	struct spdk_fc_wwn fc_portname;

	/* list of remote ports (i.e. initiators) connected to nport */
	TAILQ_HEAD(, spdk_nvmf_fc_rem_port_info) rem_port_list;
	uint32_t rport_count;

	/* list of associations to nport */
	TAILQ_HEAD(, spdk_nvmf_fc_association) fc_associations;
	uint32_t assoc_count;
	struct spdk_nvmf_fc_port *fc_port;
	TAILQ_ENTRY(spdk_nvmf_fc_nport) link; /* list of nports on a hw port. */
};

/* This structure holds commands as they are received off the wire.
 * It must be dynamically paired with a full request object
 * (spdk_nvmf_fc_request) to service a request. It is separate
 * from the request because RDMA does not appear to order
 * completions, so occasionally we'll get a new incoming
 * command when there aren't any free request objects.
 */
struct spdk_nvmf_fc_recv {
	/*  data buffer */
	uint8_t	*buf;
	TAILQ_ENTRY(spdk_nvmf_fc_recv) link;
#ifdef DEBUG
	bool in_use;
#endif
};

struct nvme_ersp_iu {
	uint32_t status_code: 8, rsvd0: 8, ersp_len: 16;
	uint32_t response_seq_no;
	uint32_t transferred_data_len;
	uint32_t rsvd1;
	union nvmf_c2h_msg	rsp;
};
#define NVME_ERSP_IU_SIZE sizeof(struct nvme_ersp_iu)

struct spdk_nvmf_fc_request {
	struct spdk_nvmf_request req;
	struct nvme_ersp_iu ersp;
	uint32_t poller_lcore;
	uint16_t buf_index;
	fc_xri_t *xri;
	uint16_t oxid;
	uint16_t rpi;
	struct spdk_nvmf_fc_conn *fc_conn;
	struct fc_hwqp *hwqp;
	bool xri_activated;
	bool rsp_sent;
	uint32_t transfered_len;
	bool is_aborted;
	TAILQ_ENTRY(spdk_nvmf_fc_request) link;
	TAILQ_ENTRY(spdk_nvmf_fc_request) pending_link;
};

struct spdk_nvmf_fc_session {
	struct spdk_nvmf_session session;
	struct spdk_nvmf_fc_association *fc_assoc;
};

struct spdk_nvmf_fc_conn {

	struct spdk_nvmf_conn conn;

	uint64_t conn_id;
	uint16_t qid;
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

	/* The number of RDMA READ and WRITE requests that are outstanding */
	uint16_t cur_fc_rw_depth;

	/* Receives that are waiting for a request object */
	TAILQ_HEAD(, spdk_nvmf_fc_recv) incoming_queue;

	/* Requests that are not in use */
	TAILQ_HEAD(, spdk_nvmf_fc_request) free_queue;

	/* Requests that are waiting to obtain a data buffer */
	TAILQ_HEAD(, spdk_nvmf_fc_request) pending_data_buf_queue;

	/* Requests that are waiting to perform an READ or WRITE */
	TAILQ_HEAD(, spdk_nvmf_fc_request) pending_fc_rw_queue;

	/* Array of size "max_queue_depth" containing requests. */
	struct spdk_nvmf_fc_request *reqs;

	/* Array of size "max_queue_depth" containing recvs. */
	struct spdk_nvmf_fc_recv *recvs;

	/* Array of size "max_queue_depth" containing 64 byte capsules
	 * used for receive.
	 */
	union nvmf_h2c_msg *cmds;
	union nvmf_c2h_msg *cpls;

	/* Array of size "max_queue_depth * InCapsuleDataSize" containing
	 * buffers to be used for in capsule data.
	 */
	void *bufs;

	struct spdk_nvmf_fc_association *fc_assoc;

	/* additional FC info here - TBD */
	uint16_t rpi;

	/* for association's connection list */
	TAILQ_ENTRY(spdk_nvmf_fc_conn) assoc_link;

	/* for hwqp's connection list */
	TAILQ_ENTRY(spdk_nvmf_fc_conn) link;
};

/* Poller errors */
/* NOTE: Change this when there is a global errors file */
typedef enum {
	NVMF_FC_POLLER_API_SUCCESS = 0,
	NVMF_FC_POLLER_API_ERROR,
	NVMF_FC_POLLER_API_INVALID_ARG,
	NVMF_FC_POLLER_API_NO_CONN_ID,
	NVMF_FC_POLLER_API_DUP_CONN_ID,
	NVMF_FC_POLLER_API_OXID_NOT_FOUND,
} nvmf_fc_poller_api_ret_t;

/* Poller API definitions */

typedef enum {
	NVMF_FC_POLLER_API_ADD_CONNECTION,
	NVMF_FC_POLLER_API_DEL_CONNECTION,
	NVMF_FC_POLLER_API_QUIESCE_QUEUE,
	NVMF_FC_POLLER_API_ACTIVATE_QUEUE,
	NVMF_FC_POLLER_API_ABTS_RECEIVED,
	NVMF_FC_POLLER_API_ADAPTER_EVENT,
	NVMF_FC_POLLER_API_AEN,
} nvmf_fc_poller_api_t;

typedef void (*nvmf_fc_poller_api_cb)(void *cb_data,
				      nvmf_fc_poller_api_ret_t ret);

struct nvmf_poller_api_cb_info {
	nvmf_fc_poller_api_cb cb_func;
	void *cb_data;
	nvmf_fc_poller_api_ret_t ret;
};

struct nvmf_fc_poller_api_add_connection_args {
	struct spdk_nvmf_fc_conn *fc_conn;
	struct fc_hwqp *hwqp;
	struct nvmf_poller_api_cb_info cb_info;
};

struct nvmf_fc_poller_api_del_connection_args {
	struct spdk_nvmf_fc_conn *fc_conn;
	struct fc_hwqp *hwqp;
	struct nvmf_poller_api_cb_info cb_info;
};

struct nvmf_fc_poller_api_quiesce_queue_args {
	struct fc_hwqp *hwqp;
	struct nvmf_poller_api_cb_info cb_info;
};

struct nvmf_fc_poller_api_activate_queue_args {
	struct fc_hwqp *hwqp;
	struct nvmf_poller_api_cb_info cb_info;
};

struct nvmf_fc_poller_api_abts_recvd_args {
	fc_abts_ctx_t *ctx;
	struct fc_hwqp *hwqp;
	struct nvmf_poller_api_cb_info cb_info;
};

/* the poller API entry point */
nvmf_fc_poller_api_ret_t nvmf_fc_poller_api(uint32_t lcore,
		nvmf_fc_poller_api_t api,
		void *api_args);

#define SPDK_NVMF_FC_HOST_ID_LEN    16
#define SPDK_NVMF_FC_NQN_MAX_LEN    256

/* data buffer for non-read/write commands */
struct nvmf_fc_aq_data_buf {
	SLIST_ENTRY(nvmf_fc_aq_data_buf) link;
};

struct spdk_nvmf_fc_association {
	uint64_t assoc_id;
	uint32_t s_id;
	struct spdk_nvmf_fc_nport *tgtport;
	struct spdk_nvmf_fc_rem_port_info *rport;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_fc_session *fc_session;
	spdk_fc_object_state_t assoc_state;

	char host_id[SPDK_NVMF_FC_HOST_ID_LEN];
	char host_nqn[SPDK_NVMF_FC_NQN_MAX_LEN];
	char sub_nqn[SPDK_NVMF_FC_NQN_MAX_LEN];

	uint16_t conn_count;
	TAILQ_HEAD(fc_conns, spdk_nvmf_fc_conn) fc_conns;

	TAILQ_ENTRY(spdk_nvmf_fc_association) link;
	void *ls_del_op_ctx;
};


/* LS Request */
struct nvmf_fc_ls_rqst {
	bcm_buffer_desc_t rqstbuf;
	bcm_buffer_desc_t rspbuf;
	uint32_t rqst_len;
	uint32_t rsp_len;
	uint32_t rpi;
	fc_xri_t *xri;
	uint16_t oxid;
	void *private_data; /* for RQ handler only (LS does not touch) */
};

/* NVME FC CMND IU formats */
struct nvme_xfer_rdy_iu {
	uint32_t relative_offset;
	uint32_t burst_len;
	uint32_t rsvd;
};
#define NVME_XFER_RDY_IU_SIZE sizeof(struct nvme_xfer_rdy_iu)

struct nvme_cmnd_iu {
	uint32_t scsi_id: 8, fc_id: 8, cmnd_iu_len: 16;
	uint32_t rsvd0: 24, flags: 8;
	uint64_t conn_id;
	uint32_t cmnd_seq_num;
	uint32_t data_len;
	union nvmf_h2c_msg cmd;
	uint32_t rsvd1[2];
};
#define NVME_CMND_IU_SIZE sizeof(struct nvme_cmnd_iu)


struct nvme_rsp_iu {
	uint32_t data[3];
};

#define BCM_RQ_BUFFER_SIZE	 2048	// Driver needs to use this macro.
#define BCM_MAX_LS_REQ_CMD_SIZE  1536
#define BCM_MAX_RESP_BUFFER_SIZE 64
#define BCM_MAX_IOVECS		 (MAX_NUM_OF_IOVECTORS + 2)	// This needs to be in sync with spdk_nvmf_request.
#define BCM_MAX_IOVECS_BUFFER_SIZE (BCM_MAX_IOVECS * sizeof(bcm_sge_t))

/* RQ NVMe Command Buffer Overlay Structure */
typedef struct __attribute__((__packed__)) nvmf_fc_rq_buf_nvme_cmd {
	struct nvme_cmnd_iu cmd_iu; /* 96 bytes */
	struct nvme_xfer_rdy_iu xfer_rdy; /* 12 bytes */
	struct spdk_nvmf_fc_request *fc_req;
	struct bcm_sge sge[(BCM_MAX_IOVECS + 2)];  /* 2 extra for skips */
	uint8_t rsvd[BCM_RQ_BUFFER_SIZE - (sizeof(struct nvme_cmnd_iu)
					   + sizeof(struct nvme_xfer_rdy_iu)
					   + sizeof(struct spdk_nvmf_fc_request *)
					   + (sizeof(struct bcm_sge) * (BCM_MAX_IOVECS + 2)))];
} nvmf_fc_rq_buf_nvme_cmd_t;

SPDK_STATIC_ASSERT((sizeof(struct nvme_cmnd_iu)
		    + sizeof(struct nvme_xfer_rdy_iu)
		    + sizeof(struct spdk_nvmf_fc_request *)
		    + (sizeof(struct bcm_sge) * (BCM_MAX_IOVECS + 2))) <  BCM_RQ_BUFFER_SIZE, "RQ Buffer overflow");

SPDK_STATIC_ASSERT(sizeof(nvmf_fc_rq_buf_nvme_cmd_t) == BCM_RQ_BUFFER_SIZE, "RQ Buffer overflow");

/* RQ Buffer LS Overlay Structure */
typedef struct __attribute__((__packed__)) nvmf_fc_rq_buf_ls_request {
	uint8_t rqst[BCM_MAX_LS_REQ_CMD_SIZE];
	uint8_t resp[BCM_MAX_RESP_BUFFER_SIZE];
	struct nvmf_fc_ls_rqst ls_rqst;  /* ~48 bytes */
	uint8_t rsvd[BCM_RQ_BUFFER_SIZE - (sizeof(struct nvmf_fc_ls_rqst) +
					   BCM_MAX_RESP_BUFFER_SIZE + BCM_MAX_LS_REQ_CMD_SIZE)];
} nvmf_fc_rq_buf_ls_request_t;


SPDK_STATIC_ASSERT(((sizeof(struct nvmf_fc_ls_rqst) + BCM_MAX_RESP_BUFFER_SIZE +
		     BCM_MAX_LS_REQ_CMD_SIZE)) < BCM_RQ_BUFFER_SIZE, "RQ Buffer overflow");
SPDK_STATIC_ASSERT(sizeof(nvmf_fc_rq_buf_ls_request_t) == BCM_RQ_BUFFER_SIZE, "RQ Buffer overflow");

/* external LS handling functions */
void spdk_nvmf_fc_ls_init(void);
void spdk_nvmf_fc_ls_fini(void);
typedef void (*spdk_nvmf_fc_del_assoc_cb)(void *arg, uint32_t err);
int spdk_nvmf_fc_delete_association(struct spdk_nvmf_fc_nport *tgtport,
				    uint64_t assoc_id,
				    spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
				    void *cb_data);
void spdk_nvmf_fc_handle_ls_rqst(uint32_t s_id,
				 struct spdk_nvmf_fc_nport *tgtport,
				 struct spdk_nvmf_fc_rem_port_info *rport,
				 struct nvmf_fc_ls_rqst *ls_rqst);

/* Add a port to the global port list */
void spdk_nvmf_fc_port_list_add(struct spdk_nvmf_fc_port *fc_port);

/* Return a fc port with a matching handle from the global list. */

/*  Return a fc nport with a matching handle. */
struct spdk_nvmf_fc_nport *spdk_nvmf_fc_nport_get(uint8_t port_hdl, uint16_t nport_hdl);

/*  Return a fc port with a matching handle. */
struct spdk_nvmf_fc_port *spdk_nvmf_fc_port_list_get(uint8_t port_hdl);

/* protos */
void spdk_nvmf_fc_init_poller(struct spdk_nvmf_fc_port *fc_port, struct fc_hwqp *hwqp);
void spdk_nvmf_fc_add_poller(struct fc_hwqp *hwqp);
void spdk_nvmf_fc_delete_poller(struct fc_hwqp *hwqp);
void spdk_nvmf_fc_handle_abts_frame(struct spdk_nvmf_fc_nport *nport, uint16_t rpi,
				    uint16_t oxid, uint16_t rxid);

/* Function protos for connect & disconnect callbacks from subsystem */
void spdk_nvmf_fc_subsys_connect_cb(void *cb_ctx, struct spdk_nvmf_request *req);
void spdk_nvmf_fc_subsys_disconnect_cb(void *cb_ctx, struct spdk_nvmf_conn *conn);

fc_xri_t *spdk_nvmf_fc_get_xri(struct fc_hwqp *hwqp);
int spdk_nvmf_fc_put_xri(struct fc_hwqp *hwqp, fc_xri_t *xri);

/* Returns true if the port is in offline state */
bool spdk_nvmf_fc_port_is_offline(struct spdk_nvmf_fc_port *fc_port);

spdk_err_t spdk_nvmf_fc_port_set_online(struct spdk_nvmf_fc_port *fc_port);

spdk_err_t spdk_nvmf_fc_port_set_offline(struct spdk_nvmf_fc_port *fc_port);

spdk_err_t spdk_nvmf_fc_port_add_nport(struct spdk_nvmf_fc_port *fc_port,
				       struct spdk_nvmf_fc_nport *nport);

spdk_err_t spdk_nvmf_fc_port_remove_nport(struct spdk_nvmf_fc_port *fc_port,
		struct spdk_nvmf_fc_nport *nport);

spdk_err_t spdk_nvmf_hwqp_port_set_online(struct fc_hwqp *hwqp);

spdk_err_t spdk_nvmf_hwqp_port_set_offline(struct fc_hwqp *hwqp);

uint32_t spdk_nvmf_fc_nport_get_association_count(struct spdk_nvmf_fc_nport *nport);

/* Returns true if the Nport is empty of all associations */
bool spdk_nvmf_fc_nport_is_association_empty(struct spdk_nvmf_fc_nport *nport);

bool spdk_nvmf_fc_nport_is_rport_empty(struct spdk_nvmf_fc_nport *nport);

spdk_err_t spdk_nvmf_fc_nport_set_state(struct spdk_nvmf_fc_nport *nport,
					spdk_fc_object_state_t state);

spdk_err_t spdk_nvmf_fc_assoc_set_state(struct spdk_nvmf_fc_association *assoc,
					spdk_fc_object_state_t state);

bool spdk_nvmf_fc_nport_add_rem_port(struct spdk_nvmf_fc_nport *nport,
				     struct spdk_nvmf_fc_rem_port_info *rem_port);

bool spdk_nvmf_fc_nport_remove_rem_port(struct spdk_nvmf_fc_nport *nport,
					struct spdk_nvmf_fc_rem_port_info *rem_port);

uint32_t spdk_nvmf_fc_get_prli_service_params(void);

uint32_t spdk_nvmf_bcm_fc_calc_max_q_depth(uint32_t nRQ, uint32_t RQsz,
		uint32_t mA, uint32_t mAC,
		uint32_t AQsz);

spdk_err_t spdk_nvmf_fc_rport_set_state(
	struct spdk_nvmf_fc_rem_port_info *rport, spdk_fc_object_state_t state);

#endif
