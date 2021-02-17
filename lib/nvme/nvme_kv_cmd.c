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

#include "nvme_internal.h"

static inline void _nvme_kv_cmd_set_key(struct spdk_nvme_cmd *cmd, uint128_t key)
{
	cmd->kvkey0 = *((uint32_t *)&key + 0);
	cmd->kvkey1 = *((uint32_t *)&key + 1);
	cmd->kvkey2 = *((uint32_t *)&key + 2);
	cmd->kvkey3 = *((uint32_t *)&key + 3);
}

static inline uint128_t _nvme_kv_cmd_get_key(struct spdk_nvme_cmd *cmd)
{
	uint128_t key = 0;
	*((uint32_t *)&key + 0) = cmd->kvkey0;
	*((uint32_t *)&key + 1) = cmd->kvkey1;
	*((uint32_t *)&key + 2) = cmd->kvkey2;
	*((uint32_t *)&key + 3) = cmd->kvkey3;
	return key;
}


/*
 * Setup store request
 */
static void
_nvme_kv_cmd_setup_store_request(struct spdk_nvme_ns *ns, struct nvme_request *req,
				 uint128_t key,
				 uint32_t buffer_size,
				 uint32_t offset,
				 uint32_t io_flags, uint32_t option)
{
	struct spdk_nvme_cmd	*cmd;

	cmd = &req->cmd;
	_nvme_kv_cmd_set_key(cmd, key);
	cmd->cdw10 = buffer_size;

	// cdw11:
	// [0:7] key_size
	// [8:15] store option
	cmd->cdw11 = ((uint32_t)((option & 0xFF) << 8) | (KV_MAX_KEY_SIZE & 0xFF));
}

/*
 * Setup retrieve request
 */
static void
_nvme_kv_cmd_setup_retrieve_request(struct spdk_nvme_ns *ns, struct nvme_request *req,
				    uint128_t key, uint32_t buffer_size,
				    uint32_t option)
{
	struct spdk_nvme_cmd    *cmd;

	cmd = &req->cmd;
	_nvme_kv_cmd_set_key(cmd, key);
	cmd->cdw10 = buffer_size;

	// cdw11:
	// [0:7] key_size
	// [8:15] store option
	cmd->cdw11 = ((uint32_t)((option & 0xFF) << 8) | (KV_MAX_KEY_SIZE & 0xFF));
}

/*
 */
static void
_nvme_kv_cmd_setup_delete_request(struct spdk_nvme_ns *ns, struct nvme_request *req, uint128_t key)
{
	struct spdk_nvme_cmd    *cmd;

	cmd = &req->cmd;
	_nvme_kv_cmd_set_key(cmd, key);
	cmd->cdw10 = 0;

	// cdw11:
	// [0:7] key_size
	cmd->cdw11 = (KV_MAX_KEY_SIZE & 0xFF);
}

/*
 */
static void
_nvme_kv_cmd_setup_exist_request(struct spdk_nvme_ns *ns, struct nvme_request *req,
				 uint128_t key_size)
{
	struct spdk_nvme_cmd    *cmd;

	cmd = &req->cmd;
	_nvme_kv_cmd_set_key(cmd, key);
	cmd->cdw10 = 0;

	// cdw11:
	// [0:7] key_size
	cmd->cdw11 = (KV_MAX_KEY_SIZE & 0xFF);
}

/*
 * _nvme_kv_cmd_allocate_request
 *   Allocate request and fill payload/metadata.
 *   We use metadata/payload in different ways in different commands.
 *                         metadata         payload
 *   Store/Retrieve        key              value
 *   Delete                key              N/A
 *   Exist                 result list      key array
 *   Iterate               N/A              key list
 */
static struct nvme_request *
_nvme_kv_cmd_allocate_request(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      const struct nvme_payload *payload,
			      uint32_t buffer_size, uint32_t payload_offset, uint32_t md_offset,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc, uint32_t keyspace_id)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd    *cmd;

	req = nvme_allocate_request(qpair, payload, buffer_size, cb_fn, cb_arg);
	if (req == NULL) {
		return NULL;
	}

	cmd = &req->cmd;
	cmd->opc = opc;
	cmd->nsid = keyspace_id;

	req->payload_offset = payload_offset;
	req->md_offset = md_offset;

	return req;
}

/**
 * \brief Submits a KV Store I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV Store I/O
 * \param qpair I/O queue pair to submit the request
 * \param keyspace_id namespace id of key
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
 *          default = 0, compression = 1, idempotent = 2
 * \param is_store store=troe or append=false
	    SPDK_NVME_OPC_KV_STORE(0x81),  SPDK_NVME_OPC_KV_APPEND(0x83)
 * \return 0 if successfully submitted, SPDK_KV_ERR_DD_NO_AVAILABLE_RESOURCE if an nvme_request
 *           structure cannot be allocated for the I/O request, SPDK_KV_ERR_DD_INVALID_PARAM if
 *           key_length or buffer_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int
spdk_nvme_kv_cmd_store(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       uint32_t keyspace_id, uint128_t key,
		       void *buffer, uint32_t buffer_length,
		       uint32_t offset,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		       uint32_t io_flags, uint32_t option)
{
	int ret = SPDK_NVME_SC_SUCCESS;
	struct nvme_request *req;
	struct nvme_payload payload;

	if (buffer_length > KV_MAX_VALUE_SIZE) {
		return SPDK_NVME_SC_KV_INVALID_VALUE_SIZE;
	}

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;

	req = _nvme_kv_cmd_allocate_request(ns, qpair, &payload, buffer_length,
					    0, 0, cb_fn, cb_arg, SPDK_NVME_OPC_KV_STORE, keyspace_id);

	if (NULL == req) {
		return -ENOMEM;
	}

	_nvme_kv_cmd_setup_store_request(ns, req, key, buffer_length, offset, io_flags, option);

	return nvme_qpair_submit_request(qpair, req);
}

/**
 * \brief Submits a KV Retrieve I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV Retrieve I/O
 * \param qpair I/O queue pair to submit the request
 * \param keyspace_id namespace id of key
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
 *     default = 0, decompression = 1
 *
 * \return 0 if successfully submitted, SPDK_KV_ERR_DD_NO_AVAILABLE_RESOURCE if an nvme_request
 *           structure cannot be allocated for the I/O request, SPDK_KV_ERR_DD_INVALID_PARAM if
 *           key_length or buffer_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int
spdk_nvme_kv_cmd_retrieve(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			  uint32_t keyspace_id, uint128_t key,
			  void *buffer, uint32_t buffer_length,
			  uint32_t offset,
			  spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t option)
{
	int ret = SPDK_NVME_SC_SUCCESS;
	struct nvme_request *req;
	struct nvme_payload payload;

	if (buffer_length > KV_MAX_VALUE_SIZE) {
		return SPDK_NVME_SC_KV_INVALID_VALUE_SIZE;
	}

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;

	req = _nvme_kv_cmd_allocate_request(ns, qpair, &payload, buffer_length, 0, 0, cb_fn, cb_arg,
					    SPDK_NVME_OPC_KV_RETRIEVE, keyspace_id);
	if (NULL == req) {
		return -ENOMEM;
	}

	_nvme_kv_cmd_setup_retrieve_request(ns, req,
					    key, buffer_length, option);

	return nvme_qpair_submit_request(qpair, req);
}

/**
 * \brief Submits a KV Delete I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV DeleteI/O
 * \param qpair I/O queue pair to submit the request
 * \param keyspace_id namespace id of key
 * \param key virtual address pointer to the value
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, SPDK_KV_ERR_DD_NO_AVAILABLE_RESOURCE if an nvme_request
 *           structure cannot be allocated for the I/O request, SPDK_KV_ERR_DD_INVALID_PARAM if
 *           key_length or buffer_length is too large.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int
spdk_nvme_kv_cmd_delete(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			uint32_t keyspace_id, uint128_t key,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	int ret = SPDK_NVME_SC_SUCCESS;
	struct nvme_request *req;
	struct nvme_payload payload;

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = NULL;

	req = _nvme_kv_cmd_allocate_request(ns, qpair, &payload,
					    0, //Payload length is 0 for delete command
					    0, 0, cb_fn, cb_arg, SPDK_NVME_OPC_KV_DELETE, keyspace_id);

	if (NULL == req) {
		return -ENOMEM;
	}

	_nvme_kv_cmd_setup_delete_request(ns, req, key);

	return nvme_qpair_submit_request(qpair, req);

}


/**
 * \brief Submits a KV Exist I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the KV Exist I/O
 * \param qpair I/O queue pair to submit the request
 * \param keys virtual address pointer to the key array
 * \param key_length length (in bytes) of the key
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, SPDK_KV_ERR_DD_NO_AVAILABLE_RESOURCE if an nvme_request
 *           structure cannot be allocated for the I/O request
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int
spdk_nvme_kv_cmd_exist(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       uint32_t keyspace_id, uint128_t key,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	int ret = SPDK_NVME_SC_SUCCESS;
	struct nvme_request *req;
	struct nvme_payload payload;

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = NULL;

	req = _nvme_kv_cmd_allocate_request(ns, qpair, &payload,
					    0, //payload length is 0 for exist command
					    0, 0, cb_fn, cb_arg, SPDK_NVME_OPC_KV_EXIST, keyspace_id);

	if (NULL == req) {
		return -ENOMEM;
	}

	_nvme_kv_cmd_setup_exist_request(ns, req, key);

	return nvme_qpair_submit_request(qpair, req);

}
