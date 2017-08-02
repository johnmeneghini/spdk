#if !defined(__OCSU_SPDK_H__)
#define __OCSU_SPDK_H__

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_atomic.h>
#include <rte_cycles.h>

#include "spdk/env.h"


#ifdef USE_PCIACCESS
#include <pciaccess.h>
#else
#include <rte_pci.h>
#endif

typedef struct ocs_uspace {
        uint32_t instance_index;
        struct spdk_pci_device *pdev;
        ocsu_memref_t bars[PCI_MAX_BAR];
        uint32_t bar_count;

        const char *desc;
        uint16_t pci_vendor;
        uint16_t pci_device;
        char businfo[16];
        uint32_t n_msix_vec;
        int intr_enabled;
        struct device *ocs_dev;

        uint32_t dmabuf_next_instance;
        uint8_t  if_type;
        ocsu_memref_t bmbx;
} ocs_uspace_t;

#define ocsu_free(buf)                  rte_free(buf)
#endif
