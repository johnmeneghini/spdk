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

#include "bdev_rocksdb.h"

struct rpc_construct_null {
	char *name;
	char *uuid;
	char *db_path;
	char *db_backup_path;
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
	{"db_path", offsetof(struct rpc_construct_null, db_path), spdk_json_decode_string},
	{"db_backup_path", offsetof(struct rpc_construct_null, db_backup_path), spdk_json_decode_string},
};

static void
rpc_bdev_rocksdb_create(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_construct_null req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_uuid *uuid = NULL;
	struct spdk_uuid decoded_uuid;
	struct spdk_bdev *bdev;
	struct spdk_rocksdb_bdev_opts opts = {};
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_construct_null_decoders,
				    SPDK_COUNTOF(rpc_construct_null_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_null, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (req.db_path == NULL || strlen(req.db_path) == 0) {
		spdk_jsonrpc_send_error_response(request, -EINVAL,
						 "No db path specified");
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
	opts.db_path = req.db_path;
	opts.db_backup_path = req.db_backup_path;
	rc = bdev_rocksdb_create(&bdev, &opts);
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
SPDK_RPC_REGISTER("bdev_rocksdb_create", rpc_bdev_rocksdb_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_rocksdb_create, construct_rocksdb_bdev)

struct rpc_delete_rocksdb {
	char *name;
};

static void
free_rpc_delete_rocksdb(struct rpc_delete_rocksdb *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_rocksdb_decoders[] = {
	{"name", offsetof(struct rpc_delete_rocksdb, name), spdk_json_decode_string},
};

static void
rpc_bdev_rocksdb_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_bool(w, bdeverrno == 0);
	spdk_jsonrpc_end_result(request, w);
}

static void
rpc_bdev_rocksdb_delete(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_delete_rocksdb req = {NULL};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_delete_rocksdb_decoders,
				    SPDK_COUNTOF(rpc_delete_rocksdb_decoders),
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

	bdev_rocksdb_delete(bdev, rpc_bdev_rocksdb_delete_cb, request);

	free_rpc_delete_rocksdb(&req);

	return;

cleanup:
	free_rpc_delete_rocksdb(&req);
}
SPDK_RPC_REGISTER("bdev_rocksdb_delete", rpc_bdev_rocksdb_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_rocksdb_delete, delete_rocksdb_bdev)
