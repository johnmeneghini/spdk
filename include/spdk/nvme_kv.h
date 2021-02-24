/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2018-2019 NetApp.  All Rights Reserved.
 *   The term "NetApp" refers to NetApp Inc. and/or its subsidiaries.
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
 * \file
 * NVMe driver public API extension for Zoned Namespace Command Set
 */

#ifndef SPDK_NVME_KV_H
#define SPDK_NVME_KV_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/nvme.h"

#define KV_MAX_KEY_SIZE (16)
#define KV_MIN_KEY_SIZE (4)
#define KV_MAX_VALUE_SIZE (1<<21)
#define KV_KEY_STRING_LEN (37)

/**
 * KV command status codes
 */
enum spdk_nvme_kv_command_status_code {

	SPDK_NVME_SC_KV_INVALID_VALUE_SIZE              = 0x85,
	SPDK_NVME_SC_KV_INVALID_KEY_SIZE                = 0x86,
	SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST              = 0x87,
	SPDK_NVME_SC_KV_UNRECOVERED_ERROR               = 0x88,
	SPDK_NVME_SC_KV_KEY_EXISTS                      = 0x89,

};

/**
 * NVM command set opcodes
 */
enum spdk_nvme_kv_opcode {
	/* Key Value Command Set Commands */
	SPDK_NVME_OPC_KV_STORE				= 0x01,
	SPDK_NVME_OPC_KV_RETRIEVE			= 0x02,
	SPDK_NVME_OPC_KV_DELETE				= 0x10,
	SPDK_NVME_OPC_KV_EXIST				= 0x14,
	SPDK_NVME_OPC_KV_LIST				= 0x06,
};

enum spdk_nvme_kv_feat {
	SPDK_NVME_FEAT_KEY_VALUE_CONFIG		= 0x20,
};

/**
 * Data used by Set Features/Get Features \ref SPDK_NVME_FEAT_KEY_VALUE_CONFIG
 */
union spdk_nvme_feat_key_value_config {
	uint32_t raw;
	struct {
		/** Non-Operational Power State Permissive Mode Enable */
		uint32_t ednek : 1;
		uint32_t reserved : 31;
	} bits;
};

typedef __uint128_t spdk_nvme_kv_key_t;
SPDK_STATIC_ASSERT(sizeof(spdk_nvme_kv_key_t) == KV_MAX_KEY_SIZE, "Incorrect key size");


union spdk_nvme_kv_cmd_cdw10 {
	uint32_t raw;
	struct {
		/* Host Buffer Size */
		uint32_t hbs;
	} kv_list;

	struct {
		/* Host Buffer Size */
		uint32_t hbs;
	} kv_retrieve;

	struct {
		/* Value size */
		uint32_t vs;
	} kv_store;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_kv_cmd_cdw10) == 4, "Incorrect size");

union spdk_nvme_kv_cmd_cdw11 {
	uint32_t raw;

	struct {
		/* Key Length */
		uint8_t kl;
		uint8_t reserved[3];
	} kv_del;

	struct {
		/* Key Length */
		uint8_t kl;
		uint8_t reserved[3];
	} kv_list;

	struct {
		/* Key Length */
		uint8_t kl;
		struct {
			/* Return uncompressed data */
			uint8_t raw_data : 1;
			uint8_t reserved    : 7;
		} ro;
		uint8_t reserved[2];
	} kv_retrieve;

	struct {
		/* Key Length */
		uint8_t kl;
		uint8_t reserved[3];
	} kv_exist;

	struct {
		/* Key Length */
		uint8_t kl;
		struct {
			/** controller shall not store the KV value if the KV key does not exists. */
			uint8_t overwrite_only : 1;

			/** controller shall not store the KV value if the KV key exists. */
			uint8_t no_overwrite : 1;

			/** controller shall not compress the KV value. */
			uint8_t no_compression  : 1;

			uint8_t reserved  : 5;
		} so;
		uint8_t reserved[2];
	} kv_store;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_kv_cmd_cdw11) == 4, "Incorrect size");

struct spdk_nvme_kv_cmd {
	/* dword 0 */
	uint16_t opc	:  8;	/* opcode */
	uint16_t fuse	:  2;	/* fused operation */
	uint16_t rsvd1	:  4;
	uint16_t psdt	:  2;
	uint16_t cid;		/* command identifier */

	/* dword 1 */
	uint32_t nsid;		/* namespace identifier */

	/* dword 2-3 */
	uint32_t kvkey0;	/* KV KEY [bytes 3:0] */
	uint32_t kvkey1;	/* KV KEY [bytes 7:4] */

	/* dword 4-5 */
	uint64_t mptr;		/* metadata pointer */

	/* dword 6-9: data pointer */
	union {
		struct {
			uint64_t prp1;		/* prp entry 1 */
			uint64_t prp2;		/* prp entry 2 */
		} prp;

		struct spdk_nvme_sgl_descriptor sgl1;
	} dptr;

	/* command-specific */
	union {
		uint32_t cdw10;
		union spdk_nvme_kv_cmd_cdw10 cdw10_bits;
	};
	/* command-specific */
	union {
		uint32_t cdw11;
		union spdk_nvme_kv_cmd_cdw11 cdw11_bits;
	};

	/* dword 12-15 */
	uint32_t cdw12;		/* command-specific */
	uint32_t cdw13;		/* command-specific */
	uint32_t kvkey2;	/* KV KEY [bytes 11:8] */
	uint32_t kvkey3;	/* KV KEY [bytes 15:12] */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_kv_cmd) == 64, "Incorrect size");

struct spdk_nvme_kv_ns_format_data {
	uint16_t     kv_key_max_len;
	uint8_t      rsvd;
	struct {
		/* NS write protect flag */
		uint8_t rp : 2;
		uint8_t reserved : 6;
	} addl_format;
	uint32_t kv_value_max_len;
	uint32_t kv_max_num_keys;
	uint8_t  reserved[4];
};

struct spdk_nvme_kv_ns_data {
	/** namespace size */
	uint64_t		nsze;

	uint8_t			resv0[8];

	/** namespace utilization */
	uint64_t		nuse;

	/** namespace features */
	struct {
		/** thin provisioning */
		uint8_t		thin_prov : 1;

		/** NAWUN, NAWUPF, and NACWU are defined for this namespace */
		uint8_t		ns_atomic_write_unit : 1;

		/** Supports Deallocated or Unwritten LBA error for this namespace */
		uint8_t		dealloc_or_unwritten_error : 1;

		/** Non-zero NGUID and EUI64 for namespace are never reused */
		uint8_t		guid_never_reused : 1;

		uint8_t		reserved1 : 4;
	} nsfeat;

	/** number of kv formats */
	uint8_t			nkvf;

	/** namespace multi-path I/O and namespace sharing capabilities */
	struct {
		uint8_t		can_share : 1;
		uint8_t		reserved : 7;
	} nmic;

	/** reservation capabilities */
	union {
		struct {
			/** supports persist through power loss */
			uint8_t		persist : 1;

			/** supports write exclusive */
			uint8_t		write_exclusive : 1;

			/** supports exclusive access */
			uint8_t		exclusive_access : 1;

			/** supports write exclusive - registrants only */
			uint8_t		write_exclusive_reg_only : 1;

			/** supports exclusive access - registrants only */
			uint8_t		exclusive_access_reg_only : 1;

			/** supports write exclusive - all registrants */
			uint8_t		write_exclusive_all_reg : 1;

			/** supports exclusive access - all registrants */
			uint8_t		exclusive_access_all_reg : 1;

			/** supports ignore existing key */
			uint8_t		ignore_existing_key : 1;
		} rescap;
		uint8_t		raw;
	} nsrescap;
	/** format progress indicator */
	struct {
		uint8_t		percentage_remaining : 7;
		uint8_t		fpi_supported : 1;
	} fpi;

	uint8_t			resv1[3];

	/** Namespace Optimal Value Granularity  */
	uint32_t		novg;

	/** ANA group identifier */
	uint32_t		anagrpid;

	uint8_t			resv2[3];

	/** namespace attributes */
	uint8_t			nsattr;

	/** NVM Set Identifier  */
	uint16_t			nvmsetid;

	/** Endurance Group Identifier   */
	uint16_t			endgid;

	/** namespace globally unique identifier */
	uint8_t			nguid[16];

	/** IEEE extended unique identifier */
	uint64_t		eui64;

	/** KV format support */
	struct spdk_nvme_kv_ns_format_data kvf[16];

	uint8_t			reserved6[3512];

	uint8_t			vendor_specific[256];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_kv_ns_data) == 4096, "Incorrect size");

struct spdk_nvme_kv_ns_list_key_data {
	uint16_t kl; /** Key length */
	spdk_nvme_kv_key_t key;
	uint8_t pad[(sizeof(uint16_t) + sizeof(spdk_nvme_kv_key_t)) % 4];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_kv_ns_list_key_data) % 4 == 0, "Incorrect size");

struct spdk_nvme_kv_ns_list_data {
	uint32_t     nrk; /** Number of returned keys */
	struct spdk_nvme_kv_ns_list_key_data keys[];
};

void
spdk_nvme_kv_cmd_set_key(struct spdk_nvme_kv_cmd *cmd, spdk_nvme_kv_key_t key);

spdk_nvme_kv_key_t
spdk_nvme_kv_cmd_get_key(struct spdk_nvme_kv_cmd *cmd);

const struct spdk_nvme_kv_ns_data *
spdk_nvme_kv_ns_get_data(struct spdk_nvme_ns *ns);

int
spdk_nvme_kv_cmd_store(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       spdk_nvme_kv_key_t key, void *buffer, uint32_t buffer_length, uint32_t offset,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t option);

int
spdk_nvme_kv_cmd_retrieve(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			  spdk_nvme_kv_key_t key, void *buffer, uint32_t buffer_length, uint32_t offset,
			  spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t option);

int
spdk_nvme_kv_cmd_delete(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			spdk_nvme_kv_key_t key, spdk_nvme_cmd_cb cb_fn, void *cb_arg);

int
spdk_nvme_kv_cmd_exist(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       spdk_nvme_kv_key_t key, spdk_nvme_cmd_cb cb_fn, void *cb_arg);


/**
 * Convert Key in spdk_uuid into lowercase textual format.
 *
 * \param uuid_str User-provided string buffer to write the textual format into.
 * \param uuid_str_size Size of uuid_str buffer. Must be at least SPDK_UUID_STRING_LEN.
 * \param uuid UUID to convert to textual format.
 *
 * \return 0 on success, or negative errno on failure.
 */
int spdk_kv_key_fmt_lower(char *uuid_str, size_t uuid_str_size, const spdk_nvme_kv_key_t *key);
int spdk_kv_cmd_fmt_lower(char *uuid_str, size_t uuid_str_size,
			  const struct spdk_nvme_kv_cmd *kv_cmd);


#ifdef __cplusplus
}
#endif

#endif
