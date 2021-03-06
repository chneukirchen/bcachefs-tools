/*
 * Primary bucket allocation code
 *
 * Copyright 2012 Google, Inc.
 *
 * Allocation in bcache is done in terms of buckets:
 *
 * Each bucket has associated an 8 bit gen; this gen corresponds to the gen in
 * btree pointers - they must match for the pointer to be considered valid.
 *
 * Thus (assuming a bucket has no dirty data or metadata in it) we can reuse a
 * bucket simply by incrementing its gen.
 *
 * The gens (along with the priorities; it's really the gens are important but
 * the code is named as if it's the priorities) are written in an arbitrary list
 * of buckets on disk, with a pointer to them in the journal header.
 *
 * When we invalidate a bucket, we have to write its new gen to disk and wait
 * for that write to complete before we use it - otherwise after a crash we
 * could have pointers that appeared to be good but pointed to data that had
 * been overwritten.
 *
 * Since the gens and priorities are all stored contiguously on disk, we can
 * batch this up: We fill up the free_inc list with freshly invalidated buckets,
 * call prio_write(), and when prio_write() finishes we pull buckets off the
 * free_inc list and optionally discard them.
 *
 * free_inc isn't the only freelist - if it was, we'd often have to sleep while
 * priorities and gens were being written before we could allocate. c->free is a
 * smaller freelist, and buckets on that list are always ready to be used.
 *
 * If we've got discards enabled, that happens when a bucket moves from the
 * free_inc list to the free list.
 *
 * It's important to ensure that gens don't wrap around - with respect to
 * either the oldest gen in the btree or the gen on disk. This is quite
 * difficult to do in practice, but we explicitly guard against it anyways - if
 * a bucket is in danger of wrapping around we simply skip invalidating it that
 * time around, and we garbage collect or rewrite the priorities sooner than we
 * would have otherwise.
 *
 * bch2_bucket_alloc() allocates a single bucket from a specific device.
 *
 * bch2_bucket_alloc_set() allocates one or more buckets from different devices
 * in a given filesystem.
 *
 * invalidate_buckets() drives all the processes described above. It's called
 * from bch2_bucket_alloc() and a few other places that need to make sure free
 * buckets are ready.
 *
 * invalidate_buckets_(lru|fifo)() find buckets that are available to be
 * invalidated, and then invalidate them and stick them on the free_inc list -
 * in either lru or fifo order.
 */

#include "bcachefs.h"
#include "alloc.h"
#include "btree_cache.h"
#include "btree_io.h"
#include "btree_update.h"
#include "btree_gc.h"
#include "buckets.h"
#include "checksum.h"
#include "clock.h"
#include "debug.h"
#include "error.h"
#include "extents.h"
#include "io.h"
#include "journal.h"
#include "super-io.h"

#include <linux/blkdev.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/random.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/sched/task.h>
#include <linux/sort.h>
#include <trace/events/bcachefs.h>

static void bch2_recalc_min_prio(struct bch_fs *, struct bch_dev *, int);

/* Ratelimiting/PD controllers */

static void pd_controllers_update(struct work_struct *work)
{
	struct bch_fs *c = container_of(to_delayed_work(work),
					   struct bch_fs,
					   pd_controllers_update);
	struct bch_dev *ca;
	unsigned i, iter;

	/* All units are in bytes */
	u64 faster_tiers_size	= 0;
	u64 faster_tiers_dirty	= 0;

	u64 copygc_can_free	= 0;

	rcu_read_lock();
	for (i = 0; i < ARRAY_SIZE(c->tiers); i++) {
		bch2_pd_controller_update(&c->tiers[i].pd,
				div_u64(faster_tiers_size *
					c->tiering_percent, 100),
				faster_tiers_dirty,
				-1);

		for_each_member_device_rcu(ca, c, iter, &c->tiers[i].devs) {
			struct bch_dev_usage stats = bch2_dev_usage_read(c, ca);

			u64 size = bucket_to_sector(ca, ca->mi.nbuckets -
					ca->mi.first_bucket) << 9;
			u64 dirty = bucket_to_sector(ca,
					stats.buckets[BCH_DATA_USER]) << 9;
			u64 free = bucket_to_sector(ca,
					__dev_buckets_free(ca, stats)) << 9;
			/*
			 * Bytes of internal fragmentation, which can be
			 * reclaimed by copy GC
			 */
			s64 fragmented = (bucket_to_sector(ca,
						stats.buckets[BCH_DATA_USER] +
						stats.buckets[BCH_DATA_CACHED]) -
					  (stats.sectors[BCH_DATA_USER] +
					   stats.sectors[BCH_DATA_CACHED])) << 9;

			fragmented = max(0LL, fragmented);

			bch2_pd_controller_update(&ca->copygc_pd,
						 free, fragmented, -1);

			faster_tiers_size		+= size;
			faster_tiers_dirty		+= dirty;

			copygc_can_free			+= fragmented;
		}
	}

	rcu_read_unlock();

	/*
	 * Throttle foreground writes if tier 0 is running out of free buckets,
	 * and either tiering or copygc can free up space.
	 *
	 * Target will be small if there isn't any work to do - we don't want to
	 * throttle foreground writes if we currently have all the free space
	 * we're ever going to have.
	 *
	 * Otherwise, if there's work to do, try to keep 20% of tier0 available
	 * for foreground writes.
	 */
	if (c->fastest_tier)
		copygc_can_free = U64_MAX;

	schedule_delayed_work(&c->pd_controllers_update,
			      c->pd_controllers_update_seconds * HZ);
}

/* Persistent alloc info: */

static unsigned bch_alloc_val_u64s(const struct bch_alloc *a)
{
	unsigned bytes = offsetof(struct bch_alloc, data);

	if (a->fields & (1 << BCH_ALLOC_FIELD_READ_TIME))
		bytes += 2;
	if (a->fields & (1 << BCH_ALLOC_FIELD_WRITE_TIME))
		bytes += 2;

	return DIV_ROUND_UP(bytes, sizeof(u64));
}

static const char *bch2_alloc_invalid(const struct bch_fs *c,
				      struct bkey_s_c k)
{
	if (k.k->p.inode >= c->sb.nr_devices ||
	    !c->devs[k.k->p.inode])
		return "invalid device";

	switch (k.k->type) {
	case BCH_ALLOC: {
		struct bkey_s_c_alloc a = bkey_s_c_to_alloc(k);

		if (bch_alloc_val_u64s(a.v) != bkey_val_u64s(a.k))
			return "incorrect value size";
		break;
	}
	default:
		return "invalid type";
	}

	return NULL;
}

static void bch2_alloc_to_text(struct bch_fs *c, char *buf,
			       size_t size, struct bkey_s_c k)
{
	buf[0] = '\0';

	switch (k.k->type) {
	case BCH_ALLOC:
		break;
	}
}

const struct bkey_ops bch2_bkey_alloc_ops = {
	.key_invalid	= bch2_alloc_invalid,
	.val_to_text	= bch2_alloc_to_text,
};

static inline unsigned get_alloc_field(const u8 **p, unsigned bytes)
{
	unsigned v;

	switch (bytes) {
	case 1:
		v = **p;
		break;
	case 2:
		v = le16_to_cpup((void *) *p);
		break;
	case 4:
		v = le32_to_cpup((void *) *p);
		break;
	default:
		BUG();
	}

	*p += bytes;
	return v;
}

static inline void put_alloc_field(u8 **p, unsigned bytes, unsigned v)
{
	switch (bytes) {
	case 1:
		**p = v;
		break;
	case 2:
		*((__le16 *) *p) = cpu_to_le16(v);
		break;
	case 4:
		*((__le32 *) *p) = cpu_to_le32(v);
		break;
	default:
		BUG();
	}

	*p += bytes;
}

static void bch2_alloc_read_key(struct bch_fs *c, struct bkey_s_c k)
{
	struct bch_dev *ca;
	struct bkey_s_c_alloc a;
	struct bucket_mark new;
	struct bucket *g;
	const u8 *d;

	if (k.k->type != BCH_ALLOC)
		return;

	a = bkey_s_c_to_alloc(k);
	ca = bch_dev_bkey_exists(c, a.k->p.inode);

	if (a.k->p.offset >= ca->mi.nbuckets)
		return;

	lg_local_lock(&c->usage_lock);

	g = bucket(ca, a.k->p.offset);
	bucket_cmpxchg(g, new, ({
		new.gen = a.v->gen;
		new.gen_valid = 1;
	}));

	d = a.v->data;
	if (a.v->fields & (1 << BCH_ALLOC_FIELD_READ_TIME))
		g->prio[READ] = get_alloc_field(&d, 2);
	if (a.v->fields & (1 << BCH_ALLOC_FIELD_WRITE_TIME))
		g->prio[WRITE] = get_alloc_field(&d, 2);

	lg_local_unlock(&c->usage_lock);
}

int bch2_alloc_read(struct bch_fs *c, struct list_head *journal_replay_list)
{
	struct journal_replay *r;
	struct btree_iter iter;
	struct bkey_s_c k;
	struct bch_dev *ca;
	unsigned i;
	int ret;

	for_each_btree_key(&iter, c, BTREE_ID_ALLOC, POS_MIN, 0, k) {
		bch2_alloc_read_key(c, k);
		bch2_btree_iter_cond_resched(&iter);
	}

	ret = bch2_btree_iter_unlock(&iter);
	if (ret)
		return ret;

	list_for_each_entry(r, journal_replay_list, list) {
		struct bkey_i *k, *n;
		struct jset_entry *entry;

		for_each_jset_key(k, n, entry, &r->j)
			if (entry->btree_id == BTREE_ID_ALLOC)
				bch2_alloc_read_key(c, bkey_i_to_s_c(k));
	}

	mutex_lock(&c->prio_clock[READ].lock);
	for_each_member_device(ca, c, i) {
		down_read(&ca->bucket_lock);
		bch2_recalc_min_prio(c, ca, READ);
		up_read(&ca->bucket_lock);
	}
	mutex_unlock(&c->prio_clock[READ].lock);

	mutex_lock(&c->prio_clock[WRITE].lock);
	for_each_member_device(ca, c, i) {
		down_read(&ca->bucket_lock);
		bch2_recalc_min_prio(c, ca, WRITE);
		up_read(&ca->bucket_lock);
	}
	mutex_unlock(&c->prio_clock[WRITE].lock);

	return 0;
}

static int __bch2_alloc_write_key(struct bch_fs *c, struct bch_dev *ca,
				  size_t b, struct btree_iter *iter,
				  u64 *journal_seq)
{
	struct bucket_mark m;
	__BKEY_PADDED(k, DIV_ROUND_UP(sizeof(struct bch_alloc), 8)) alloc_key;
	struct bucket *g;
	struct bkey_i_alloc *a;
	u8 *d;
	int ret;

	bch2_btree_iter_set_pos(iter, POS(ca->dev_idx, b));

	do {
		ret = btree_iter_err(bch2_btree_iter_peek_slot(iter));
		if (ret)
			break;

		lg_local_lock(&c->usage_lock);
		g = bucket(ca, b);

		/* read mark under btree node lock: */
		m = READ_ONCE(g->mark);
		a = bkey_alloc_init(&alloc_key.k);
		a->k.p		= iter->pos;
		a->v.fields	= 0;
		a->v.gen	= m.gen;
		set_bkey_val_u64s(&a->k, bch_alloc_val_u64s(&a->v));

		d = a->v.data;
		if (a->v.fields & (1 << BCH_ALLOC_FIELD_READ_TIME))
			put_alloc_field(&d, 2, g->prio[READ]);
		if (a->v.fields & (1 << BCH_ALLOC_FIELD_WRITE_TIME))
			put_alloc_field(&d, 2, g->prio[WRITE]);
		lg_local_unlock(&c->usage_lock);

		ret = bch2_btree_insert_at(c, NULL, NULL, journal_seq,
					   BTREE_INSERT_ATOMIC|
					   BTREE_INSERT_NOFAIL|
					   BTREE_INSERT_USE_RESERVE|
					   BTREE_INSERT_USE_ALLOC_RESERVE|
					   BTREE_INSERT_NOWAIT,
					   BTREE_INSERT_ENTRY(iter, &a->k_i));
		bch2_btree_iter_cond_resched(iter);
	} while (ret == -EINTR);

	return ret;
}

int bch2_alloc_replay_key(struct bch_fs *c, struct bpos pos)
{
	struct bch_dev *ca;
	struct btree_iter iter;
	int ret;

	if (pos.inode >= c->sb.nr_devices || !c->devs[pos.inode])
		return 0;

	ca = bch_dev_bkey_exists(c, pos.inode);

	if (pos.offset >= ca->mi.nbuckets)
		return 0;

	bch2_btree_iter_init(&iter, c, BTREE_ID_ALLOC, POS_MIN,
			     BTREE_ITER_SLOTS|BTREE_ITER_INTENT);

	ret = __bch2_alloc_write_key(c, ca, pos.offset, &iter, NULL);
	bch2_btree_iter_unlock(&iter);
	return ret;
}

int bch2_alloc_write(struct bch_fs *c)
{
	struct bch_dev *ca;
	unsigned i;
	int ret = 0;

	for_each_rw_member(ca, c, i) {
		struct btree_iter iter;
		unsigned long bucket;

		bch2_btree_iter_init(&iter, c, BTREE_ID_ALLOC, POS_MIN,
				     BTREE_ITER_SLOTS|BTREE_ITER_INTENT);

		down_read(&ca->bucket_lock);
		for_each_set_bit(bucket, ca->buckets_dirty, ca->mi.nbuckets) {
			ret = __bch2_alloc_write_key(c, ca, bucket, &iter, NULL);
			if (ret)
				break;

			clear_bit(bucket, ca->buckets_dirty);
		}
		up_read(&ca->bucket_lock);
		bch2_btree_iter_unlock(&iter);

		if (ret) {
			percpu_ref_put(&ca->io_ref);
			break;
		}
	}

	return ret;
}

/* Bucket IO clocks: */

static void bch2_recalc_min_prio(struct bch_fs *c, struct bch_dev *ca, int rw)
{
	struct prio_clock *clock = &c->prio_clock[rw];
	struct bucket_array *buckets = bucket_array(ca);
	struct bucket *g;
	u16 max_delta = 1;
	unsigned i;

	lockdep_assert_held(&c->prio_clock[rw].lock);

	/* Determine min prio for this particular device */
	for_each_bucket(g, buckets)
		max_delta = max(max_delta, (u16) (clock->hand - g->prio[rw]));

	ca->min_prio[rw] = clock->hand - max_delta;

	/*
	 * This may possibly increase the min prio for the whole device, check
	 * that as well.
	 */
	max_delta = 1;

	for_each_member_device(ca, c, i)
		max_delta = max(max_delta,
				(u16) (clock->hand - ca->min_prio[rw]));

	clock->min_prio = clock->hand - max_delta;
}

static void bch2_rescale_prios(struct bch_fs *c, int rw)
{
	struct prio_clock *clock = &c->prio_clock[rw];
	struct bucket_array *buckets;
	struct bch_dev *ca;
	struct bucket *g;
	unsigned i;

	trace_rescale_prios(c);

	for_each_member_device(ca, c, i) {
		down_read(&ca->bucket_lock);
		buckets = bucket_array(ca);

		for_each_bucket(g, buckets)
			g->prio[rw] = clock->hand -
			(clock->hand - g->prio[rw]) / 2;

		bch2_recalc_min_prio(c, ca, rw);

		up_read(&ca->bucket_lock);
	}
}

static void bch2_inc_clock_hand(struct io_timer *timer)
{
	struct prio_clock *clock = container_of(timer,
						struct prio_clock, rescale);
	struct bch_fs *c = container_of(clock,
					struct bch_fs, prio_clock[clock->rw]);
	u64 capacity;

	mutex_lock(&clock->lock);

	clock->hand++;

	/* if clock cannot be advanced more, rescale prio */
	if (clock->hand == (u16) (clock->min_prio - 1))
		bch2_rescale_prios(c, clock->rw);

	mutex_unlock(&clock->lock);

	capacity = READ_ONCE(c->capacity);

	if (!capacity)
		return;

	/*
	 * we only increment when 0.1% of the filesystem capacity has been read
	 * or written too, this determines if it's time
	 *
	 * XXX: we shouldn't really be going off of the capacity of devices in
	 * RW mode (that will be 0 when we're RO, yet we can still service
	 * reads)
	 */
	timer->expire += capacity >> 10;

	bch2_io_timer_add(&c->io_clock[clock->rw], timer);
}

static void bch2_prio_timer_init(struct bch_fs *c, int rw)
{
	struct prio_clock *clock = &c->prio_clock[rw];

	clock->hand		= 1;
	clock->rw		= rw;
	clock->rescale.fn	= bch2_inc_clock_hand;
	clock->rescale.expire	= c->capacity >> 10;
	mutex_init(&clock->lock);
}

/* Background allocator thread: */

/*
 * Scans for buckets to be invalidated, invalidates them, rewrites prios/gens
 * (marking them as invalidated on disk), then optionally issues discard
 * commands to the newly free buckets, then puts them on the various freelists.
 */

static void verify_not_on_freelist(struct bch_fs *c, struct bch_dev *ca,
				   size_t bucket)
{
	if (expensive_debug_checks(c) &&
	    test_bit(BCH_FS_ALLOCATOR_STARTED, &c->flags)) {
		size_t iter;
		long i;
		unsigned j;

		for (j = 0; j < RESERVE_NR; j++)
			fifo_for_each_entry(i, &ca->free[j], iter)
				BUG_ON(i == bucket);
		fifo_for_each_entry(i, &ca->free_inc, iter)
			BUG_ON(i == bucket);
	}
}

#define BUCKET_GC_GEN_MAX	96U

/**
 * wait_buckets_available - wait on reclaimable buckets
 *
 * If there aren't enough available buckets to fill up free_inc, wait until
 * there are.
 */
static int wait_buckets_available(struct bch_fs *c, struct bch_dev *ca)
{
	unsigned long gc_count = c->gc_count;
	int ret = 0;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop()) {
			ret = -1;
			break;
		}

		if (gc_count != c->gc_count)
			ca->inc_gen_really_needs_gc = 0;

		if ((ssize_t) (dev_buckets_available(c, ca) -
			       ca->inc_gen_really_needs_gc) >=
		    (ssize_t) fifo_free(&ca->free_inc))
			break;

		up_read(&c->gc_lock);
		schedule();
		try_to_freeze();
		down_read(&c->gc_lock);
	}

	__set_current_state(TASK_RUNNING);
	return ret;
}

static bool bch2_can_invalidate_bucket(struct bch_dev *ca,
				       size_t bucket,
				       struct bucket_mark mark)
{
	u8 gc_gen;

	if (!is_available_bucket(mark))
		return false;

	gc_gen = bucket_gc_gen(ca, bucket);

	if (gc_gen >= BUCKET_GC_GEN_MAX / 2)
		ca->inc_gen_needs_gc++;

	if (gc_gen >= BUCKET_GC_GEN_MAX)
		ca->inc_gen_really_needs_gc++;

	return gc_gen < BUCKET_GC_GEN_MAX;
}

static void bch2_invalidate_one_bucket(struct bch_fs *c, struct bch_dev *ca,
				       size_t bucket)
{
	struct bucket_mark m;

	spin_lock(&c->freelist_lock);
	if (!bch2_invalidate_bucket(c, ca, bucket, &m)) {
		spin_unlock(&c->freelist_lock);
		return;
	}

	verify_not_on_freelist(c, ca, bucket);
	BUG_ON(!fifo_push(&ca->free_inc, bucket));
	spin_unlock(&c->freelist_lock);

	/* gc lock held: */
	bucket_io_clock_reset(c, ca, bucket, READ);
	bucket_io_clock_reset(c, ca, bucket, WRITE);

	if (m.cached_sectors) {
		ca->allocator_invalidating_data = true;
	} else if (m.journal_seq_valid) {
		u64 journal_seq = atomic64_read(&c->journal.seq);
		u64 bucket_seq	= journal_seq;

		bucket_seq &= ~((u64) U16_MAX);
		bucket_seq |= m.journal_seq;

		if (bucket_seq > journal_seq)
			bucket_seq -= 1 << 16;

		ca->allocator_journal_seq_flush =
			max(ca->allocator_journal_seq_flush, bucket_seq);
	}
}

/*
 * Determines what order we're going to reuse buckets, smallest bucket_key()
 * first.
 *
 *
 * - We take into account the read prio of the bucket, which gives us an
 *   indication of how hot the data is -- we scale the prio so that the prio
 *   farthest from the clock is worth 1/8th of the closest.
 *
 * - The number of sectors of cached data in the bucket, which gives us an
 *   indication of the cost in cache misses this eviction will cause.
 *
 * - If hotness * sectors used compares equal, we pick the bucket with the
 *   smallest bucket_gc_gen() - since incrementing the same bucket's generation
 *   number repeatedly forces us to run mark and sweep gc to avoid generation
 *   number wraparound.
 */

static unsigned long bucket_sort_key(struct bch_fs *c, struct bch_dev *ca,
				     size_t b, struct bucket_mark m)
{
	/*
	 * Time since last read, scaled to [0, 8) where larger value indicates
	 * more recently read data:
	 */
	unsigned long hotness =
		(bucket(ca, b)->prio[READ]	- ca->min_prio[READ]) * 7 /
		(c->prio_clock[READ].hand	- ca->min_prio[READ]);

	/* How much we want to keep the data in this bucket: */
	unsigned long data_wantness =
		(hotness + 1) * bucket_sectors_used(m);

	unsigned long needs_journal_commit =
		bucket_needs_journal_commit(m, c->journal.last_seq_ondisk);

	return  (data_wantness << 9) |
		(needs_journal_commit << 8) |
		bucket_gc_gen(ca, b);
}

static inline int bucket_alloc_cmp(alloc_heap *h,
				   struct alloc_heap_entry l,
				   struct alloc_heap_entry r)
{
	return (l.key > r.key) - (l.key < r.key);
}

static void find_reclaimable_buckets_lru(struct bch_fs *c, struct bch_dev *ca)
{
	struct bucket_array *buckets;
	struct alloc_heap_entry e;
	size_t b;

	ca->alloc_heap.used = 0;

	mutex_lock(&c->prio_clock[READ].lock);
	down_read(&ca->bucket_lock);

	buckets = bucket_array(ca);

	bch2_recalc_min_prio(c, ca, READ);

	/*
	 * Find buckets with lowest read priority, by building a maxheap sorted
	 * by read priority and repeatedly replacing the maximum element until
	 * all buckets have been visited.
	 */
	for (b = ca->mi.first_bucket; b < ca->mi.nbuckets; b++) {
		struct bucket_mark m = READ_ONCE(buckets->b[b].mark);

		if (!bch2_can_invalidate_bucket(ca, b, m))
			continue;

		e = (struct alloc_heap_entry) {
			.bucket = b,
			.key	= bucket_sort_key(c, ca, b, m)
		};

		heap_add_or_replace(&ca->alloc_heap, e, -bucket_alloc_cmp);
	}

	up_read(&ca->bucket_lock);
	mutex_unlock(&c->prio_clock[READ].lock);

	heap_resort(&ca->alloc_heap, bucket_alloc_cmp);

	/*
	 * If we run out of buckets to invalidate, bch2_allocator_thread() will
	 * kick stuff and retry us
	 */
	while (!fifo_full(&ca->free_inc) &&
	       heap_pop(&ca->alloc_heap, e, bucket_alloc_cmp))
		bch2_invalidate_one_bucket(c, ca, e.bucket);
}

static void find_reclaimable_buckets_fifo(struct bch_fs *c, struct bch_dev *ca)
{
	struct bucket_array *buckets = bucket_array(ca);
	struct bucket_mark m;
	size_t b, checked;

	for (checked = 0;
	     checked < ca->mi.nbuckets && !fifo_full(&ca->free_inc);
	     checked++) {
		if (ca->fifo_last_bucket <  ca->mi.first_bucket ||
		    ca->fifo_last_bucket >= ca->mi.nbuckets)
			ca->fifo_last_bucket = ca->mi.first_bucket;

		b = ca->fifo_last_bucket++;

		m = READ_ONCE(buckets->b[b].mark);

		if (bch2_can_invalidate_bucket(ca, b, m))
			bch2_invalidate_one_bucket(c, ca, b);
	}
}

static void find_reclaimable_buckets_random(struct bch_fs *c, struct bch_dev *ca)
{
	struct bucket_array *buckets = bucket_array(ca);
	struct bucket_mark m;
	size_t checked;

	for (checked = 0;
	     checked < ca->mi.nbuckets / 2 && !fifo_full(&ca->free_inc);
	     checked++) {
		size_t b = bch2_rand_range(ca->mi.nbuckets -
					   ca->mi.first_bucket) +
			ca->mi.first_bucket;

		m = READ_ONCE(buckets->b[b].mark);

		if (bch2_can_invalidate_bucket(ca, b, m))
			bch2_invalidate_one_bucket(c, ca, b);
	}
}

static void find_reclaimable_buckets(struct bch_fs *c, struct bch_dev *ca)
{
	ca->inc_gen_needs_gc			= 0;
	ca->inc_gen_really_needs_gc		= 0;

	switch (ca->mi.replacement) {
	case CACHE_REPLACEMENT_LRU:
		find_reclaimable_buckets_lru(c, ca);
		break;
	case CACHE_REPLACEMENT_FIFO:
		find_reclaimable_buckets_fifo(c, ca);
		break;
	case CACHE_REPLACEMENT_RANDOM:
		find_reclaimable_buckets_random(c, ca);
		break;
	}
}

static int size_t_cmp(const void *_l, const void *_r)
{
	const size_t *l = _l, *r = _r;

	return (*l > *r) - (*l < *r);
}

static void sort_free_inc(struct bch_fs *c, struct bch_dev *ca)
{
	BUG_ON(ca->free_inc.front);

	spin_lock(&c->freelist_lock);
	sort(ca->free_inc.data,
	     ca->free_inc.back,
	     sizeof(ca->free_inc.data[0]),
	     size_t_cmp, NULL);
	spin_unlock(&c->freelist_lock);
}

static int bch2_invalidate_free_inc(struct bch_fs *c, struct bch_dev *ca,
				    u64 *journal_seq, size_t nr)
{
	struct btree_iter iter;
	int ret = 0;

	bch2_btree_iter_init(&iter, c, BTREE_ID_ALLOC, POS(ca->dev_idx, 0),
			     BTREE_ITER_SLOTS|BTREE_ITER_INTENT);

	/*
	 * XXX: if ca->nr_invalidated != 0, just return if we'd block doing the
	 * btree update or journal_res_get
	 */
	while (ca->nr_invalidated < min(nr, fifo_used(&ca->free_inc))) {
		size_t b = fifo_idx_entry(&ca->free_inc, ca->nr_invalidated);

		ret = __bch2_alloc_write_key(c, ca, b, &iter, journal_seq);
		if (ret)
			break;

		ca->nr_invalidated++;
	}

	bch2_btree_iter_unlock(&iter);
	return ret;
}

static bool __push_invalidated_bucket(struct bch_fs *c, struct bch_dev *ca, size_t bucket)
{
	unsigned i;

	/*
	 * Don't remove from free_inc until after it's added to
	 * freelist, so gc can find it:
	 */
	spin_lock(&c->freelist_lock);
	for (i = 0; i < RESERVE_NR; i++)
		if (fifo_push(&ca->free[i], bucket)) {
			fifo_pop(&ca->free_inc, bucket);
			--ca->nr_invalidated;
			closure_wake_up(&c->freelist_wait);
			spin_unlock(&c->freelist_lock);
			return true;
		}
	spin_unlock(&c->freelist_lock);

	return false;
}

static int push_invalidated_bucket(struct bch_fs *c, struct bch_dev *ca, size_t bucket)
{
	int ret = 0;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);

		if (__push_invalidated_bucket(c, ca, bucket))
			break;

		if ((current->flags & PF_KTHREAD) &&
		    kthread_should_stop()) {
			ret = -1;
			break;
		}

		schedule();
		try_to_freeze();
	}

	__set_current_state(TASK_RUNNING);
	return ret;
}

/*
 * Given an invalidated, ready to use bucket: issue a discard to it if enabled,
 * then add it to the freelist, waiting until there's room if necessary:
 */
static int discard_invalidated_buckets(struct bch_fs *c, struct bch_dev *ca)
{
	while (ca->nr_invalidated) {
		size_t bucket = fifo_peek(&ca->free_inc);

		BUG_ON(fifo_empty(&ca->free_inc) || !ca->nr_invalidated);

		if (ca->mi.discard &&
		    blk_queue_discard(bdev_get_queue(ca->disk_sb.bdev)))
			blkdev_issue_discard(ca->disk_sb.bdev,
					     bucket_to_sector(ca, bucket),
					     ca->mi.bucket_size, GFP_NOIO, 0);

		if (push_invalidated_bucket(c, ca, bucket))
			return -1;
	}

	return 0;
}

/**
 * bch_allocator_thread - move buckets from free_inc to reserves
 *
 * The free_inc FIFO is populated by find_reclaimable_buckets(), and
 * the reserves are depleted by bucket allocation. When we run out
 * of free_inc, try to invalidate some buckets and write out
 * prios and gens.
 */
static int bch2_allocator_thread(void *arg)
{
	struct bch_dev *ca = arg;
	struct bch_fs *c = ca->fs;
	u64 journal_seq;
	int ret;

	set_freezable();

	while (1) {
		while (1) {
			ret = discard_invalidated_buckets(c, ca);
			if (ret)
				return 0;

			if (fifo_empty(&ca->free_inc))
				break;

			journal_seq = 0;
			ret = bch2_invalidate_free_inc(c, ca, &journal_seq, SIZE_MAX);
			if (ret)
				return 0;

			if (ca->allocator_invalidating_data)
				ret = bch2_journal_flush_seq(&c->journal, journal_seq);
			else if (ca->allocator_journal_seq_flush)
				ret = bch2_journal_flush_seq(&c->journal,
						       ca->allocator_journal_seq_flush);

			/*
			 * journal error - buckets haven't actually been
			 * invalidated, can't discard them:
			 */
			if (ret)
				return 0;
		}

		/* Reset front/back so we can easily sort fifo entries later: */
		ca->free_inc.front = ca->free_inc.back	= 0;
		ca->allocator_journal_seq_flush		= 0;
		ca->allocator_invalidating_data		= false;

		down_read(&c->gc_lock);
		if (test_bit(BCH_FS_GC_FAILURE, &c->flags)) {
			up_read(&c->gc_lock);
			return 0;
		}

		while (1) {
			/*
			 * Find some buckets that we can invalidate, either
			 * they're completely unused, or only contain clean data
			 * that's been written back to the backing device or
			 * another cache tier
			 */

			find_reclaimable_buckets(c, ca);
			trace_alloc_batch(ca, fifo_used(&ca->free_inc),
					  ca->free_inc.size);

			if ((ca->inc_gen_needs_gc >= ca->free_inc.size ||
			     (!fifo_full(&ca->free_inc) &&
			      ca->inc_gen_really_needs_gc >=
			      fifo_free(&ca->free_inc))) &&
			    c->gc_thread) {
				atomic_inc(&c->kick_gc);
				wake_up_process(c->gc_thread);
			}

			if (fifo_full(&ca->free_inc))
				break;

			if (wait_buckets_available(c, ca)) {
				up_read(&c->gc_lock);
				return 0;
			}
		}
		up_read(&c->gc_lock);

		sort_free_inc(c, ca);

		/*
		 * free_inc is now full of newly-invalidated buckets: next,
		 * write out the new bucket gens:
		 */
	}
}

/* Allocation */

/*
 * Open buckets represent a bucket that's currently being allocated from.  They
 * serve two purposes:
 *
 *  - They track buckets that have been partially allocated, allowing for
 *    sub-bucket sized allocations - they're used by the sector allocator below
 *
 *  - They provide a reference to the buckets they own that mark and sweep GC
 *    can find, until the new allocation has a pointer to it inserted into the
 *    btree
 *
 * When allocating some space with the sector allocator, the allocation comes
 * with a reference to an open bucket - the caller is required to put that
 * reference _after_ doing the index update that makes its allocation reachable.
 */

void __bch2_open_bucket_put(struct bch_fs *c, struct open_bucket *ob)
{
	struct bch_dev *ca = bch_dev_bkey_exists(c, ob->ptr.dev);

	spin_lock(&ob->lock);
	bch2_mark_alloc_bucket(c, ca, PTR_BUCKET_NR(ca, &ob->ptr),
			       false, gc_pos_alloc(c, ob), 0);
	ob->valid = false;
	spin_unlock(&ob->lock);

	spin_lock(&c->freelist_lock);
	ob->freelist = c->open_buckets_freelist;
	c->open_buckets_freelist = ob - c->open_buckets;
	c->open_buckets_nr_free++;
	spin_unlock(&c->freelist_lock);

	closure_wake_up(&c->open_buckets_wait);
}

static struct open_bucket *bch2_open_bucket_alloc(struct bch_fs *c)
{
	struct open_bucket *ob;

	BUG_ON(!c->open_buckets_freelist || !c->open_buckets_nr_free);

	ob = c->open_buckets + c->open_buckets_freelist;
	c->open_buckets_freelist = ob->freelist;
	atomic_set(&ob->pin, 1);

	c->open_buckets_nr_free--;
	return ob;
}

/* _only_ for allocating the journal and btree roots on a brand new fs: */
int bch2_bucket_alloc_startup(struct bch_fs *c, struct bch_dev *ca)
{
	struct bucket_array *buckets;
	ssize_t b;

	rcu_read_lock();
	buckets = bucket_array(ca);

	for (b = ca->mi.first_bucket; b < ca->mi.nbuckets; b++)
		if (is_available_bucket(buckets->b[b].mark)) {
			bch2_mark_alloc_bucket(c, ca, b, true,
					gc_pos_alloc(c, NULL),
					BCH_BUCKET_MARK_MAY_MAKE_UNAVAILABLE|
					BCH_BUCKET_MARK_GC_LOCK_HELD);
			set_bit(b, ca->buckets_dirty);
			goto success;
		}
	b = -1;
success:
	rcu_read_unlock();
	return b;
}

static inline unsigned open_buckets_reserved(enum alloc_reserve reserve)
{
	switch (reserve) {
	case RESERVE_ALLOC:
		return 0;
	case RESERVE_BTREE:
		return BTREE_NODE_RESERVE / 2;
	default:
		return BTREE_NODE_RESERVE;
	}
}

/**
 * bch_bucket_alloc - allocate a single bucket from a specific device
 *
 * Returns index of bucket on success, 0 on failure
 * */
int bch2_bucket_alloc(struct bch_fs *c, struct bch_dev *ca,
		      enum alloc_reserve reserve,
		      bool may_alloc_partial,
		      struct closure *cl)
{
	struct bucket_array *buckets;
	struct open_bucket *ob;
	long bucket;

	spin_lock(&c->freelist_lock);
	if (may_alloc_partial &&
	    ca->open_buckets_partial_nr) {
		int ret = ca->open_buckets_partial[--ca->open_buckets_partial_nr];
		c->open_buckets[ret].on_partial_list = false;
		spin_unlock(&c->freelist_lock);
		return ret;
	}

	if (unlikely(c->open_buckets_nr_free <= open_buckets_reserved(reserve))) {
		if (cl)
			closure_wait(&c->open_buckets_wait, cl);
		spin_unlock(&c->freelist_lock);
		trace_open_bucket_alloc_fail(ca, reserve);
		return OPEN_BUCKETS_EMPTY;
	}

	if (likely(fifo_pop(&ca->free[RESERVE_NONE], bucket)))
		goto out;

	switch (reserve) {
	case RESERVE_ALLOC:
		if (fifo_pop(&ca->free[RESERVE_BTREE], bucket))
			goto out;
		break;
	case RESERVE_BTREE:
		if (fifo_used(&ca->free[RESERVE_BTREE]) * 2 >=
		    ca->free[RESERVE_BTREE].size &&
		    fifo_pop(&ca->free[RESERVE_BTREE], bucket))
			goto out;
		break;
	case RESERVE_MOVINGGC:
		if (fifo_pop(&ca->free[RESERVE_MOVINGGC], bucket))
			goto out;
		break;
	default:
		break;
	}

	if (unlikely(test_bit(BCH_FS_BRAND_NEW_FS, &c->flags)) &&
	    (bucket = bch2_bucket_alloc_startup(c, ca)) >= 0)
		goto out;

	spin_unlock(&c->freelist_lock);

	trace_bucket_alloc_fail(ca, reserve);
	return FREELIST_EMPTY;
out:
	verify_not_on_freelist(c, ca, bucket);

	ob = bch2_open_bucket_alloc(c);

	spin_lock(&ob->lock);
	lg_local_lock(&c->usage_lock);
	buckets = bucket_array(ca);

	ob->valid	= true;
	ob->sectors_free = ca->mi.bucket_size;
	ob->ptr		= (struct bch_extent_ptr) {
		.gen	= buckets->b[bucket].mark.gen,
		.offset	= bucket_to_sector(ca, bucket),
		.dev	= ca->dev_idx,
	};

	bucket_io_clock_reset(c, ca, bucket, READ);
	bucket_io_clock_reset(c, ca, bucket, WRITE);

	lg_local_unlock(&c->usage_lock);
	spin_unlock(&ob->lock);

	spin_unlock(&c->freelist_lock);

	bch2_wake_allocator(ca);

	trace_bucket_alloc(ca, reserve);
	return ob - c->open_buckets;
}

struct dev_alloc_list bch2_wp_alloc_list(struct bch_fs *c,
					 struct write_point *wp,
					 struct bch_devs_mask *devs)
{
	struct dev_alloc_list ret = { .nr = 0 };
	struct bch_dev *ca, *ca2;
	unsigned i, j;

	for_each_member_device_rcu(ca, c, i, devs) {
		for (j = 0; j < ret.nr; j++) {
			unsigned idx = ret.devs[j];

			ca2 = rcu_dereference(c->devs[idx]);
			if (!ca2)
				break;

			if (ca->mi.tier < ca2->mi.tier)
				break;

			if (ca->mi.tier == ca2->mi.tier &&
			    wp->next_alloc[i] < wp->next_alloc[idx])
				break;
		}

		array_insert_item(ret.devs, ret.nr, j, i);
	}

	return ret;
}

void bch2_wp_rescale(struct bch_fs *c, struct bch_dev *ca,
		     struct write_point *wp)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(wp->next_alloc); i++)
		wp->next_alloc[i] >>= 1;
}

static enum bucket_alloc_ret __bch2_bucket_alloc_set(struct bch_fs *c,
					struct write_point *wp,
					unsigned nr_replicas,
					enum alloc_reserve reserve,
					struct bch_devs_mask *devs,
					struct closure *cl)
{
	enum bucket_alloc_ret ret = NO_DEVICES;
	struct dev_alloc_list devs_sorted;
	u64 buckets_free;
	unsigned i;

	BUG_ON(nr_replicas > ARRAY_SIZE(wp->ptrs));

	if (wp->nr_ptrs >= nr_replicas)
		return ALLOC_SUCCESS;

	rcu_read_lock();
	devs_sorted = bch2_wp_alloc_list(c, wp, devs);

	for (i = 0; i < devs_sorted.nr; i++) {
		struct bch_dev *ca =
			rcu_dereference(c->devs[devs_sorted.devs[i]]);
		int ob;

		if (!ca)
			continue;

		ob = bch2_bucket_alloc(c, ca, reserve,
				       wp->type == BCH_DATA_USER, cl);
		if (ob < 0) {
			ret = ob;
			if (ret == OPEN_BUCKETS_EMPTY)
				break;
			continue;
		}

		BUG_ON(ob <= 0 || ob > U8_MAX);
		BUG_ON(wp->nr_ptrs >= ARRAY_SIZE(wp->ptrs));
		wp->ptrs[wp->nr_ptrs++] = c->open_buckets + ob;

		buckets_free = U64_MAX, dev_buckets_free(c, ca);
		if (buckets_free)
			wp->next_alloc[ca->dev_idx] +=
				div64_u64(U64_MAX, buckets_free *
					  ca->mi.bucket_size);
		else
			wp->next_alloc[ca->dev_idx] = U64_MAX;
		bch2_wp_rescale(c, ca, wp);

		__clear_bit(ca->dev_idx, devs->d);

		if (wp->nr_ptrs == nr_replicas) {
			ret = ALLOC_SUCCESS;
			break;
		}
	}

	EBUG_ON(reserve == RESERVE_MOVINGGC &&
		ret != ALLOC_SUCCESS &&
		ret != OPEN_BUCKETS_EMPTY);
	rcu_read_unlock();
	return ret;
}

static int bch2_bucket_alloc_set(struct bch_fs *c, struct write_point *wp,
				unsigned nr_replicas,
				enum alloc_reserve reserve,
				struct bch_devs_mask *devs,
				struct closure *cl)
{
	bool waiting = false;

	while (1) {
		switch (__bch2_bucket_alloc_set(c, wp, nr_replicas,
						reserve, devs, cl)) {
		case ALLOC_SUCCESS:
			if (waiting)
				closure_wake_up(&c->freelist_wait);

			return 0;

		case NO_DEVICES:
			if (waiting)
				closure_wake_up(&c->freelist_wait);
			return -EROFS;

		case FREELIST_EMPTY:
			if (!cl)
				return -ENOSPC;

			if (waiting)
				return -EAGAIN;

			/* Retry allocation after adding ourself to waitlist: */
			closure_wait(&c->freelist_wait, cl);
			waiting = true;
			break;
		case OPEN_BUCKETS_EMPTY:
			return cl ? -EAGAIN : -ENOSPC;
		default:
			BUG();
		}
	}
}

/* Sector allocator */

static void writepoint_drop_ptrs(struct bch_fs *c,
				 struct write_point *wp,
				 struct bch_devs_mask *devs,
				 unsigned nr_ptrs_dislike)
{
	int i;

	if (!nr_ptrs_dislike)
		return;

	for (i = wp->nr_ptrs - 1; i >= 0; --i) {
		struct open_bucket *ob = wp->ptrs[i];
		struct bch_dev *ca = bch_dev_bkey_exists(c, ob->ptr.dev);

		if (nr_ptrs_dislike && !test_bit(ob->ptr.dev, devs->d)) {
			BUG_ON(ca->open_buckets_partial_nr >=
			       ARRAY_SIZE(ca->open_buckets_partial));

			spin_lock(&c->freelist_lock);
			ob->on_partial_list = true;
			ca->open_buckets_partial[ca->open_buckets_partial_nr++] =
				ob - c->open_buckets;
			spin_unlock(&c->freelist_lock);

			closure_wake_up(&c->open_buckets_wait);
			closure_wake_up(&c->freelist_wait);

			array_remove_item(wp->ptrs, wp->nr_ptrs, i);
			--nr_ptrs_dislike;
		}
	}
}

static void verify_not_stale(struct bch_fs *c, const struct write_point *wp)
{
#ifdef CONFIG_BCACHEFS_DEBUG
	struct open_bucket *ob;
	unsigned i;

	writepoint_for_each_ptr(wp, ob, i) {
		struct bch_dev *ca = bch_dev_bkey_exists(c, ob->ptr.dev);

		BUG_ON(ptr_stale(ca, &ob->ptr));
	}
#endif
}

static int open_bucket_add_buckets(struct bch_fs *c,
				   struct bch_devs_mask *_devs,
				   struct write_point *wp,
				   struct bch_devs_list *devs_have,
				   unsigned nr_replicas,
				   enum alloc_reserve reserve,
				   struct closure *cl)
{
	struct bch_devs_mask devs = c->rw_devs[wp->type];
	struct open_bucket *ob;
	unsigned i;

	if (wp->nr_ptrs >= nr_replicas)
		return 0;

	/* Don't allocate from devices we already have pointers to: */
	for (i = 0; i < devs_have->nr; i++)
		__clear_bit(devs_have->devs[i], devs.d);

	writepoint_for_each_ptr(wp, ob, i)
		__clear_bit(ob->ptr.dev, devs.d);

	if (_devs)
		bitmap_and(devs.d, devs.d, _devs->d, BCH_SB_MEMBERS_MAX);

	return bch2_bucket_alloc_set(c, wp, nr_replicas, reserve, &devs, cl);
}

static struct write_point *__writepoint_find(struct hlist_head *head,
					     unsigned long write_point)
{
	struct write_point *wp;

	hlist_for_each_entry_rcu(wp, head, node)
		if (wp->write_point == write_point)
			return wp;

	return NULL;
}

static struct hlist_head *writepoint_hash(struct bch_fs *c,
					  unsigned long write_point)
{
	unsigned hash =
		hash_long(write_point, ilog2(ARRAY_SIZE(c->write_points_hash)));

	return &c->write_points_hash[hash];
}

static struct write_point *writepoint_find(struct bch_fs *c,
					   unsigned long write_point)
{
	struct write_point *wp, *oldest;
	struct hlist_head *head;

	if (!(write_point & 1UL)) {
		wp = (struct write_point *) write_point;
		mutex_lock(&wp->lock);
		return wp;
	}

	head = writepoint_hash(c, write_point);
restart_find:
	wp = __writepoint_find(head, write_point);
	if (wp) {
lock_wp:
		mutex_lock(&wp->lock);
		if (wp->write_point == write_point)
			goto out;
		mutex_unlock(&wp->lock);
		goto restart_find;
	}

	oldest = NULL;
	for (wp = c->write_points;
	     wp < c->write_points + ARRAY_SIZE(c->write_points);
	     wp++)
		if (!oldest || time_before64(wp->last_used, oldest->last_used))
			oldest = wp;

	mutex_lock(&oldest->lock);
	mutex_lock(&c->write_points_hash_lock);
	wp = __writepoint_find(head, write_point);
	if (wp && wp != oldest) {
		mutex_unlock(&c->write_points_hash_lock);
		mutex_unlock(&oldest->lock);
		goto lock_wp;
	}

	wp = oldest;
	hlist_del_rcu(&wp->node);
	wp->write_point = write_point;
	hlist_add_head_rcu(&wp->node, head);
	mutex_unlock(&c->write_points_hash_lock);
out:
	wp->last_used = sched_clock();
	return wp;
}

/*
 * Get us an open_bucket we can allocate from, return with it locked:
 */
struct write_point *bch2_alloc_sectors_start(struct bch_fs *c,
				struct bch_devs_mask *devs,
				struct write_point_specifier write_point,
				struct bch_devs_list *devs_have,
				unsigned nr_replicas,
				unsigned nr_replicas_required,
				enum alloc_reserve reserve,
				unsigned flags,
				struct closure *cl)
{
	struct write_point *wp;
	struct open_bucket *ob;
	unsigned i, nr_ptrs_dislike = 0, nr_ptrs_have = 0;
	int ret;

	BUG_ON(!nr_replicas || !nr_replicas_required);

	wp = writepoint_find(c, write_point.v);

	/* does ob have ptrs we don't need? */
	writepoint_for_each_ptr(wp, ob, i)
		if (bch2_dev_list_has_dev(*devs_have, ob->ptr.dev))
			nr_ptrs_have++;
		else if (devs && !test_bit(ob->ptr.dev, devs->d))
			nr_ptrs_dislike++;

	ret = open_bucket_add_buckets(c, devs, wp, devs_have,
				nr_replicas + nr_ptrs_have + nr_ptrs_dislike,
				reserve, cl);
	if (ret && ret != -EROFS)
		goto err;

	if (wp->nr_ptrs <
	    nr_ptrs_have + nr_ptrs_dislike + nr_replicas_required) {
		ret = -EROFS;
		goto err;
	}

	if ((int) wp->nr_ptrs - nr_ptrs_dislike < nr_replicas)
		nr_ptrs_dislike = clamp_t(int, wp->nr_ptrs - nr_replicas,
					  0, nr_ptrs_dislike);

	/* Remove pointers we don't want to use: */
	writepoint_drop_ptrs(c, wp, devs, nr_ptrs_dislike);

	/*
	 * Move pointers to devices we already have to end of open bucket
	 * pointer list - note that removing pointers we don't want to use might
	 * have changed nr_ptrs_have:
	 */
	if (nr_ptrs_have) {
		i = nr_ptrs_have = 0;
		while (i < wp->nr_ptrs - nr_ptrs_have)
			if (bch2_dev_list_has_dev(*devs_have, wp->ptrs[i]->ptr.dev)) {
				nr_ptrs_have++;
				swap(wp->ptrs[i], wp->ptrs[wp->nr_ptrs - nr_ptrs_have]);
			} else {
				i++;
			}
	}

	wp->nr_ptrs_can_use =
		min_t(unsigned, nr_replicas, wp->nr_ptrs - nr_ptrs_have);

	BUG_ON(wp->nr_ptrs_can_use < nr_replicas_required ||
	       wp->nr_ptrs_can_use > wp->nr_ptrs);

	wp->sectors_free = UINT_MAX;

	for (i = 0; i < wp->nr_ptrs_can_use; i++)
		wp->sectors_free = min(wp->sectors_free,
				       wp->ptrs[i]->sectors_free);

	BUG_ON(!wp->sectors_free || wp->sectors_free == UINT_MAX);

	verify_not_stale(c, wp);

	return wp;
err:
	mutex_unlock(&wp->lock);
	return ERR_PTR(ret);
}

/*
 * Append pointers to the space we just allocated to @k, and mark @sectors space
 * as allocated out of @ob
 */
void bch2_alloc_sectors_append_ptrs(struct bch_fs *c, struct write_point *wp,
				    struct bkey_i_extent *e, unsigned sectors)
{
	unsigned i;

	BUG_ON(sectors > wp->sectors_free);
	wp->sectors_free -= sectors;

	for (i = 0; i < wp->nr_ptrs_can_use; i++) {
		struct open_bucket *ob = wp->ptrs[i];
		struct bch_dev *ca = bch_dev_bkey_exists(c, ob->ptr.dev);
		struct bch_extent_ptr tmp = ob->ptr;

		EBUG_ON(bch2_extent_has_device(extent_i_to_s_c(e), ob->ptr.dev));

		tmp.cached = bkey_extent_is_cached(&e->k);
		tmp.offset += ca->mi.bucket_size - ob->sectors_free;
		extent_ptr_append(e, tmp);

		BUG_ON(sectors > ob->sectors_free);
		ob->sectors_free -= sectors;
	}
}

/*
 * Append pointers to the space we just allocated to @k, and mark @sectors space
 * as allocated out of @ob
 */
void bch2_alloc_sectors_done(struct bch_fs *c, struct write_point *wp)
{
	int i;

	for (i = wp->nr_ptrs - 1; i >= 0; --i) {
		struct open_bucket *ob = wp->ptrs[i];

		if (!ob->sectors_free) {
			array_remove_item(wp->ptrs, wp->nr_ptrs, i);
			bch2_open_bucket_put(c, ob);
		}
	}

	mutex_unlock(&wp->lock);
}

/* Startup/shutdown (ro/rw): */

void bch2_recalc_capacity(struct bch_fs *c)
{
	struct bch_tier *fastest_tier = NULL, *slowest_tier = NULL, *tier;
	struct bch_dev *ca;
	u64 total_capacity, capacity = 0, reserved_sectors = 0;
	unsigned long ra_pages = 0;
	unsigned i, j;

	lockdep_assert_held(&c->state_lock);

	for_each_online_member(ca, c, i) {
		struct backing_dev_info *bdi = ca->disk_sb.bdev->bd_bdi;

		ra_pages += bdi->ra_pages;
	}

	bch2_set_ra_pages(c, ra_pages);

	/* Find fastest, slowest tiers with devices: */

	for (tier = c->tiers;
	     tier < c->tiers + ARRAY_SIZE(c->tiers); tier++) {
		if (!dev_mask_nr(&tier->devs))
			continue;
		if (!fastest_tier)
			fastest_tier = tier;
		slowest_tier = tier;
	}

	c->fastest_tier = fastest_tier != slowest_tier ? fastest_tier : NULL;
	c->fastest_devs = fastest_tier != slowest_tier ? &fastest_tier->devs : NULL;

	if (!fastest_tier)
		goto set_capacity;

	/*
	 * Capacity of the filesystem is the capacity of all the devices in the
	 * slowest (highest) tier - we don't include lower tier devices.
	 */
	for_each_member_device_rcu(ca, c, i, &slowest_tier->devs) {
		size_t reserve = 0;

		/*
		 * We need to reserve buckets (from the number
		 * of currently available buckets) against
		 * foreground writes so that mainly copygc can
		 * make forward progress.
		 *
		 * We need enough to refill the various reserves
		 * from scratch - copygc will use its entire
		 * reserve all at once, then run against when
		 * its reserve is refilled (from the formerly
		 * available buckets).
		 *
		 * This reserve is just used when considering if
		 * allocations for foreground writes must wait -
		 * not -ENOSPC calculations.
		 */
		for (j = 0; j < RESERVE_NONE; j++)
			reserve += ca->free[j].size;

		reserve += ca->free_inc.size;

		reserve += ARRAY_SIZE(c->write_points);

		if (ca->mi.tier)
			reserve += 1;	/* tiering write point */
		reserve += 1;		/* btree write point */

		reserved_sectors += bucket_to_sector(ca, reserve);

		capacity += bucket_to_sector(ca, ca->mi.nbuckets -
					     ca->mi.first_bucket);
	}
set_capacity:
	total_capacity = capacity;

	capacity *= (100 - c->opts.gc_reserve_percent);
	capacity = div64_u64(capacity, 100);

	BUG_ON(reserved_sectors > total_capacity);

	capacity = min(capacity, total_capacity - reserved_sectors);

	c->capacity = capacity;

	if (c->capacity) {
		bch2_io_timer_add(&c->io_clock[READ],
				 &c->prio_clock[READ].rescale);
		bch2_io_timer_add(&c->io_clock[WRITE],
				 &c->prio_clock[WRITE].rescale);
	} else {
		bch2_io_timer_del(&c->io_clock[READ],
				 &c->prio_clock[READ].rescale);
		bch2_io_timer_del(&c->io_clock[WRITE],
				 &c->prio_clock[WRITE].rescale);
	}

	/* Wake up case someone was waiting for buckets */
	closure_wake_up(&c->freelist_wait);
}

static void bch2_stop_write_point(struct bch_fs *c, struct bch_dev *ca,
				  struct write_point *wp)
{
	struct bch_devs_mask not_self;

	bitmap_complement(not_self.d, ca->self.d, BCH_SB_MEMBERS_MAX);

	mutex_lock(&wp->lock);
	writepoint_drop_ptrs(c, wp, &not_self, wp->nr_ptrs);
	mutex_unlock(&wp->lock);
}

static bool bch2_dev_has_open_write_point(struct bch_fs *c, struct bch_dev *ca)
{
	struct open_bucket *ob;
	bool ret = false;

	for (ob = c->open_buckets;
	     ob < c->open_buckets + ARRAY_SIZE(c->open_buckets);
	     ob++) {
		spin_lock(&ob->lock);
		if (ob->valid && !ob->on_partial_list &&
		    ob->ptr.dev == ca->dev_idx)
			ret = true;
		spin_unlock(&ob->lock);
	}

	return ret;
}

/* device goes ro: */
void bch2_dev_allocator_remove(struct bch_fs *c, struct bch_dev *ca)
{
	unsigned i;

	BUG_ON(ca->alloc_thread);

	/* First, remove device from allocation groups: */

	clear_bit(ca->dev_idx, c->tiers[ca->mi.tier].devs.d);
	for (i = 0; i < ARRAY_SIZE(c->rw_devs); i++)
		clear_bit(ca->dev_idx, c->rw_devs[i].d);

	/*
	 * Capacity is calculated based off of devices in allocation groups:
	 */
	bch2_recalc_capacity(c);

	/* Next, close write points that point to this device... */
	for (i = 0; i < ARRAY_SIZE(c->write_points); i++)
		bch2_stop_write_point(c, ca, &c->write_points[i]);

	bch2_stop_write_point(c, ca, &ca->copygc_write_point);
	bch2_stop_write_point(c, ca, &c->tiers[ca->mi.tier].wp);
	bch2_stop_write_point(c, ca, &c->btree_write_point);

	mutex_lock(&c->btree_reserve_cache_lock);
	while (c->btree_reserve_cache_nr) {
		struct btree_alloc *a =
			&c->btree_reserve_cache[--c->btree_reserve_cache_nr];

		bch2_open_bucket_put_refs(c, &a->ob.nr, a->ob.refs);
	}
	mutex_unlock(&c->btree_reserve_cache_lock);

	/*
	 * Wake up threads that were blocked on allocation, so they can notice
	 * the device can no longer be removed and the capacity has changed:
	 */
	closure_wake_up(&c->freelist_wait);

	/*
	 * journal_res_get() can block waiting for free space in the journal -
	 * it needs to notice there may not be devices to allocate from anymore:
	 */
	wake_up(&c->journal.wait);

	/* Now wait for any in flight writes: */

	closure_wait_event(&c->open_buckets_wait,
			   !bch2_dev_has_open_write_point(c, ca));
}

/* device goes rw: */
void bch2_dev_allocator_add(struct bch_fs *c, struct bch_dev *ca)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(c->rw_devs); i++)
		if (ca->mi.data_allowed & (1 << i))
			set_bit(ca->dev_idx, c->rw_devs[i].d);
	set_bit(ca->dev_idx, c->tiers[ca->mi.tier].devs.d);
}

/* stop allocator thread: */
void bch2_dev_allocator_stop(struct bch_dev *ca)
{
	struct task_struct *p = ca->alloc_thread;

	ca->alloc_thread = NULL;

	/*
	 * We need an rcu barrier between setting ca->alloc_thread = NULL and
	 * the thread shutting down to avoid bch2_wake_allocator() racing:
	 *
	 * XXX: it would be better to have the rcu barrier be asynchronous
	 * instead of blocking us here
	 */
	synchronize_rcu();

	if (p) {
		kthread_stop(p);
		put_task_struct(p);
	}
}

/* start allocator thread: */
int bch2_dev_allocator_start(struct bch_dev *ca)
{
	struct task_struct *p;

	/*
	 * allocator thread already started?
	 */
	if (ca->alloc_thread)
		return 0;

	p = kthread_create(bch2_allocator_thread, ca, "bcache_allocator");
	if (IS_ERR(p))
		return PTR_ERR(p);

	get_task_struct(p);
	ca->alloc_thread = p;
	wake_up_process(p);
	return 0;
}

static void allocator_start_issue_discards(struct bch_fs *c)
{
	struct bch_dev *ca;
	unsigned dev_iter;
	size_t i, bu;

	for_each_rw_member(ca, c, dev_iter) {
		unsigned done = 0;

		fifo_for_each_entry(bu, &ca->free_inc, i) {
			if (done == ca->nr_invalidated)
				break;

			blkdev_issue_discard(ca->disk_sb.bdev,
					     bucket_to_sector(ca, bu),
					     ca->mi.bucket_size, GFP_NOIO, 0);
			done++;
		}
	}
}

static int __bch2_fs_allocator_start(struct bch_fs *c)
{
	struct bch_dev *ca;
	size_t bu, i, devs_have_enough = 0;
	unsigned dev_iter;
	u64 journal_seq = 0;
	bool invalidating_data = false;
	int ret = 0;

	if (test_bit(BCH_FS_GC_FAILURE, &c->flags))
		return -1;

	/* Scan for buckets that are already invalidated: */
	for_each_rw_member(ca, c, dev_iter) {
		struct btree_iter iter;
		struct bucket_mark m;
		struct bkey_s_c k;

		for_each_btree_key(&iter, c, BTREE_ID_ALLOC, POS(ca->dev_idx, 0), 0, k) {
			if (k.k->type != BCH_ALLOC)
				continue;

			bu = k.k->p.offset;
			m = READ_ONCE(bucket(ca, bu)->mark);

			if (!is_available_bucket(m) || m.cached_sectors)
				continue;

			bch2_mark_alloc_bucket(c, ca, bu, true,
					gc_pos_alloc(c, NULL),
					BCH_BUCKET_MARK_MAY_MAKE_UNAVAILABLE|
					BCH_BUCKET_MARK_GC_LOCK_HELD);

			fifo_push(&ca->free_inc, bu);
			ca->nr_invalidated++;

			if (fifo_full(&ca->free_inc))
				break;
		}
		bch2_btree_iter_unlock(&iter);
	}

	/* did we find enough buckets? */
	for_each_rw_member(ca, c, dev_iter)
		devs_have_enough += (fifo_used(&ca->free_inc) >=
				     ca->free[RESERVE_BTREE].size);

	if (devs_have_enough >= c->opts.metadata_replicas)
		return 0;

	/* clear out free_inc - find_reclaimable_buckets() assumes it's empty */
	for_each_rw_member(ca, c, dev_iter)
		discard_invalidated_buckets(c, ca);

	for_each_rw_member(ca, c, dev_iter) {
		BUG_ON(!fifo_empty(&ca->free_inc));
		ca->free_inc.front = ca->free_inc.back	= 0;

		find_reclaimable_buckets(c, ca);
		sort_free_inc(c, ca);

		invalidating_data |= ca->allocator_invalidating_data;

		fifo_for_each_entry(bu, &ca->free_inc, i)
			if (!fifo_push(&ca->free[RESERVE_BTREE], bu))
				break;
	}

	/*
	 * We're moving buckets to freelists _before_ they've been marked as
	 * invalidated on disk - we have to so that we can allocate new btree
	 * nodes to mark them as invalidated on disk.
	 *
	 * However, we can't _write_ to any of these buckets yet - they might
	 * have cached data in them, which is live until they're marked as
	 * invalidated on disk:
	 */
	if (invalidating_data)
		set_bit(BCH_FS_HOLD_BTREE_WRITES, &c->flags);
	else
		allocator_start_issue_discards(c);

	/*
	 * XXX: it's possible for this to deadlock waiting on journal reclaim,
	 * since we're holding btree writes. What then?
	 */

	for_each_rw_member(ca, c, dev_iter) {
		ret = bch2_invalidate_free_inc(c, ca, &journal_seq,
					       ca->free[RESERVE_BTREE].size);
		if (ret) {
			percpu_ref_put(&ca->io_ref);
			return ret;
		}
	}

	if (invalidating_data) {
		ret = bch2_journal_flush_seq(&c->journal, journal_seq);
		if (ret)
			return ret;
	}

	if (invalidating_data)
		allocator_start_issue_discards(c);

	for_each_rw_member(ca, c, dev_iter)
		while (ca->nr_invalidated) {
			BUG_ON(!fifo_pop(&ca->free_inc, bu));
			ca->nr_invalidated--;
		}

	set_bit(BCH_FS_ALLOCATOR_STARTED, &c->flags);

	/* now flush dirty btree nodes: */
	if (invalidating_data) {
		struct bucket_table *tbl;
		struct rhash_head *pos;
		struct btree *b;

		clear_bit(BCH_FS_HOLD_BTREE_WRITES, &c->flags);
again:
		rcu_read_lock();
		for_each_cached_btree(b, c, tbl, i, pos)
			if (btree_node_dirty(b) && (!b->written || b->level)) {
				rcu_read_unlock();
				six_lock_read(&b->lock);
				bch2_btree_node_write(c, b, SIX_LOCK_read);
				six_unlock_read(&b->lock);
				goto again;
			}
		rcu_read_unlock();
	}

	return 0;
}

int bch2_fs_allocator_start(struct bch_fs *c)
{
	struct bch_dev *ca;
	unsigned i;
	int ret;

	down_read(&c->gc_lock);
	ret = __bch2_fs_allocator_start(c);
	up_read(&c->gc_lock);

	if (ret)
		return ret;

	for_each_rw_member(ca, c, i) {
		ret = bch2_dev_allocator_start(ca);
		if (ret) {
			percpu_ref_put(&ca->io_ref);
			return ret;
		}
	}

	return bch2_alloc_write(c);
}

void bch2_fs_allocator_init(struct bch_fs *c)
{
	struct open_bucket *ob;
	struct write_point *wp;
	unsigned i;

	mutex_init(&c->write_points_hash_lock);
	spin_lock_init(&c->freelist_lock);
	bch2_prio_timer_init(c, READ);
	bch2_prio_timer_init(c, WRITE);

	/* open bucket 0 is a sentinal NULL: */
	spin_lock_init(&c->open_buckets[0].lock);

	for (ob = c->open_buckets + 1;
	     ob < c->open_buckets + ARRAY_SIZE(c->open_buckets); ob++) {
		spin_lock_init(&ob->lock);
		c->open_buckets_nr_free++;

		ob->freelist = c->open_buckets_freelist;
		c->open_buckets_freelist = ob - c->open_buckets;
	}

	writepoint_init(&c->btree_write_point, BCH_DATA_BTREE);

	for (i = 0; i < ARRAY_SIZE(c->tiers); i++)
		writepoint_init(&c->tiers[i].wp, BCH_DATA_USER);

	for (wp = c->write_points;
	     wp < c->write_points + ARRAY_SIZE(c->write_points); wp++) {
		writepoint_init(wp, BCH_DATA_USER);

		wp->last_used	= sched_clock();
		wp->write_point	= (unsigned long) wp;
		hlist_add_head_rcu(&wp->node, writepoint_hash(c, wp->write_point));
	}

	c->pd_controllers_update_seconds = 5;
	INIT_DELAYED_WORK(&c->pd_controllers_update, pd_controllers_update);
}
