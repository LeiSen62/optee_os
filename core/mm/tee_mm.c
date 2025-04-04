// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2014, STMicroelectronics International N.V.
 */

#include <kernel/panic.h>
#include <kernel/spinlock.h>
#include <kernel/tee_common.h>
#include <mm/tee_mm.h>
#include <mm/tee_pager.h>
#include <pta_stats.h>
#include <trace.h>
#include <util.h>

bool tee_mm_init(tee_mm_pool_t *pool, paddr_t lo, paddr_size_t size,
		 uint8_t shift, uint32_t flags)
{
	paddr_size_t rounded = 0;
	paddr_t initial_lo = lo;

	if (pool == NULL)
		return false;

	lo = ROUNDUP2(lo, 1 << shift);
	rounded = lo - initial_lo;
	size = ROUNDDOWN2(size - rounded, 1 << shift);

	assert(((uint64_t)size >> shift) < (uint64_t)UINT32_MAX);

	*pool = (tee_mm_pool_t){
		.lo = lo,
		.size = size,
		.shift = shift,
		.flags = flags,
	};

	pool->entry = malloc_flags(pool->flags | MAF_ZERO_INIT, NULL,
				   MALLOC_DEFAULT_ALIGNMENT,
				   sizeof(tee_mm_entry_t));
	if (pool->entry == NULL)
		return false;

	if (pool->flags & TEE_MM_POOL_HI_ALLOC)
		pool->entry->offset = ((size - 1) >> shift) + 1;

	pool->entry->pool = pool;
	pool->lock = SPINLOCK_UNLOCK;

	return true;
}

void tee_mm_final(tee_mm_pool_t *pool)
{
	if (pool == NULL || pool->entry == NULL)
		return;

	while (pool->entry->next != NULL)
		tee_mm_free(pool->entry->next);
	free_flags(pool->flags, pool->entry);
	pool->entry = NULL;
}

static void tee_mm_add(tee_mm_entry_t *p, tee_mm_entry_t *nn)
{
	/* add to list */
	nn->next = p->next;
	p->next = nn;
}

#ifdef CFG_WITH_STATS
static size_t tee_mm_stats_allocated(tee_mm_pool_t *pool)
{
	tee_mm_entry_t *entry;
	uint32_t sz = 0;

	if (!pool)
		return 0;

	entry = pool->entry;
	while (entry) {
		sz += entry->size;
		entry = entry->next;
	}

	return sz << pool->shift;
}

void tee_mm_get_pool_stats(tee_mm_pool_t *pool, struct pta_stats_alloc *stats,
			   bool reset)
{
	uint32_t exceptions;

	if (!pool)
		return;

	memset(stats, 0, sizeof(*stats));

	exceptions = cpu_spin_lock_xsave(&pool->lock);

	stats->size = pool->size;
	stats->max_allocated = pool->max_allocated;
	stats->allocated = tee_mm_stats_allocated(pool);

	if (reset)
		pool->max_allocated = 0;
	cpu_spin_unlock_xrestore(&pool->lock, exceptions);
}

static void update_max_allocated(tee_mm_pool_t *pool)
{
	size_t sz = tee_mm_stats_allocated(pool);

	if (sz > pool->max_allocated)
		pool->max_allocated = sz;
}
#else /* CFG_WITH_STATS */
static inline void update_max_allocated(tee_mm_pool_t *pool __unused)
{
}
#endif /* CFG_WITH_STATS */

tee_mm_entry_t *tee_mm_alloc_flags(tee_mm_pool_t *pool, size_t size,
				   uint32_t flags)
{
	size_t psize = 0;
	tee_mm_entry_t *entry = NULL;
	tee_mm_entry_t *nn = NULL;
	size_t remaining = 0;
	uint32_t exceptions = 0;

	/* Check that pool is initialized */
	if (!pool || !pool->entry)
		return NULL;

	flags &= ~MAF_NEX;	/* This flag must come from pool->flags */
	flags |= pool->flags;
	nn  = malloc_flags(flags, NULL, MALLOC_DEFAULT_ALIGNMENT,
			   sizeof(tee_mm_entry_t));
	if (!nn)
		return NULL;

	exceptions = cpu_spin_lock_xsave(&pool->lock);

	entry = pool->entry;
	if (!size)
		psize = 0;
	else
		psize = ((size - 1) >> pool->shift) + 1;

	/* find free slot */
	if (pool->flags & TEE_MM_POOL_HI_ALLOC) {
		while (entry->next != NULL && psize >
		       (entry->offset - entry->next->offset -
			entry->next->size))
			entry = entry->next;
	} else {
		while (entry->next != NULL && psize >
		       (entry->next->offset - entry->size - entry->offset))
			entry = entry->next;
	}

	/* check if we have enough memory */
	if (entry->next == NULL) {
		if (pool->flags & TEE_MM_POOL_HI_ALLOC) {
			/*
			 * entry->offset is a "block count" offset from
			 * pool->lo. The byte offset is
			 * (entry->offset << pool->shift).
			 * In the HI_ALLOC allocation scheme the memory is
			 * allocated from the end of the segment, thus to
			 * validate there is sufficient memory validate that
			 * (entry->offset << pool->shift) > size.
			 */
			if ((entry->offset << pool->shift) < size) {
				/* out of memory */
				goto err;
			}
		} else {
			if (!pool->size)
				panic("invalid pool");

			remaining = pool->size;
			remaining -= ((entry->offset + entry->size) <<
				      pool->shift);

			if (remaining < size) {
				/* out of memory */
				goto err;
			}
		}
	}

	tee_mm_add(entry, nn);

	if (pool->flags & TEE_MM_POOL_HI_ALLOC)
		nn->offset = entry->offset - psize;
	else
		nn->offset = entry->offset + entry->size;
	nn->size = psize;
	nn->pool = pool;

	update_max_allocated(pool);

	cpu_spin_unlock_xrestore(&pool->lock, exceptions);
	return nn;
err:
	cpu_spin_unlock_xrestore(&pool->lock, exceptions);
	free_flags(flags, nn);
	return NULL;
}

static inline bool fit_in_gap(tee_mm_pool_t *pool, tee_mm_entry_t *e,
			      paddr_t offslo, paddr_t offshi)
{
	if (pool->flags & TEE_MM_POOL_HI_ALLOC) {
		if (offshi > e->offset ||
		    (e->next != NULL &&
		     (offslo < e->next->offset + e->next->size)) ||
		    (offshi << pool->shift) - 1 > pool->size)
			/* memory not available */
			return false;
	} else {
		if (offslo < (e->offset + e->size) ||
		    (e->next != NULL && (offshi > e->next->offset)) ||
		    (offshi << pool->shift) > pool->size)
			/* memory not available */
			return false;
	}

	return true;
}

tee_mm_entry_t *tee_mm_alloc2(tee_mm_pool_t *pool, paddr_t base, size_t size)
{
	tee_mm_entry_t *entry;
	paddr_t offslo;
	paddr_t offshi;
	tee_mm_entry_t *mm;
	uint32_t exceptions;

	/* Check that pool is initialized */
	if (!pool || !pool->entry)
		return NULL;

	/* Wrapping and sanity check */
	if ((base + size) < base || base < pool->lo)
		return NULL;

	mm  = malloc_flags(pool->flags, NULL, MALLOC_DEFAULT_ALIGNMENT,
			   sizeof(tee_mm_entry_t));
	if (!mm)
		return NULL;

	exceptions = cpu_spin_lock_xsave(&pool->lock);

	entry = pool->entry;
	offslo = (base - pool->lo) >> pool->shift;
	offshi = ((base - pool->lo + size - 1) >> pool->shift) + 1;

	/* find slot */
	if (pool->flags & TEE_MM_POOL_HI_ALLOC) {
		while (entry->next != NULL &&
		       offshi < entry->next->offset + entry->next->size)
			entry = entry->next;
	} else {
		while (entry->next != NULL && offslo > entry->next->offset)
			entry = entry->next;
	}

	/* Check that memory is available */
	if (!fit_in_gap(pool, entry, offslo, offshi))
		goto err;

	tee_mm_add(entry, mm);

	mm->offset = offslo;
	mm->size = offshi - offslo;
	mm->pool = pool;

	update_max_allocated(pool);
	cpu_spin_unlock_xrestore(&pool->lock, exceptions);
	return mm;
err:
	cpu_spin_unlock_xrestore(&pool->lock, exceptions);
	free_flags(pool->flags, mm);
	return NULL;
}

void tee_mm_free(tee_mm_entry_t *p)
{
	tee_mm_entry_t *entry;
	uint32_t exceptions;

	if (!p || !p->pool)
		return;

	exceptions = cpu_spin_lock_xsave(&p->pool->lock);
	entry = p->pool->entry;

	/* remove entry from list */
	while (entry->next != NULL && entry->next != p)
		entry = entry->next;

	if (!entry->next)
		panic("invalid mm_entry");

	entry->next = entry->next->next;
	cpu_spin_unlock_xrestore(&p->pool->lock, exceptions);

	free_flags(p->pool->flags, p);
}

size_t tee_mm_get_bytes(const tee_mm_entry_t *mm)
{
	if (!mm || !mm->pool)
		return 0;
	else
		return mm->size << mm->pool->shift;
}

bool tee_mm_addr_is_within_range(const tee_mm_pool_t *pool, paddr_t addr)
{
	return pool && addr >= pool->lo &&
		addr <= (pool->lo + (pool->size - 1));
}

bool tee_mm_is_empty(tee_mm_pool_t *pool)
{
	bool ret;
	uint32_t exceptions;

	if (pool == NULL || pool->entry == NULL)
		return true;

	exceptions = cpu_spin_lock_xsave(&pool->lock);
	ret = pool->entry == NULL || pool->entry->next == NULL;
	cpu_spin_unlock_xrestore(&pool->lock, exceptions);

	return ret;
}

tee_mm_entry_t *tee_mm_find(const tee_mm_pool_t *pool, paddr_t addr)
{
	tee_mm_entry_t *entry = pool->entry;
	uint16_t offset = (addr - pool->lo) >> pool->shift;
	uint32_t exceptions;

	if (!tee_mm_addr_is_within_range(pool, addr))
		return NULL;

	exceptions = cpu_spin_lock_xsave(&((tee_mm_pool_t *)pool)->lock);

	while (entry->next != NULL) {
		entry = entry->next;

		if ((offset >= entry->offset) &&
		    (offset < (entry->offset + entry->size))) {
			cpu_spin_unlock_xrestore(&((tee_mm_pool_t *)pool)->lock,
						 exceptions);
			return entry;
		}
	}

	cpu_spin_unlock_xrestore(&((tee_mm_pool_t *)pool)->lock, exceptions);
	return NULL;
}

uintptr_t tee_mm_get_smem(const tee_mm_entry_t *mm)
{
	return (mm->offset << mm->pool->shift) + mm->pool->lo;
}
