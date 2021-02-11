/**
 *   BSD LICENSE
 *
 *   Copyright (c) 2017 Samsung Electronics Co., Ltd.
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
 *     * Neither the name of Samsung Electronics Co., Ltd. nor the names of
 *       its contributors may be used to endorse or promote products derived
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

#ifndef _KVNVME_SPDK_H_
#define _KVNVME_SPDK_H_

#include "spdk/nvme.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KV_MAX_KEY_SIZE (255)
#define KV_MIN_KEY_SIZE (4)
#define KV_MAX_VALUE_SIZE (1<<21)
#define KV_MAX_EMBED_KEY_SIZE (16)

enum spdk_nvme_samsung_nvm_opcode {

	SPDK_NVME_OPC_KV_STORE      = 0x81,
	SPDK_NVME_OPC_KV_RETRIEVE   = 0x90,
	SPDK_NVME_OPC_KV_DELETE     = 0xA1,
	SPDK_NVME_OPC_KV_ITERATE_REQUEST	= 0xB1,
	SPDK_NVME_OPC_KV_ITERATE_READ		= 0xB2,
	SPDK_NVME_OPC_KV_APPEND     = 0x83,
	SPDK_NVME_OPC_KV_EXIST	= 0xB3,

};

/**
 * @brief options used for store operation
 */
enum spdk_kv_result {
	SPDK_KV_SUCCESS = 0,						/**<  successful */

	//0x00 ~ 0xFF for Device error
	SPDK_KV_ERR_INVALID_VALUE_SIZE = 0x01,                       /**<  invalid value length(size) */
	SPDK_KV_ERR_INVALID_VALUE_OFFSET = 0x02,                     /**<  invalid value offset */
	SPDK_KV_ERR_INVALID_KEY_SIZE = 0x03,                         /**<  invalid key length(size) */
	SPDK_KV_ERR_INVALID_OPTION = 0x04,                           /**<  invalid I/O option */
	SPDK_KV_ERR_INVALID_KEYSPACE_ID = 0x05,                      /**<  invalid keyspace ID (should be 0 or 1. 2018-08027) */
	//0x06 ~ 0x07 are reserved
	SPDK_KV_ERR_MISALIGNED_VALUE_SIZE = 0x08,                    /**<  misaligned value length(size) */
	SPDK_KV_ERR_MISALIGNED_VALUE_OFFSET = 0x09,                  /**<  misaligned value offset */
	SPDK_KV_ERR_MISALIGNED_KEY_SIZE = 0x0A,                      /**<  misaligned key length(size) */
	//0x0B ~ 0x0F are reserved
	SPDK_KV_ERR_NOT_EXIST_KEY = 0x10,                            /**<  not existing key (unmapped key) */
	SPDK_KV_ERR_UNRECOVERED_ERROR = 0x11,                        /**<  internal I/O error */
	SPDK_KV_ERR_CAPACITY_EXCEEDED = 0x12,                        /**<  capacity limit */
	//0x13 ~ 0x7F are reserved
	SPDK_KV_ERR_IDEMPOTENT_STORE_FAIL = 0x80,                    /**<  overwrite fail when given key is already written with IDEMPOTENT option */
	SPDK_KV_ERR_MAXIMUM_VALUE_SIZE_LIMIT_EXCEEDED = 0x81,        /**<  value of given key is already full(KV_MAX_TOTAL_VALUE_LEN) */

	SPDK_KV_ERR_ITERATE_FAIL_TO_PROCESS_REQUEST = 0x90,          /**<  fail to read/close handle with given handle id */
	SPDK_KV_ERR_ITERATE_NO_AVAILABLE_HANDLE = 0x91,              /**<  no more available handle */
	SPDK_KV_ERR_ITERATE_HANDLE_ALREADY_OPENED = 0x92,            /**<  fail to open iterator with given prefix/bitmask as it is already opened */
	SPDK_KV_ERR_ITERATE_READ_EOF = 0x93,                         /**<  end-of-file for iterate_read with given iterator */
	SPDK_KV_ERR_ITERATE_REQUEST_FAIL = 0x94,                     /**<  fail to process the iterate request due to FW internal status */
	SPDK_KV_ERR_ITERATE_TCG_LOCKED = 0x95,                       /**<  iterate TCG locked */
	SPDK_KV_ERR_ITERATE_ERROR = 0x96,                            /**<  an error while iterate, closing the iterate handle is recommended */

	//0x100 ~ 0x1FF for DD Error
	SPDK_KV_ERR_DD_NO_DEVICE = 0x100,
	SPDK_KV_ERR_DD_INVALID_PARAM = 0x101,
	SPDK_KV_ERR_DD_INVALID_QUEUE_TYPE = 0x102,
	SPDK_KV_ERR_DD_NO_AVAILABLE_RESOURCE = 0x103,
	SPDK_KV_ERR_DD_NO_AVAILABLE_QUEUE = 0x104,
	SPDK_KV_ERR_DD_UNSUPPORTED_CMD = 0x105,
	SPDK_KV_ERR_DD_ITERATE_COND_INVALID = 0x106,  /**<  iterator condition is not valid */


	//0x200 ~ 0x2FF for SDK Error
	SPDK_KV_ERR_SDK_OPEN = 0x200,                                /**<  device(sdk) open failed */
	SPDK_KV_ERR_SDK_CLOSE = 0x201,                               /**<  device(sdk) close failed */
	SPDK_KV_ERR_CACHE_NO_CACHED_KEY = 0x202,                     /**<  (kv cache) cache miss */
	SPDK_KV_ERR_CACHE_INVALID_PARAM = 0x203,                     /**<  (kv cache) invalid parameters */
	SPDK_KV_ERR_HEAP_ALLOC_FAILURE = 0x204,                      /**<  heap allocation fail for sdk operations */
	SPDK_KV_ERR_SLAB_ALLOC_FAILURE = 0x205,                      /**<  slab allocation fail for sdk operations */
	SPDK_KV_ERR_SDK_INVALID_PARAM = 0x206,                       /**<  invalid parameters for sdk operations */

	//0x300 ~ 0x3FF for uncertain error types
	SPDK_KV_WRN_MORE = 0x300,                                    /**<  more results are available(for iterate) */
	SPDK_KV_ERR_BUFFER = 0x301,                                  /**<  not enough buffer(for retrieve, exist, iterate) */
	SPDK_KV_ERR_DECOMPRESSION = 0x302,                           /**<  retrieving uncompressed value with KV_RETRIEVE_DECOMPRESSION option */
	SPDK_KV_ERR_IO = 0x303,					/**<  SDK operation error (remained type for compatibility) */
};

typedef void (*spdk_nvme_cmd_cb)(void *, const struct spdk_nvme_cpl *);
/**
 * \brief Submits a KV Store I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV Store I/O
 * \param qpair I/O queue pair to submit the request
 * \param ns_id namespace id of key
 * \param key virtual address pointer to the value
 * \param key_length length (in bytes) of the key
 * \param buffer virtual address pointer to the value
 * \param buffer_length length (in bytes) of the value
 * \param offset offset of value (in bytes)
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_NVME_IO_FLAGS_* entries
 *                      in spdk/nvme_spec.h, for this I/O.
 * \param option option to pass to NVMe command
 *          0 - Idempotent; 1 - Post; 2 - Append
 * \param is_store store or append
 *          1 = store, 0 = append
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *           structure cannot be allocated for the I/O request, EINVAL if
 *           key_length or buffer_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int
spdk_nvme_kv_cmd_store(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       uint32_t ns_id, void *key, uint32_t key_length,
		       void *buffer, uint32_t buffer_length,
		       uint32_t offset,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		       uint32_t io_flags,
		       uint8_t  option, uint8_t is_store);

/**
 * \brief Submits a KV Retrieve I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV Retrieve I/O
 * \param qpair I/O queue pair to submit the request
 * \param ns_id namespace id of key
 * \param key virtual address pointer to the value
 * \param key_length length (in bytes) of the key
 * \param buffer virtual address pointer to the value
 * \param buffer_length length (in bytes) of the value
 * \param offset offset of value (in bytes)
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_NVME_IO_FLAGS_* entries
 *                      in spdk/nvme_spec.h, for this I/O.
 * \param option option to pass to NVMe command
 *     No option supported for retrieve I/O yet.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *           structure cannot be allocated for the I/O request, EINVAL if
 *           key_length or buffer_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int
spdk_nvme_kv_cmd_retrieve(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			  uint32_t ns_id, void *key, uint32_t key_length,
			  void *buffer, uint32_t buffer_length,
			  uint32_t offset,
			  spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			  uint32_t io_flags, uint32_t option);

/**
 * \brief Submits a KV Delete I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV DeleteI/O
 * \param qpair I/O queue pair to submit the request
 * \param ns_id namespace id of key
 * \param key virtual address pointer to the value
 * \param key_length length (in bytes) of the key
 * \param buffer_length length (in bytes) of the value
 * \param offset offset of value (in bytes)
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_NVME_IO_FLAGS_* entries
 *                      in spdk/nvme_spec.h, for this I/O.
 * \param option option to pass to NVMe command
 *     No option supported for retrieve I/O yet.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *           structure cannot be allocated for the I/O request, EINVAL if
 *           key_length or buffer_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int
spdk_nvme_kv_cmd_delete(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			uint32_t ns_id, void *key, uint32_t key_length, uint32_t buffer_length, uint32_t offset,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			uint32_t io_flags, uint8_t  option);

/**
 * \brief Submits a KV Exist I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV Exist I/O
 * \param qpair I/O queue pair to submit the request
 * \param ns_id namespace id of key
 * \param key virtual address pointer to the value
 * \param key_length length (in bytes) of the key
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_NVME_IO_FLAGS_* entries
 *                      in spdk/nvme_spec.h, for this I/O.
 * \param option option to pass to NVMe command
 *       0 - Fixed size; 1 - Variable size
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *           structure cannot be allocated for the I/O request, EINVAL if
 *           key_length or buffer_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int
spdk_nvme_kv_cmd_exist(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       uint32_t ns_id, void *key, uint32_t key_length,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		       uint32_t io_flags, uint8_t  option);

/**
 * \brief Submits a KV Iterate Open Request to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV Iterate I/O
 * \param qpair I/O queue pair to submit the request
 * \param keyspace_id keyspace_id ( KV_KEYSPACE_IODATA = 0, KV_KEYSPACE_IODATA
 * \param bitmask Iterator bitmask (4 bytes)
 * \param prefix Iterator prefix (4 bytes)
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_NVME_IO_FLAGS_* entries
 *                      in spdk/nvme_spec.h, for this I/O.
 * \param option option to pass to NVMe command
 *       0 - Fixed size; 1 - Variable size
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *           structure cannot be allocated for the I/O request, EINVAL if
 *           key_length or buffer_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int
spdk_nvme_kv_cmd_iterate_open(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      uint8_t keyspace_id, uint32_t bitmask, uint32_t prefix,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			      uint32_t io_flags, uint8_t  option);

/**
 * \brief Submits a KV Iterate Read to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV Iterate I/O
 * \param qpair I/O queue pair to submit the request
 * \param iterator pointer to Iterator (3 bytes)
 * \param buffer virtual address pointer to the return buffer
 * \param buffer_length length (in bytes) of the return buffer
 * \param buffer_offset offset (in bytes) of the return buffer
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_NVME_IO_FLAGS_* entries
 *                      in spdk/nvme_spec.h, for this I/O.
 * \param option option to pass to NVMe command
 *       0 - Fixed size; 1 - Variable size
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *           structure cannot be allocated for the I/O request, EINVAL if
 *           key_length or buffer_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int
spdk_nvme_kv_cmd_iterate_read(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      uint8_t iterator, void *buffer, uint32_t buffer_length, uint32_t buffer_offset,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			      uint32_t io_flags, uint8_t  option);

/**
 * \brief Submits a KV Iterate I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV Iterate I/O
 * \param qpair I/O queue pair to submit the request
 * \param iterator pointer to Iterator (3 bytes)
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_NVME_IO_FLAGS_* entries
 *                      in spdk/nvme_spec.h, for this I/O.
 * \param option option to pass to NVMe command
 *       0 - Fixed size; 1 - Variable size
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *           structure cannot be allocated for the I/O request, EINVAL if
 *           key_length or buffer_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int
spdk_nvme_kv_cmd_iterate_close(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			       uint8_t iterator,
			       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			       uint32_t io_flags, uint8_t  option);

/**
 * \brief Submits a KV Iterate I/O to the specified NVMe namespace.
 * \param ns NVMe namespace to submit the KV Iterate I/O
 *
 * \return  uint32_t the maximum io queue size of the nvme namespace
 *
 */
uint16_t spdk_nvme_ns_get_max_io_queue_size(struct spdk_nvme_ns *ns);

#ifdef __cplusplus
}
#endif

#endif
