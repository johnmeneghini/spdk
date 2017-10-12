/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 1992-2017 NetApp, Inc.
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
#ifndef FC_ADM_API_H
#define FC_ADM_API_H

#include "spdk/error.h"
#include "nvmf_fc/bcm_fc.h"

/**
 * \enum	spdk_fc_event_t
 *
 * \brief	Types events for SPDK master thread.
 */
typedef enum {
	SPDK_FC_HW_PORT_INIT,
	SPDK_FC_HW_PORT_ONLINE,
	SPDK_FC_HW_PORT_OFFLINE,
	SPDK_FC_HW_PORT_RESET,
	SPDK_FC_NPORT_CREATE,
	SPDK_FC_NPORT_DELETE,
	SPDK_FC_IT_ADD,    /* PRLI */
	SPDK_FC_IT_DELETE, /* PRLI */
	SPDK_FC_ABTS_RECV,
	SPDK_FC_LINK_BREAK,
	SPDK_FC_ADAPTER_ERR1, /* Firmware Dump */
	SPDK_FC_ADAPTER_ERR2,
	SPDK_FC_UNRECOVERABLE_ERR,
	SPDK_FC_EVENT_MAX,
} spdk_fc_event_t;

/**
 * \struct spdk_nvmf_bcm_fc_hw_port_init_args
 *
 * \brief  Arguemnts for HW port init event.
 */
struct spdk_nvmf_bcm_fc_hw_port_init_args {
	uint8_t                           port_handle;
	uint32_t                          xri_base;
	uint32_t                          xri_count;
	struct spdk_nvmf_bcm_fc_hw_queues ls_queue;
	uint32_t                          io_queue_cnt;
	struct spdk_nvmf_bcm_fc_hw_queues io_queues[NVMF_FC_MAX_IO_QUEUES];
	void                             *cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_hw_port_init_args spdk_nvmf_bcm_fc_hw_port_init_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_hw_port_online_args
 *
 * \brief  Arguemnts for HW port online event.
 */
struct spdk_nvmf_bcm_fc_hw_port_online_args {
	uint8_t port_handle;
	void   *cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_hw_port_online_args spdk_nvmf_bcm_fc_hw_port_online_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_hw_port_offline_args
 *
 * \brief  Arguemnts for HW port offline event.
 */
struct spdk_nvmf_bcm_fc_hw_port_offline_args {
	uint8_t port_handle;
	void   *cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_hw_port_offline_args spdk_nvmf_bcm_fc_hw_port_offline_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_hw_port_reset_args
 *
 * \brief  Arguemnts for HW port reset event.
 */
struct spdk_nvmf_bcm_fc_hw_port_reset_args {
	uint8_t port_handle;
	void   *cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_hw_port_reset_args spdk_nvmf_bcm_fc_hw_port_reset_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_nport_create_args
 *
 * \brief  Arguemnts for n-port add event.
 */
struct spdk_nvmf_bcm_fc_nport_create_args {
	uint8_t                     port_handle;
	uint32_t                    nport_handle;
	uint32_t                    d_id;
	struct spdk_nvmf_bcm_fc_wwn fc_nodename;
	struct spdk_nvmf_bcm_fc_wwn fc_portname;
	uint32_t                    subsys_id; /* Subsystemid */
	void                       *cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_nport_create_args spdk_nvmf_bcm_fc_nport_create_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_nport_delete_args
 *
 * \brief  Arguemnts for n-port delete event.
 */
struct spdk_nvmf_bcm_fc_nport_delete_args {
	uint8_t  port_handle;
	uint32_t nport_handle;
	uint32_t subsys_id; /* Subsystemid */
	void    *cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_nport_delete_args spdk_nvmf_bcm_fc_nport_delete_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_hw_i_t_add_args
 *
 * \brief  Arguemnts for I_T add event.
 */
struct spdk_nvmf_bcm_fc_hw_i_t_add_args {
	uint8_t                      port_handle;
	uint32_t                     nport_handle;
	uint16_t                     itn_handle;
	uint32_t                     rpi;
	uint32_t                     s_id;
	uint32_t                     initiator_prli_info;
	uint32_t                     target_prli_info; /* populated by the SPDK master */
	struct spdk_nvmf_bcm_fc_wwn  fc_nodename;
	struct spdk_nvmf_bcm_fc_wwn  fc_portname;
	void                        *cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_hw_i_t_add_args spdk_nvmf_bcm_fc_hw_i_t_add_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_hw_i_t_delete_args
 *
 * \brief  Arguemnts for I_T delete event.
 */
struct spdk_nvmf_bcm_fc_hw_i_t_delete_args {
	uint8_t  port_handle;
	uint32_t nport_handle;
	uint32_t rpi;
	uint32_t s_id;
	void    *cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_hw_i_t_delete_args spdk_nvmf_bcm_fc_hw_i_t_delete_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_abts_args
 *
 * \brief  Arguemnts for ABTS  event.
 */
struct spdk_nvmf_bcm_fc_abts_args {
	uint8_t  port_handle;
	uint32_t nport_handle;
	uint32_t rpi;
	uint16_t oxid, rxid;
	void    *cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_abts_args spdk_nvmf_bcm_fc_abts_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_link_break_args
 *
 * \brief  Arguemnts for link break event.
 */
struct spdk_nvmf_bcm_fc_link_break_args {
	uint8_t port_handle;
};

typedef struct spdk_nvmf_bcm_fc_link_break_args spdk_nvmf_bcm_fc_link_break_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_adapter_event_args
 *
 * \brief  Arguemnts for adapter event.
 */
struct spdk_nvmf_bcm_fc_adapter_event_args {
};

typedef struct spdk_nvmf_bcm_fc_adapter_event_args spdk_nvmf_bcm_fc_adapter_event_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_unrecoverable_error_args
 *
 * \brief  Arguemnts for unrecoverable error event
 */
struct spdk_nvmf_bcm_fc_unrecoverable_error_event_args {
};

typedef struct spdk_nvmf_bcm_fc_unrecoverable_error_event_args
	spdk_nvmf_bcm_fc_unrecoverable_error_event_args_t;

/**
 * \brief Pointer to the callback function to the FCT driver.
 */
typedef void (*spdk_nvmf_bcm_fc_callback)(uint8_t port_handle,
		spdk_fc_event_t event_type,
		void *arg, spdk_err_t err);

/**
 * \struct spdk_nvmf_bcm_fc_nport_del_cb_data
 *
 * \brief  The callback structure for nport-delete
 */
struct spdk_nvmf_bcm_fc_nport_del_cb_data {
	struct spdk_nvmf_bcm_fc_nport *nport;
	uint8_t                    port_handle;
	spdk_nvmf_bcm_fc_callback  fc_cb_func;
	void                      *fc_cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_nport_del_cb_data spdk_nvmf_fc_nport_del_cb_data_t;

/**
 * \brief	.
 *
 * The function pushes an event into the master reactors event queue.
 *
 * \param[in] event_type - Type of the event.
 * \param[in] args - Pointer to the argument structure.
 * \param[in] cb_func - Callback function into fc driver
 * \return - Error code.
 */
spdk_err_t
spdk_nvmf_bcm_fc_master_enqueue_event(spdk_fc_event_t event_type,
				      void *args,
				      spdk_nvmf_bcm_fc_callback cb_func);

/**
 * \struct spdk_nvmf_bcm_fc_master_ops
 *
 * \brief       Operations provided by the SPDK master.
 *              The driver uses this function table to call into SPDK
 */
struct spdk_nvmf_bcm_fc_master_ops {
	/*
	 * This function is called to enqueue an event in the masters event queue.
	 */
	spdk_err_t (*enqueue_event)(spdk_fc_event_t event_type, void *args,
				    spdk_nvmf_bcm_fc_callback cb_func);

};

typedef struct spdk_nvmf_bcm_fc_master_ops spdk_fc_ops_t;

extern spdk_err_t
spdk_nvmf_bcm_fc_tgt_add_port(const char *trname, struct spdk_nvmf_bcm_fc_nport *nport);

extern spdk_err_t
spdk_nvmf_bcm_fc_tgt_remove_port(const char *trname, struct spdk_nvmf_bcm_fc_nport *nport);

#endif
