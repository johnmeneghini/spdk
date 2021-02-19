/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NetApp, Inc. All rights reserved.
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

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include "bdev_kv_null.h"

struct rpc_construct_null {
	char *name;
	char *uuid;
	uint32_t max_num_keys;
	uint32_t max_value_size;
};

static void
free_rpc_construct_null(struct rpc_construct_null *req)
{
	free(req->name);
	free(req->uuid);
}

static const struct spdk_json_object_decoder rpc_construct_null_decoders[] = {
	{"name", offsetof(struct rpc_construct_null, name), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_construct_null, uuid), spdk_json_decode_string, true},
	{"max_num_keys", offsetof(struct rpc_construct_null, max_num_keys), spdk_json_decode_uint64},
	{"max_value_size", offsetof(struct rpc_construct_null, max_value_size), spdk_json_decode_uint32},
};

static void
rpc_bdev_kv_null_create(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_construct_null req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_uuid *uuid = NULL;
	struct spdk_uuid decoded_uuid;
	struct spdk_bdev *bdev;
	struct spdk_kv_null_bdev_opts opts = {};
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_construct_null_decoders,
				    SPDK_COUNTOF(rpc_construct_null_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_null, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}


	if (req.max_num_keys == 0) {
		spdk_jsonrpc_send_error_response(request, -EINVAL,
						 "Number of keys must be greater than 0");
		goto cleanup;
	}

	if (req.max_value_size == 0) {
		spdk_jsonrpc_send_error_response(request, -EINVAL,
						 "Maximum value size must be greater than 0");
		goto cleanup;
	}

	if (req.uuid) {
		if (spdk_uuid_parse(&decoded_uuid, req.uuid)) {
			spdk_jsonrpc_send_error_response(request, -EINVAL,
							 "Failed to parse bdev UUID");
			goto cleanup;
		}
		uuid = &decoded_uuid;
	}

	opts.name = req.name;
	opts.uuid = uuid;
	opts.max_num_keys = req.max_num_keys;
	opts.max_value_size = req.max_value_size;
	rc = bdev_kv_null_create(&bdev, &opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, bdev->name);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_construct_null(&req);
	return;

cleanup:
	free_rpc_construct_null(&req);
}
SPDK_RPC_REGISTER("bdev_kv_null_create", rpc_bdev_kv_null_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_kv_null_create, construct_null_bdev)

struct rpc_delete_kv_null {
	char *name;
};

static void
free_rpc_delete_kv_null(struct rpc_delete_kv_null *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_kv_null_decoders[] = {
	{"name", offsetof(struct rpc_delete_kv_null, name), spdk_json_decode_string},
};

static void
rpc_bdev_kv_null_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_bool(w, bdeverrno == 0);
	spdk_jsonrpc_end_result(request, w);
}

static void
rpc_bdev_kv_null_delete(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_delete_kv_null req = {NULL};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_delete_kv_null_decoders,
				    SPDK_COUNTOF(rpc_delete_kv_null_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	bdev_kv_null_delete(bdev, rpc_bdev_kv_null_delete_cb, request);

	free_rpc_delete_kv_null(&req);

	return;

cleanup:
	free_rpc_delete_kv_null(&req);
}
SPDK_RPC_REGISTER("bdev_kv_null_delete", rpc_bdev_kv_null_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_kv_null_delete, delete_kv_null_bdev)
