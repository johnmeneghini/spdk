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

/** \file
 * General utility functions
 */

#ifndef SPDK_UTIL_H
#define SPDK_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/env.h"
#include "spdk/nvmf.h"

#define spdk_min(a,b) (((a)<(b))?(a):(b))
#define spdk_max(a,b) (((a)>(b))?(a):(b))

#define SPDK_COUNTOF(arr) (sizeof(arr) / sizeof((arr)[0]))

static inline uint32_t
spdk_u32log2(uint32_t x)
{
	if (x == 0) {
		/* log(0) is undefined */
		return 0;
	}
	return 31u - __builtin_clz(x);
}

static inline uint32_t
spdk_align32pow2(uint32_t x)
{
	return 1u << (1 + spdk_u32log2(x - 1));
}

/*
 * uuid1 == uuid2 return true
 */
static inline bool
spdk_nvmf_uuids_equal(struct spdk_uuid *uuid1, struct spdk_uuid *uuid2)
{
	uint64_t *val1 = (uint64_t *)uuid1;
	uint64_t *val2 = (uint64_t *)uuid2;

	/*
	 * The uuid is a 16-byte array.  However it is quicker for us to
	 * treat it as 2 uint64_t's.
	 */
	return ((val1[0] == val2[0]) && (val1[1] == val2[1]));
}

static inline const char *
spdk_print_fabric_cmd(uint8_t fctype)
{

	switch (fctype) {
	case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET: /* 0x00 */
		return "PROPERTY_SET";
	case SPDK_NVMF_FABRIC_COMMAND_CONNECT:      /* 0x01 */
		return "CONNECT";
	case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET: /* 0x04 */
		return "PROPERTY_GET";
	case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND: /* 0x05 */
		return "AUTHENTICATION_SEND";
	case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV: /* 0x06 */
		return "AUTHENTICATION_RECV";
	case SPDK_NVMF_FABRIC_COMMAND_START_VENDOR_SPECIFIC:  /* 0xC0 */
		return "VENDOR_SPECIFIC";
	default:
		return "Unknown";
	}
}

#ifdef __cplusplus
}
#endif

#endif
