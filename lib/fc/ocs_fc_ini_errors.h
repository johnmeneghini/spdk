/*
 *  Copyright (c) 2016 Broadcom.  All Rights Reserved.
 *  The term "Broadcom" refers to Broadcom Limited and/or its subsidiaries.
 *
 *   BSD LICENSE
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

/**
 * @file
 * FC initiator error code definitions.
 */

#ifndef _OCS_FC_INI_ERRORS_H
#define _OCS_FC_INI_ERRORS_H 

#include "ocs_hal.h"

/* Errors returned by FC initiator functions */
#define SPDK_FC_INI_ERROR_BASE  100  /* base error code */
typedef enum _spdk_fc_ini_error_codes {
	SPDK_FC_INI_SUCCESS = 0,
	SPDK_FC_INI_ERROR_NOT_INIT = SPDK_FC_INI_ERROR_BASE,
	SPDK_FC_INI_ERROR_DEVICE_NOT_READY,
	SPDK_FC_INI_ERROR_XFER_LEN,
	SPDK_FC_INI_ERROR_GENCNT,
	SPDK_FC_INI_ERROR_TIMEOUT,
	SPDK_FC_INI_ERROR_INVALID,
	SPDK_FC_INI_ERROR_ILLEGAL_REQUEST,
	SPDK_FC_INI_ERROR_NO_MEM,
	SPDK_FC_INI_ERROR_GENERIC,
	SPDK_FC_INI_ABORTED,
	SPDK_FC_INI_ERROR_TMF,

	/* Mapping of HAL error codes */

	SPDK_FC_INI_ERROR_HAL_ERROR = OCS_HAL_RTN_ERROR,

} SPDK_FC_INI_ERROR_CODES;

#endif /* _OCS_FC_INI_ERRORS_H */
