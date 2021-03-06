/*
 * Copyright (c) 2000-2011 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */
/*
 *	File:	kern/kalloc.c
 *	Author:	Avadis Tevanian, Jr.
 *	Date:	1985
 *
 *	General kernel memory allocator.  This allocator is designed
 *	to be used by the kernel to manage dynamic memory fast.
 */

#include <zone_debug.h>

#include <mach/boolean.h>
#include <mach/machine/vm_types.h>
#include <mach/vm_param.h>
#include <kern/misc_protos.h>
#include <kern/zalloc.h>
#include <kern/kalloc.h>
#include <kern/lock.h>
#include <kern/ledger.h>
#include <vm/vm_kern.h>
#include <vm/vm_object.h>
#include <vm/vm_map.h>
#include <libkern/OSMalloc.h>

#ifdef MACH_BSD
zone_t kalloc_zone(vm_size_t);
#endif

#define KALLOC_MAP_SIZE_MIN  (16 * 1024 * 1024)
#define KALLOC_MAP_SIZE_MAX  (128 * 1024 * 1024)
vm_map_t kalloc_map;
vm_size_t kalloc_max;
vm_size_t kalloc_max_prerounded;
vm_size_t kalloc_kernmap_size;	/* size of kallocs that can come from kernel map */

unsigned int kalloc_large_inuse;
vm_size_t    kalloc_large_total;
vm_size_t    kalloc_large_max;
vm_size_t    kalloc_largest_allocated = 0;
uint64_t    kalloc_large_sum;

int	kalloc_fake_zone_index = -1; /* index of our fake zone in statistics arrays */

vm_offset_t	kalloc_map_min;
vm_offset_t	kalloc_map_max;

#ifdef	MUTEX_ZONE
/*
 * Diagnostic code to track mutexes separately rather than via the 2^ zones
 */
	zone_t		lck_mtx_zone;
#endif

static void
KALLOC_ZINFO_SALLOC(vm_size_t bytes)
{
	thread_t thr = current_thread();
	task_t task;
	zinfo_usage_t zinfo;

	ledger_debit(thr->t_ledger, task_ledgers.tkm_shared, bytes);

	if (kalloc_fake_zone_index != -1 && 
	    (task = thr->task) != NULL && (zinfo = task->tkm_zinfo) != NULL)
		zinfo[kalloc_fake_zone_index].alloc += bytes;
}

static void
KALLOC_ZINFO_SFREE(vm_size_t bytes)
{
	thread_t thr = current_thread();
	task_t task;
	zinfo_usage_t zinfo;

	ledger_credit(thr->t_ledger, task_ledgers.tkm_shared, bytes);

	if (kalloc_fake_zone_index != -1 && 
	    (task = thr->task) != NULL && (zinfo = task->tkm_zinfo) != NULL)
		zinfo[kalloc_fake_zone_index].free += bytes;
}

/*
 *	All allocations of size less than kalloc_max are rounded to the
 *	next nearest sized zone.  This allocator is built on top of
 *	the zone allocator.  A zone is created for each potential size
 *	that we are willing to get in small blocks.
 *
 *	We assume that kalloc_max is not greater than 64K;
 *
 *	Note that kalloc_max is somewhat confusingly named.
 *	It represents the first power of two for which no zone exists.
 *	kalloc_max_prerounded is the smallest allocation size, before
 *	rounding, for which no zone exists.
 *
 *	Also if the allocation size is more than kalloc_kernmap_size 
 *	then allocate from kernel map rather than kalloc_map.
 */

#if KALLOC_MINSIZE == 16 && KALLOC_LOG2_MINALIGN == 4

/*
 * "Legacy" aka "power-of-2" backing zones with 16-byte minimum
 * size and alignment.  Users of this profile would probably
 * benefit from some tuning.
 */

#define K_ZONE_SIZES			\
	16,				\
	32,				\
/* 6 */	64,				\
	128,				\
	256,				\
/* 9 */	512,				\
	1024,				\
	2048,				\
/* C */	4096


#define K_ZONE_NAMES			\
	"kalloc.16",			\
	"kalloc.32",			\
/* 6 */	"kalloc.64",			\
	"kalloc.128",			\
	"kalloc.256",			\
/* 9 */	"kalloc.512",			\
	"kalloc.1024",			\
	"kalloc.2048",			\
/* C */	"kalloc.4096"

#define K_ZONE_MAXIMA			\
	1024,				\
	4096,				\
/* 6 */	4096,				\
	4096,				\
	4096,				\
/* 9 */	1024,				\
	1024,				\
	1024,				\
/* C */	1024

#elif KALLOC_MINSIZE == 8 && KALLOC_LOG2_MINALIGN == 3

/*
 * Tweaked for ARM (and x64) in 04/2011
 */

#define K_ZONE_SIZES			\
/* 3 */	8,				\
	16,	24,			\
	32,	40,	48,		\
/* 6 */	64,	88,	112, 		\
	128, 	192,			\
	256, 	384,			\
/* 9 */	512,	768, 			\
	1024,	1536,			\
	2048,	3072,			\
	4096,	6144

#define K_ZONE_NAMES			\
/* 3 */	"kalloc.8",			\
	"kalloc.16",	"kalloc.24",	\
	"kalloc.32",	"kalloc.40",	"kalloc.48",	\
/* 6 */	"kalloc.64",	"kalloc.88",	"kalloc.112",	\
	"kalloc.128",	"kalloc.192",	\
	"kalloc.256",	"kalloc.384",	\
/* 9 */	"kalloc.512",	"kalloc.768",	\
	"kalloc.1024",	"kalloc.1536",	\
	"kalloc.2048",	"kalloc.3072",	\
	"kalloc.4096",	"kalloc.6144"

#define	K_ZONE_MAXIMA			\
/* 3 */	1024,				\
	1024,	1024,			\
	4096,	4096,	4096,		\
/* 6 */	4096,	4096,	4096,		\
	4096,	4096,			\
	4096,	4096,			\
/* 9 */	1024,	1024,			\
	1024,	1024,			\
	1024,	1024,			\
/* C */	1024,	64

#else
#error	missing zone size parameters for kalloc
#endif

#define KALLOC_MINALIGN (1 << KALLOC_LOG2_MINALIGN)

static const int k_zone_size[] = {
	K_ZONE_SIZES,
	8192,
	16384,
/* F */	32768
};

#define N_K_ZONE	(sizeof (k_zone_size) / sizeof (k_zone_size[0]))

/*
 * Many kalloc() allocations are for small structures containing a few
 * pointers and longs - the k_zone_dlut[] direct lookup table, indexed by
 * size normalized to the minimum alignment, finds the right zone index
 * for them in one dereference.
 */

#define INDEX_ZDLUT(size)	\
			(((size) + KALLOC_MINALIGN - 1) / KALLOC_MINALIGN)
#define N_K_ZDLUT	(2048 / KALLOC_MINALIGN)
				/* covers sizes [0 .. 2048 - KALLOC_MINALIGN] */
#define MAX_SIZE_ZDLUT	((N_K_ZDLUT - 1) * KALLOC_MINALIGN)

static int8_t k_zone_dlut[N_K_ZDLUT];	/* table of indices into k_zone[] */

/*
 * If there's no hit in the DLUT, then start searching from k_zindex_start.
 */
static int k_zindex_start;

static zone_t k_zone[N_K_ZONE];

static const char *k_zone_name[N_K_ZONE] = {
	K_ZONE_NAMES,
	"kalloc.8192",
	"kalloc.16384",
/* F */	"kalloc.32768"
};

/*
 *  Max number of elements per zone.  zinit rounds things up correctly
 *  Doing things this way permits each zone to have a different maximum size
 *  based on need, rather than just guessing; it also
 *  means its patchable in case you're wrong!
 */
unsigned int k_zone_max[N_K_ZONE] = {
	K_ZONE_MAXIMA,
	4096,
	64,
/* F */	64
};

/* #define KALLOC_DEBUG		1 */

/* forward declarations */
void * kalloc_canblock(
		vm_size_t	size,
		boolean_t	canblock);


lck_grp_t *kalloc_lck_grp;
lck_mtx_t kalloc_lock;

#define kalloc_spin_lock()	lck_mtx_lock_spin(&kalloc_lock)
#define kalloc_unlock()		lck_mtx_unlock(&kalloc_lock)


/* OSMalloc local data declarations */
static
queue_head_t    OSMalloc_tag_list;

lck_grp_t *OSMalloc_tag_lck_grp;
lck_mtx_t OSMalloc_tag_lock;

#define OSMalloc_tag_spin_lock()	lck_mtx_lock_spin(&OSMalloc_tag_lock)
#define OSMalloc_tag_unlock()		lck_mtx_unlock(&OSMalloc_tag_lock)


/* OSMalloc forward declarations */
void OSMalloc_init(void);
void OSMalloc_Tagref(OSMallocTag	tag);
void OSMalloc_Tagrele(OSMallocTag	tag);

/*
 *	Initialize the memory allocator.  This should be called only
 *	once on a system wide basis (i.e. first processor to get here
 *	does the initialization).
 *
 *	This initializes all of the zones.
 */

void
kalloc_init(
	void)
{
	kern_return_t retval;
	vm_offset_t min;
	vm_size_t size, kalloc_map_size;
	register int i;

	/* 
	 * Scale the kalloc_map_size to physical memory size: stay below 
	 * 1/8th the total zone map size, or 128 MB (for a 32-bit kernel).
	 */
	kalloc_map_size = (vm_size_t)(sane_size >> 5);
#if !__LP64__
	if (kalloc_map_size > KALLOC_MAP_SIZE_MAX)
		kalloc_map_size = KALLOC_MAP_SIZE_MAX;
#endif /* !__LP64__ */
	if (kalloc_map_size < KALLOC_MAP_SIZE_MIN)
		kalloc_map_size = KALLOC_MAP_SIZE_MIN;

	retval = kmem_suballoc(kernel_map, &min, kalloc_map_size,
			       FALSE, VM_FLAGS_ANYWHERE | VM_FLAGS_PERMANENT,
			       &kalloc_map);

	if (retval != KERN_SUCCESS)
		panic("kalloc_init: kmem_suballoc failed");

	kalloc_map_min = min;
	kalloc_map_max = min + kalloc_map_size - 1;

	/*
	 *	Ensure that zones up to size 8192 bytes exist.
	 *	This is desirable because messages are allocated
	 *	with kalloc, and messages up through size 8192 are common.
	 */

	if (PAGE_SIZE < 16*1024)
		kalloc_max = 16*1024;
	else
		kalloc_max = PAGE_SIZE;
	kalloc_max_prerounded = kalloc_max / 2 + 1;
	/* size it to be more than 16 times kalloc_max (256k) for allocations from kernel map */
	kalloc_kernmap_size = (kalloc_max * 16) + 1;
	kalloc_largest_allocated = kalloc_kernmap_size;

	/*
	 *	Allocate a zone for each size we are going to handle.
	 *	We specify non-paged memory.  Don't charge the caller
	 *	for the allocation, as we aren't sure how the memory
	 *	will be handled.
	 */
	for (i = 0; (size = k_zone_size[i]) < kalloc_max; i++) {
		k_zone[i] = zinit(size, k_zone_max[i] * size, size,
				  k_zone_name[i]);
		zone_change(k_zone[i], Z_CALLERACCT, FALSE);
	}

	/*
	 * Build the Direct LookUp Table for small allocations
	 */
	for (i = 0, size = 0; i <= N_K_ZDLUT; i++, size += KALLOC_MINALIGN) {
		int zindex = 0;

		while ((vm_size_t)k_zone_size[zindex] < size)
			zindex++;

		if (i == N_K_ZDLUT) {
			k_zindex_start = zindex;
			break;
		}
		k_zone_dlut[i] = (int8_t)zindex;
	}

#ifdef KALLOC_DEBUG
	printf("kalloc_init: k_zindex_start %d\n", k_zindex_start);

	/*
	 * Do a quick synthesis to see how well/badly we can
	 * find-a-zone for a given size.
	 * Useful when debugging/tweaking the array of zone sizes.
	 * Cache misses probably more critical than compare-branches!
	 */
	for (i = 0; i < (int)N_K_ZONE; i++) {
		vm_size_t testsize = (vm_size_t)k_zone_size[i] - 1;
		int compare = 0;
		int zindex;

		if (testsize < MAX_SIZE_ZDLUT) {
			compare += 1;	/* 'if' (T) */

			long dindex = INDEX_ZDLUT(testsize);
			zindex = (int)k_zone_dlut[dindex];

		} else if (testsize < kalloc_max_prerounded) {

			compare += 2;	/* 'if' (F), 'if' (T) */

			zindex = k_zindex_start;
			while ((vm_size_t)k_zone_size[zindex] < testsize) {
				zindex++;
				compare++;	/* 'while' (T) */
			}
			compare++;	/* 'while' (F) */
		} else
			break;	/* not zone-backed */

		zone_t z = k_zone[zindex];
		printf("kalloc_init: req size %4lu: %11s took %d compare%s\n",
		    (unsigned long)testsize, z->zone_name, compare,
		    compare == 1 ? "" : "s");
	}
#endif
	kalloc_lck_grp = lck_grp_alloc_init("kalloc.large", LCK_GRP_ATTR_NULL);
	lck_mtx_init(&kalloc_lock, kalloc_lck_grp, LCK_ATTR_NULL);
	OSMalloc_init();
#ifdef	MUTEX_ZONE	
	lck_mtx_zone = zinit(sizeof(struct _lck_mtx_), 1024*256, 4096, "lck_mtx");
#endif	
}

/*
 * Given an allocation size, return the kalloc zone it belongs to.
 * Direct LookUp Table variant.
 */
static __inline zone_t
get_zone_dlut(vm_size_t size)
{
	long dindex = INDEX_ZDLUT(size);
	int zindex = (int)k_zone_dlut[dindex];
	return (k_zone[zindex]);
}

/* As above, but linear search k_zone_size[] for the next zone that fits. */

static __inline zone_t
get_zone_search(vm_size_t size, int zindex)
{
	assert(size < kalloc_max_prerounded);

	while ((vm_size_t)k_zone_size[zindex] < size)
		zindex++;

	assert((unsigned)zindex < N_K_ZONE &&
	    (vm_size_t)k_zone_size[zindex] < kalloc_max);

	return (k_zone[zindex]);
}

void *
kalloc_canblock(
		vm_size_t	size,
		boolean_t       canblock)
{
	zone_t z;

	if (size < MAX_SIZE_ZDLUT)
		z = get_zone_dlut(size);
	else if (size < kalloc_max_prerounded)
		z = get_zone_search(size, k_zindex_start);
	else {
		/*
		 * If size is too large for a zone, then use kmem_alloc.
		 * (We use kmem_alloc instead of kmem_alloc_kobject so that
		 * krealloc can use kmem_realloc.)
		 */
		vm_map_t alloc_map;
		void *addr;

		/* kmem_alloc could block so we return if noblock */
		if (!canblock) {
			return(NULL);
		}

		if (size >= kalloc_kernmap_size)
		        alloc_map = kernel_map;
		else
			alloc_map = kalloc_map;

		if (kmem_alloc(alloc_map, (vm_offset_t *)&addr, size) != KERN_SUCCESS) {
			if (alloc_map != kernel_map) {
				if (kmem_alloc(kernel_map, (vm_offset_t *)&addr, size) != KERN_SUCCESS)
					addr = NULL;
			}
			else
				addr = NULL;
		}

		if (addr != NULL) {
			kalloc_spin_lock();
			/*
			 * Thread-safe version of the workaround for 4740071
			 * (a double FREE())
			 */
			if (size > kalloc_largest_allocated)
				kalloc_largest_allocated = size;

		        kalloc_large_inuse++;
		        kalloc_large_total += size;
			kalloc_large_sum += size;

			if (kalloc_large_total > kalloc_large_max)
			        kalloc_large_max = kalloc_large_total;

			kalloc_unlock();

			KALLOC_ZINFO_SALLOC(size);
		}
		return(addr);
	}
#ifdef KALLOC_DEBUG
	if (size > z->elem_size)
		panic("%s: z %p (%s) but requested size %lu", __func__,
		    z, z->zone_name, (unsigned long)size);
#endif
	assert(size <= z->elem_size);
	return (zalloc_canblock(z, canblock));
}

void *
kalloc(
       vm_size_t size)
{
	return( kalloc_canblock(size, TRUE) );
}

void *
kalloc_noblock(
	       vm_size_t size)
{
	return( kalloc_canblock(size, FALSE) );
}

volatile SInt32 kfree_nop_count = 0;

void
kfree(
	void 		*data,
	vm_size_t	size)
{
	zone_t z;

	if (size < MAX_SIZE_ZDLUT)
		z = get_zone_dlut(size);
	else if (size < kalloc_max_prerounded)
		z = get_zone_search(size, k_zindex_start);
	else {
		/* if size was too large for a zone, then use kmem_free */

		vm_map_t alloc_map = kernel_map;

		if ((((vm_offset_t) data) >= kalloc_map_min) && (((vm_offset_t) data) <= kalloc_map_max))
			alloc_map = kalloc_map;
		if (size > kalloc_largest_allocated) {
			        /*
				 * work around double FREEs of small MALLOCs
				 * this used to end up being a nop
				 * since the pointer being freed from an
				 * alloc backed by the zalloc world could
				 * never show up in the kalloc_map... however,
				 * the kernel_map is a different issue... since it
				 * was released back into the zalloc pool, a pointer
				 * would have gotten written over the 'size' that 
				 * the MALLOC was retaining in the first 4 bytes of
				 * the underlying allocation... that pointer ends up 
				 * looking like a really big size on the 2nd FREE and
				 * pushes the kfree into the kernel_map...  we
				 * end up removing a ton of virtual space before we panic
				 * this check causes us to ignore the kfree for a size
				 * that must be 'bogus'... note that it might not be due
				 * to the above scenario, but it would still be wrong and
				 * cause serious damage.
				 */

				OSAddAtomic(1, &kfree_nop_count);
			        return;
		}
		kmem_free(alloc_map, (vm_offset_t)data, size);

		kalloc_spin_lock();

		kalloc_large_total -= size;
		kalloc_large_inuse--;

		kalloc_unlock();

		KALLOC_ZINFO_SFREE(size);
		return;
	}

	/* free to the appropriate zone */
#ifdef KALLOC_DEBUG
	if (size > z->elem_size)
		panic("%s: z %p (%s) but requested size %lu", __func__,
		    z, z->zone_name, (unsigned long)size);
#endif
	assert(size <= z->elem_size);
	zfree(z, data);
}

#ifdef MACH_BSD
zone_t
kalloc_zone(
	vm_size_t       size)
{
	if (size < MAX_SIZE_ZDLUT)
		return (get_zone_dlut(size));
	if (size <= kalloc_max)
		return (get_zone_search(size, k_zindex_start));
	return (ZONE_NULL);
}
#endif

void
kalloc_fake_zone_init(int zone_index)
{
	kalloc_fake_zone_index = zone_index;
}

void
kalloc_fake_zone_info(int *count, 
		      vm_size_t *cur_size, vm_size_t *max_size, vm_size_t *elem_size, vm_size_t *alloc_size,
		      uint64_t *sum_size, int *collectable, int *exhaustable, int *caller_acct)
{
	*count      = kalloc_large_inuse;
	*cur_size   = kalloc_large_total;
	*max_size   = kalloc_large_max;

	if (kalloc_large_inuse) {
		*elem_size  = kalloc_large_total / kalloc_large_inuse;
		*alloc_size = kalloc_large_total / kalloc_large_inuse;
	} else {
		*elem_size  = 0;
		*alloc_size = 0;
	}
	*sum_size   = kalloc_large_sum;
	*collectable = 0;
	*exhaustable = 0;
	*caller_acct = 0;
}


void
OSMalloc_init(
	void)
{
	queue_init(&OSMalloc_tag_list);

	OSMalloc_tag_lck_grp = lck_grp_alloc_init("OSMalloc_tag", LCK_GRP_ATTR_NULL);
	lck_mtx_init(&OSMalloc_tag_lock, OSMalloc_tag_lck_grp, LCK_ATTR_NULL);
}

OSMallocTag
OSMalloc_Tagalloc(
	const char			*str,
	uint32_t			flags)
{
	OSMallocTag       OSMTag;

	OSMTag = (OSMallocTag)kalloc(sizeof(*OSMTag));

	bzero((void *)OSMTag, sizeof(*OSMTag));

	if (flags & OSMT_PAGEABLE)
		OSMTag->OSMT_attr = OSMT_ATTR_PAGEABLE;

	OSMTag->OSMT_refcnt = 1;

	strncpy(OSMTag->OSMT_name, str, OSMT_MAX_NAME);

	OSMalloc_tag_spin_lock();
	enqueue_tail(&OSMalloc_tag_list, (queue_entry_t)OSMTag);
	OSMalloc_tag_unlock();
	OSMTag->OSMT_state = OSMT_VALID;
	return(OSMTag);
}

void
OSMalloc_Tagref(
	 OSMallocTag		tag)
{
	if (!((tag->OSMT_state & OSMT_VALID_MASK) == OSMT_VALID)) 
		panic("OSMalloc_Tagref():'%s' has bad state 0x%08X\n", tag->OSMT_name, tag->OSMT_state);

	(void)hw_atomic_add(&tag->OSMT_refcnt, 1);
}

void
OSMalloc_Tagrele(
	 OSMallocTag		tag)
{
	if (!((tag->OSMT_state & OSMT_VALID_MASK) == OSMT_VALID))
		panic("OSMalloc_Tagref():'%s' has bad state 0x%08X\n", tag->OSMT_name, tag->OSMT_state);

	if (hw_atomic_sub(&tag->OSMT_refcnt, 1) == 0) {
		if (hw_compare_and_store(OSMT_VALID|OSMT_RELEASED, OSMT_VALID|OSMT_RELEASED, &tag->OSMT_state)) {
			OSMalloc_tag_spin_lock();
			(void)remque((queue_entry_t)tag);
			OSMalloc_tag_unlock();
			kfree((void*)tag, sizeof(*tag));
		} else
			panic("OSMalloc_Tagrele():'%s' has refcnt 0\n", tag->OSMT_name);
	}
}

void
OSMalloc_Tagfree(
	 OSMallocTag		tag)
{
	if (!hw_compare_and_store(OSMT_VALID, OSMT_VALID|OSMT_RELEASED, &tag->OSMT_state))
		panic("OSMalloc_Tagfree():'%s' has bad state 0x%08X \n", tag->OSMT_name, tag->OSMT_state);

	if (hw_atomic_sub(&tag->OSMT_refcnt, 1) == 0) {
		OSMalloc_tag_spin_lock();
		(void)remque((queue_entry_t)tag);
		OSMalloc_tag_unlock();
		kfree((void*)tag, sizeof(*tag));
	}
}

void *
OSMalloc(
	uint32_t			size,
	OSMallocTag			tag)
{
	void			*addr=NULL;
	kern_return_t	kr;

	OSMalloc_Tagref(tag);
	if ((tag->OSMT_attr & OSMT_PAGEABLE)
	    && (size & ~PAGE_MASK)) {

		if ((kr = kmem_alloc_pageable(kernel_map, (vm_offset_t *)&addr, size)) != KERN_SUCCESS)
			addr = NULL;
	} else 
		addr = kalloc((vm_size_t)size);

	if (!addr)
		OSMalloc_Tagrele(tag);

	return(addr);
}

void *
OSMalloc_nowait(
	uint32_t			size,
	OSMallocTag			tag)
{
	void	*addr=NULL;

	if (tag->OSMT_attr & OSMT_PAGEABLE)
		return(NULL);

	OSMalloc_Tagref(tag);
	/* XXX: use non-blocking kalloc for now */
	addr = kalloc_noblock((vm_size_t)size);
	if (addr == NULL)
		OSMalloc_Tagrele(tag);

	return(addr);
}

void *
OSMalloc_noblock(
	uint32_t			size,
	OSMallocTag			tag)
{
	void	*addr=NULL;

	if (tag->OSMT_attr & OSMT_PAGEABLE)
		return(NULL);

	OSMalloc_Tagref(tag);
	addr = kalloc_noblock((vm_size_t)size);
	if (addr == NULL)
		OSMalloc_Tagrele(tag);

	return(addr);
}

void
OSFree(
	void				*addr,
	uint32_t			size,
	OSMallocTag			tag) 
{
	if ((tag->OSMT_attr & OSMT_PAGEABLE)
	    && (size & ~PAGE_MASK)) {
		kmem_free(kernel_map, (vm_offset_t)addr, size);
	} else
		kfree((void *)addr, size);

	OSMalloc_Tagrele(tag);
}
