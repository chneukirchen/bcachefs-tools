#ifndef _BCACHEFS_BTREE_UPDATE_INTERIOR_H
#define _BCACHEFS_BTREE_UPDATE_INTERIOR_H

#include "btree_cache.h"
#include "btree_locking.h"
#include "btree_update.h"

struct btree_reserve {
	struct disk_reservation	disk_res;
	unsigned		nr;
	struct btree		*b[BTREE_RESERVE_MAX];
};

void __bch2_btree_calc_format(struct bkey_format_state *, struct btree *);
bool bch2_btree_node_format_fits(struct bch_fs *c, struct btree *,
				struct bkey_format *);

/* Btree node freeing/allocation: */

/*
 * Tracks a btree node that has been (or is about to be) freed in memory, but
 * has _not_ yet been freed on disk (because the write that makes the new
 * node(s) visible and frees the old hasn't completed yet)
 */
struct pending_btree_node_free {
	bool			index_update_done;

	__le64			seq;
	enum btree_id		btree_id;
	unsigned		level;
	__BKEY_PADDED(key, BKEY_BTREE_PTR_VAL_U64s_MAX);
};

/*
 * Tracks an in progress split/rewrite of a btree node and the update to the
 * parent node:
 *
 * When we split/rewrite a node, we do all the updates in memory without
 * waiting for any writes to complete - we allocate the new node(s) and update
 * the parent node, possibly recursively up to the root.
 *
 * The end result is that we have one or more new nodes being written -
 * possibly several, if there were multiple splits - and then a write (updating
 * an interior node) which will make all these new nodes visible.
 *
 * Additionally, as we split/rewrite nodes we free the old nodes - but the old
 * nodes can't be freed (their space on disk can't be reclaimed) until the
 * update to the interior node that makes the new node visible completes -
 * until then, the old nodes are still reachable on disk.
 *
 */
struct btree_update {
	struct closure			cl;
	struct bch_fs			*c;

	struct list_head		list;

	/* What kind of update are we doing? */
	enum {
		BTREE_INTERIOR_NO_UPDATE,
		BTREE_INTERIOR_UPDATING_NODE,
		BTREE_INTERIOR_UPDATING_ROOT,
		BTREE_INTERIOR_UPDATING_AS,
	} mode;

	unsigned			must_rewrite:1;
	unsigned			nodes_written:1;

	enum btree_id			btree_id;

	struct btree_reserve		*reserve;

	/*
	 * BTREE_INTERIOR_UPDATING_NODE:
	 * The update that made the new nodes visible was a regular update to an
	 * existing interior node - @b. We can't write out the update to @b
	 * until the new nodes we created are finished writing, so we block @b
	 * from writing by putting this btree_interior update on the
	 * @b->write_blocked list with @write_blocked_list:
	 */
	struct btree			*b;
	struct list_head		write_blocked_list;

	/*
	 * BTREE_INTERIOR_UPDATING_AS: btree node we updated was freed, so now
	 * we're now blocking another btree_update
	 * @parent_as - btree_update that's waiting on our nodes to finish
	 * writing, before it can make new nodes visible on disk
	 * @wait - list of child btree_updates that are waiting on this
	 * btree_update to make all the new nodes visible before they can free
	 * their old btree nodes
	 */
	struct btree_update		*parent_as;
	struct closure_waitlist		wait;

	/*
	 * We may be freeing nodes that were dirty, and thus had journal entries
	 * pinned: we need to transfer the oldest of those pins to the
	 * btree_update operation, and release it when the new node(s)
	 * are all persistent and reachable:
	 */
	struct journal_entry_pin	journal;

	u64				journal_seq;

	/*
	 * Nodes being freed:
	 * Protected by c->btree_node_pending_free_lock
	 */
	struct pending_btree_node_free	pending[BTREE_MAX_DEPTH + GC_MERGE_NODES];
	unsigned			nr_pending;

	/* New nodes, that will be made reachable by this update: */
	struct btree			*new_nodes[BTREE_MAX_DEPTH * 2 + GC_MERGE_NODES];
	unsigned			nr_new_nodes;

	/* Only here to reduce stack usage on recursive splits: */
	struct keylist			parent_keys;
	/*
	 * Enough room for btree_split's keys without realloc - btree node
	 * pointers never have crc/compression info, so we only need to acount
	 * for the pointers for three keys
	 */
	u64				inline_keys[BKEY_BTREE_PTR_U64s_MAX * 3];
};

#define for_each_pending_btree_node_free(c, as, p)			\
	list_for_each_entry(as, &c->btree_interior_update_list, list)	\
		for (p = as->pending; p < as->pending + as->nr_pending; p++)

void bch2_btree_node_free_inmem(struct bch_fs *, struct btree *,
				struct btree_iter *);
void bch2_btree_node_free_never_inserted(struct bch_fs *, struct btree *);
void bch2_btree_open_bucket_put(struct bch_fs *, struct btree *);

struct btree *__bch2_btree_node_alloc_replacement(struct btree_update *,
						  struct btree *,
						  struct bkey_format);

void bch2_btree_update_done(struct btree_update *);
struct btree_update *
bch2_btree_update_start(struct bch_fs *, enum btree_id, unsigned,
			unsigned, struct closure *);

void bch2_btree_interior_update_will_free_node(struct btree_update *,
					       struct btree *);

void bch2_btree_insert_node(struct btree_update *, struct btree *,
			    struct btree_iter *, struct keylist *);
int bch2_btree_split_leaf(struct bch_fs *, struct btree_iter *, unsigned);

int __bch2_foreground_maybe_merge(struct bch_fs *, struct btree_iter *,
				  unsigned, enum btree_node_sibling);

static inline int bch2_foreground_maybe_merge_sibling(struct bch_fs *c,
					struct btree_iter *iter,
					unsigned level,
					enum btree_node_sibling sib)
{
	struct btree *b;

	if (!bch2_btree_node_relock(iter, level))
		return 0;

	b = iter->l[level].b;
	if (b->sib_u64s[sib] > c->btree_foreground_merge_threshold)
		return 0;

	return __bch2_foreground_maybe_merge(c, iter, level, sib);
}

static inline void bch2_foreground_maybe_merge(struct bch_fs *c,
					       struct btree_iter *iter,
					       unsigned level)
{
	bch2_foreground_maybe_merge_sibling(c, iter, level, btree_prev_sib);
	bch2_foreground_maybe_merge_sibling(c, iter, level, btree_next_sib);
}

void bch2_btree_set_root_for_read(struct bch_fs *, struct btree *);
void bch2_btree_root_alloc(struct bch_fs *, enum btree_id);

static inline unsigned btree_update_reserve_required(struct bch_fs *c,
						     struct btree *b)
{
	unsigned depth = btree_node_root(c, b)->level - b->level;

	return btree_reserve_required_nodes(depth);
}

static inline void btree_node_reset_sib_u64s(struct btree *b)
{
	b->sib_u64s[0] = b->nr.live_u64s;
	b->sib_u64s[1] = b->nr.live_u64s;
}

static inline void *btree_data_end(struct bch_fs *c, struct btree *b)
{
	return (void *) b->data + btree_bytes(c);
}

static inline struct bkey_packed *unwritten_whiteouts_start(struct bch_fs *c,
							    struct btree *b)
{
	return (void *) ((u64 *) btree_data_end(c, b) - b->whiteout_u64s);
}

static inline struct bkey_packed *unwritten_whiteouts_end(struct bch_fs *c,
							  struct btree *b)
{
	return btree_data_end(c, b);
}

static inline void *write_block(struct btree *b)
{
	return (void *) b->data + (b->written << 9);
}

static inline bool bset_written(struct btree *b, struct bset *i)
{
	return (void *) i < write_block(b);
}

static inline bool bset_unwritten(struct btree *b, struct bset *i)
{
	return (void *) i > write_block(b);
}

static inline unsigned bset_end_sector(struct bch_fs *c, struct btree *b,
				       struct bset *i)
{
	return round_up(bset_byte_offset(b, vstruct_end(i)),
			block_bytes(c)) >> 9;
}

static inline unsigned btree_write_set_buffer(struct btree *b)
{
	/*
	 * Could buffer up larger amounts of keys for btrees with larger keys,
	 * pending benchmarking:
	 */
	return 4 << 10;
}

static inline struct btree_node_entry *want_new_bset(struct bch_fs *c,
						     struct btree *b)
{
	struct bset *i = btree_bset_last(b);
	unsigned offset = max_t(unsigned, b->written << 9,
				bset_byte_offset(b, vstruct_end(i)));
	ssize_t remaining_space = (ssize_t) btree_bytes(c) - (ssize_t)
		(offset + sizeof(struct btree_node_entry) +
		 b->whiteout_u64s * sizeof(u64) +
		 b->uncompacted_whiteout_u64s * sizeof(u64));

	EBUG_ON(offset > btree_bytes(c));

	if ((unlikely(bset_written(b, i)) &&
	     remaining_space > block_bytes(c)) ||
	    (unlikely(vstruct_bytes(i) > btree_write_set_buffer(b)) &&
	     remaining_space > btree_write_set_buffer(b)))
		return (void *) b->data + offset;

	return NULL;
}

static inline void unreserve_whiteout(struct btree *b, struct bset_tree *t,
				      struct bkey_packed *k)
{
	if (bset_written(b, bset(b, t))) {
		EBUG_ON(b->uncompacted_whiteout_u64s <
			bkeyp_key_u64s(&b->format, k));
		b->uncompacted_whiteout_u64s -=
			bkeyp_key_u64s(&b->format, k);
	}
}

static inline void reserve_whiteout(struct btree *b, struct bset_tree *t,
				    struct bkey_packed *k)
{
	if (bset_written(b, bset(b, t))) {
		BUG_ON(!k->needs_whiteout);
		b->uncompacted_whiteout_u64s +=
			bkeyp_key_u64s(&b->format, k);
	}
}

static inline size_t bch_btree_keys_u64s_remaining(struct bch_fs *c,
						   struct btree *b)
{
	struct bset *i = btree_bset_last(b);
	unsigned used = bset_byte_offset(b, vstruct_end(i)) / sizeof(u64) +
		b->whiteout_u64s +
		b->uncompacted_whiteout_u64s;
	unsigned total = c->opts.btree_node_size << 6;

	EBUG_ON(used > total);

	if (bset_written(b, i))
		return 0;

	return total - used;
}

/*
 * write lock must be held on @b (else the dirty bset that we were going to
 * insert into could be written out from under us)
 */
static inline bool bch2_btree_node_insert_fits(struct bch_fs *c,
					      struct btree *b, unsigned u64s)
{
	if (unlikely(btree_node_fake(b)))
		return false;

	if (btree_node_is_extents(b)) {
		/* The insert key might split an existing key
		 * (bch2_insert_fixup_extent() -> BCH_EXTENT_OVERLAP_MIDDLE case:
		 */
		u64s += BKEY_EXTENT_U64s_MAX;
	}

	return u64s <= bch_btree_keys_u64s_remaining(c, b);
}

static inline bool journal_res_insert_fits(struct btree_insert *trans,
					   struct btree_insert_entry *insert)
{
	unsigned u64s = 0;
	struct btree_insert_entry *i;

	/*
	 * If we didn't get a journal reservation, we're in journal replay and
	 * we're not journalling updates:
	 */
	if (!trans->journal_res.ref)
		return true;

	for (i = insert; i < trans->entries + trans->nr; i++)
		u64s += jset_u64s(i->k->k.u64s + i->extra_res);

	return u64s <= trans->journal_res.u64s;
}

ssize_t bch2_btree_updates_print(struct bch_fs *, char *);

#endif /* _BCACHEFS_BTREE_UPDATE_INTERIOR_H */
