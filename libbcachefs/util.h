#ifndef _BCACHEFS_UTIL_H
#define _BCACHEFS_UTIL_H

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/closure.h>
#include <linux/errno.h>
#include <linux/freezer.h>
#include <linux/kernel.h>
#include <linux/sched/clock.h>
#include <linux/llist.h>
#include <linux/log2.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#define PAGE_SECTOR_SHIFT	(PAGE_SHIFT - 9)
#define PAGE_SECTORS		(1UL << PAGE_SECTOR_SHIFT)

struct closure;

#ifdef CONFIG_BCACHEFS_DEBUG

#define EBUG_ON(cond)		BUG_ON(cond)
#define atomic_dec_bug(v)	BUG_ON(atomic_dec_return(v) < 0)
#define atomic_inc_bug(v, i)	BUG_ON(atomic_inc_return(v) <= i)
#define atomic_sub_bug(i, v)	BUG_ON(atomic_sub_return(i, v) < 0)
#define atomic_add_bug(i, v)	BUG_ON(atomic_add_return(i, v) < 0)
#define atomic_long_dec_bug(v)		BUG_ON(atomic_long_dec_return(v) < 0)
#define atomic_long_sub_bug(i, v)	BUG_ON(atomic_long_sub_return(i, v) < 0)
#define atomic64_dec_bug(v)	BUG_ON(atomic64_dec_return(v) < 0)
#define atomic64_inc_bug(v, i)	BUG_ON(atomic64_inc_return(v) <= i)
#define atomic64_sub_bug(i, v)	BUG_ON(atomic64_sub_return(i, v) < 0)
#define atomic64_add_bug(i, v)	BUG_ON(atomic64_add_return(i, v) < 0)

#define memcpy(dst, src, len)						\
({									\
	void *_dst = (dst);						\
	const void *_src = (src);					\
	size_t _len = (len);						\
									\
	BUG_ON(!((void *) (_dst) >= (void *) (_src) + (_len) ||		\
		 (void *) (_dst) + (_len) <= (void *) (_src)));		\
	memcpy(_dst, _src, _len);					\
})

#else /* DEBUG */

#define EBUG_ON(cond)
#define atomic_dec_bug(v)	atomic_dec(v)
#define atomic_inc_bug(v, i)	atomic_inc(v)
#define atomic_sub_bug(i, v)	atomic_sub(i, v)
#define atomic_add_bug(i, v)	atomic_add(i, v)
#define atomic_long_dec_bug(v)		atomic_long_dec(v)
#define atomic_long_sub_bug(i, v)	atomic_long_sub(i, v)
#define atomic64_dec_bug(v)	atomic64_dec(v)
#define atomic64_inc_bug(v, i)	atomic64_inc(v)
#define atomic64_sub_bug(i, v)	atomic64_sub(i, v)
#define atomic64_add_bug(i, v)	atomic64_add(i, v)

#endif

#ifndef __CHECKER__
#define __flatten __attribute__((flatten))
#else
/* sparse doesn't know about attribute((flatten)) */
#define __flatten
#endif

#ifdef __LITTLE_ENDIAN
#define CPU_BIG_ENDIAN		0
#else
#define CPU_BIG_ENDIAN		1
#endif

/* type hackery */

#define type_is_exact(_val, _type)					\
	__builtin_types_compatible_p(typeof(_val), _type)

#define type_is(_val, _type)						\
	(__builtin_types_compatible_p(typeof(_val), _type) ||		\
	 __builtin_types_compatible_p(typeof(_val), const _type))

static inline void vpfree(void *p, size_t size)
{
	if (is_vmalloc_addr(p))
		vfree(p);
	else
		free_pages((unsigned long) p, get_order(size));
}

static inline void *vpmalloc(size_t size, gfp_t gfp_mask)
{
	return (void *) __get_free_pages(gfp_mask|__GFP_NOWARN,
					 get_order(size)) ?:
		__vmalloc(size, gfp_mask, PAGE_KERNEL);
}

static inline void kvpfree(void *p, size_t size)
{
	if (size < PAGE_SIZE)
		kfree(p);
	else
		vpfree(p, size);
}

static inline void *kvpmalloc(size_t size, gfp_t gfp_mask)
{
	return size < PAGE_SIZE
		? kmalloc(size, gfp_mask)
		: vpmalloc(size, gfp_mask);
}

void mempool_free_vp(void *element, void *pool_data);
void *mempool_alloc_vp(gfp_t gfp_mask, void *pool_data);

static inline int mempool_init_vp_pool(mempool_t *pool, int min_nr, size_t size)
{
	return mempool_init(pool, min_nr, mempool_alloc_vp,
			    mempool_free_vp, (void *) size);
}

#define HEAP(type)							\
struct {								\
	size_t size, used;						\
	type *data;							\
}

#define DECLARE_HEAP(type, name) HEAP(type) name

#define init_heap(heap, _size, gfp)					\
({									\
	(heap)->used = 0;						\
	(heap)->size = (_size);						\
	(heap)->data = kvpmalloc((heap)->size * sizeof((heap)->data[0]),\
				 (gfp));				\
})

#define free_heap(heap)							\
do {									\
	kvpfree((heap)->data, (heap)->size * sizeof((heap)->data[0]));	\
	(heap)->data = NULL;						\
} while (0)

#define heap_swap(h, i, j)	swap((h)->data[i], (h)->data[j])

#define heap_peek(h)							\
({									\
	EBUG_ON(!(h)->used);						\
	(h)->data[0];							\
})

#define heap_full(h)	((h)->used == (h)->size)

#define heap_sift_down(h, i, cmp)					\
do {									\
	size_t _c, _j = i;						\
									\
	for (; _j * 2 + 1 < (h)->used; _j = _c) {			\
		_c = _j * 2 + 1;					\
		if (_c + 1 < (h)->used &&				\
		    cmp(h, (h)->data[_c], (h)->data[_c + 1]) >= 0)	\
			_c++;						\
									\
		if (cmp(h, (h)->data[_c], (h)->data[_j]) >= 0)		\
			break;						\
		heap_swap(h, _c, _j);					\
	}								\
} while (0)

#define heap_sift_up(h, i, cmp)						\
do {									\
	while (i) {							\
		size_t p = (i - 1) / 2;					\
		if (cmp(h, (h)->data[i], (h)->data[p]) >= 0)		\
			break;						\
		heap_swap(h, i, p);					\
		i = p;							\
	}								\
} while (0)

#define heap_add(h, new, cmp)						\
({									\
	bool _r = !heap_full(h);					\
	if (_r) {							\
		size_t _i = (h)->used++;				\
		(h)->data[_i] = new;					\
									\
		heap_sift_up(h, _i, cmp);				\
	}								\
	_r;								\
})

#define heap_add_or_replace(h, new, cmp)				\
do {									\
	if (!heap_add(h, new, cmp) &&					\
	    cmp(h, new, heap_peek(h)) >= 0) {				\
		(h)->data[0] = new;					\
		heap_sift_down(h, 0, cmp);				\
	}								\
} while (0)

#define heap_del(h, i, cmp)						\
do {									\
	size_t _i = (i);						\
									\
	BUG_ON(_i >= (h)->used);					\
	(h)->used--;							\
	heap_swap(h, _i, (h)->used);					\
	heap_sift_up(h, _i, cmp);					\
	heap_sift_down(h, _i, cmp);					\
} while (0)

#define heap_pop(h, d, cmp)						\
({									\
	bool _r = (h)->used;						\
	if (_r) {							\
		(d) = (h)->data[0];					\
		heap_del(h, 0, cmp);					\
	}								\
	_r;								\
})

#define heap_resort(heap, cmp)						\
do {									\
	ssize_t _i;							\
	for (_i = (ssize_t) (heap)->used / 2 -  1; _i >= 0; --_i)	\
		heap_sift_down(heap, _i, cmp);				\
} while (0)

/*
 * Simple array based allocator - preallocates a number of elements and you can
 * never allocate more than that, also has no locking.
 *
 * Handy because if you know you only need a fixed number of elements you don't
 * have to worry about memory allocation failure, and sometimes a mempool isn't
 * what you want.
 *
 * We treat the free elements as entries in a singly linked list, and the
 * freelist as a stack - allocating and freeing push and pop off the freelist.
 */

#define DECLARE_ARRAY_ALLOCATOR(type, name, size)			\
	struct {							\
		type	*freelist;					\
		type	data[size];					\
	} name

#define array_alloc(array)						\
({									\
	typeof((array)->freelist) _ret = (array)->freelist;		\
									\
	if (_ret)							\
		(array)->freelist = *((typeof((array)->freelist) *) _ret);\
									\
	_ret;								\
})

#define array_free(array, ptr)						\
do {									\
	typeof((array)->freelist) _ptr = ptr;				\
									\
	*((typeof((array)->freelist) *) _ptr) = (array)->freelist;	\
	(array)->freelist = _ptr;					\
} while (0)

#define array_allocator_init(array)					\
do {									\
	typeof((array)->freelist) _i;					\
									\
	BUILD_BUG_ON(sizeof((array)->data[0]) < sizeof(void *));	\
	(array)->freelist = NULL;					\
									\
	for (_i = (array)->data;					\
	     _i < (array)->data + ARRAY_SIZE((array)->data);		\
	     _i++)							\
		array_free(array, _i);					\
} while (0)

#define array_freelist_empty(array)	((array)->freelist == NULL)

#define ANYSINT_MAX(t)							\
	((((t) 1 << (sizeof(t) * 8 - 2)) - (t) 1) * (t) 2 + (t) 1)

int bch2_strtoint_h(const char *, int *);
int bch2_strtouint_h(const char *, unsigned int *);
int bch2_strtoll_h(const char *, long long *);
int bch2_strtoull_h(const char *, unsigned long long *);

static inline int bch2_strtol_h(const char *cp, long *res)
{
#if BITS_PER_LONG == 32
	return bch2_strtoint_h(cp, (int *) res);
#else
	return bch2_strtoll_h(cp, (long long *) res);
#endif
}

static inline int bch2_strtoul_h(const char *cp, long *res)
{
#if BITS_PER_LONG == 32
	return bch2_strtouint_h(cp, (unsigned int *) res);
#else
	return bch2_strtoull_h(cp, (unsigned long long *) res);
#endif
}

#define strtoi_h(cp, res)						\
	( type_is(*res, int)		? bch2_strtoint_h(cp, (void *) res)\
	: type_is(*res, long)		? bch2_strtol_h(cp, (void *) res)\
	: type_is(*res, long long)	? bch2_strtoll_h(cp, (void *) res)\
	: type_is(*res, unsigned)	? bch2_strtouint_h(cp, (void *) res)\
	: type_is(*res, unsigned long)	? bch2_strtoul_h(cp, (void *) res)\
	: type_is(*res, unsigned long long) ? bch2_strtoull_h(cp, (void *) res)\
	: -EINVAL)

#define strtoul_safe(cp, var)						\
({									\
	unsigned long _v;						\
	int _r = kstrtoul(cp, 10, &_v);					\
	if (!_r)							\
		var = _v;						\
	_r;								\
})

#define strtoul_safe_clamp(cp, var, min, max)				\
({									\
	unsigned long _v;						\
	int _r = kstrtoul(cp, 10, &_v);					\
	if (!_r)							\
		var = clamp_t(typeof(var), _v, min, max);		\
	_r;								\
})

#define strtoul_safe_restrict(cp, var, min, max)			\
({									\
	unsigned long _v;						\
	int _r = kstrtoul(cp, 10, &_v);					\
	if (!_r && _v >= min && _v <= max)				\
		var = _v;						\
	else								\
		_r = -EINVAL;						\
	_r;								\
})

#define snprint(buf, size, var)						\
	snprintf(buf, size,						\
		   type_is(var, int)		? "%i\n"		\
		 : type_is(var, unsigned)	? "%u\n"		\
		 : type_is(var, long)		? "%li\n"		\
		 : type_is(var, unsigned long)	? "%lu\n"		\
		 : type_is(var, s64)		? "%lli\n"		\
		 : type_is(var, u64)		? "%llu\n"		\
		 : type_is(var, char *)		? "%s\n"		\
		 : "%i\n", var)

ssize_t bch2_hprint(char *buf, s64 v);

bool bch2_is_zero(const void *, size_t);

ssize_t bch2_scnprint_string_list(char *, size_t, const char * const[], size_t);

ssize_t bch2_read_string_list(const char *, const char * const[]);

ssize_t bch2_scnprint_flag_list(char *, size_t, const char * const[], u64);
u64 bch2_read_flag_list(char *, const char * const[]);

struct time_stats {
	spinlock_t	lock;
	u64		count;
	/*
	 * all fields are in nanoseconds, averages are ewmas stored left shifted
	 * by 8
	 */
	u64		last_duration;
	u64		max_duration;
	u64		average_duration;
	u64		average_frequency;
	u64		last;
};

void bch2_time_stats_clear(struct time_stats *stats);
void __bch2_time_stats_update(struct time_stats *stats, u64 time);
void bch2_time_stats_update(struct time_stats *stats, u64 time);

static inline unsigned local_clock_us(void)
{
	return local_clock() >> 10;
}

#define NSEC_PER_ns			1L
#define NSEC_PER_us			NSEC_PER_USEC
#define NSEC_PER_ms			NSEC_PER_MSEC
#define NSEC_PER_sec			NSEC_PER_SEC

#define __print_time_stat(stats, name, stat, units)			\
	sysfs_print(name ## _ ## stat ## _ ## units,			\
		    div_u64((stats)->stat >> 8, NSEC_PER_ ## units))

#define sysfs_print_time_stats(stats, name,				\
			       frequency_units,				\
			       duration_units)				\
do {									\
	__print_time_stat(stats, name,					\
			  average_frequency,	frequency_units);	\
	__print_time_stat(stats, name,					\
			  average_duration,	duration_units);	\
	sysfs_print(name ## _ ##count, (stats)->count);			\
	sysfs_print(name ## _ ##last_duration ## _ ## duration_units,	\
			div_u64((stats)->last_duration,			\
				NSEC_PER_ ## duration_units));		\
	sysfs_print(name ## _ ##max_duration ## _ ## duration_units,	\
			div_u64((stats)->max_duration,			\
				NSEC_PER_ ## duration_units));		\
									\
	sysfs_print(name ## _last_ ## frequency_units, (stats)->last	\
		    ? div_s64(local_clock() - (stats)->last,		\
			      NSEC_PER_ ## frequency_units)		\
		    : -1LL);						\
} while (0)

#define sysfs_clear_time_stats(stats, name)				\
do {									\
	if (attr == &sysfs_ ## name ## _clear)				\
		bch2_time_stats_clear(stats);				\
} while (0)

#define sysfs_time_stats_attribute(name,				\
				   frequency_units,			\
				   duration_units)			\
write_attribute(name ## _clear);					\
read_attribute(name ## _count);						\
read_attribute(name ## _average_frequency_ ## frequency_units);		\
read_attribute(name ## _average_duration_ ## duration_units);		\
read_attribute(name ## _last_duration_ ## duration_units);		\
read_attribute(name ## _max_duration_ ## duration_units);		\
read_attribute(name ## _last_ ## frequency_units)

#define sysfs_time_stats_attribute_list(name,				\
					frequency_units,		\
					duration_units)			\
&sysfs_ ## name ## _clear,						\
&sysfs_ ## name ## _count,						\
&sysfs_ ## name ## _average_frequency_ ## frequency_units,		\
&sysfs_ ## name ## _average_duration_ ## duration_units,		\
&sysfs_ ## name ## _last_duration_ ## duration_units,			\
&sysfs_ ## name ## _max_duration_ ## duration_units,			\
&sysfs_ ## name ## _last_ ## frequency_units,

#define ewma_add(ewma, val, weight)					\
({									\
	typeof(ewma) _ewma = (ewma);					\
	typeof(weight) _weight = (weight);				\
									\
	(((_ewma << _weight) - _ewma) + (val)) >> _weight;		\
})

struct bch_ratelimit {
	/* Next time we want to do some work, in nanoseconds */
	u64			next;

	/*
	 * Rate at which we want to do work, in units per nanosecond
	 * The units here correspond to the units passed to
	 * bch2_ratelimit_increment()
	 */
	unsigned		rate;
};

static inline void bch2_ratelimit_reset(struct bch_ratelimit *d)
{
	d->next = local_clock();
}

u64 bch2_ratelimit_delay(struct bch_ratelimit *);
void bch2_ratelimit_increment(struct bch_ratelimit *, u64);
int bch2_ratelimit_wait_freezable_stoppable(struct bch_ratelimit *);

struct bch_pd_controller {
	struct bch_ratelimit	rate;
	unsigned long		last_update;

	s64			last_actual;
	s64			smoothed_derivative;

	unsigned		p_term_inverse;
	unsigned		d_smooth;
	unsigned		d_term;

	/* for exporting to sysfs (no effect on behavior) */
	s64			last_derivative;
	s64			last_proportional;
	s64			last_change;
	s64			last_target;

	/* If true, the rate will not increase if bch2_ratelimit_delay()
	 * is not being called often enough. */
	bool			backpressure;
};

void bch2_pd_controller_update(struct bch_pd_controller *, s64, s64, int);
void bch2_pd_controller_init(struct bch_pd_controller *);
size_t bch2_pd_controller_print_debug(struct bch_pd_controller *, char *);

#define sysfs_pd_controller_attribute(name)				\
	rw_attribute(name##_rate);					\
	rw_attribute(name##_rate_bytes);				\
	rw_attribute(name##_rate_d_term);				\
	rw_attribute(name##_rate_p_term_inverse);			\
	read_attribute(name##_rate_debug)

#define sysfs_pd_controller_files(name)					\
	&sysfs_##name##_rate,						\
	&sysfs_##name##_rate_bytes,					\
	&sysfs_##name##_rate_d_term,					\
	&sysfs_##name##_rate_p_term_inverse,				\
	&sysfs_##name##_rate_debug

#define sysfs_pd_controller_show(name, var)				\
do {									\
	sysfs_hprint(name##_rate,		(var)->rate.rate);	\
	sysfs_print(name##_rate_bytes,		(var)->rate.rate);	\
	sysfs_print(name##_rate_d_term,		(var)->d_term);		\
	sysfs_print(name##_rate_p_term_inverse,	(var)->p_term_inverse);	\
									\
	if (attr == &sysfs_##name##_rate_debug)				\
		return bch2_pd_controller_print_debug(var, buf);		\
} while (0)

#define sysfs_pd_controller_store(name, var)				\
do {									\
	sysfs_strtoul_clamp(name##_rate,				\
			    (var)->rate.rate, 1, UINT_MAX);		\
	sysfs_strtoul_clamp(name##_rate_bytes,				\
			    (var)->rate.rate, 1, UINT_MAX);		\
	sysfs_strtoul(name##_rate_d_term,	(var)->d_term);		\
	sysfs_strtoul_clamp(name##_rate_p_term_inverse,			\
			    (var)->p_term_inverse, 1, INT_MAX);		\
} while (0)

#define __DIV_SAFE(n, d, zero)						\
({									\
	typeof(n) _n = (n);						\
	typeof(d) _d = (d);						\
	_d ? _n / _d : zero;						\
})

#define DIV_SAFE(n, d)	__DIV_SAFE(n, d, 0)

#define container_of_or_null(ptr, type, member)				\
({									\
	typeof(ptr) _ptr = ptr;						\
	_ptr ? container_of(_ptr, type, member) : NULL;			\
})

#define RB_INSERT(root, new, member, cmp)				\
({									\
	__label__ dup;							\
	struct rb_node **n = &(root)->rb_node, *parent = NULL;		\
	typeof(new) this;						\
	int res, ret = -1;						\
									\
	while (*n) {							\
		parent = *n;						\
		this = container_of(*n, typeof(*(new)), member);	\
		res = cmp(new, this);					\
		if (!res)						\
			goto dup;					\
		n = res < 0						\
			? &(*n)->rb_left				\
			: &(*n)->rb_right;				\
	}								\
									\
	rb_link_node(&(new)->member, parent, n);			\
	rb_insert_color(&(new)->member, root);				\
	ret = 0;							\
dup:									\
	ret;								\
})

#define RB_SEARCH(root, search, member, cmp)				\
({									\
	struct rb_node *n = (root)->rb_node;				\
	typeof(&(search)) this, ret = NULL;				\
	int res;							\
									\
	while (n) {							\
		this = container_of(n, typeof(search), member);		\
		res = cmp(&(search), this);				\
		if (!res) {						\
			ret = this;					\
			break;						\
		}							\
		n = res < 0						\
			? n->rb_left					\
			: n->rb_right;					\
	}								\
	ret;								\
})

#define RB_GREATER(root, search, member, cmp)				\
({									\
	struct rb_node *n = (root)->rb_node;				\
	typeof(&(search)) this, ret = NULL;				\
	int res;							\
									\
	while (n) {							\
		this = container_of(n, typeof(search), member);		\
		res = cmp(&(search), this);				\
		if (res < 0) {						\
			ret = this;					\
			n = n->rb_left;					\
		} else							\
			n = n->rb_right;				\
	}								\
	ret;								\
})

#define RB_FIRST(root, type, member)					\
	container_of_or_null(rb_first(root), type, member)

#define RB_LAST(root, type, member)					\
	container_of_or_null(rb_last(root), type, member)

#define RB_NEXT(ptr, member)						\
	container_of_or_null(rb_next(&(ptr)->member), typeof(*ptr), member)

#define RB_PREV(ptr, member)						\
	container_of_or_null(rb_prev(&(ptr)->member), typeof(*ptr), member)

/* Does linear interpolation between powers of two */
static inline unsigned fract_exp_two(unsigned x, unsigned fract_bits)
{
	unsigned fract = x & ~(~0 << fract_bits);

	x >>= fract_bits;
	x   = 1 << x;
	x  += (x * fract) >> fract_bits;

	return x;
}

void bch2_bio_map(struct bio *bio, void *base);

static inline sector_t bdev_sectors(struct block_device *bdev)
{
	return bdev->bd_inode->i_size >> 9;
}

#define closure_bio_submit(bio, cl)					\
do {									\
	closure_get(cl);						\
	submit_bio(bio);						\
} while (0)

#define kthread_wait_freezable(cond)					\
({									\
	int _ret = 0;							\
	while (1) {							\
		set_current_state(TASK_INTERRUPTIBLE);			\
		if (kthread_should_stop()) {				\
			_ret = -1;					\
			break;						\
		}							\
									\
		if (cond)						\
			break;						\
									\
		schedule();						\
		try_to_freeze();					\
	}								\
	set_current_state(TASK_RUNNING);				\
	_ret;								\
})

size_t bch2_rand_range(size_t);

void memcpy_to_bio(struct bio *, struct bvec_iter, void *);
void memcpy_from_bio(void *, struct bio *, struct bvec_iter);

static inline void __memcpy_u64s(void *dst, const void *src,
				 unsigned u64s)
{
#ifdef CONFIG_X86_64
	long d0, d1, d2;
	asm volatile("rep ; movsq"
		     : "=&c" (d0), "=&D" (d1), "=&S" (d2)
		     : "0" (u64s), "1" (dst), "2" (src)
		     : "memory");
#else
	u64 *d = dst;
	const u64 *s = src;

	while (u64s--)
		*d++ = *s++;
#endif
}

static inline void memcpy_u64s(void *dst, const void *src,
			       unsigned u64s)
{
	EBUG_ON(!(dst >= src + u64s * sizeof(u64) ||
		 dst + u64s * sizeof(u64) <= src));

	__memcpy_u64s(dst, src, u64s);
}

static inline void __memmove_u64s_down(void *dst, const void *src,
				       unsigned u64s)
{
	__memcpy_u64s(dst, src, u64s);
}

static inline void memmove_u64s_down(void *dst, const void *src,
				     unsigned u64s)
{
	EBUG_ON(dst > src);

	__memmove_u64s_down(dst, src, u64s);
}

static inline void __memmove_u64s_up(void *_dst, const void *_src,
				     unsigned u64s)
{
	u64 *dst = (u64 *) _dst + u64s - 1;
	u64 *src = (u64 *) _src + u64s - 1;

#ifdef CONFIG_X86_64
	long d0, d1, d2;
	asm volatile("std ;\n"
		     "rep ; movsq\n"
		     "cld ;\n"
		     : "=&c" (d0), "=&D" (d1), "=&S" (d2)
		     : "0" (u64s), "1" (dst), "2" (src)
		     : "memory");
#else
	while (u64s--)
		*dst-- = *src--;
#endif
}

static inline void memmove_u64s_up(void *dst, const void *src,
				   unsigned u64s)
{
	EBUG_ON(dst < src);

	__memmove_u64s_up(dst, src, u64s);
}

static inline void memmove_u64s(void *dst, const void *src,
				unsigned u64s)
{
	if (dst < src)
		__memmove_u64s_down(dst, src, u64s);
	else
		__memmove_u64s_up(dst, src, u64s);
}

static inline struct bio_vec next_contig_bvec(struct bio *bio,
					      struct bvec_iter *iter)
{
	struct bio_vec bv = bio_iter_iovec(bio, *iter);

	bio_advance_iter(bio, iter, bv.bv_len);
#ifndef CONFIG_HIGHMEM
	while (iter->bi_size) {
		struct bio_vec next = bio_iter_iovec(bio, *iter);

		if (page_address(bv.bv_page) + bv.bv_offset + bv.bv_len !=
		    page_address(next.bv_page) + next.bv_offset)
			break;

		bv.bv_len += next.bv_len;
		bio_advance_iter(bio, iter, next.bv_len);
	}
#endif
	return bv;
}

#define __bio_for_each_contig_segment(bv, bio, iter, start)		\
	for (iter = (start);						\
	     (iter).bi_size &&						\
		((bv = next_contig_bvec((bio), &(iter))), 1);)

#define bio_for_each_contig_segment(bv, bio, iter)			\
	__bio_for_each_contig_segment(bv, bio, iter, (bio)->bi_iter)

size_t bch_scnmemcpy(char *, size_t, const char *, size_t);

void sort_cmp_size(void *base, size_t num, size_t size,
	  int (*cmp_func)(const void *, const void *, size_t),
	  void (*swap_func)(void *, void *, size_t));

/* just the memmove, doesn't update @_nr */
#define __array_insert_item(_array, _nr, _pos)				\
	memmove(&(_array)[(_pos) + 1],					\
		&(_array)[(_pos)],					\
		sizeof((_array)[0]) * ((_nr) - (_pos)))

#define array_insert_item(_array, _nr, _pos, _new_item)			\
do {									\
	__array_insert_item(_array, _nr, _pos);				\
	(_nr)++;							\
	(_array)[(_pos)] = (_new_item);					\
} while (0)

#define array_remove_items(_array, _nr, _pos, _nr_to_remove)		\
do {									\
	(_nr) -= (_nr_to_remove);					\
	memmove(&(_array)[(_pos)],					\
		&(_array)[(_pos) + (_nr_to_remove)],			\
		sizeof((_array)[0]) * ((_nr) - (_pos)));		\
} while (0)

#define array_remove_item(_array, _nr, _pos)				\
	array_remove_items(_array, _nr, _pos, 1)

#endif /* _BCACHEFS_UTIL_H */
