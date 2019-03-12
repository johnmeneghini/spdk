#ifndef SPDK_NVMF_FAULT_INJECTS_H
#define SPDK_NVMF_FAULT_INJECTS_H

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/assert.h"
#include "spdk/string.h"
#include "spdk/util.h"


#define SPDK_FI_EVENT_NONE               0xff
#define SPDK_FI_EVENT_CLEAR_ALL          0


enum spdk_nvmf_fault_injects {
	/* FC-Transport Fault Codes */
	SPDK_FC_PUT_LS_PENDING_Q = 0,
	SPDK_FC_PUT_IO_PENDING_Q = 1,
	/* RDMA-Transport Fault Codes */
	SPDK_RDMA_DUMMY_FAULT_CODE = 50,
	/* NVMeoF Protocol Fault Codes */
	SPDK_NVMF_DUMMY_FAULT_CODE = 100,
	/* Bdev Fault Codes */
	SPDK_BDEV_DUMMY_FAULT_CODE = 150,
	SPDK_NVMF_FINAL_NUM_FAULT /* Always make sure this is last */
};

#if (defined(DEBUG) || defined(SPDK_NVMF_FAULT_NONDEBUG))
#define SPDK_NVMF_FAULT(f) spdk_nvmf_fault(f)
#else
#define SPDK_NVMF_FAULT(f) (0)
#endif

bool spdk_nvmf_fault(enum spdk_nvmf_fault_injects f);
void spdk_nvmf_fault_init(void);
void spdk_nvmf_fault_uninit(void);
void spdk_nvmf_fault_set(enum spdk_nvmf_fault_injects f, int count, int countdown);
void spdk_nvmf_fault_clear_all(void);

#endif
