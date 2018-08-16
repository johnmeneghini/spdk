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

/*
 * Broadcom FC SLI-4 definitions
 */

#ifndef __BCM_SLI_FC_H__
#define __BCM_SLI_FC_H__

#include "spdk/stdinc.h"

#define BCM_MAJOR_CODE_STANDARD 0
#define BCM_MAJOR_CODE_SENTINEL 1

#define BCM_CQE_CODE_OFFSET	14

#define PTR_TO_ADDR32_HI(x)  (uint32_t)((uint64_t)(x & 0xFFFFFFFF00000000LL) >> 32);
#define PTR_TO_ADDR32_LO(x)  (uint32_t)((uint64_t)x & 0x0FFFFFFFFLL);

/* FC CQ Event Codes */
#define BCM_CQE_CODE_WORK_REQUEST_COMPLETION    0x01
#define BCM_CQE_CODE_RELEASE_WQE                0x02
#define BCM_CQE_CODE_RQ_ASYNC                   0x04
#define BCM_CQE_CODE_XRI_ABORTED                0x05
#define BCM_CQE_CODE_RQ_COALESCING              0x06
#define BCM_CQE_CODE_RQ_CONSUMPTION             0x07
#define BCM_CQE_CODE_MEASUREMENT_REPORTING      0x08
#define BCM_CQE_CODE_RQ_ASYNC_V1                0x09
#define BCM_CQE_CODE_OPTIMIZED_WRITE_CMD        0x0B
#define BCM_CQE_CODE_OPTIMIZED_WRITE_DATA       0x0C
#define BCM_CQE_CODE_NVME_ERSP_COMPLETION       0x0D
#define BCM_CQE_CODE_RQ_MARKER                  0x1D

/* FC Completion Status Codes */
#define BCM_FC_WCQE_STATUS_SUCCESS              0x00
#define BCM_FC_WCQE_STATUS_FCP_RSP_FAILURE      0x01
#define BCM_FC_WCQE_STATUS_REMOTE_STOP          0x02
#define BCM_FC_WCQE_STATUS_LOCAL_REJECT         0x03
#define BCM_FC_WCQE_STATUS_NPORT_RJT            0x04
#define BCM_FC_WCQE_STATUS_FABRIC_RJT           0x05
#define BCM_FC_WCQE_STATUS_NPORT_BSY            0x06
#define BCM_FC_WCQE_STATUS_FABRIC_BSY           0x07
#define BCM_FC_WCQE_STATUS_LS_RJT               0x09
#define BCM_FC_WCQE_STATUS_RX_BUFF_OVERRUN      0x0a
#define BCM_FC_WCQE_STATUS_CMD_REJECT           0x0b
#define BCM_FC_WCQE_STATUS_FCP_TGT_LENCHECK     0x0c
#define BCM_FC_WCQE_STATUS_RQ_BUF_LEN_EXCEEDED  0x11
#define BCM_FC_WCQE_STATUS_RQ_INSUFF_BUF_NEEDED 0x12
#define BCM_FC_WCQE_STATUS_RQ_INSUFF_FRM_DISC   0x13
#define BCM_FC_WCQE_STATUS_RQ_DMA_FAILURE       0x14
#define BCM_FC_WCQE_STATUS_FCP_RSP_TRUNCATE     0x15
#define BCM_FC_WCQE_STATUS_DI_ERROR             0x16
#define BCM_FC_WCQE_STATUS_BA_RJT               0x17
#define BCM_FC_WCQE_STATUS_RQ_INSUFF_XRI_NEEDED 0x18
#define BCM_FC_WCQE_STATUS_RQ_INSUFF_XRI_DISC   0x19
#define BCM_FC_WCQE_STATUS_RX_ERROR_DETECT      0x1a
#define BCM_FC_WCQE_STATUS_RX_ABORT_REQUEST     0x1b

/* Driver generated status codes; better not overlap with chip's status codes! */
#define BCM_FC_WCQE_STATUS_TARGET_WQE_TIMEOUT  	0xff
#define BCM_FC_WCQE_STATUS_SHUTDOWN            	0xfe
#define BCM_FC_WCQE_STATUS_DISPATCH_ERROR      	0xfd

#define SLI4_FC_COALESCE_RQ_SUCCESS		0x10
#define SLI4_FC_COALESCE_RQ_INSUFF_XRI_NEEDED	0x18


/* DI_ERROR Extended Status */
#define BCM_FC_DI_ERROR_GE     (1 << 0) /* Guard Error */
#define BCM_FC_DI_ERROR_AE     (1 << 1) /* Application Tag Error */
#define BCM_FC_DI_ERROR_RE     (1 << 2) /* Reference Tag Error */
#define BCM_FC_DI_ERROR_TDPV   (1 << 3) /* Total Data Placed Valid */
#define BCM_FC_DI_ERROR_UDB    (1 << 4) /* Uninitialized DIF Block */
#define BCM_FC_DI_ERROR_EDIR   (1 << 5) /* Error direction */

/* Local Reject Reason Codes */
#define BCM_FC_LOCAL_REJECT_MISSING_CONTINUE       0x01
#define BCM_FC_LOCAL_REJECT_SEQUENCE_TIMEOUT       0x02
#define BCM_FC_LOCAL_REJECT_INTERNAL_ERROR         0x03
#define BCM_FC_LOCAL_REJECT_INVALID_RPI            0x04
#define BCM_FC_LOCAL_REJECT_NO_XRI                 0x05
#define BCM_FC_LOCAL_REJECT_ILLEGAL_COMMAND        0x06
#define BCM_FC_LOCAL_REJECT_XCHG_DROPPED           0x07
#define BCM_FC_LOCAL_REJECT_ILLEGAL_FIELD          0x08
#define BCM_FC_LOCAL_REJECT_NO_ABORT_MATCH         0x0c
#define BCM_FC_LOCAL_REJECT_TX_DMA_FAILED          0x0d
#define BCM_FC_LOCAL_REJECT_RX_DMA_FAILED          0x0e
#define BCM_FC_LOCAL_REJECT_ILLEGAL_FRAME          0x0f
#define BCM_FC_LOCAL_REJECT_NO_RESOURCES           0x11
#define BCM_FC_LOCAL_REJECT_FCP_CONF_FAILURE       0x12
#define BCM_FC_LOCAL_REJECT_ILLEGAL_LENGTH         0x13
#define BCM_FC_LOCAL_REJECT_UNSUPPORTED_FEATURE    0x14
#define BCM_FC_LOCAL_REJECT_ABORT_IN_PROGRESS      0x15
#define BCM_FC_LOCAL_REJECT_ABORT_REQUESTED        0x16
#define BCM_FC_LOCAL_REJECT_RCV_BUFFER_TIMEOUT     0x17
#define BCM_FC_LOCAL_REJECT_LOOP_OPEN_FAILURE      0x18
#define BCM_FC_LOCAL_REJECT_LINK_DOWN              0x1a
#define BCM_FC_LOCAL_REJECT_CORRUPTED_DATA         0x1b
#define BCM_FC_LOCAL_REJECT_CORRUPTED_RPI          0x1c
#define BCM_FC_LOCAL_REJECT_OUT_OF_ORDER_DATA      0x1d
#define BCM_FC_LOCAL_REJECT_OUT_OF_ORDER_ACK       0x1e
#define BCM_FC_LOCAL_REJECT_DUP_FRAME              0x1f
#define BCM_FC_LOCAL_REJECT_LINK_CONTROL_FRAME     0x20
#define BCM_FC_LOCAL_REJECT_BAD_HOST_ADDRESS       0x21
#define BCM_FC_LOCAL_REJECT_MISSING_HDR_BUFFER     0x23
#define BCM_FC_LOCAL_REJECT_MSEQ_CHAIN_CORRUPTED   0x24
#define BCM_FC_LOCAL_REJECT_ABORTMULT_REQUESTED    0x25
#define BCM_FC_LOCAL_REJECT_BUFFER_SHORTAGE        0x28
#define BCM_FC_LOCAL_REJECT_RCV_XRIBUF_WAITING     0x29
#define BCM_FC_LOCAL_REJECT_INVALID_VPI            0x2e
#define BCM_FC_LOCAL_REJECT_MISSING_XRIBUF         0x30
#define BCM_FC_LOCAL_REJECT_INVALID_RELOFFSET      0x40
#define BCM_FC_LOCAL_REJECT_MISSING_RELOFFSET      0x41
#define BCM_FC_LOCAL_REJECT_INSUFF_BUFFERSPACE     0x42
#define BCM_FC_LOCAL_REJECT_MISSING_SI             0x43
#define BCM_FC_LOCAL_REJECT_MISSING_ES             0x44
#define BCM_FC_LOCAL_REJECT_INCOMPLETE_XFER        0x45
#define BCM_FC_LOCAL_REJECT_SLER_FAILURE           0x46
#define BCM_FC_LOCAL_REJECT_SLER_CMD_RCV_FAILURE   0x47
#define BCM_FC_LOCAL_REJECT_SLER_REC_RJT_ERR       0x48
#define BCM_FC_LOCAL_REJECT_SLER_REC_SRR_RETRY_ERR 0x49
#define BCM_FC_LOCAL_REJECT_SLER_SRR_RJT_ERR       0x4a
#define BCM_FC_LOCAL_REJECT_SLER_RRQ_RJT_ERR       0x4c
#define BCM_FC_LOCAL_REJECT_SLER_RRQ_RETRY_ERR     0x4d
#define BCM_FC_LOCAL_REJECT_SLER_ABTS_ERR          0x4e

#define BCM_FC_ASYNC_RQ_SUCCESS			   0x10
#define BCM_FC_ASYNC_RQ_BUF_LEN_EXCEEDED	   0x11
#define BCM_FC_ASYNC_RQ_INSUFF_BUF_NEEDED	   0x12
#define BCM_FC_ASYNC_RQ_INSUFF_BUF_FRM_DISC	   0x13
#define BCM_FC_ASYNC_RQ_DMA_FAILURE		   0x14

/**
 * Work Queue Entry (WQE) types.
 */
#define BCM_WQE_ABORT			0x0f
#define BCM_WQE_ELS_REQUEST64		0x8a
#define BCM_WQE_FCP_IBIDIR64		0xac
#define BCM_WQE_FCP_IREAD64		0x9a
#define BCM_WQE_FCP_IWRITE64		0x98
#define BCM_WQE_FCP_ICMND64		0x9c
#define BCM_WQE_FCP_TRECEIVE64		0xa1
#define BCM_WQE_FCP_CONT_TRECEIVE64	0xe5
#define BCM_WQE_FCP_TRSP64		0xa3
#define BCM_WQE_FCP_TSEND64		0x9f
#define BCM_WQE_GEN_REQUEST64		0xc2
#define BCM_WQE_SEND_FRAME		0xe1
#define BCM_WQE_XMIT_BCAST64		0X84
#define BCM_WQE_XMIT_BLS_RSP		0x97
#define BCM_WQE_ELS_RSP64		0x95
#define BCM_WQE_XMIT_SEQUENCE64		0x82
#define BCM_WQE_REQUEUE_XRI		0x93
#define BCM_WQE_MARKER			0xe6

/**
 * WQE command types.
 */
#define BCM_CMD_FCP_IREAD64_WQE		0x00
#define BCM_CMD_FCP_ICMND64_WQE		0x00
#define BCM_CMD_FCP_IWRITE64_WQE	0x01
#define BCM_CMD_FCP_TRECEIVE64_WQE	0x02
#define BCM_CMD_FCP_TRSP64_WQE		0x03
#define BCM_CMD_FCP_TSEND64_WQE		0x07
#define BCM_CMD_GEN_REQUEST64_WQE	0x08
#define BCM_CMD_XMIT_BCAST64_WQE	0x08
#define BCM_CMD_XMIT_BLS_RSP64_WQE	0x08
#define BCM_CMD_ABORT_WQE		0x08
#define BCM_CMD_XMIT_SEQUENCE64_WQE	0x08
#define BCM_CMD_REQUEUE_XRI_WQE		0x0A
#define BCM_CMD_SEND_FRAME_WQE		0x0a
#define BCM_CMD_MARKER_WQE		0x0a

#define BCM_WQE_SIZE			0x05
#define BCM_WQE_EXT_SIZE		0x06

#define BCM_WQE_BYTES			(16 * sizeof(uint32_t))
#define BCM_WQE_EXT_BYTES		(32 * sizeof(uint32_t))

#define BCM_ELS_REQUEST64_CONTEXT_RPI	0x0
#define BCM_ELS_REQUEST64_CONTEXT_VPI	0x1
#define BCM_ELS_REQUEST64_CONTEXT_VFI	0x2
#define BCM_ELS_REQUEST64_CONTEXT_FCFI	0x3

#define BCM_ELS_REQUEST64_CLASS_2	0x1
#define BCM_ELS_REQUEST64_CLASS_3	0x2

#define BCM_ELS_REQUEST64_DIR_WRITE	0x0
#define BCM_ELS_REQUEST64_DIR_READ	0x1

#define BCM_ELS_REQUEST64_OTHER		0x0
#define BCM_ELS_REQUEST64_LOGO		0x1
#define BCM_ELS_REQUEST64_FDISC		0x2
#define BCM_ELS_REQUEST64_FLOGIN	0x3
#define BCM_ELS_REQUEST64_PLOGI		0x4

#define BCM_ELS_REQUEST64_CMD_GEN		0x08
#define BCM_ELS_REQUEST64_CMD_NON_FABRIC	0x0c
#define BCM_ELS_REQUEST64_CMD_FABRIC		0x0d

#define BCM_IO_CONTINUATION		BIT(0)	/** The XRI associated with this IO is already active */
#define BCM_IO_AUTO_GOOD_RESPONSE	BIT(1)	/** Automatically generate a good RSP frame */
#define BCM_IO_NO_ABORT			BIT(2)
#define BCM_IO_DNRX			BIT(3)	/** Set the DNRX bit because no auto xref rdy buffer is posted */

/* Mask for ccp (CS_CTL) */
#define BCM_MASK_CCP	0xfe /* Upper 7 bits of CS_CTL is priority */

#define BCM_BDE_TYPE_BDE_64		0x00	/** Generic 64-bit data */
#define BCM_BDE_TYPE_BDE_IMM		0x01	/** Immediate data */
#define BCM_BDE_TYPE_BLP		0x40	/** Buffer List Pointer */

#define BCM_SGE_TYPE_DATA		0x00
#define BCM_SGE_TYPE_SKIP		0x0c

#define BCM_ABORT_CRITERIA_XRI_TAG      0x01

/* BLS RJT Error codes */
#define BCM_BLS_REJECT_CODE_UNABLE_TO_PERFORM	0x09
#define BCM_BLS_REJECT_EXP_NOINFO		0x00
#define BCM_BLS_REJECT_EXP_INVALID_OXID		0x03

/* Support for sending ABTS in case of sequence errors */
#define BCM_SUPPORT_ABTS_FOR_SEQ_ERRORS		true

/* Support for MARKER */
#define BCM_SUPPORT_ABTS_MARKERS                true

#define BCM_MARKER_CATAGORY_ALL_RQ		0x1
#define BCM_MARKER_CATAGORY_ALL_RQ_EXCEPT_ONE	0x2

/* FC CQE Types */
typedef enum {
	BCM_FC_QENTRY_ASYNC,
	BCM_FC_QENTRY_MQ,
	BCM_FC_QENTRY_RQ,
	BCM_FC_QENTRY_WQ,
	BCM_FC_QENTRY_WQ_RELEASE,
	BCM_FC_QENTRY_OPT_WRITE_CMD,
	BCM_FC_QENTRY_OPT_WRITE_DATA,
	BCM_FC_QENTRY_XABT,
	BCM_FC_QENTRY_NVME_ERSP,
	BCM_FC_QENTRY_MAX,         /* must be last */
} bcm_qentry_type_e;

typedef enum  {
	BCM_FC_QUEUE_TYPE_EQ,
	BCM_FC_QUEUE_TYPE_CQ_WQ,
	BCM_FC_QUEUE_TYPE_CQ_RQ,
	BCM_FC_QUEUE_TYPE_WQ,
	BCM_FC_QUEUE_TYPE_RQ_HDR,
	BCM_FC_QUEUE_TYPE_RQ_DATA,
} bcm_fc_queue_type_e;

/* SGE structure */
typedef struct bcm_sge {
	uint32_t	buffer_address_high;
	uint32_t	buffer_address_low;
	uint32_t	data_offset: 27,
			sge_type: 4,
			last: 1;
	uint32_t	buffer_length;
} bcm_sge_t;

#define BCM_SGE_SIZE sizeof(struct bcm_sge)

/* SLI BDE structure */
typedef struct bcm_bde {
	uint32_t	buffer_length: 24,
			bde_type: 8;
	union {
		struct {
			uint32_t buffer_address_low;
			uint32_t buffer_address_high;
		} data;
		struct {
			uint32_t offset;
			uint32_t rsvd2;
		} imm;
		struct {
			uint32_t sgl_segment_address_low;
			uint32_t sgl_segment_address_high;
		} blp;
	} u;
} bcm_bde_t;

/* FS-5 FC frame NVME header */
typedef struct fc_frame_hdr {
	__be32 	r_ctl: 8,
		d_id: 24;
	__be32  cs_ctl: 8,
		s_id: 24;
	__be32  type: 8,
		f_ctl: 24;
	__be32  seq_id: 8,
		df_ctl: 8,
		seq_cnt: 16;
	__be32  ox_id: 16,
		rx_id: 16;
	__be32 parameter;

} fc_frame_hdr_t;

typedef struct fc_frame_hdr_le {
	uint32_t	d_id: 24,
			r_ctl: 8;
	uint32_t	s_id: 24,
			cs_ctl: 8;
	uint32_t	f_ctl: 24,
			type: 8;
	uint32_t	seq_cnt: 16,
			df_ctl: 8,
			seq_id: 8;
	uint32_t	rx_id: 16,
			ox_id: 16;
	uint32_t	parameter;

} fc_frame_hdr_le_t;


/* Generic DMA buffer descriptor */
typedef struct bcm_buffer_desc {
	void *virt;
	uint64_t phys;
	size_t len;

	/* Internal */
	uint32_t buf_index;

} bcm_buffer_desc_t;

/* Common queue definition structure */
typedef struct bcm_sli_queue {
	/* general queue housekeeping fields */
	uint16_t  head, tail, used;
	uint32_t  posted_limit;    /* number of CQE/EQE to process before ringing doorbell */
	uint32_t  processed_limit; /* number of CQE/EQE to process in a shot */
	uint16_t  type;            /* bcm_fc_queue_type_e queue type */

	/* the following fields set by the FC driver */
	uint16_t  qid;           /* f/w Q_ID */
	uint16_t  size;          /* size of each entry */
	uint16_t  max_entries;   /* number of entries */
	void 	  *address;      /* queue address */
	void 	  *doorbell_reg; /* queue doorbell register address */
} bcm_sli_queue_t;

/* EQ/CQ structure */
typedef struct fc_eventq {
	bcm_sli_queue_t q;
	bool auto_arm_flag;     /* set by poller thread only */
} fc_eventq_t;

/* ABTS hadling context */
typedef struct fc_abts_ctx {
	bool handled;
	uint16_t hwqps_responded;
	uint16_t rpi;
	uint16_t oxid;
	uint16_t rxid;
	struct spdk_nvmf_bcm_fc_nport *nport;
	uint16_t nport_hdl;
	uint8_t port_hdl;
	void *abts_poller_args;
	void *sync_poller_args;
	int num_hwqps;
	bool queue_synced;
	uint64_t u_id;
	struct spdk_nvmf_bcm_fc_hwqp *ls_hwqp;
	uint16_t fcp_rq_id;
} fc_abts_ctx_t;

/* Caller context */
typedef void (*spdk_nvmf_bcm_fc_caller_cb)(void *hwqp, int32_t status, void *args);

typedef struct fc_caller_ctx {
	void *ctx;
	spdk_nvmf_bcm_fc_caller_cb cb;
	void *cb_args;
	TAILQ_ENTRY(fc_caller_ctx) link;
} fc_caller_ctx_t;

/* WQ related */
typedef void (*bcm_fc_wqe_cb)(void *hwqp, uint8_t *cqe, int32_t status, void *args);

#define MAX_WQ_WQEC_CNT 5
#define MAX_REQTAG_POOL_SIZE 8191 /* Should be one less than DPDK ring */
typedef struct fc_wqe_reqtag {
	uint16_t index;
	bcm_fc_wqe_cb cb;
	void *cb_args;
} fc_reqtag_t;

#define MAX_WQ_ENTRIES 4096
typedef struct fc_wrkq {
	bcm_sli_queue_t q;
	uint32_t num_buffers;
	bcm_buffer_desc_t *buffer;  /* BDE buffer descriptor array */

	/* internal */
	uint32_t wqec_count;
	struct spdk_ring *reqtag_ring;
	fc_reqtag_t *reqtag_objs;
	fc_reqtag_t *p_reqtags[MAX_REQTAG_POOL_SIZE];
} fc_wrkq_t;

#define MAX_RQ_ENTRIES 4096
/* RQ structure */
typedef struct fc_rcvq {
	bcm_sli_queue_t q;
	uint32_t num_buffers;
	bcm_buffer_desc_t *buffer;      /* RQ buffer descriptor array */
	/* internal */
	uint32_t rq_map[MAX_RQ_ENTRIES];
} fc_rcvq_t;

/* Doorbell register structure definitions */
typedef struct eqdoorbell {
	uint32_t eq_id            : 9;
	uint32_t ci               : 1;
	uint32_t qt               : 1;
	uint32_t eq_id_ext        : 5;
	uint32_t num_popped       : 13;
	uint32_t arm              : 1;
	uint32_t rsvd             : 1;
	uint32_t solicit_enable   : 1;
} eqdoorbell_t;

typedef struct cqdoorbell {
	uint32_t cq_id            : 10;
	uint32_t qt               : 1;
	uint32_t cq_id_ext        : 5;
	uint32_t num_popped       : 13;
	uint32_t arm              : 1;
	uint32_t rsvd             : 1;
	uint32_t solicit_enable   : 1;
} cqdoorbell_t;

typedef struct wqdoorbell {
	uint32_t wq_id          : 16;
	uint32_t wq_index       : 8;
	uint32_t num_posted     : 8;
} wqdoorbell_t;

typedef struct rqdoorbell {
	uint32_t rq_id          : 16;
	uint32_t num_posted     : 14;
	uint32_t rsvd           : 2;
} rqdoorbell_t;

typedef union doorbell_u {
	eqdoorbell_t eqdoorbell;
	cqdoorbell_t cqdoorbell;
	wqdoorbell_t wqdoorbell;
	rqdoorbell_t rqdoorbell;
	uint32_t     doorbell;
} doorbell_t;

/* EQE bit definition */
typedef struct eqe {
	uint32_t  valid        : 1;
	uint32_t  major_code   : 3;
	uint32_t  minor_code   : 12;
	uint32_t  resource_id  : 16;
} eqe_t;

/* CQE bit definitions */
typedef struct cqe {
	union {
		struct {
			uint32_t  word0;
			uint32_t  word1;
			uint32_t  word2;
			uint32_t  word3;
		} words;

		// Generic
		struct {
			uint8_t   hw_status;
			uint8_t   status;
			uint16_t  request_tag;
			union {
				uint32_t wqe_specific;
				uint32_t total_data_placed;
			} word1;

			uint32_t ext_status;
			uint32_t rsvd0          : 16;
			uint32_t event_code     : 8;
			uint32_t pri            : 3;
			uint32_t pv             : 1;
			uint32_t xb             : 1;
			uint32_t rha            : 1;
			uint32_t rsvd1          : 1;
			uint32_t valid          : 1;
		} generic;

		struct {
			uint32_t hw_status      : 8;
			uint32_t status         : 8;
			uint32_t request_tag    : 16;
			uint32_t wqe_specific_1;
			uint32_t wqe_specific_2;
			uint32_t rsvd0          : 15;
			uint32_t qx             : 1;
			uint32_t code           : 8;
			uint32_t pri            : 3;
			uint32_t pv             : 1;
			uint32_t xb             : 1;
			uint32_t rsvd1          : 2;
			uint32_t valid          : 1;
		} wcqe;

		// WQE Release CQE
		struct {
			uint32_t rsvd0;
			uint32_t rsvd1;
			uint32_t wqe_index      : 16;
			uint32_t wqe_id         : 16;
			uint32_t rsvd2          : 16;
			uint32_t event_code     : 8;
			uint32_t rsvd3          : 7;
			uint32_t valid          : 1;
		} release;

		struct {
			uint32_t rsvd0           : 8;
			uint32_t status          : 8;
			uint32_t rq_element_index: 12;
			uint32_t rsvd1           : 4;
			uint32_t rsvd2;
			uint32_t fcfi            : 6;
			uint32_t rq_id           : 10;
			uint32_t payload_data_placement_length: 16;
			uint32_t sof_byte        : 8;
			uint32_t eof_byte        : 8;
			uint32_t code            : 8;
			uint32_t header_data_placement_length: 6;
			uint32_t rsvd3           : 1;
			uint32_t valid           : 1;
		} async_rcqe;

		struct  {
			uint32_t rsvd0           : 8;
			uint32_t status          : 8;
			uint32_t rq_element_index: 15;
			uint32_t rsvd1           : 1;
			uint32_t fcfi            : 6;
			uint32_t rsvd2           : 26;
			uint32_t rq_id           : 16;
			uint32_t payload_data_placement_length: 16;
			uint32_t sof_byte        : 8;
			uint32_t eof_byte        : 8;
			uint32_t code            : 8;
			uint32_t header_data_placement_length: 6;
			uint32_t rsvd3           : 1;
			uint32_t valid           : 1;
		} async_rcqe_v1;

		struct {
			uint32_t rsvd0           : 8;
			uint32_t status          : 8;
			uint32_t rq_element_index: 15;
			uint32_t iv              : 1;
			uint32_t tag_lower;
			uint32_t tag_higher;
			uint32_t rq_id           : 16;
			uint32_t code            : 8;
			uint32_t rsvd1           : 7;
			uint32_t valid           : 1;
		} async_marker;

		struct {
			uint32_t rsvd0           : 8;
			uint32_t status          : 8;
			uint32_t rq_element_index: 15;
			uint32_t iv              : 1;
			uint32_t fcfi            : 6;
			uint32_t rsvd1           : 8;
			uint32_t oox             : 1;
			uint32_t agxr            : 1;
			uint32_t xri             : 16;
			uint32_t rq_id			 : 16;
			uint32_t payload_data_placement_length: 16;
			uint32_t rpi             : 16;
			uint32_t code            : 8;
			uint32_t header_data_placement_length: 6;
			uint32_t rsvd2           : 1;
			uint32_t valid           : 1;
		} optimized_write_cmd_cqe;

		struct  {
			uint32_t hw_status       : 8;
			uint32_t status          : 8;
			uint32_t xri             : 16;
			uint32_t total_data_placed;
			uint32_t extended_status;
			uint32_t rsvd0           : 16;
			uint32_t code            : 8;
			uint32_t pri             : 3;
			uint32_t pv              : 1;
			uint32_t xb              : 1;
			uint32_t rha             : 1;
			uint32_t rsvd1           : 1;
			uint32_t valid           : 1;
		} optimized_write_data_cqe;

		struct  {
			uint32_t rsvd0            : 8;
			uint32_t status           : 8;
			uint32_t rq_element_index : 12;
			uint32_t rsvd1            : 4;
			uint32_t rsvd2;
			uint32_t rq_id            : 16;
			uint32_t sequence_reporting_placement_length: 16;
			uint32_t rsvd3            : 16;
			uint32_t code             : 8;
			uint32_t rsvd4            : 7;
			uint32_t valid            : 1;
		} coalescing_rcqe;

		struct {
			uint32_t rsvd0           : 8;
			uint32_t status          : 8;
			uint32_t rsvd1           : 16;
			uint32_t extended_status;
			uint32_t xri             : 16;
			uint32_t remote_xid      : 16;
			uint32_t rsvd2           : 16;
			uint32_t code            : 8;
			uint32_t xr              : 1;
			uint32_t rsvd3           : 3;
			uint32_t eo              : 1;
			uint32_t br              : 1;
			uint32_t ia              : 1;
			uint32_t valid           : 1;
		} xri_aborted_cqe;

		struct {
			uint32_t rsvd0           : 32;
			uint32_t rsvd1           : 32;
			uint32_t wqe_index       : 16;
			uint32_t wq_id           : 16;
			uint32_t rsvd2           : 16;
			uint32_t code            : 8;
			uint32_t rsvd3           : 7;
			uint32_t valid           : 1;
		} wqec;

		// NVME ERSP CQE
		struct {
			uint16_t nvme_cqe_1;
			uint16_t request_tag;
			uint32_t nvme_cqe_0;
			uint32_t rsn;
			uint32_t sghd           : 16;
			uint32_t code           : 8;
			uint32_t pri            : 3;
			uint32_t pv             : 1;
			uint32_t xb             : 1;
			uint32_t rha            : 1;
			uint32_t rsvd           : 1;
			uint32_t valid          : 1;
		} ersp;

	} u;

} cqe_t;

/* CQE types */
typedef struct bcm_fc_async_rcqe {
	uint32_t rsvd0 : 8,
		 status: 8,
		 rq_element_index: 12,
		 rsvd1: 4;
	uint32_t rsvd2;
	uint32_t fcfi: 6,
		 rq_id: 10,
		 payload_data_placement_length: 16;
	uint32_t sof_byte: 8,
		 eof_byte: 8,
		 code: 8,
		 header_data_placement_length: 6,
		 rsvd3: 1,
		 vld: 1;

} bcm_fc_async_rcqe_t;

typedef struct bcm_fc_coalescing_rcqe {
	uint32_t rsvd0: 8,
		 status: 8,
		 rq_element_index: 12,
		 rsvd1: 4;
	uint32_t rsvd2;
	uint32_t rq_id: 16,
		 sequence_reporting_placement_length: 16;
	uint32_t rsvd3: 16,
		 code: 8,
		 rsvd4: 7,
		 vld: 1;
} bcm_fc_coalescing_rcqe_t;

typedef struct bcm_fc_async_rcqe_v1 {

	uint32_t rsvd0: 8,
		 status: 8,
		 rq_element_index: 15,
		 rsvd1: 1;
	uint32_t fcfi: 6,
		 rsvd2: 26;
	uint32_t rq_id: 16,
		 payload_data_placement_length: 16;
	uint32_t sof_byte: 8,
		 eof_byte: 8,
		 code: 8,
		 header_data_placement_length: 6,
		 rsvd3: 1,
		 vld: 1;

} bcm_fc_async_rcqe_v1_t;


typedef struct bcm_fc_async_rcqe_marker {
	uint32_t rsvd0: 8,
		 status: 8,
		 rq_element_index: 15,
		 rsvd1: 1;
	uint32_t tag_lower;
	uint32_t tag_higher;
	uint32_t rq_id: 16,
		 code: 8,
		 : 6,
		 : 1,
		 vld: 1;
} bcm_fc_async_rcqe_marker_t;

typedef struct bcm_fc_optimized_write_cmd_cqe {
	uint32_t rsvd0: 8,
		 status: 8,
		 rq_element_index: 15,
		 iv: 1;
	uint32_t fcfi: 6,
		 rsvd1: 8,
		 oox: 1,
		 agxr: 1,
		 xri: 16;
	uint32_t rq_id: 16,
		 payload_data_placement_length: 16;
	uint32_t rpi: 16,
		 code: 8,
		 header_data_placement_length: 6,
		 rsvd2: 1,
		 vld: 1;
} bcm_fc_optimized_write_cmd_cqe_t;

typedef struct bcm_fc_optimized_write_data_cqe {
	uint32_t hw_status: 8,
		 status: 8,
		 xri: 16;
	uint32_t total_data_placed;
	uint32_t extended_status;
	uint32_t rsvd0: 16,
		 code: 8,
		 pri: 3,
		 pv: 1,
		 xb: 1,
		 rha: 1,
		 rsvd1: 1,
		 vld: 1;
} bcm_fc_optimized_write_data_cqe_t;


/* WQE commands */
typedef struct bcm_fcp_treceive64_wqe {
	bcm_bde_t	bde;
	uint32_t	payload_offset_length;
	uint32_t	relative_offset;
	/**
	 * DWord 5 can either be the task retry identifier (HLM=0) or
	 * the remote N_Port ID (HLM=1), or if implementing the Skyhawk
	 * T10-PI workaround, the secondary xri tag
	 */
	union {
		uint32_t	sec_xri_tag: 16,
				: 16;
		uint32_t	dword;
	} dword5;
	uint32_t	xri_tag: 16,
			context_tag: 16;
	uint32_t dif: 2,
		 ct: 2,
		 bs: 3,
		 : 1,
		 command: 8,
		 class: 3,
			 ar: 1,
			 pu: 2,
			 conf: 1,
			 lnk: 1,
			 timer: 8;
	uint32_t	abort_tag;
	uint32_t	request_tag: 16,
			remote_xid: 16;

	uint32_t	ebde_cnt: 4,
			nvme: 1,
			appid: 1,
			oas: 1,
			len_loc: 2,
			qosd: 1,
			wchn: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			sr: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t	cmd_type: 4,
			irsp: 1,
			pbde: 1,
			: 1,
			wqec: 1,
			irsplen: 4,
			: 4,
			cq_id: 16;

	uint32_t	fcp_data_receive_length;
	bcm_bde_t	first_data_bde; /* reserved if performance hints disabled */
} bcm_fcp_treceive64_wqe_t;

typedef struct bcm_fcp_trsp64_wqe {
	bcm_bde_t	bde;
	uint32_t	fcp_response_length;
	uint32_t	rsvd4;
	/**
	 * DWord 5 can either be the task retry identifier (HLM=0) or
	 * the remote N_Port ID (HLM=1)
	 */
	uint32_t	dword5;
	uint32_t	xri_tag: 16,
			rpi: 16;
	uint32_t	: 2,
			ct: 2,
			dnrx: 1,
			: 3,
			command: 8,
			class: 3,
				ag: 1,
				pu: 2,
				conf: 1,
				lnk: 1,
				timer: 8;
	uint32_t	abort_tag;
	uint32_t	request_tag: 16,
			remote_xid: 16;
	uint32_t	ebde_cnt: 4,
			nvme: 1,
			appid: 1,
			oas: 1,
			len_loc: 2,
			qosd: 1,
			wchn: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			sr: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t	cmd_type: 4,
			irsp: 1,
			: 2,
			wqec: 1,
			irsplen: 4,
			: 4,
			cq_id: 16;
	uint32_t	rsvd12;
	uint32_t	rsvd13;
	uint32_t	rsvd14;
	uint32_t	rsvd15;
	uint32_t	inline_rsp;
	uint32_t	rsvdN[15];
} bcm_fcp_trsp64_wqe_t;

typedef struct bcm_fcp_tsend64_wqe {
	bcm_bde_t	bde;
	uint32_t	payload_offset_length;
	uint32_t	relative_offset;
	/**
	 * DWord 5 can either be the task retry identifier (HLM=0) or
	 * the remote N_Port ID (HLM=1)
	 */
	uint32_t	dword5;
	uint32_t	xri_tag: 16,
			rpi: 16;
	uint32_t	dif: 2,
			ct: 2,
			bs: 3,
			: 1,
			command: 8,
			class: 3,
				ar: 1,
				pu: 2,
				conf: 1,
				lnk: 1,
				timer: 8;
	uint32_t	abort_tag;
	uint32_t	request_tag: 16,
			remote_xid: 16;
	uint32_t	ebde_cnt: 4,
			nvme: 1,
			appid: 1,
			oas: 1,
			len_loc: 2,
			qosd: 1,
			wchn: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			sr: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t	cmd_type: 4,
			irsp: 1,
			: 1,
			sriu: 1,
			wqec: 1,
			irsplen: 4,
			: 4,
			cq_id: 16;
	uint32_t	fcp_data_transmit_length;
	bcm_bde_t	first_data_bde;	/* reserved if performance hints disabled */
} bcm_fcp_tsend64_wqe_t;

typedef struct bcm_xmit_sequence64_wqe_s {
	bcm_bde_t	bde;
	uint32_t	remote_n_port_id: 24,
			: 8;
	uint32_t	relative_offset;
	uint32_t        : 2,
			si: 1,
			ft: 1,
			: 2,
			xo: 1,
			ls: 1,
			df_ctl: 8,
			type: 8,
			r_ctl: 8;
	uint32_t        xri_tag: 16,
			context_tag: 16;
	uint32_t        dif: 2,
			ct: 2,
			bs: 3,
			: 1,
			command: 8,
			class: 3,
				: 1,
				  pu: 2,
				  : 2,
				    timer: 8;
	uint32_t        abort_tag;
	uint32_t        request_tag: 16,
			remote_xid: 16;
	uint32_t        ebde_cnt: 4,
			: 3,
			len_loc: 2,
			qosd: 1,
			: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			sr: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t        cmd_type: 4,
			: 3,
			wqec: 1,
			: 8,
			cq_id: 16;
	uint32_t        sequence_payload_len;
	uint32_t        rsvd13;
	uint32_t        rsvd14;
	uint32_t        rsvd15;
	uint32_t	rsvd16[16];
} bcm_xmit_sequence64_wqe_t;

typedef struct bcm_generic_wqe_s {
	uint32_t	rsvd[6];
	uint32_t        xri_tag: 16,
			context_tag: 16;
	uint32_t	rsvd1;
	uint32_t	abort_tag;
	uint32_t        request_tag: 16,
			: 16;
	uint32_t        ebde_cnt: 4,
			: 3,
			len_loc: 2,
			qosd: 1,
			: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			sr: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t        cmd_type: 4,
			: 3,
			wqec: 1,
			: 8,
			cq_id: 16;
	uint32_t	rsvd2[20];
} bcm_generic_wqe_t;


typedef struct bcm_abort_wqe_s {
	uint32_t	rsvd0;
	uint32_t	rsvd1;
	uint32_t	ext_t_tag;
	uint32_t	ia: 1,
			ir: 1,
			: 6,
			criteria: 8,
			: 16;
	uint32_t	ext_t_mask;
	uint32_t	t_mask;
	uint32_t	xri_tag: 16,
			context_tag: 16;
	uint32_t	: 2,
			ct: 2,
			: 4,
			command: 8,
			class: 3,
				: 1,
				  pu: 2,
				  : 2,
				    timer: 8;
	uint32_t	t_tag;
	uint32_t	request_tag: 16,
			: 16;
	uint32_t	ebde_cnt: 4,
			: 3,
			len_loc: 2,
			qosd: 1,
			: 1,
			xbl: 1,
			: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t	cmd_type: 4,
			: 3,
			wqec: 1,
			: 8,
			cq_id: 16;
} bcm_abort_wqe_t;

typedef struct bcm_xmit_bls_rsp_wqe_s {
	uint32_t	payload_word0;
	uint32_t	rx_id: 16,
			ox_id: 16;
	uint32_t	high_seq_cnt: 16,
			low_seq_cnt: 16;
	uint32_t	rsvd3;
	uint32_t	local_n_port_id: 24,
			: 8;
	uint32_t	remote_id: 24,
			: 6,
			ar: 1,
			xo: 1;
	uint32_t	xri_tag: 16,
			context_tag: 16;
	uint32_t	: 2,
			ct: 2,
			: 4,
			command: 8,
			class: 3,
				: 1,
				  pu: 2,
				  : 2,
				    timer: 8;
	uint32_t	abort_tag;
	uint32_t	request_tag: 16,
			: 16;
	uint32_t	ebde_cnt: 4,
			: 3,
			len_loc: 2,
			qosd: 1,
			: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t	cmd_type: 4,
			: 3,
			wqec: 1,
			: 8,
			cq_id: 16;
	uint32_t	temporary_rpi: 16,
			: 16;
	uint32_t	rsvd13;
	uint32_t	rsvd14;
	uint32_t	rsvd15;
} bcm_xmit_bls_rsp_wqe_t;

typedef struct bcm_send_frame_wqe_s {
	bcm_bde_t	bde;
	uint32_t	frame_length;
	uint32_t	fc_header_0_1[2];
	uint32_t	xri_tag: 16,
			context_tag: 16;
	uint32_t	: 2,
			ct: 2,
			: 4,
			command: 8,
			class: 3,
				: 1,
				  pu: 2,
				  : 2,
				    timer: 8;
	uint32_t        abort_tag;
	uint32_t	request_tag: 16,
			eof: 8,
			sof: 8;
	uint32_t	ebde_cnt: 4,
			: 3,
			lenloc: 2,
			qosd: 1,
			wchn: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t	cmd_type: 4,
			: 3,
			wqec: 1,
			: 8,
			cq_id: 16;
	uint32_t	fc_header_2_5[4];
} bcm_send_frame_wqe_t;

typedef struct bcm_gen_request64_wqe_s {
	bcm_bde_t	bde;
	uint32_t	request_payload_length;
	uint32_t	relative_offset;
	uint32_t	: 8,
			df_ctl: 8,
			type: 8,
			r_ctl: 8;
	uint32_t	xri_tag: 16,
			context_tag: 16;
	uint32_t	: 2,
			ct: 2,
			: 4,
			command: 8,
			class: 3,
				: 1,
				  pu: 2,
				  : 2,
				    timer: 8;
	uint32_t	abort_tag;
	uint32_t	request_tag: 16,
			: 16;
	uint32_t	ebde_cnt: 4,
			: 3,
			len_loc: 2,
			qosd: 1,
			: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t	cmd_type: 4,
			: 3,
			wqec: 1,
			: 8,
			cq_id: 16;
	uint32_t	remote_n_port_id: 24,
			: 8;
	uint32_t	rsvd13;
	uint32_t	rsvd14;
	uint32_t	max_response_payload_length;
} bcm_gen_request64_wqe_t;

typedef struct bcm_marker_wqe_s {
	uint32_t	rsvd0[3];
	uint32_t	marker_catagery: 2,
			: 30;
	uint32_t	tag_lower;
	uint32_t	tag_higher;
	uint32_t	rsvd1;
	uint32_t        : 8,
			command: 8,
			: 16;
	uint32_t	rsvd2;
	uint32_t	: 16,
			rq_id: 16;
	uint32_t	ebde_cnt: 4,
			: 3,
			len_loc: 2,
			qosd: 1,
			rsvd3: 22;
	uint32_t	cmd_type: 4,
			: 3,
			wqec: 1,
			: 8,
			cq_id: 16;
	uint32_t	rsvd4[4];
} bcm_marker_wqe_t;

#endif
