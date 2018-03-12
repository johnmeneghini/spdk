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

/*
 * Approximately amount of memory to dump
 * the 3 entries from each of the 16 IO queues and 1 LS queue.
 */
#define SPDK_FC_HW_DUMP_BUF_SIZE (10 * 4096)
#define SPDK_FC_HW_DUMP_REASON_STR_MAX_SIZE 256
#define SPDK_MAX_NUM_OF_FC_PORTS 32
#define SPDK_NVMF_PORT_ID_MAX_LEN 32
#define SPDK_NVMF_FC_LOG_STR_SIZE 255

#define FC_LS_HWQP_ID (SPDK_MAX_NUM_OF_FC_PORTS * NVMF_FC_MAX_IO_QUEUES) + 1

/*
 * Queue poller intervals (microseconds)
 */
#define SPDK_NVMF_BCM_FC_IOQ_POLLER_INTERVAL 0
#define SPDK_NVMF_BCM_FC_AQ_POLLER_INTERVAL  100000
#define SPDK_NVMF_BCM_FC_LS_POLLER_INTERVAL  100000

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
	SPDK_FC_HW_PORT_DUMP,
	SPDK_FC_UNRECOVERABLE_ERR,
	SPDK_FC_EVENT_MAX,
} spdk_fc_event_t;

/**
 * \struct spdk_nvmf_bcm_fc_dump_assoc_id_args
 *
 * \brief  Arguments for to dump assoc id
 */
struct spdk_nvmf_bcm_fc_dump_assoc_id_args {
	uint8_t                           pport_handle;
	uint16_t                          nport_handle;
	uint32_t                          assoc_id;
};

typedef struct spdk_nvmf_bcm_fc_dump_assoc_id_args spdk_nvmf_bcm_fc_dump_assoc_id_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_hw_port_init_args
 *
 * \brief  Arguments for HW port init event.
 */
struct spdk_nvmf_bcm_fc_hw_port_init_args {
	uint8_t                           port_handle;
	uint32_t                          xri_base;
	uint32_t                          xri_count;
	struct spdk_nvmf_bcm_fc_hw_queues ls_queue;
	uint32_t                          io_queue_cnt;
	struct spdk_nvmf_bcm_fc_hw_queues io_queues[NVMF_FC_MAX_IO_QUEUES];
	void                             *cb_ctx;
	void                             *port_ctx;
	uint16_t                          fcp_rq_id; /* Base rq ID of SCSI queue */
};

typedef struct spdk_nvmf_bcm_fc_hw_port_init_args spdk_nvmf_bcm_fc_hw_port_init_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_hw_port_online_args
 *
 * \brief  Arguments for HW port link break event.
 */
typedef struct {
	uint8_t port_handle;
	void   *cb_ctx;
} spdk_nvmf_bcm_hw_port_link_break_args_t;

/**
 *
 * \brief  Arguments for HW port online event.
 */
struct spdk_nvmf_bcm_fc_hw_port_online_args {
	uint8_t port_handle;
	void   *cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_hw_port_online_args spdk_nvmf_bcm_fc_hw_port_online_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_hw_port_offline_args
 *
 * \brief  Arguments for HW port offline event.
 */
struct spdk_nvmf_bcm_fc_hw_port_offline_args {
	uint8_t port_handle;
	void   *cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_hw_port_offline_args spdk_nvmf_bcm_fc_hw_port_offline_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_nport_create_args
 *
 * \brief  Arguments for n-port add event.
 */
struct spdk_nvmf_bcm_fc_nport_create_args {
	uint8_t                     port_handle;
	uint16_t                    nport_handle;
	struct spdk_uuid   	    container_uuid; /* UUID of the nports container */
	struct spdk_uuid   	    nport_uuid;     /* Unique UUID for the nport */
	uint32_t                    d_id;
	struct spdk_nvmf_bcm_fc_wwn fc_nodename;
	struct spdk_nvmf_bcm_fc_wwn fc_portname;
	uint32_t                    subsys_id; /* Subsystemid */
	char                        port_id[SPDK_NVMF_PORT_ID_MAX_LEN];
	void                       *cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_nport_create_args spdk_nvmf_bcm_fc_nport_create_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_nport_delete_args
 *
 * \brief  Arguments for n-port delete event.
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
 * \brief  Arguments for I_T add event.
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
 * \brief  Arguments for I_T delete event.
 */
struct spdk_nvmf_bcm_fc_hw_i_t_delete_args {
	uint8_t  port_handle;
	uint32_t nport_handle;
	uint16_t itn_handle;    // Only used by FC driver; unused in SPDK
	uint32_t rpi;
	uint32_t s_id;
	void    *cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_hw_i_t_delete_args spdk_nvmf_bcm_fc_hw_i_t_delete_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_abts_args
 *
 * \brief  Arguments for ABTS  event.
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
 * \brief  Arguments for link break event.
 */
struct spdk_nvmf_bcm_fc_link_break_args {
	uint8_t port_handle;
};

typedef struct spdk_nvmf_bcm_fc_link_break_args spdk_nvmf_bcm_fc_link_break_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_hw_port_reset_args
 *
 * \brief  Arguments for port reset event.
 */
struct spdk_nvmf_bcm_fc_hw_port_reset_args {
	uint8_t    port_handle;
	bool       dump_queues;
	char       reason[SPDK_FC_HW_DUMP_REASON_STR_MAX_SIZE];
	uint32_t **dump_buf;
	void      *cb_ctx;
};

typedef struct spdk_nvmf_bcm_fc_hw_port_reset_args spdk_nvmf_bcm_fc_hw_port_reset_args_t;

/**
 * \struct spdk_nvmf_bcm_fc_unrecoverable_error_args
 *
 * \brief  Arguments for unrecoverable error event
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
 * \brief  The callback structure for HW port link break event
 */
struct spdk_nvmf_bcm_fc_port_link_break_cb_data {
	spdk_nvmf_bcm_hw_port_link_break_args_t *args;
	spdk_nvmf_bcm_fc_nport_delete_args_t        nport_del_args;
	spdk_nvmf_bcm_fc_callback       cb_func;
};

typedef struct spdk_nvmf_bcm_fc_port_link_break_cb_data spdk_nvmf_bcm_fc_port_link_break_cb_data_t;

/**
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
 * \brief  The callback structure for it-delete
 */
struct spdk_nvmf_bcm_fc_i_t_del_cb_data {
	struct spdk_nvmf_bcm_fc_nport            *nport;
	struct spdk_nvmf_bcm_fc_remote_port_info *rport;
	uint8_t                                   port_handle;
	spdk_nvmf_bcm_fc_callback                 fc_cb_func;
	void                                     *fc_cb_ctx;
};

typedef struct spdk_nvmf_bmc_fc_i_t_del_cb_data spdk_nvmf_bcm_fc_i_t_del_cb_data_t;


typedef void (*spdk_nvmf_bcm_fc_i_t_delete_assoc_cb_fn)(void *arg, uint32_t err);

/**
 * \brief  The callback structure for the it-delete-assoc callback
 */
struct spdk_nvmf_bcm_fc_i_t_del_assoc_cb_data {
	struct spdk_nvmf_bcm_fc_nport            *nport;
	struct spdk_nvmf_bcm_fc_remote_port_info *rport;
	uint8_t                                    port_handle;
	spdk_nvmf_bcm_fc_i_t_delete_assoc_cb_fn    cb_func;
	void                                      *cb_ctx;
};

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

/*
 * Call back function pointer for HW port quiesce.
 */
typedef void (*spdk_nvmf_bcm_fc_hw_port_quiesce_cb_fn)(void *ctx, spdk_err_t err);

/**
 * \struct spdk_nvmf_bcm_fc_hw_port_quiesce_ctx
 *
 * \brief Context structure for quiescing a hardware port
 */
struct spdk_nvmf_bcm_fc_hw_port_quiesce_ctx {
	int                quiesce_count;
	void              *ctx;
	spdk_nvmf_bcm_fc_hw_port_quiesce_cb_fn cb_func;
};

typedef struct spdk_nvmf_bcm_fc_hw_port_quiesce_ctx spdk_nvmf_bcm_fc_hw_port_quiesce_ctx_t;

/**
 * \struct spdk_nvmf_bcm_fc_hw_port_reset_ctx
 *
 * \brief Context structure used to reset a hardware port
 */
struct spdk_nvmf_bcm_fc_hw_port_reset_ctx {
	void       *reset_args;
	spdk_nvmf_bcm_fc_callback reset_cb_func;
};

typedef struct spdk_nvmf_bcm_fc_hw_port_reset_ctx spdk_nvmf_bcm_fc_hw_port_reset_ctx_t;

/**
 * \struct spdk_nvmf_bcm_fc_hw_port_dump_ctx
 *
 * \brief info structure for dumping a queue
 */
struct spdk_nvmf_bcm_fc_queue_dump_info {
	char *buffer;
	int   offset;
};

typedef struct spdk_nvmf_bcm_fc_queue_dump_info spdk_nvmf_bcm_fc_queue_dump_info_t;

/**
  * \brief Pass the given event to the associated lcore with an application context
  */
void spdk_post_event(void *context, struct spdk_event *event);

/**
  * \brief Function to print a list of all the FC ports
  */
void spdk_nvmf_bcm_fc_tgt_print_port_list(void *arg1, void *arg2);

/**
  * \brief Function to print the contents of an FC port
  */
void spdk_nvmf_bcm_fc_tgt_print_port(void *arg1, void *arg2);

/**
  * \brief Function to print the contents of a given FC Nport
  * \param[in] port_hdl - The global id of the FC port
  * \param[in] nport_hdl - The id of the Nport in that FC port
  */
void spdk_nvmf_bcm_fc_tgt_print_nport(void *arg1, void *arg2);

/**
  * \brief Function to print the contents of a given HWQP
  * \param[in] hwqp_id - The global id of the HWQP
  */
void spdk_nvmf_bcm_fc_tgt_print_hwqp(void *arg1, void *arg2);

/**
  * \brief Function to print the contents of a given association
  * \param[in] spdk_nvmf_bcm_fc_dump_assoc_id_args_t
  * Contains port_hdl, nport_hdl, assoc_id
  */
void spdk_nvmf_bcm_fc_tgt_print_assoc(void *arg1, void *arg2);

/**
  * \brief Function to print the contents of a given connection
  * \param[in] hwqp_id - The global id of the HWQP
  * \param[in] conn_id - The connection id
  */
void spdk_nvmf_bcm_fc_tgt_print_conn(void *arg1, void *arg2);

#endif
