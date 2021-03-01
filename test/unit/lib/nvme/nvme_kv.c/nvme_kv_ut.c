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

#include "spdk_cunit.h"

#include "spdk/env.h"

#include "nvme/nvme_kv.c"

#include "common/lib/test_env.c"

SPDK_LOG_REGISTER_COMPONENT(nvme_kv)

static void
test_nvme_kv_parse(void)
{
	struct spdk_nvme_kv_key_t key;
	char key_str[KV_KEY_STRING_LEN];

	int rv = spdk_kv_key_parse("", &key);
	CU_ASSERT(rv != 0);

	rv = spdk_kv_key_parse("hello", &key);
	CU_ASSERT(rv == 0);
	spdk_kv_key_fmt_lower(key_str, sizeof(key_str), key.kl, key.key);
	CU_ASSERT(key.kl == strlen("hello"));
	CU_ASSERT(strcmp("0x68656c6c-6f", key_str) == 0);

	rv = spdk_kv_key_parse("0x0", &key);
	CU_ASSERT(rv == 0);
	spdk_kv_key_fmt_lower(key_str, sizeof(key_str), key.kl, key.key);
	CU_ASSERT(key.kl == 1);
	CU_ASSERT(strcmp("0x00", key_str) == 0);

	rv = spdk_kv_key_parse("0x00", &key);
	CU_ASSERT(rv == 0);
	spdk_kv_key_fmt_lower(key_str, sizeof(key_str), key.kl, key.key);
	CU_ASSERT(key.kl == 1);
	CU_ASSERT(strcmp("0x00", key_str) == 0);

	rv = spdk_kv_key_parse("0x11", &key);
	CU_ASSERT(rv == 0);
	spdk_kv_key_fmt_lower(key_str, sizeof(key_str), key.kl, key.key);
	CU_ASSERT(key.kl == 1);
	CU_ASSERT(strcmp("0x11", key_str) == 0);

	rv = spdk_kv_key_parse("0x112", &key);
	CU_ASSERT(rv == 0);
	spdk_kv_key_fmt_lower(key_str, sizeof(key_str), key.kl, key.key);
	CU_ASSERT(key.kl == 2);
	CU_ASSERT(strcmp("0x1102", key_str) == 0);

	rv = spdk_kv_key_parse("0x11223344", &key);
	CU_ASSERT(rv == 0);
	spdk_kv_key_fmt_lower(key_str, sizeof(key_str), key.kl, key.key);
	CU_ASSERT(key.kl == 4);
	CU_ASSERT(strcmp("0x11223344", key_str) == 0);

	rv = spdk_kv_key_parse("0x11223344-11223344-11223344-11223344", &key);
	CU_ASSERT(rv == 0);
	spdk_kv_key_fmt_lower(key_str, sizeof(key_str), key.kl, key.key);
	CU_ASSERT(key.kl == 16);
	CU_ASSERT(strcmp("0x11223344-11223344-11223344-11223344", key_str) == 0);

}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_kv", NULL, NULL);

	CU_ADD_TEST(suite, test_nvme_kv_parse);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
