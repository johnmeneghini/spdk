/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *	 notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *	 notice, this list of conditions and the following disclaimer in
 *	 the documentation and/or other materials provided with the
 *	 distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *	 contributors may be used to endorse or promote products derived
 *	 from this software without specific prior written permission.
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
#include "spdk/env.h"

#include <string.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_memzone.h>
#include <rte_version.h>

extern struct rte_tailq_elem rte_ring_tailq;

/**
 * Enqueue several objects on the ring (multi-producers safe).
 *
 */
int
spdk_ring_mp_enqueue_bulk(struct spdk_ring *sr, void *const *obj_table,
			  unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_mp_enqueue_bulk(r, obj_table, n);
}

/**
 * Enqueue several objects on a spdk ring (NOT multi-producers safe).
 *
 */
int
spdk_ring_sp_enqueue_bulk(struct spdk_ring *sr, void *const *obj_table,
			  unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_sp_enqueue_bulk(r, obj_table, n);
}

/**
 * Enqueue several objects on a spdk ring.
 *
 * This function calls the multi-producer or the single-producer
 * version depending on the default behavior that was specified at
 * ring creation time (see flags).
 *
 */
int
spdk_ring_enqueue_bulk(struct spdk_ring *sr, void *const *obj_table,
		       unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_enqueue_bulk(r, obj_table, n);
}

/**
 * Enqueue one object on a spdk ring (multi-producers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * producer index atomically.
 *
 */
int
spdk_ring_mp_enqueue(struct spdk_ring *sr, void *obj)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_mp_enqueue(r, obj);
}

/**
 * Enqueue one object on a spdk ring (NOT multi-producers safe).
 *
 */
int
spdk_ring_sp_enqueue(struct spdk_ring *sr, void *obj)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_sp_enqueue(r, obj);
}

/**
 * Enqueue one object on a spdk ring.
 *
 * This function calls the multi-producer or the single-producer
 * version, depending on the default behaviour that was specified at
 * ring creation time (see flags).
 *
 */
int
spdk_ring_enqueue(struct spdk_ring *sr, void *obj)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_enqueue(r, obj);
}

/**
 * Dequeue several objects from a spdk	ring (multi-consumers safe).
 *
 */
int
spdk_ring_mc_dequeue_bulk(struct spdk_ring *sr, void **obj_table, unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_mc_dequeue_bulk(r, obj_table, n);
}

/**
 * Dequeue several objects from a spdk ring (NOT multi-consumers safe).
 *
 */
int
spdk_ring_sc_dequeue_bulk(struct spdk_ring *sr, void **obj_table, unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_sc_dequeue_bulk(r, obj_table, n);
}

/**
 * Dequeue several objects from a spdk ring.
 *
 * This function calls the multi-consumers or the single-consumer
 * version, depending on the default behaviour that was specified at
 * ring creation time (see flags).
 *
 */
int
spdk_ring_dequeue_bulk(struct spdk_ring *sr, void **obj_table, unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_dequeue_bulk(r, obj_table, n);
}

/**
 * Dequeue one object from a spdk ring (multi-consumers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * consumer index atomically.
 *
 */
int
spdk_ring_mc_dequeue(struct spdk_ring *sr, void **obj_p)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_mc_dequeue(r, obj_p);
}

/**
 * Dequeue one object from a spdk ring (NOT multi-consumers safe).
 *
 */
int
spdk_ring_sc_dequeue(struct spdk_ring *sr, void **obj_p)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_sc_dequeue(r, obj_p);
}

/**
 * Dequeue one object from a spdk ring.
 *
 * This function calls the multi-consumers or the single-consumer
 * version depending on the default behaviour that was specified at
 * ring creation time (see flags).
 *
 */
int
spdk_ring_dequeue(struct spdk_ring *sr, void **obj_p)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_dequeue(r, obj_p);
}

/**
 * Test if a spdk ring is full.
 *
 */
int
spdk_ring_full(const struct spdk_ring *sr)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_full(r);
}

/**
 * Test if a spdk ring is empty.
 *
 */
int
spdk_ring_empty(const struct spdk_ring *sr)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_empty(r);
}

/**
 * Return the number of entries in a spdk ring.
 *
 */
unsigned
spdk_ring_count(const struct spdk_ring *sr)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_count(r);
}

/**
 * Return the number of free entries in a spdk ring.
 *
 */
unsigned
spdk_ring_free_count(const struct spdk_ring *sr)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_free_count(r);
}

/**
 * Enqueue several objects on the ring (multi-producers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * producer index atomically.
 *
 */
unsigned
spdk_ring_mp_enqueue_burst(struct spdk_ring *sr, void *const *obj_table,
			   unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_mp_enqueue_burst(r, obj_table, n);
}

/**
 * Enqueue several objects on a spdk ring (NOT multi-producers safe).
 *
 */
unsigned
spdk_ring_sp_enqueue_burst(struct spdk_ring *sr, void *const *obj_table,
			   unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_sp_enqueue_burst(r, obj_table, n);
}

/**
 * Enqueue several objects on a spdk ring.
 *
 * This function calls the multi-producer or the single-producer
 * version depending on the default behavior that was specified at
 * ring creation time (see flags).
 *
 */
unsigned
spdk_ring_enqueue_burst(struct spdk_ring *sr, void *const *obj_table,
			unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_enqueue_burst(r, obj_table, n);
}

/**
 * Dequeue several objects from a spdk ring (multi-consumers safe). When the request
 * objects are more than the available objects, only dequeue the actual number
 * of objects
 *
 * This function uses a "compare and set" instruction to move the
 * consumer index atomically.
 *
 */
unsigned
spdk_ring_mc_dequeue_burst(struct spdk_ring *sr, void **obj_table, unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_mc_dequeue_burst(r, obj_table, n);
}

/**
 * Dequeue several objects from a spdk ring (NOT multi-consumers safe).When the
 * request objects are more than the available objects, only dequeue the
 * actual number of objects
 */
unsigned
spdk_ring_sc_dequeue_burst(struct spdk_ring *sr, void **obj_table, unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_sc_dequeue_burst(r, obj_table, n);
}

/**
 * Dequeue multiple objects from a spdk ring up to a maximum number.
 *
 * This function calls the multi-consumers or the single-consumer
 * version, depending on the default behaviour that was specified at
 * ring creation time (see flags).
 */
unsigned
spdk_ring_dequeue_burst(struct spdk_ring *sr, void **obj_table, unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_dequeue_burst(r, obj_table, n);
}


/**
 * Return the size of memory occupied by a ring
 */
ssize_t
spdk_ring_get_memsize(unsigned count)
{
	return rte_ring_get_memsize(count);
}

/**
 * Create a ring
 */
struct spdk_ring *
spdk_ring_create(const char *name, unsigned count, int socket_id,
		 unsigned flags)
{
	return (struct spdk_ring *) rte_ring_create(name, count, socket_id, flags);
}

/**
 * Free the ring
 */
void
spdk_ring_free(struct spdk_ring *sr)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	rte_ring_free(r);
}

/**
 * Change the high water mark. If *count* is 0, water marking is
 * disabled
 */
int
spdk_ring_set_water_mark(struct spdk_ring *sr, unsigned count)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_set_water_mark(r, count);
}

/**
 *  Dump the status of the ring on the console
 */
void
spdk_ring_dump(FILE *f, const struct spdk_ring *sr)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	rte_ring_dump(f, r);
}

/**
 * Dump the status of all rings on the console
 */
void
spdk_ring_list_dump(FILE *f)
{
	rte_ring_list_dump(f);
}

/**
 * Search a ring from its name
 */
struct spdk_ring *
spdk_ring_lookup(const char *name)
{
	return (struct spdk_ring *)rte_ring_lookup(name);
}
