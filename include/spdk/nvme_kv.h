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
#include "spdk/nvme.h"

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

/**
 * Get the Key Value Command Set Specific Identify Namespace data
 * as defined by the NVMe Key Value Command Set Specification.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace.
 *
 * \return a pointer to the namespace data, or NULL if the namespace is not
 * a KV Namespace.
 */
const struct spdk_nvme_kv_ns_data *spdk_nvme_kv_ns_get_data(struct spdk_nvme_ns *ns);

/**
 * \brief Submits a KV Store I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV Store I/O
 * \param qpair I/O queue pair to submit the request
 * \param key Pointer to the key value to store value at
 * \param buffer virtual address pointer to the value
 * \param buffer_length length (in bytes) of the value
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param option option to pass to NVMe command
 *          default = 0, no overwrite = 1, overwrite only = 2, compression = 4
 * \return 0 if successfully submitted, -ENOMEM if an nvme_request
 *           structure cannot be allocated for the I/O request, SPDK_NVME_SC_KV_INVALID_KEY_SIZE
 *           or SPDK_NVME_SC_KV_INVALID_VALUE_SIZE if key_length or buffer_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int spdk_nvme_kv_cmd_store(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   struct spdk_nvme_kv_key_t *key, void *buffer, uint32_t buffer_length,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t option);

/**
 * \brief Submits a KV Retrieve I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV Retrieve I/O
 * \param qpair I/O queue pair to submit the request
 * \param key Pointer to the key value to retrieve the value of
 * \param buffer virtual address pointer to the value
 * \param buffer_length length (in bytes) of the value
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param option option to pass to NVMe command
 *     default = 0, decompression = 1
 * \return 0 if successfully submitted, -ENOMEM if an nvme_request
 *           structure cannot be allocated for the I/O request, SPDK_NVME_SC_KV_INVALID_KEY_SIZE
 *           or SPDK_NVME_SC_KV_INVALID_VALUE_SIZE if key_length or buffer_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int spdk_nvme_kv_cmd_retrieve(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      struct spdk_nvme_kv_key_t *key, void *buffer, uint32_t buffer_length,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t option);

/**
 * \brief Delete a value specified by key in the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV DeleteI/O
 * \param qpair I/O queue pair to submit the request
 * \param key Pointer to the key value to delete
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \return 0 if successfully submitted, -ENOMEM if an nvme_request
 *           structure cannot be allocated for the I/O request, SPDK_NVME_SC_KV_INVALID_KEY_SIZE
 *           if key_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int spdk_nvme_kv_cmd_delete(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			    struct spdk_nvme_kv_key_t *key, spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Tests if a key exists in specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV Exist I/O
 * \param qpair I/O queue pair to submit the request
 * \param key Pointer to the key value to test
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \return 0 if successfully submitted, -ENOMEM if an nvme_request
 *           structure cannot be allocated for the I/O request, SPDK_NVME_SC_KV_INVALID_KEY_SIZE
 *           if key_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int spdk_nvme_kv_cmd_exist(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   struct spdk_nvme_kv_key_t *key, spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Lists keys starting at the specified key the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV Retrieve I/O
 * \param qpair I/O queue pair to submit the request
 * \param key Pointer to the key value to begin listing keys
 * \param buffer virtual address pointer to the value
 * \param buffer_length length (in bytes) of the value
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \return 0 if successfully submitted, -ENOMEM if an nvme_request
 *           structure cannot be allocated for the I/O request, SPDK_NVME_SC_KV_INVALID_KEY_SIZE
 *           or SPDK_NVME_SC_KV_INVALID_VALUE_SIZE if key_length or buffer_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int spdk_nvme_kv_cmd_list(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			  struct spdk_nvme_kv_key_t *key, void *buffer, uint32_t buffer_length,
			  spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Get the maximum key length for KV namespace.
 *
 * \param ns Opaque handle to NVMe namespace.
 *
 * \return Maximum key length the naespace supports.
 */
uint32_t spdk_nvme_kv_get_max_key_len(struct spdk_nvme_ns *ns);

/**
 * Get the maximum value size for KV namespace.
 *
 * \param ns Opaque handle to NVMe namespace.
 *
 * \return Maximum value size the naespace supports.
 */
uint32_t spdk_nvme_kv_get_max_value_len(struct spdk_nvme_ns *ns);

/**
 * Get the maximum number of keys a KV namespace can have (0 if unlimited).
 *
 * \param ns Opaque handle to NVMe namespace.
 *
 * \return Maximum value size the naespace supports.
 */
uint32_t spdk_nvme_kv_get_max_num_keys(struct spdk_nvme_ns *ns);


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
