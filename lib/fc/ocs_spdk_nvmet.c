/*
 * Copyright (c) 2011-2015, Emulex
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "ocs.h"
#include "ocs_os.h"
#include "ocs_els.h"
#include "ocs_spdk_nvmet.h"
#include "ocs_tgt_api.h"
#include "fc_adm_api.h"

static struct spdk_nvmf_bcm_fc_master_ops *g_nvmf_fc_ops = NULL;

void
spdk_nvmf_fc_register_ops(struct spdk_nvmf_bcm_fc_master_ops *ops)
{
	g_nvmf_fc_ops = ops;
}

static void
ocs_fill_nvme_sli_queue(ocs_t *ocs,
		sli4_queue_t *sli4_q,
		bcm_sli_queue_t *sli_q)

{
	sli_q->qid = sli4_q->id;
	sli_q->size = sli4_q->size;
	sli_q->address	= sli4_q->dma.virt;
	sli_q->max_entries = sli4_q->length;
	sli_q->doorbell_reg =
		ocs->ocs_os.bars[sli4_q->doorbell_rset].vaddr +
		sli4_q->doorbell_offset;
}

static void
ocs_free_nvme_buffers(bcm_buffer_desc_t *buffers)
{
	if (!buffers) {
		return;
	}

	/*
	 * We just need to free the first buffer pointed address
	 * Because all the buffers are allocated in one shot.
	 */
	if (buffers->virt) {
		spdk_dma_free(buffers->virt);
	}

	spdk_free(buffers);
}

static bcm_buffer_desc_t *
ocs_alloc_nvme_buffers(int size, int num_entries)
{
	int i;
	void *virt;
	uint64_t phys;
	bcm_buffer_desc_t *buffers = NULL, *buffer;

	buffers = spdk_calloc(num_entries, sizeof(bcm_buffer_desc_t));
	if (!buffers) {
		goto error;
	}

	virt = spdk_dma_zmalloc((size * num_entries), 4096, &phys);
	if (!virt) {
		goto error;
	}

	for (i = 0; i < num_entries; i++) {
		buffer = buffers + i;

		buffer->len  = size;
		buffer->virt = (uint8_t *)virt + (i * size);
		buffer->phys = phys + (i * size);
	}

	return buffers;
error:
	if (buffers) {
		spdk_free(buffers);
	}
	return NULL;
}

/* APIs */

#if 0
/* NPORT */
static void
ocs_cb_nport_offline(uint8_t port_handle, spdk_fc_event_t event_type,
	void *ctx, spdk_err_t err)
{
	spdk_nport_offline_args_t *args = ctx;

	if (err) {
		ocs_log_err(NULL, "%s: ocs%d nport offline failed.\n",
				__func__, args->port_handle);
	} else {
		ocs_log_err(NULL, "%s: ocs%d nport offline success.\n",
				__func__, args->port_handle);
	}
	free(args);
}

int
ocs_nvme_nport_offline(ocs_t *ocs)
{
	spdk_nport_offline_args_t *args;
	spdk_err_t rc;

	args = ocs_malloc(NULL, sizeof(spdk_nport_offline_args_t), OCS_M_ZERO);
	if (!args) {
		goto err;
	}

	args->port_handle = ocs->instance_index;

	/* We register physical port as nport. Real nports not supported yet. */
	args->nport_handle = 0;
	args->cb_ctx = args;

	if (g_nvmf_fc_ops) { /* Post to master */
		rc = g_nvmf_fc_ops->enqueue_event(SPDK_FC_NPORT_OFFLINE, args,
						  ocs_cb_nport_offline);
		if (rc) {
			goto err;
		}
	}

	return 0; /* Queued */
err:
	if (args) {
		free(args);
	}

	return -1;
}

static void
ocs_cb_nport_online(uint8_t port_handle, spdk_fc_event_t event_type,
	void *ctx, spdk_err_t err)
{
	spdk_nport_online_args_t *args = ctx;

	if (err) {
		ocs_log_err(NULL, "%s: ocs%d nport online failed.\n",
				__func__, args->port_handle);
	} else {
		ocs_log_err(NULL, "%s: ocs%d nport online success.\n",
				__func__, args->port_handle);
	}
	free(args);
}

int
ocs_nvme_nport_online(ocs_t *ocs)
{
	spdk_nport_online_args_t *args;
	spdk_err_t rc;

	args = ocs_malloc(NULL, sizeof(spdk_nport_online_args_t), OCS_M_ZERO);
	if (!args) {
		goto err;
	}

	args->port_handle = ocs->instance_index;

	/* We register physical port as nport. Real nports not supported yet. */
	args->nport_handle = 0;
	args->cb_ctx = args;

	if (g_nvmf_fc_ops) { /* Post to master */
		rc = g_nvmf_fc_ops->enqueue_event(SPDK_FC_NPORT_ONLINE, args,
						  ocs_cb_nport_online);
		if (rc) {
			goto err;
		}
	}

	return 0; /* Queued */
err:
	if (args) {
		free(args);
	}

	return -1;
}
#endif

static void
ocs_cb_nport_create(uint8_t port_handle, spdk_fc_event_t event_type, 
	void *ctx, spdk_err_t err)
{
	spdk_nvmf_bcm_fc_nport_create_args_t *args = ctx;

	if (err) {
		ocs_log_err(NULL, "%s: ocs%d nport create failed.\n",
				__func__, args->port_handle);
	} else {
		ocs_log_info(NULL, "%s: ocs%d nport create success.\n",
				__func__, args->port_handle);

		/* Bring the NPORT online. */
//		ocs_t *ocs = NULL;
//		ocs = ocs_get_instance(args->port_handle);
//		if (!ocs || ocs_nvme_nport_online(ocs)) {
//			ocs_log_info(NULL, "%s: ocs%d nport online failed.\n",
//				__func__, args->port_handle);
//		}
	}

	free(args);
}

int
ocs_nvme_nport_create(ocs_sport_t *sport)
{
	ocs_t *ocs = sport->ocs;
	spdk_nvmf_bcm_fc_nport_create_args_t *args;
	spdk_err_t rc;

	args = ocs_malloc(NULL, sizeof(spdk_nvmf_bcm_fc_nport_create_args_t),
					  OCS_M_ZERO);
	if (!args) {
		goto err;
	}

	args->port_handle = ocs->instance_index;

	/* We register physical port as nport. Real nports not supported yet. */
	args->nport_handle = 0;
	args->fc_nodename.u.wwn = ocs_get_wwn(&ocs->hal, OCS_HAL_WWN_NODE);
	args->fc_portname.u.wwn = ocs_get_wwn(&ocs->hal, OCS_HAL_WWN_PORT);
	args->subsys_id	= 1; /* Use only first subsystem ID. */
	args->d_id = sport->fc_id;
	args->cb_ctx = args;

	if (g_nvmf_fc_ops) { /* Post to master */
		rc = g_nvmf_fc_ops->enqueue_event(SPDK_FC_NPORT_CREATE, (void *) args,
			ocs_cb_nport_create);
		if (rc) {
			goto err;
		}
	}

	return 0; /* Queued */
err:
	if (args) {
		free(args);
	}

	return -1;
}

/* HW Port */

void
ocs_hw_port_cleanup(ocs_t *ocs)
{
	if (ocs && ocs->tgt_ocs.args) {
		int i;
		spdk_nvmf_bcm_fc_hw_port_init_args_t *args = ocs->tgt_ocs.args;

		ocs_free_nvme_buffers(args->ls_queue.wq.buffer);
		ocs_free_nvme_buffers(args->ls_queue.rq_hdr.buffer);
		ocs_free_nvme_buffers(args->ls_queue.rq_payload.buffer);

		for (i = 0; i < NVMF_FC_MAX_IO_QUEUES; i ++) {
			ocs_free_nvme_buffers(args->io_queues[i].wq.buffer);
			ocs_free_nvme_buffers(args->io_queues[i].rq_hdr.buffer);
			ocs_free_nvme_buffers(args->io_queues[i].rq_payload.buffer);
		}

		free(args);
		ocs->tgt_ocs.args = NULL;
	}
}

static void
ocs_cb_hw_port_create(uint8_t port_handle, spdk_fc_event_t event_type,
	void *ctx, spdk_err_t err)
{
	spdk_nvmf_bcm_fc_hw_port_init_args_t *args = ctx;
	ocs_t *ocs = NULL;

	if (err) {
		ocs_log_err(NULL, "%s: ocs%d port create failed.\n",
				__func__, args->port_handle);
		free(args);
	} else {
		ocs_log_info(NULL, "%s: ocs%d port create success.\n",
				__func__, args->port_handle);

		ocs = ocs_get_instance(args->port_handle);
		if (ocs) {
			ocs->tgt_ocs.args = args; /* Save this for cleanup. */
		}
	}

}

/* 
 * This is code is based on netapp queue topology assumptions
 * RQ0 = SCSI + ELS
 * RQ1 = NVME LS
 * RQ2 - RQN = NVME IOs.
 */
int
ocs_nvme_hw_port_create(ocs_t *ocs)
{
	int i;
	ocs_hal_t *hal = &ocs->hal;
	spdk_nvmf_bcm_fc_hw_port_init_args_t *args;
	spdk_err_t rc;

	ocs->tgt_ocs.args = NULL;

	args = ocs_malloc(NULL, sizeof(spdk_nvmf_bcm_fc_hw_port_init_args_t), OCS_M_ZERO);
	if (!args) {
		goto error;
	}

	args->port_handle = ocs->instance_index;
	args->xri_base =
		*(ocs->hal.sli.config.extent[SLI_RSRC_FCOE_XRI].base) +
		ocs->hal.sli.config.extent[SLI_RSRC_FCOE_XRI].size;
	args->xri_count = ocs->hal.sli.config.extent[SLI_RSRC_FCOE_XRI].size;
	args->cb_ctx = args;

	/* Fill NVME LS event queues. */
	ocs_fill_nvme_sli_queue(ocs, hal->hal_eq[1]->queue,
			&args->ls_queue.eq.q);
	ocs_fill_nvme_sli_queue(ocs, hal->hal_wq[1]->cq->queue,
			&args->ls_queue.cq_wq.q);
	ocs_fill_nvme_sli_queue(ocs, hal->hal_rq[1]->cq->queue,
			&args->ls_queue.cq_rq.q);

	/* LS WQ */
	ocs_fill_nvme_sli_queue(ocs, hal->hal_wq[1]->queue,
			&args->ls_queue.wq.q);

	/* LS RQ Hdr */
	ocs_fill_nvme_sli_queue(ocs, hal->hal_rq[1]->hdr,
			&args->ls_queue.rq_hdr.q);
	args->ls_queue.rq_hdr.buffer =
		ocs_alloc_nvme_buffers(OCS_HAL_RQ_SIZE_HDR,
				args->ls_queue.rq_hdr.q.max_entries);
	if (!args->ls_queue.rq_hdr.buffer) {
		goto error;
	}
	args->ls_queue.rq_hdr.num_buffers = args->ls_queue.rq_hdr.q.max_entries;


	/* LS RQ Payload */
	ocs_fill_nvme_sli_queue(ocs, hal->hal_rq[1]->data,
			&args->ls_queue.rq_payload.q);
	args->ls_queue.rq_payload.buffer =
		ocs_alloc_nvme_buffers(OCS_HAL_RQ_SIZE_PAYLOAD,
				args->ls_queue.rq_payload.q.max_entries);
	if (!args->ls_queue.rq_payload.buffer) {
		goto error;
	}
	args->ls_queue.rq_payload.num_buffers =
		args->ls_queue.rq_payload.q.max_entries;

	for (i = 0; i < NVMF_FC_MAX_IO_QUEUES; i ++) {
		ocs_fill_nvme_sli_queue(ocs, hal->hal_eq[i + 2]->queue,
				&args->io_queues[i].eq.q);
		ocs_fill_nvme_sli_queue(ocs, hal->hal_wq[i + 2]->cq->queue,
				&args->io_queues[i].cq_wq.q);
		ocs_fill_nvme_sli_queue(ocs, hal->hal_rq[i + 2]->cq->queue,
				&args->io_queues[i].cq_rq.q);

		/* IO WQ */
		ocs_fill_nvme_sli_queue(ocs, hal->hal_wq[i + 2]->queue,
				&args->io_queues[i].wq.q);

		/* IO RQ Hdr */
		ocs_fill_nvme_sli_queue(ocs, hal->hal_rq[i + 2]->hdr,
				&args->io_queues[i].rq_hdr.q);
		args->io_queues[i].rq_hdr.buffer =
			ocs_alloc_nvme_buffers(OCS_HAL_RQ_SIZE_HDR,
					args->io_queues[i].rq_hdr.q.max_entries);
		if (!args->io_queues[i].rq_hdr.buffer) {
			goto error;
		}
		args->io_queues[i].rq_hdr.num_buffers =
			args->io_queues[i].rq_hdr.q.max_entries;


		/* IO RQ Payload */
		ocs_fill_nvme_sli_queue(ocs, hal->hal_rq[i + 2]->data,
				&args->io_queues[i].rq_payload.q);
		args->io_queues[i].rq_payload.buffer =
			ocs_alloc_nvme_buffers(OCS_HAL_RQ_SIZE_PAYLOAD,
					args->io_queues[i].rq_payload.q.max_entries);
		if (!args->io_queues[i].rq_payload.buffer) {
			goto error;
		}
		args->io_queues[i].rq_payload.num_buffers =
			args->io_queues[i].rq_payload.q.max_entries;

		args->io_queue_cnt ++;
	}

	if (g_nvmf_fc_ops) { /* Post to master */
		rc = g_nvmf_fc_ops->enqueue_event(SPDK_FC_HW_PORT_INIT, args,
			ocs_cb_hw_port_create);
		if (rc) {
			goto error;
		}
	}

	return 0; /* Queued */
error:
	if (args) {
		ocs_free_nvme_buffers(args->ls_queue.rq_hdr.buffer);
		ocs_free_nvme_buffers(args->ls_queue.rq_payload.buffer);

		for (i = 0; i < NVMF_FC_MAX_IO_QUEUES; i ++) {
			ocs_free_nvme_buffers(args->io_queues[i].rq_hdr.buffer);
			ocs_free_nvme_buffers(args->io_queues[i].rq_payload.buffer);
		}
		free(args);
	}
	return -1;

}

struct port_cb_ctx {
	ocs_sport_t *sport;
	void *args;
};

static void
ocs_cb_hw_port_online(uint8_t port_handle, spdk_fc_event_t event_type,
	void *in, spdk_err_t err)
{
	struct port_cb_ctx *ctx = in;
	spdk_nvmf_bcm_fc_hw_port_online_args_t *args = ctx->args;

	if (err) {
		ocs_log_err(NULL, "%s: ocs%d online failed.\n",
				__func__, args->port_handle);
	} else {
		ocs_log_info(NULL, "%s: ocs%d online success.\n",
				__func__, args->port_handle);

		/* Create dummy NPORT for this physical port. */
		if (ocs_nvme_nport_create(ctx->sport)) {
			ocs_log_err(NULL, "%s: ocs%d nport create failed.\n",
				__func__, args->port_handle);
		}
	}

	free(ctx->args);
	free(ctx);
}

int
ocs_nvme_process_hw_port_online(ocs_sport_t *sport)
{
	ocs_t *ocs = sport->ocs;
	spdk_nvmf_bcm_fc_hw_port_online_args_t *args;
	spdk_err_t rc;
	struct port_cb_ctx *ctx = NULL;

	args = ocs_malloc(NULL, sizeof(spdk_nvmf_bcm_fc_hw_port_online_args_t),
					  OCS_M_ZERO);
	if (!args) {
		goto err;
	}
	args->port_handle = ocs->instance_index;

	ctx = ocs_malloc(NULL, sizeof(struct port_cb_ctx), OCS_M_ZERO);
	if (!ctx) {
		goto err;
	}
	ctx->args = args;
	ctx->sport = sport;
	args->cb_ctx = ctx;

	if (g_nvmf_fc_ops) { /* Post to master */
		rc = g_nvmf_fc_ops->enqueue_event(SPDK_FC_HW_PORT_ONLINE, args,
			ocs_cb_hw_port_online);
		if (rc) {
			goto err;
		}
	}

	return 0; /* Queued */
err:
	if (args) {
		free(args);
	}

	if (ctx) {
		free(ctx);
	}
	return -1;
}

static void
ocs_cb_hw_port_offline(uint8_t port_handle, spdk_fc_event_t event_type, 
	void *ctx, spdk_err_t err)
{
	spdk_nvmf_bcm_fc_hw_port_offline_args_t *args = ctx;

	if (err) {
		ocs_log_err(NULL, "%s: ocs%d offline failed.\n",
				__func__, args->port_handle);
	} else {
		ocs_log_err(NULL, "%s: ocs%d offline success.\n",
				__func__, args->port_handle);
	}

	free(args);
}

int
ocs_nvme_process_hw_port_offline(ocs_sport_t *sport)
{
	ocs_t *ocs = sport->ocs;
	spdk_nvmf_bcm_fc_hw_port_offline_args_t *args;
	spdk_err_t rc;

	args = ocs_malloc(NULL, sizeof(spdk_nvmf_bcm_fc_hw_port_offline_args_t),
					  OCS_M_ZERO);
	if (!args) {
		goto err;
	}

	args->port_handle = ocs->instance_index;
	args->cb_ctx = args;

	if (g_nvmf_fc_ops) { /* Post to master */
		rc = g_nvmf_fc_ops->enqueue_event(SPDK_FC_HW_PORT_OFFLINE,
			args, ocs_cb_hw_port_offline);
		if (rc) {
			goto err;
		}
	}

	return 0; /* Queued */
err:
	if (args) {
		free(args);
	}

	return -1;
}

static void
ocs_cb_abts_cb(uint8_t port_handle, spdk_fc_event_t event_type,
	void *ctx, spdk_err_t err)
{
	free(ctx);
}

int
ocs_nvme_process_abts(uint16_t oxid, uint16_t rxid, uint32_t rpi)
{
	spdk_nvmf_bcm_fc_abts_args_t *args;
	spdk_err_t rc;

	args = ocs_malloc(NULL, sizeof(spdk_nvmf_bcm_fc_abts_args_t), OCS_M_ZERO);
	if (!args) {
		goto err;
	}

	args->port_handle = 0;
	args->nport_handle = 0;
	args->oxid = oxid;
	args->rxid = rxid;
	args->rpi = rpi;
	args->cb_ctx = args;

	if (g_nvmf_fc_ops) { /* Post to master */
		rc = g_nvmf_fc_ops->enqueue_event(SPDK_FC_ABTS_RECV, args,
			ocs_cb_abts_cb);
		if (rc) {
			goto err;
		}
	}

	return 0; /* Queued */
err:
	if (args) {
		free(args);
	}

	return -1;
}

/* IT nexus */
struct prl_cb_ctx {
	ocs_io_t *io;
	uint16_t ox_id;
	void *args;
	uint32_t lcore_id;
	spdk_event_fn cb_func;
	spdk_err_t err;
};

static void
ocs_cb_prli_plrlo(uint8_t port_handle, spdk_fc_event_t event_type,
	void *in, spdk_err_t err)
{
	struct spdk_event *event;
	struct prl_cb_ctx *ctx = in;

	ctx->err = err;

	/* Schedule this on the request lcore */
	event = spdk_event_allocate(ctx->lcore_id, ctx->cb_func, in, NULL);
	spdk_event_call(event);
}


static void
ocs_cb_prli(void *arg1, void *arg2)
{
	struct prl_cb_ctx *ctx = arg1;
	spdk_nvmf_bcm_fc_hw_i_t_add_args_t *args = ctx->args;

	if (!ctx->err) {
		ocs_log_info(NULL, "%s: ocs%d NVME PRLI accepted.\n",
				__func__, args->port_handle);
		ocs_send_prli_acc(ctx->io, ctx->ox_id, FC_TYPE_NVME, NULL, NULL);
	} else {
		ocs_log_info(NULL, "%s: ocs%d NVME PRLI rejected.\n",
				__func__, args->port_handle);
		ocs_send_ls_rjt(ctx->io, ctx->ox_id, FC_REASON_UNABLE_TO_PERFORM,
				FC_EXPL_NO_ADDITIONAL,	0, NULL, NULL);
	}

	free(ctx->args);
	free(ctx);
}

int
ocs_nvme_process_prli(ocs_io_t *io, uint16_t ox_id)
{
	ocs_node_t *node;
	spdk_nvmf_bcm_fc_hw_i_t_add_args_t  *args = NULL;
	struct prl_cb_ctx *ctx = NULL;
	spdk_err_t rc;

	if (!io || !(node = io->node)) {
		goto err;
	}

	args = ocs_malloc(NULL, sizeof(spdk_nvmf_bcm_fc_hw_i_t_add_args_t),
					  OCS_M_ZERO);
	if (!args) {
		goto err;
	}

	ctx = ocs_malloc(NULL, sizeof(struct prl_cb_ctx), OCS_M_ZERO);
	if (!ctx) {
		goto err;
	}
	ctx->args = args;
	ctx->io   = io;
	ctx->ox_id = ox_id;
	ctx->lcore_id = spdk_env_get_current_core();
	ctx->cb_func = ocs_cb_prli;

	args->port_handle  = node->ocs->instance_index;
	args->nport_handle = node->sport->instance_index;
	args->rpi = node->rnode.indicator;
	args->s_id = node->rnode.fc_id;
	args->fc_nodename.u.wwn = ocs_node_get_wwnn(node);
	args->fc_portname.u.wwn = ocs_node_get_wwpn(node);
	args->target_prli_info   = node->nvme_prli_service_params;

	args->cb_ctx = ctx;

	if (g_nvmf_fc_ops) { /* Post to master */
		rc = g_nvmf_fc_ops->enqueue_event(SPDK_FC_IT_ADD, args,
			ocs_cb_prli_plrlo);
		if (rc) {
			goto err;
		}
	}

	return 0; /* Queued */
err:
	if (args) {
		free(args);
	}
	if (ctx) {
		free(ctx);
	}
	return -1;
}


static void
ocs_cb_prlo(void *arg1, void *arg2)
{
	struct prl_cb_ctx *ctx = arg1;

	if (!ctx->err) {
		ocs_send_prlo_acc(ctx->io, ctx->ox_id, FC_TYPE_NVME, NULL, NULL);
		/* Check if we need to do node cleanup to free RPI. */
	} else {
		ocs_send_ls_rjt(ctx->io, ctx->ox_id, FC_REASON_UNABLE_TO_PERFORM,
				FC_EXPL_NO_ADDITIONAL,	0, NULL, NULL);
	}

	free(ctx->args);
	free(ctx);
}

int
ocs_nvme_process_prlo(ocs_io_t *io, uint16_t ox_id)
{
	ocs_node_t *node;
	spdk_nvmf_bcm_fc_hw_i_t_delete_args_t *args = NULL;
	struct prl_cb_ctx *ctx = NULL;
	spdk_err_t rc;

	if (!io || !(node = io->node)) {
		goto err;
	}

	args = ocs_malloc(NULL, sizeof(spdk_nvmf_bcm_fc_hw_i_t_delete_args_t),
					  OCS_M_ZERO);
	if (!args) {
		goto err;
	}

	ctx = ocs_malloc(NULL, sizeof(struct prl_cb_ctx), OCS_M_ZERO);
	if (!ctx) {
		goto err;
	}
	ctx->args = args;
	ctx->io   = io;
	ctx->ox_id = ox_id;
	ctx->lcore_id = spdk_env_get_current_core();
	ctx->cb_func = ocs_cb_prlo;

	args->port_handle  = node->ocs->instance_index;
	args->nport_handle = node->sport->instance_index;
	args->rpi = node->rnode.indicator;
	args->s_id = node->rnode.fc_id;
	args->cb_ctx = ctx;

	if (g_nvmf_fc_ops) { /* Post to master */
		rc = g_nvmf_fc_ops->enqueue_event(SPDK_FC_IT_DELETE, args,
			ocs_cb_prli_plrlo);
		if (rc) {
			goto err;
		}
	}

	return 0; /* Queued */
err:
	if (args) {
		free(args);
	}
	if (ctx) {
		free(ctx);
	}
	return -1;
}
