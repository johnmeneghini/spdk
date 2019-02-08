/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Netapp
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
#include "spdk/fault_injects.h"

// Uncomment the following define to enable fault inject in non-debug builds.
// This define should ALWAYS be commented-out when submitting!
//#define SPDK_NVMF_FAULT_NONDEBUG  1

#if (defined(DEBUG) || defined(SPDK_NVMF_FAULT_NONDEBUG))
static nvmf_eal_mutex_t spdk_nvmf_fault_lock;
static uint8_t spdk_nvmf_fault_count[SPDK_NVMF_FINAL_NUM_FAULT];
static uint8_t spdk_nvmf_fault_countdown[SPDK_NVMF_FINAL_NUM_FAULT];
#endif

void
spdk_nvmf_fault_init(void)
{
#if (defined(DEBUG) || defined(SPDK_NVMF_FAULT_NONDEBUG))
	bzero(spdk_nvmf_fault_count, sizeof(uint8_t) * SPDK_NVMF_FINAL_NUM_FAULT);
	bzero(spdk_nvmf_fault_countdown, sizeof(uint8_t) * SPDK_NVMF_FINAL_NUM_FAULT);
	nvmf_eal_mutex_init(&spdk_nvmf_fault_lock, 0);
#endif
}

void
spdk_nvmf_fault_uninit(void)
{
#if (defined(DEBUG) || defined(SPDK_NVMF_FAULT_NONDEBUG))
	nvmf_eal_mutex_destroy(&spdk_nvmf_fault_lock);
#endif
}

bool
spdk_nvmf_fault(enum spdk_nvmf_fault_injects f)
{
#if (defined(DEBUG) || defined(SPDK_NVMF_FAULT_NONDEBUG))
	nvmf_eal_mutex_lock(&spdk_nvmf_fault_lock);

	assert(f < SPDK_NVMF_FINAL_NUM_FAULT);

	if (spdk_nvmf_fault_countdown[f] > 0) {
		spdk_nvmf_fault_countdown[f]--;
	}

	if (spdk_nvmf_fault_countdown[f] == 0 && spdk_nvmf_fault_count[f]) {
		/* Fault count of 0xff means infinite fault */
		if (spdk_nvmf_fault_count[f] < 0xff) {
			spdk_nvmf_fault_count[f]--;
		}
		nvmf_eal_mutex_unlock(&spdk_nvmf_fault_lock);
		return true;
	}
	nvmf_eal_mutex_unlock(&spdk_nvmf_fault_lock);
#endif
	return false;
}


void
spdk_nvmf_fault_set(enum spdk_nvmf_fault_injects f, int count, int countdown)
{
#if (defined(DEBUG) || defined(SPDK_NVMF_FAULT_NONDEBUG))
	assert(f < SPDK_NVMF_FINAL_NUM_FAULT);
	nvmf_eal_mutex_lock(&spdk_nvmf_fault_lock);
	spdk_nvmf_fault_countdown[f] = countdown;
	spdk_nvmf_fault_count[f] = count;
	nvmf_eal_mutex_unlock(&spdk_nvmf_fault_lock);
#endif
}

void
spdk_nvmf_fault_clear_all()
{
#if (defined(DEBUG) || defined(SPDK_NVMF_FAULT_NONDEBUG))
	bzero(spdk_nvmf_fault_count, sizeof(uint8_t) * SPDK_NVMF_FINAL_NUM_FAULT);
	bzero(spdk_nvmf_fault_countdown, sizeof(uint8_t) * SPDK_NVMF_FINAL_NUM_FAULT);
#endif
}
