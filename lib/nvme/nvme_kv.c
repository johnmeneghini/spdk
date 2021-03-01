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

#include "spdk/nvme_kv.h"
#include "nvme_internal.h"

const struct spdk_nvme_kv_ns_data *
spdk_nvme_kv_ns_get_data(struct spdk_nvme_ns *ns)
{
	return ns->ctrlr->nsdata_kv[ns->id - 1];
}

void
spdk_nvme_kv_cmd_set_key(const struct spdk_nvme_kv_key_t *key, struct spdk_nvme_kv_cmd *cmd)
{
	memcpy(&cmd->kvkey0, ((uint32_t *)&key->key) + 0, sizeof(cmd->kvkey0));
	memcpy(&cmd->kvkey1, ((uint32_t *)&key->key) + 1, sizeof(cmd->kvkey1));
	memcpy(&cmd->kvkey2, ((uint32_t *)&key->key) + 2, sizeof(cmd->kvkey2));
	memcpy(&cmd->kvkey3, ((uint32_t *)&key->key) + 3, sizeof(cmd->kvkey3));
}

void
spdk_nvme_kv_cmd_get_key(const struct spdk_nvme_kv_cmd *cmd, struct spdk_nvme_kv_key_t *key)
{
	switch (cmd->opc) {
	case SPDK_NVME_OPC_KV_STORE:
		key->kl = cmd->cdw11_bits.kv_store.kl;
		break;
	case SPDK_NVME_OPC_KV_RETRIEVE:
		key->kl = cmd->cdw11_bits.kv_retrieve.kl;
		break;
	case SPDK_NVME_OPC_KV_DELETE:
		key->kl = cmd->cdw11_bits.kv_del.kl;
		break;
	case SPDK_NVME_OPC_KV_EXIST:
		key->kl = cmd->cdw11_bits.kv_exist.kl;
		break;
	case SPDK_NVME_OPC_KV_LIST:
		key->kl = cmd->cdw11_bits.kv_list.kl;
		break;
	}
	memcpy(((uint32_t *)&key->key) + 0, &cmd->kvkey0, sizeof(cmd->kvkey0));
	memcpy(((uint32_t *)&key->key) + 1, &cmd->kvkey0, sizeof(cmd->kvkey1));
	memcpy(((uint32_t *)&key->key) + 2, &cmd->kvkey0, sizeof(cmd->kvkey2));
	memcpy(((uint32_t *)&key->key) + 3, &cmd->kvkey0, sizeof(cmd->kvkey3));
}

int spdk_kv_key_parse(const char *str, struct spdk_nvme_kv_key_t *key)
{
	memset(key, 0, sizeof(*key));
	if (str == NULL || strlen(str) == 0) {
		return -EINVAL;
	}
	size_t str_len = strlen(str);
	if (str_len >= 3 && str[0] == '0' && str[1] == 'x') {
		/** Parse a string of the form '0x11223344-11223344-11223344-11223344 */
		str += 2; /* Skip over prefix */
		str_len -= 2;
		while (str_len && key->kl < KV_MAX_KEY_SIZE) {
			if (*str == '-') {
				if (key->kl && key->kl % 4) {
					return -EINVAL;
				}
				str++;
				str_len--;
				continue;
			}
			uint32_t bytes_read = 0;
			int rv = sscanf(str, "%2hhx%n", &key->key[key->kl], &bytes_read);
			if (rv <= 0) {
				return -EINVAL;
			}
			assert(bytes_read <= str_len);
			assert(bytes_read <= 2);
			str += bytes_read;
			key->kl++;
			str_len -= bytes_read;
		}
	} else {
		if (str_len > sizeof(key->key)) {
			return -EINVAL;
		}
		key->kl = spdk_min(sizeof(key->key), str_len);
		memcpy(key->key, str, key->kl);
	}
	return 0;
}

int spdk_kv_key_fmt_lower(char *key_str, size_t key_str_size, uint32_t key_len, const uint8_t *key)
{
	uint32_t key_byte_index = 0;
	char *output_str = key_str;
	int rv = snprintf(output_str, key_str_size, "0x");
	if (rv < 0) {
		return errno;
	}
	assert(key_str_size >= (uint32_t)rv);
	key_str_size -= rv;
	output_str += rv;
	while (key_str_size && key_byte_index <= KV_MAX_KEY_SIZE && key_len) {
		if (key_str_size && key_byte_index && (key_byte_index % 4) == 0) {
			*output_str = '-';
			output_str++;
			key_str_size--;
		}
		rv = snprintf(output_str, key_str_size, "%02x", key[key_byte_index]);
		if (rv < 0) {
			return errno;
		}
		assert(key_str_size >= (uint32_t)rv);
		key_str_size -= rv;
		assert(key_len);
		key_len--;
		output_str += rv;
		key_byte_index++;
	}
	if (key_len) {
		/* Not enough bytes to store key */
		return -ENOSPC;
	}

	return 0;
}

int spdk_kv_cmd_fmt_lower(const struct spdk_nvme_kv_cmd *kv_cmd, char *key_str, size_t key_str_size)
{
	struct spdk_nvme_kv_key_t key;
	memset(&key, 0, sizeof(key));
	spdk_nvme_kv_cmd_get_key(kv_cmd, &key);
	return spdk_kv_key_fmt_lower(key_str, key_str_size, key.kl, key.key);
}


/*
 * Setup store request
 */
static void
_nvme_kv_cmd_setup_store_request(struct spdk_nvme_ns *ns, struct nvme_request *req,
				 struct spdk_nvme_kv_key_t *key,
				 uint32_t buffer_size,
				 uint32_t offset,
				 uint32_t option)
{
	struct spdk_nvme_kv_cmd	*cmd;

	cmd = (struct spdk_nvme_kv_cmd *)&req->cmd;
	spdk_nvme_kv_cmd_set_key(key, cmd);
	cmd->cdw10_bits.kv_store.vs = buffer_size;

	/**
	 * cdw11:
	 * [0:7] key_size
	 * [8:15] store option
	 */
	cmd->cdw11_bits.kv_store.kl = key->kl;
}

/*
 * Setup retrieve request
 */
static void
_nvme_kv_cmd_setup_retrieve_request(struct spdk_nvme_ns *ns, struct nvme_request *req,
				    struct spdk_nvme_kv_key_t *key, uint32_t buffer_size,
				    uint32_t option)
{
	struct spdk_nvme_kv_cmd	*cmd;

	cmd = (struct spdk_nvme_kv_cmd *)&req->cmd;
	spdk_nvme_kv_cmd_set_key(key, cmd);
	cmd->cdw10_bits.kv_retrieve.hbs = buffer_size;

	/**
	 * cdw11:
	 * [0:7] key_size
	 * [8:15] store option
	 */
	cmd->cdw11_bits.kv_retrieve.kl = key->kl;
}

/*
 */
static void
_nvme_kv_cmd_setup_delete_request(struct spdk_nvme_ns *ns, struct nvme_request *req,
				  struct spdk_nvme_kv_key_t *key)
{
	struct spdk_nvme_kv_cmd	*cmd;

	cmd = (struct spdk_nvme_kv_cmd *)&req->cmd;
	spdk_nvme_kv_cmd_set_key(key, cmd);
	cmd->cdw10 = 0;

	/**
	 * cdw11:
	 * [0:7] key_size
	 */
	cmd->cdw11_bits.kv_del.kl = key->kl;
}

/*
 */
static void
_nvme_kv_cmd_setup_exist_request(struct spdk_nvme_ns *ns, struct nvme_request *req,
				 struct spdk_nvme_kv_key_t *key)
{
	struct spdk_nvme_kv_cmd	*cmd;

	cmd = (struct spdk_nvme_kv_cmd *)&req->cmd;
	spdk_nvme_kv_cmd_set_key(key, cmd);
	cmd->cdw10 = 0;

	cmd->cdw11_bits.kv_exist.kl = key->kl;
}

/*
 * Setup list request
 */
static void
_nvme_kv_cmd_setup_list_request(struct spdk_nvme_ns *ns, struct nvme_request *req,
				struct spdk_nvme_kv_key_t *key, uint32_t buffer_size)
{
	struct spdk_nvme_kv_cmd	*cmd;

	cmd = (struct spdk_nvme_kv_cmd *)&req->cmd;
	spdk_nvme_kv_cmd_set_key(key, cmd);
	cmd->cdw10_bits.kv_list.hbs = buffer_size;

	/**
	 * cdw11:
	 * [0:7] key_size
	 * [8:15] store option
	 */
	cmd->cdw11_bits.kv_list.kl = key->kl;
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
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc)
{
	struct nvme_request	*req;
	struct spdk_nvme_kv_cmd    *cmd;

	req = nvme_allocate_request(qpair, payload, buffer_size, 0, cb_fn, cb_arg);
	if (req == NULL) {
		return NULL;
	}

	cmd = (struct spdk_nvme_kv_cmd *)&req->cmd;
	cmd->opc = opc;
	cmd->nsid = ns->id;

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
		       struct spdk_nvme_kv_key_t *key,
		       void *buffer, uint32_t buffer_length,
		       uint32_t offset,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		       uint32_t option)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	assert(key->kl > 0 && key->kl <= KV_MAX_KEY_SIZE);
	if (key->kl == 0) {
		return SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
	}
	if (key->kl > KV_MAX_KEY_SIZE) {
		return SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
	}
	if (buffer_length > KV_MAX_VALUE_SIZE) {
		return SPDK_NVME_SC_KV_INVALID_VALUE_SIZE;
	}
	payload = NVME_PAYLOAD_CONTIG(buffer, NULL);

	req = _nvme_kv_cmd_allocate_request(ns, qpair, &payload, buffer_length,
					    0, 0, cb_fn, cb_arg, SPDK_NVME_OPC_KV_STORE);

	if (NULL == req) {
		return -ENOMEM;
	}

	_nvme_kv_cmd_setup_store_request(ns, req, key, buffer_length, offset, option);

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
			  struct spdk_nvme_kv_key_t *key, void *buffer, uint32_t buffer_length,
			  uint32_t offset,
			  spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t option)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	assert(key->kl > 0 && key->kl <= KV_MAX_KEY_SIZE);
	if (key->kl == 0) {
		return SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
	}
	if (key->kl > KV_MAX_KEY_SIZE) {
		return SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
	}
	if (buffer_length > KV_MAX_VALUE_SIZE) {
		return SPDK_NVME_SC_KV_INVALID_VALUE_SIZE;
	}

	payload = NVME_PAYLOAD_CONTIG(buffer, NULL);

	req = _nvme_kv_cmd_allocate_request(ns, qpair, &payload, buffer_length, 0, 0, cb_fn, cb_arg,
					    SPDK_NVME_OPC_KV_RETRIEVE);
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
			struct spdk_nvme_kv_key_t *key,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	assert(key->kl > 0 && key->kl <= KV_MAX_KEY_SIZE);
	if (key->kl == 0) {
		return SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
	}
	if (key->kl > KV_MAX_KEY_SIZE) {
		return SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
	}

	payload = NVME_PAYLOAD_CONTIG(NULL, NULL);

	req = _nvme_kv_cmd_allocate_request(ns, qpair, &payload,
					    0, /** Payload length is 0 for delete command */
					    0, 0, cb_fn, cb_arg, SPDK_NVME_OPC_KV_DELETE);

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
		       struct spdk_nvme_kv_key_t *key, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	assert(key->kl > 0 && key->kl <= KV_MAX_KEY_SIZE);
	if (key->kl == 0) {
		return SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
	}
	if (key->kl > KV_MAX_KEY_SIZE) {
		return SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
	}

	payload = NVME_PAYLOAD_CONTIG(NULL, NULL);

	req = _nvme_kv_cmd_allocate_request(ns, qpair, &payload,
					    0, /** payload length is 0 for exist command */
					    0, 0, cb_fn, cb_arg, SPDK_NVME_OPC_KV_EXIST);

	if (NULL == req) {
		return -ENOMEM;
	}

	_nvme_kv_cmd_setup_exist_request(ns, req, key);

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
spdk_nvme_kv_cmd_list(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		      struct spdk_nvme_kv_key_t *key, void *buffer, uint32_t buffer_length,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	assert(key->kl > 0 && key->kl <= KV_MAX_KEY_SIZE);
	if (key->kl == 0) {
		return SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
	}
	if (key->kl > KV_MAX_KEY_SIZE) {
		return SPDK_NVME_SC_KV_INVALID_KEY_SIZE;
	}
	if (buffer_length > KV_MAX_VALUE_SIZE) {
		return SPDK_NVME_SC_KV_INVALID_VALUE_SIZE;
	}

	payload = NVME_PAYLOAD_CONTIG(buffer, NULL);

	req = _nvme_kv_cmd_allocate_request(ns, qpair, &payload, buffer_length, 0, 0, cb_fn, cb_arg,
					    SPDK_NVME_OPC_KV_LIST);
	if (NULL == req) {
		return -ENOMEM;
	}

	_nvme_kv_cmd_setup_list_request(ns, req,
					key, buffer_length);

	return nvme_qpair_submit_request(qpair, req);
}

