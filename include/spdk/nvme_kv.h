/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2021 NetApp.  All Rights Reserved.
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

#include "spdk/nvme_kv_spec.h"

/** 0x11223344-11223344-11223344-11223344 + terminating NULL */
#define KV_KEY_STRING_LEN (38)


void spdk_nvme_kv_cmd_set_key(const struct spdk_nvme_kv_key_t *key, struct spdk_nvme_kv_cmd *cmd);

void spdk_nvme_kv_cmd_get_key(const struct spdk_nvme_kv_cmd *cmd, struct spdk_nvme_kv_key_t *key);

/**
 * Parse the string representation of a KV key.
 * This can take one of 2 forms:
 * - An ascii string of up to 16 bytes in length, i.e. "hellokitty"
 * - A series of hex digits preceded by '0x' and grouped in to 4 byte sequences
 *   separated by '-'.  Note that the final group need not be all 4 bytes, so for
 *   example, '0x0' is a 1 byte key '0', 0x00 is also a 1 byte key with 0, while
 *   0x000 is a 2 byte key with bytes 0 and 1 set to 0.  Note also that key length
 *   is a distinguishing characteristic, so that 0x00 and 0x0000 are distinct keys
 *
 * \param str The string to parse.
 * \param key The key structure to place the results.
 *
 * \return 0 if parsing was successful and key is filled out, or negated errno
 * values on failure.
 */
int spdk_kv_key_parse(const char *str, struct spdk_nvme_kv_key_t *key);

const struct spdk_nvme_kv_ns_data *spdk_nvme_kv_ns_get_data(struct spdk_nvme_ns *ns);

int spdk_nvme_kv_cmd_store(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   struct spdk_nvme_kv_key_t *key, void *buffer, uint32_t buffer_length, uint32_t offset,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t option);

int spdk_nvme_kv_cmd_retrieve(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      struct spdk_nvme_kv_key_t *key, void *buffer, uint32_t buffer_length, uint32_t offset,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t option);

int spdk_nvme_kv_cmd_delete(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			    struct spdk_nvme_kv_key_t *key, spdk_nvme_cmd_cb cb_fn, void *cb_arg);

int spdk_nvme_kv_cmd_exist(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   struct spdk_nvme_kv_key_t *key, spdk_nvme_cmd_cb cb_fn, void *cb_arg);

int spdk_nvme_kv_cmd_list(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			  struct spdk_nvme_kv_key_t *key, void *buffer, uint32_t buffer_length,
			  spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Convert Key to lowercase textual format.
 *
 * \param key_str User-provided string buffer to write the textual format into.
 * \param key_str_size Size of uuid_str buffer. Must be at least KV_KEY_STRING_LEN.
 * \param key_len Length of input key buffer.
 * \param key key buffer.
 *
 * \return 0 on success, or negative errno on failure.
 */
int spdk_kv_key_fmt_lower(char *key_str, size_t key_str_size, uint32_t key_len,
			  const uint8_t *key);

/**
 * Convert Key in kv_cmd to lowercase textual format.
 *
 * \param kv_cmd cmd to convert.
 * \param key_str User-provided string buffer to write the textual format into.
 * \param key_str_size Size of uuid_str buffer. Must be at least KV_KEY_STRING_LEN.
 *
 * \return 0 on success, or negative errno on failure.
 */
int spdk_kv_cmd_fmt_lower(const struct spdk_nvme_kv_cmd *kv_cmd, char *key_str,
			  size_t key_str_size);


#ifdef __cplusplus
}
#endif

#endif
