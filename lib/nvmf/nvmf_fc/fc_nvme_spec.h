/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2018 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
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

#ifndef __FC_NVME_H__
#define __FC_NVME_H__

#include "spdk/stdinc.h"

/*
 * FC-NVMe Spec. Definitions
 */

#define NVME_FC_R_CTL_CMD_REQ            0x06
#define NVME_FC_R_CTL_DATA_OUT           0x01
#define NVME_FC_R_CTL_STATUS		 0x07
#define NVME_FC_R_CTL_ERSP_STATUS	 0x08
#define NVME_FC_R_CTL_CONFIRM            0x03
#define NVME_FC_R_CTL_LS_REQUEST         0x32
#define NVME_FC_R_CTL_LS_RESPONSE        0x33
#define NVME_FC_R_CTL_BA_ABTS            0x81

#define NVME_FC_F_CTL_END_SEQ            0x080000
#define NVME_FC_F_CTL_SEQ_INIT           0x010000

/* END_SEQ | LAST_SEQ | Exchange Responder | SEQ init */
#define NVME_FC_F_CTL_RSP		 0x990000

#define NVME_FC_TYPE_BLS                 0x0
#define NVME_FC_TYPE_FC_EXCHANGE         0x08
#define NVME_FC_TYPE_NVMF_DATA           0x28

#define NVME_CMND_IU_FC_ID	    	     0x28
#define NVME_CMND_IU_SCSI_ID             0xFD

#define NVME_CMND_IU_NODATA              0x00
#define NVME_CMND_IU_READ                0x10
#define NVME_CMND_IU_WRITE               0x01

#define NVME_FC_GOOD_RSP_LEN 		 12
#define SPDK_NVMF_FC_HOST_ID_LEN         16
#define SPDK_NVMF_FC_NQN_MAX_LEN         256

/*
 * NVMe over FC CMD IU
 */
struct spdk_nvmf_fc_cmnd_iu {
	uint32_t scsi_id: 8, fc_id: 8, cmnd_iu_len: 16;
	uint32_t rsvd0: 24, flags: 8;
	uint64_t conn_id;
	uint32_t cmnd_seq_num;
	uint32_t data_len;
	union nvmf_h2c_msg cmd;
	uint32_t rsvd1[2];
};

/*
 * NVMe over Extended Response IU
 */
struct spdk_nvmf_fc_ersp_iu {
	uint32_t status_code: 8, rsvd0: 8, ersp_len: 16;
	uint32_t response_seq_no;
	uint32_t transferred_data_len;
	uint32_t rsvd1;
	union nvmf_c2h_msg	rsp;
};

/*
 * Transfer ready IU
 */
struct spdk_nvmf_fc_xfer_rdy_iu {
	uint32_t relative_offset;
	uint32_t burst_len;
	uint32_t rsvd;
};

/*
 * FC-NVME Link Services data definitions
 */

/*
 * LS Reject reason_codes
 */
enum fcnvme_ls_rjt_reason {
	FCNVME_RJT_RC_NONE        = 0,     /* no reason - not to be sent */
	FCNVME_RJT_RC_INVAL       = 0x01,  /* invalid NVMe_LS command code */
	FCNVME_RJT_RC_LOGIC       = 0x03,  /* logical error */
	FCNVME_RJT_RC_UNAB        = 0x09,  /* unable to perform request */
	FCNVME_RJT_RC_UNSUP       = 0x0b,  /* command not supported */
	FCNVME_RJT_RC_INPROG      = 0x0e,  /* command already in progress */
	FCNVME_RJT_RC_INV_ASSOC   = 0x40,  /* invalid Association ID */
	FCNVME_RJT_RC_INV_CONN    = 0x41,  /* invalid Connection ID */
	FCNVME_RJT_RC_INV_PARAM   = 0x42,  /* invalid parameters */
	FCNVME_RJT_RC_INSUFF_RES  = 0x43,  /* insufficient resources */
	FCNVME_RJT_RC_INV_HOST    = 0x44,  /* invalid or rejected host */
	FCNVME_RJT_RC_VENDOR      = 0xff,  /* vendor specific error */
};

/*
 * LS Reject reason_explanation codes
 */
enum fcnvme_ls_rjt_explan {
	FCNVME_RJT_EXP_NONE	   = 0x00,  /* No additional explanation */
	FCNVME_RJT_EXP_OXID_RXID   = 0x17,  /* invalid OX_ID-RX_ID combo */
	FCNVME_RJT_EXP_UNAB_DATA   = 0x2a,  /* unable to supply data */
	FCNVME_RJT_EXP_INV_LEN     = 0x2d,  /* invalid payload length */
	FCNVME_RJT_EXP_INV_ESRP    = 0x40,  /* invalid ESRP ratio */
	FCNVME_RJT_EXP_INV_CTL_ID  = 0x41,  /* invalid controller ID */
	FCNVME_RJT_EXP_INV_Q_ID    = 0x42,  /* invalid queue ID */
	FCNVME_RJT_EXP_SQ_SIZE     = 0x43,  /* invalid submission queue size */
	FCNVME_RJT_EXP_INV_HOST_ID = 0x44,  /* invalid or rejected host ID */
	FCNVME_RJT_EXP_INV_HOSTNQN = 0x45,  /* invalid or rejected host NQN */
	FCNVME_RJT_EXP_INV_SUBNQN  = 0x46,  /* invalid or rejected subsys nqn */
};

/* for this implementation, assume small single frame rqst/rsp */
#define NVME_FC_MAX_LS_BUFFER_SIZE		2048

/* FCNVME_LS_CREATE_ASSOCIATION */
#define FCNVME_ASSOC_HOSTID_LEN     SPDK_NVMF_FC_HOST_ID_LEN
#define FCNVME_ASSOC_HOSTNQN_LEN    SPDK_NVMF_FC_NQN_MAX_LEN
#define FCNVME_ASSOC_SUBNQN_LEN     SPDK_NVMF_FC_NQN_MAX_LEN

/* for now, we don't care about counting pad at end of cr assoc cmd desc */
#define LS_CREATE_ASSOC_MIN_LEN  592
#define LS_CREATE_ASSOC_DESC_LIST_MIN_LEN 584
#define LS_CREATE_ASSOC_CMD_DESC_MIN_LEN 576

/*
 * Request payload word 0
 */
struct nvmf_fc_ls_rqst_w0 {
	uint8_t	ls_cmd;			/* FCNVME_LS_xxx */
	uint8_t zeros[3];
};

/*
 * LS request information descriptor
 */
struct nvmf_fc_lsdesc_rqst {
	__be32 desc_tag;		/* FCNVME_LSDESC_xxx */
	__be32 desc_len;
	struct nvmf_fc_ls_rqst_w0 w0;
	__be32 rsvd12;
};

/*
 * LS accept header
 */
struct nvmf_fc_ls_acc_hdr {
	struct nvmf_fc_ls_rqst_w0 w0;
	__be32 desc_list_len;
	struct nvmf_fc_lsdesc_rqst rqst;
	/* Followed by cmd-specific ACC descriptors, see next definitions */
};

/* FC-NVME Link Services */
enum {
	FCNVME_LS_RSVD = 0,
	FCNVME_LS_RJT = 1,
	FCNVME_LS_ACC = 2,
	FCNVME_LS_CREATE_ASSOCIATION = 3,
	FCNVME_LS_CREATE_CONNECTION	= 4,
	FCNVME_LS_DISCONNECT = 5,
};

/* FC-NVME Link Service Descriptors */
enum {
	FCNVME_LSDESC_RSVD = 0x0,
	FCNVME_LSDESC_RQST = 0x1,
	FCNVME_LSDESC_RJT = 0x2,
	FCNVME_LSDESC_CREATE_ASSOC_CMD = 0x3,
	FCNVME_LSDESC_CREATE_CONN_CMD = 0x4,
	FCNVME_LSDESC_DISCONN_CMD = 0x5,
	FCNVME_LSDESC_CONN_ID = 0x6,
	FCNVME_LSDESC_ASSOC_ID = 0x7,
};

/* Disconnect Scope Values */
enum {
	FCNVME_DISCONN_ASSOCIATION = 0,
	FCNVME_DISCONN_CONNECTION = 1,
};

/*
 * LS descriptor connection id
 */
struct nvmf_fc_lsdesc_conn_id {
	__be32 desc_tag;
	__be32 desc_len;
	__be64 connection_id;
};

/*
 * LS decriptor association id
 */
struct nvmf_fc_lsdesc_assoc_id {
	__be32 desc_tag;
	__be32 desc_len;
	__be64 association_id;
};

/*
 * LS Create Association descriptor
 */
struct nvmf_fc_lsdesc_cr_assoc_cmd {
	__be32  desc_tag;
	__be32  desc_len;
	__be16  ersp_ratio;
	__be16  rsvd10;
	__be32  rsvd12[9];
	__be16  cntlid;
	__be16  sqsize;
	__be32  rsvd52;
	uint8_t hostid[FCNVME_ASSOC_HOSTID_LEN];
	uint8_t hostnqn[FCNVME_ASSOC_HOSTNQN_LEN];
	uint8_t subnqn[FCNVME_ASSOC_SUBNQN_LEN];
	uint8_t rsvd584[432];
};

/*
 * LS Create Association reqeust payload
 */
struct nvmf_fc_ls_cr_assoc_rqst {
	struct nvmf_fc_ls_rqst_w0 w0;
	__be32 desc_list_len;
	struct nvmf_fc_lsdesc_cr_assoc_cmd assoc_cmd;
};

/*
 * LS Create Association accept payload
 */
struct nvmf_fc_ls_cr_assoc_acc {
	struct nvmf_fc_ls_acc_hdr hdr;
	struct nvmf_fc_lsdesc_assoc_id assoc_id;
	struct nvmf_fc_lsdesc_conn_id conn_id;
};

/*
 * LS Create IO Connection descriptor
 */
struct nvmf_fc_lsdesc_cr_conn_cmd {
	__be32 desc_tag;
	__be32 desc_len;
	__be16 ersp_ratio;
	__be16 rsvd10;
	__be32 rsvd12[9];
	__be16 qid;
	__be16 sqsize;
	__be32 rsvd52;
};

/*
 * LS Create IO Connection payload
 */
struct nvmf_fc_ls_cr_conn_rqst {
	struct nvmf_fc_ls_rqst_w0 w0;
	__be32 desc_list_len;
	struct nvmf_fc_lsdesc_assoc_id assoc_id;
	struct nvmf_fc_lsdesc_cr_conn_cmd connect_cmd;
};

/*
 * LS Create IO Connection accept payload
 */
struct nvmf_fc_ls_cr_conn_acc {
	struct nvmf_fc_ls_acc_hdr hdr;
	struct nvmf_fc_lsdesc_conn_id conn_id;
};

/*
 * LS Disconnect descriptor
 */
struct nvmf_fc_lsdesc_disconn_cmd {
	__be32 desc_tag;
	__be32 desc_len;
	__be32 rsvd8;
	__be32 rsvd12;
	__be32 rsvd16;
	__be32 rsvd20;
};

/*
 * LS Disconnect payload
 */
struct nvmf_fc_ls_disconnect_rqst {
	struct nvmf_fc_ls_rqst_w0 w0;
	__be32 desc_list_len;
	struct nvmf_fc_lsdesc_assoc_id assoc_id;
	struct nvmf_fc_lsdesc_disconn_cmd disconn_cmd;
};

/*
 * LS Disconnect accept payload
 */
struct nvmf_fc_ls_disconnect_acc {
	struct nvmf_fc_ls_acc_hdr hdr;
};

/*
 * LS Reject descriptor
 */
struct nvmf_fc_lsdesc_rjt {
	__be32 desc_tag;
	__be32 desc_len;
	uint8_t rsvd8;

	uint8_t reason_code;
	uint8_t reason_explanation;

	uint8_t vendor;
	__be32 	rsvd12;
};

/*
 * LS Reject payload
 */
struct nvmf_fc_ls_rjt {
	struct nvmf_fc_ls_rqst_w0 w0;
	__be32 desc_list_len;
	struct nvmf_fc_lsdesc_rqst rqst;
	struct nvmf_fc_lsdesc_rjt rjt;
};

#endif
