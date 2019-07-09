/*
 * Copyright (C) 2012-2014 Canonical Ltd (Maarten Lankhorst)
 *
 * Based on bo.c which bears the following copyright notice,
 * but is dual licensed:
 *
 * Copyright (c) 2006-2009 VMware, Inc., Palo Alto, CA., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
/*
 * Authors: Thomas Hellstrom <thellstrom-at-vmware-dot-com>
 */

#include <linux/reservation.h>
#include <linux/export.h>

/**
 * DOC: Reservation Object Overview
 *
 * The reservation object provides a mechanism to manage shared and
 * exclusive fences associated with a buffer.  A reservation object
 * can have attached one exclusive fence (normally associated with
 * write operations) or N shared fences (read operations).  The RCU
 * mechanism is used to protect read access to fences from locked
 * write-side updates.
 */

DEFINE_WW_CLASS(reservation_ww_class);
EXPORT_SYMBOL(reservation_ww_class);

/**
 * reservation_object_list_alloc - allocate fence list
 * @shared_max: number of fences we need space for
 *
 * Allocate a new reservation_object_list and make sure to correctly initialize
 * shared_max.
 */
static struct reservation_object_list *
reservation_object_list_alloc(unsigned int shared_max)
{
	struct reservation_object_list *list;

	list = kmalloc(offsetof(typeof(*list), shared[shared_max]), GFP_KERNEL);
	if (!list)
		return NULL;

#ifdef __linux__
	list->shared_max = (ksize(list) - offsetof(typeof(*list), shared)) /
		sizeof(*list->shared);
#elif defined(__FreeBSD__)
	list->shared_max = (offsetof(typeof(*list), shared[shared_max]) - offsetof(typeof(*list), shared)) /
		sizeof(*list->shared);
#endif

	return list;
}

/**
 * reservation_object_list_free - free fence list
 * @list: list to free
 *
 * Free a reservation_object_list and make sure to drop all references.
 */
static void reservation_object_list_free(struct reservation_object_list *list)
{
	unsigned int i;

	if (!list)
		return;

	for (i = 0; i < list->shared_count; ++i)
		dma_fence_put(rcu_dereference_protected(list->shared[i], true));

	kfree_rcu(list, rcu);
}

/**
 * reservation_object_init - initialize a reservation object
 * @obj: the reservation object
 */
void reservation_object_init(struct reservation_object *obj)
{
	ww_mutex_init(&obj->lock, &reservation_ww_class);
	RCU_INIT_POINTER(obj->fence, NULL);
	RCU_INIT_POINTER(obj->fence_excl, NULL);
}
EXPORT_SYMBOL(reservation_object_init);

/**
 * reservation_object_fini - destroys a reservation object
 * @obj: the reservation object
 */
void reservation_object_fini(struct reservation_object *obj)
{
	struct reservation_object_list *fobj;
	struct dma_fence *excl;

	/*
	 * This object should be dead and all references must have
	 * been released to it, so no need to be protected with rcu.
	 */
	excl = rcu_dereference_protected(obj->fence_excl, 1);
	if (excl)
		dma_fence_put(excl);

	fobj = rcu_dereference_protected(obj->fence, 1);
	reservation_object_list_free(fobj);
	ww_mutex_destroy(&obj->lock);
}
EXPORT_SYMBOL(reservation_object_fini);

/**
 * reservation_object_reserve_shared - Reserve space to add shared fences to
 * a reservation_object.
 * @obj: reservation object
 * @num_fences: number of fences we want to add
 *
 * Should be called before reservation_object_add_shared_fence().  Must
 * be called with obj->lock held.
 *
 * RETURNS
 * Zero for success, or -errno
 */
int reservation_object_reserve_shared(struct reservation_object *obj,
				      unsigned int num_fences)
{
	struct reservation_object_list *old, *new;
	unsigned int i, j, k, max;

	reservation_object_assert_held(obj);

	old = reservation_object_get_list(obj);

	if (old && old->shared_max) {
		if ((old->shared_count + num_fences) <= old->shared_max)
			return 0;
		else
			max = max(old->shared_count + num_fences,
				  old->shared_max * 2);
	} else {
		max = 4;
	}

	new = reservation_object_list_alloc(max);
	if (!new)
		return -ENOMEM;

	/*
	 * no need to bump fence refcounts, rcu_read access
	 * requires the use of kref_get_unless_zero, and the
	 * references from the old struct are carried over to
	 * the new.
	 */
	for (i = 0, j = 0, k = max; i < (old ? old->shared_count : 0); ++i) {
		struct dma_fence *fence;

		fence = rcu_dereference_protected(old->shared[i],
						  reservation_object_held(obj));
		if (dma_fence_is_signaled(fence))
			RCU_INIT_POINTER(new->shared[--k], fence);
		else
			RCU_INIT_POINTER(new->shared[j++], fence);
	}
	new->shared_count = j;

	/*
	 * We are not changing the effective set of fences here so can
	 * merely update the pointer to the new array; both existing
	 * readers and new readers will see exactly the same set of
	 * active (unsignaled) shared fences. Individual fences and the
	 * old array are protected by RCU and so will not vanish under
	 * the gaze of the rcu_read_lock() readers.
	 */
	rcu_assign_pointer(obj->fence, new);

	if (!old)
		return 0;

	/* Drop the references to the signaled fences */
	for (i = k; i < max; ++i) {
		struct dma_fence *fence;

		fence = rcu_dereference_protected(new->shared[i],
						  reservation_object_held(obj));
		dma_fence_put(fence);
	}
	kfree_rcu(old, rcu);

	return 0;
}
EXPORT_SYMBOL(reservation_object_reserve_shared);

/**
 * reservation_object_add_shared_fence - Add a fence to a shared slot
 * @obj: the reservation object
 * @fence: the shared fence to add
 *
 * Add a fence to a shared slot, obj->lock must be held, and
 * reservation_object_reserve_shared() has been called.
 */
void reservation_object_add_shared_fence(struct reservation_object *obj,
					 struct dma_fence *fence)
{
	struct reservation_object_list *fobj;
	struct dma_fence *old;
	unsigned int i, count;

	dma_fence_get(fence);

	reservation_object_assert_held(obj);

	fobj = reservation_object_get_list(obj);
	count = fobj->shared_count;

	for (i = 0; i < count; ++i) {

		old = rcu_dereference_protected(fobj->shared[i],
						reservation_object_held(obj));
		if (old->context == fence->context ||
		    dma_fence_is_signaled(old))
			goto replace;
	}

	BUG_ON(fobj->shared_count >= fobj->shared_max);
	old = NULL;
	count++;

replace:
	RCU_INIT_POINTER(fobj->shared[i], fence);
	/* pointer update must be visible before we extend the shared_count */
	smp_store_mb(fobj->shared_count, count);
	dma_fence_put(old);
}
EXPORT_SYMBOL(reservation_object_add_shared_fence);

/**
 * reservation_object_add_excl_fence - Add an exclusive fence.
 * @obj: the reservation object
 * @fence: the shared fence to add
 *
 * Add a fence to the exclusive slot.  The obj->lock must be held.
 */
void reservation_object_add_excl_fence(struct reservation_object *obj,
				       struct dma_fence *fence)
{
	struct dma_fence *old_fence = reservation_object_get_excl(obj);
	struct reservation_object_list *old;
	u32 i = 0;

	reservation_object_assert_held(obj);

	old = reservation_object_get_list(obj);
	if (old)
		i = old->shared_count;

	if (fence)
		dma_fence_get(fence);

	preempt_disable();
	rcu_assign_pointer(obj->fence_excl, fence);
	/* pointer update must be visible before we modify the shared_count */
	if (old)
		smp_store_mb(old->shared_count, 0);
	preempt_enable();

	/* inplace update, no shared fences */
	while (i--)
		dma_fence_put(rcu_dereference_protected(old->shared[i],
						reservation_object_held(obj)));

	dma_fence_put(old_fence);
}
EXPORT_SYMBOL(reservation_object_add_excl_fence);

/**
* reservation_object_copy_fences - Copy all fences from src to dst.
* @dst: the destination reservation object
* @src: the source reservation object
*
* Copy all fences from src to dst. dst-lock must be held.
*/
int reservation_object_copy_fences(struct reservation_object *dst,
				   struct reservation_object *src)
{
	struct reservation_object_list *src_list, *dst_list;
	struct dma_fence *old, *new;
	unsigned int i, shared_count;

	reservation_object_assert_held(dst);

	rcu_read_lock();

retry:
	reservation_object_fences(src, &new, &src_list, &shared_count);
	if (shared_count) {
		rcu_read_unlock();

		dst_list = reservation_object_list_alloc(shared_count);
		if (!dst_list)
			return -ENOMEM;

		rcu_read_lock();
		reservation_object_fences(src, &new, &src_list, &shared_count);
		if (!src_list || shared_count > dst_list->shared_max) {
			kfree(dst_list);
			goto retry;
		}

		dst_list->shared_count = 0;
		for (i = 0; i < shared_count; ++i) {
			struct dma_fence *fence;

			fence = rcu_dereference(src_list->shared[i]);
			if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT,
				     &fence->flags))
				continue;

			if (!dma_fence_get_rcu(fence)) {
				reservation_object_list_free(dst_list);
				goto retry;
			}

			if (dma_fence_is_signaled(fence)) {
				dma_fence_put(fence);
				continue;
			}

			rcu_assign_pointer(dst_list->shared[dst_list->shared_count++], fence);
		}
	} else {
		dst_list = NULL;
	}

	if (new && !dma_fence_get_rcu(new)) {
		reservation_object_list_free(dst_list);
		goto retry;
	}
	rcu_read_unlock();

	src_list = reservation_object_get_list(dst);
	old = reservation_object_get_excl(dst);

	preempt_disable();
	rcu_assign_pointer(dst->fence_excl, new);
	rcu_assign_pointer(dst->fence, dst_list);
	preempt_enable();

	reservation_object_list_free(src_list);
	dma_fence_put(old);

	return 0;
}
EXPORT_SYMBOL(reservation_object_copy_fences);

/**
 * reservation_object_get_fences_rcu - Get an object's shared and exclusive
 * fences without update side lock held
 * @obj: the reservation object
 * @pfence_excl: the returned exclusive fence (or NULL)
 * @pshared_count: the number of shared fences returned
 * @pshared: the array of shared fence ptrs returned (array is krealloc'd to
 * the required size, and must be freed by caller)
 *
 * Retrieve all fences from the reservation object. If the pointer for the
 * exclusive fence is not specified the fence is put into the array of the
 * shared fences as well. Returns either zero or -ENOMEM.
 */
int reservation_object_get_fences_rcu(struct reservation_object *obj,
				      struct dma_fence **pfence_excl,
				      unsigned *pshared_count,
				      struct dma_fence ***pshared)
{
	struct dma_fence **shared = NULL;
	struct dma_fence *fence_excl;
	unsigned int shared_count;
	int ret = 1;

	do {
		struct reservation_object_list *fobj;
		unsigned int i;
		size_t sz = 0;

		i = 0;

		rcu_read_lock();
		reservation_object_fences(obj, &fence_excl, &fobj,
					  &shared_count);

		if (fence_excl && !dma_fence_get_rcu(fence_excl))
			goto unlock;

		if (fobj)
			sz += sizeof(*shared) * fobj->shared_max;

		if (!pfence_excl && fence_excl)
			sz += sizeof(*shared);

		if (sz) {
			struct dma_fence **nshared;

			nshared = krealloc(shared, sz,
					   GFP_NOWAIT | __GFP_NOWARN);
			if (!nshared) {
				rcu_read_unlock();

				dma_fence_put(fence_excl);
				fence_excl = NULL;

				nshared = krealloc(shared, sz, GFP_KERNEL);
				if (nshared) {
					shared = nshared;
					continue;
				}

				ret = -ENOMEM;
				break;
			}
			shared = nshared;
			for (i = 0; i < shared_count; ++i) {
				shared[i] = rcu_dereference(fobj->shared[i]);
				if (!dma_fence_get_rcu(shared[i]))
					break;
			}
		}

		if (i != shared_count) {
			while (i--)
				dma_fence_put(shared[i]);
			dma_fence_put(fence_excl);
			goto unlock;
		}

		ret = 0;
unlock:
		rcu_read_unlock();
	} while (ret);

	if (pfence_excl)
		*pfence_excl = fence_excl;
	else if (fence_excl)
		shared[++shared_count] = fence_excl;

	if (!shared_count) {
		kfree(shared);
		shared = NULL;
	}

	*pshared_count = shared_count;
	*pshared = shared;
	return ret;
}
EXPORT_SYMBOL_GPL(reservation_object_get_fences_rcu);

/**
 * reservation_object_wait_timeout_rcu - Wait on reservation's objects
 * shared and/or exclusive fences.
 * @obj: the reservation object
 * @wait_all: if true, wait on all fences, else wait on just exclusive fence
 * @intr: if true, do interruptible wait
 * @timeout: timeout value in jiffies or zero to return immediately
 *
 * RETURNS
 * Returns -ERESTARTSYS if interrupted, 0 if the wait timed out, or
 * greater than zer on success.
 */
long reservation_object_wait_timeout_rcu(struct reservation_object *obj,
					 bool wait_all, bool intr,
					 unsigned long timeout)
{
	struct reservation_object_list *fobj;
	struct dma_fence *fence;
	unsigned shared_count;
	long ret = timeout ? timeout : 1;
	int i;

retry:
	rcu_read_lock();
	i = -1;

	reservation_object_fences(obj, &fence, &fobj, &shared_count);
	if (fence && !test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags)) {
		if (!dma_fence_get_rcu(fence))
			goto unlock_retry;

		if (dma_fence_is_signaled(fence)) {
			dma_fence_put(fence);
			fence = NULL;
		}

	} else {
		fence = NULL;
	}

	if (wait_all) {
		for (i = 0; !fence && i < shared_count; ++i) {
			struct dma_fence *lfence = rcu_dereference(fobj->shared[i]);

			if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT,
				     &lfence->flags))
				continue;

			if (!dma_fence_get_rcu(lfence))
				goto unlock_retry;

			if (dma_fence_is_signaled(lfence)) {
				dma_fence_put(lfence);
				continue;
			}

			fence = lfence;
			break;
		}
	}

	rcu_read_unlock();
	if (fence) {
		ret = dma_fence_wait_timeout(fence, intr, ret);
		dma_fence_put(fence);
		if (ret > 0 && wait_all && (i + 1 < shared_count))
			goto retry;
	}
	return ret;

unlock_retry:
	rcu_read_unlock();
	goto retry;
}
EXPORT_SYMBOL_GPL(reservation_object_wait_timeout_rcu);


static inline int
reservation_object_test_signaled_single(struct dma_fence *passed_fence)
{
	struct dma_fence *fence, *lfence = passed_fence;
	int ret = 1;

	if (!test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &lfence->flags)) {
		fence = dma_fence_get_rcu(lfence);
		if (!fence)
			return -1;

		ret = !!dma_fence_is_signaled(fence);
		dma_fence_put(fence);
	}
	return ret;
}

/**
 * reservation_object_test_signaled_rcu - Test if a reservation object's
 * fences have been signaled.
 * @obj: the reservation object
 * @test_all: if true, test all fences, otherwise only test the exclusive
 * fence
 *
 * RETURNS
 * true if all fences signaled, else false
 */
bool reservation_object_test_signaled_rcu(struct reservation_object *obj,
					  bool test_all)
{
	struct reservation_object_list *fobj;
	struct dma_fence *fence_excl;
	unsigned shared_count;
	int ret;

	rcu_read_lock();
retry:
	ret = true;

	reservation_object_fences(obj, &fence_excl, &fobj, &shared_count);
	if (test_all) {
		unsigned i;

		for (i = 0; i < shared_count; ++i) {
			struct dma_fence *fence = rcu_dereference(fobj->shared[i]);

			ret = reservation_object_test_signaled_single(fence);
			if (ret < 0)
				goto retry;
			else if (!ret)
				break;
		}
	}

	if (!shared_count && fence_excl) {
		ret = reservation_object_test_signaled_single(fence_excl);
		if (ret < 0)
			goto retry;
	}

	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(reservation_object_test_signaled_rcu);