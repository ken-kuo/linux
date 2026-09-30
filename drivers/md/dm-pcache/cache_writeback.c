// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/bio.h>

#include "cache.h"
#include "backing_dev.h"
#include "cache_dev.h"
#include "dm_pcache.h"

static void writeback_ctx_end(struct pcache_cache *cache, int ret)
{
	if (ret && !cache->writeback_ctx.ret) {
		pcache_dev_err(CACHE_TO_PCACHE(cache), "writeback error: %d", ret);
		cache->writeback_ctx.ret = ret;
	}

	if (!atomic_dec_and_test(&cache->writeback_ctx.pending))
		return;

	if (!cache->writeback_ctx.ret) {
		backing_dev_flush(cache->backing_dev);

		mutex_lock(&cache->dirty_tail_lock);
		cache_pos_advance(&cache->dirty_tail, cache->writeback_ctx.advance);
		cache_encode_dirty_tail(cache);
		mutex_unlock(&cache->dirty_tail_lock);
	}
	queue_delayed_work(cache_get_wq(cache), &cache->writeback_work, 0);
}

static void writeback_end_req(struct pcache_backing_dev_req *backing_req, int ret)
{
	struct pcache_cache *cache = backing_req->priv_data;

	mutex_lock(&cache->writeback_lock);
	writeback_ctx_end(cache, ret);
	mutex_unlock(&cache->writeback_lock);
}

static inline bool is_cache_clean(struct pcache_cache *cache, struct pcache_cache_pos *dirty_tail)
{
	struct dm_pcache *pcache = CACHE_TO_PCACHE(cache);
	struct pcache_cache_dev *cache_dev = cache->cache_dev;
	struct pcache_cache_kset_onmedia *kset_onmedia;
	u32 to_copy;
	int ret;

	kset_onmedia = (struct pcache_cache_kset_onmedia *)cache->wb_kset_onmedia_buf;

	to_copy = min(PCACHE_KSET_ONMEDIA_SIZE_MAX, cache_seg_remain(dirty_tail));
	ret = cmedium_read(cache_dev, kset_onmedia,
			   cmedium_off(cache_dev, cache_pos_addr(dirty_tail)),
			   to_copy);
	if (ret) {
		pcache_dev_err(pcache, "error to read kset: %d", ret);
		return true;
	}

	/* Reject a corrupted or out-of-bounds kset before reading its keys */
	if (!kset_onmedia_valid(kset_onmedia)) {
		pcache_dev_debug(pcache, "dirty_tail: %u:%u invalid kset magic: %llx, key_num: %u\n",
				dirty_tail->cache_seg->cache_seg_id, dirty_tail->seg_off,
				kset_onmedia->magic, kset_onmedia->key_num);
		return true;
	}

	/* Verify the CRC checksum for data integrity */
	if (kset_onmedia->crc != cache_kset_crc(kset_onmedia)) {
		pcache_dev_debug(pcache, "dirty_tail: %u:%u crc: %x, not expected: %x\n",
				dirty_tail->cache_seg->cache_seg_id, dirty_tail->seg_off,
				cache_kset_crc(kset_onmedia), kset_onmedia->crc);
		return true;
	}

	return false;
}

/*
 * Close the open ksets, then drain every dirty key down to the backing device
 * and wait for it.
 *
 * Both halves are needed, and each is useless alone:
 *
 * - pcache_cache_flush() only closes the open ksets, i.e. it persists
 *   *metadata* to PMEM; the data itself stays in the cache until the writeback
 *   worker walks the log from dirty_tail and pushes it to backing.
 * - The drain alone cannot see a key that is still sitting in an open kset.
 *   is_cache_clean() decides by reading the kset at dirty_tail, and an open
 *   kset has not been written yet, so a cache holding the newest writes reports
 *   itself clean. cache_key_append() closes a kset when it fills up and
 *   otherwise leaves it to a delayed work up to a second later, so under a
 *   steady writer that window is open essentially all the time.
 *
 * Draining without flushing first therefore returns success while the newest
 * writes are cache-only, and a caller that then reads the backing device gets
 * stale blocks for a range it believes is current. Flushing here rather than in
 * the caller keeps that impossible to get wrong: every caller of this function
 * is about to read backing.
 *
 * Kicks the writeback worker and polls until dirty_tail has caught up with
 * key_head. Returns 0 when the cache is clean, -ETIMEDOUT if it did not settle
 * in @timeout_ms, or -EIO if the device is being torn down.
 *
 * Context: sleeps. Must not be called from the writeback worker itself, and the
 * caller must hold neither writeback_lock nor dirty_tail_lock.
 */
int pcache_cache_writeback_all(struct pcache_cache *cache, unsigned int timeout_ms)
{
	struct dm_pcache *pcache = CACHE_TO_PCACHE(cache);
	struct pcache_cache_pos dirty_tail;
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	bool clean;
	int ret;

	ret = pcache_cache_flush(cache);
	if (ret)
		return ret;

	for (;;) {
		if (pcache_is_stopping(pcache))
			return -EIO;

		/*
		 * is_cache_clean() parses into cache->wb_kset_onmedia_buf, which
		 * the writeback worker also uses, so take writeback_lock for the
		 * check. Same order as cache_writeback_fn (writeback_lock then
		 * dirty_tail_lock), and released before sleeping so the worker
		 * can make progress.
		 */
		mutex_lock(&cache->writeback_lock);
		mutex_lock(&cache->dirty_tail_lock);
		cache_pos_copy(&dirty_tail, &cache->dirty_tail);
		mutex_unlock(&cache->dirty_tail_lock);
		clean = is_cache_clean(cache, &dirty_tail);
		mutex_unlock(&cache->writeback_lock);

		if (clean)
			return 0;

		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;

		mod_delayed_work(cache_get_wq(cache), &cache->writeback_work, 0);
		schedule_timeout_uninterruptible(HZ / 50);
	}
}

void cache_writeback_exit(struct pcache_cache *cache)
{
	cancel_delayed_work_sync(&cache->writeback_work);
	backing_dev_flush(cache->backing_dev);
	cache_tree_exit(&cache->writeback_key_tree);
}

int cache_writeback_init(struct pcache_cache *cache)
{
	int ret;

	ret = cache_tree_init(cache, &cache->writeback_key_tree, 1);
	if (ret)
		goto err;

	atomic_set(&cache->writeback_ctx.pending, 0);

	/* Queue delayed work to start writeback handling */
	queue_delayed_work(cache_get_wq(cache), &cache->writeback_work, 0);

	return 0;
err:
	return ret;
}

/*
 * Tracks an BIO-mode writeback chunk that owns a transient cache->backing
 * staging buffer. cache_key_writeback_bio allocates @buf, fills it via
 * cmedium_read, hands the buffer to backing_dev_req_create, and registers
 * writeback_kmem_buf_end so the buffer is freed when the backing write
 * completes.
 */
struct writeback_kmem_buf {
	struct pcache_cache	*cache;
	void			*buf;
};

static void writeback_kmem_buf_end(struct pcache_backing_dev_req *backing_req, int ret)
{
	struct writeback_kmem_buf *wb = backing_req->priv_data;
	struct pcache_cache *cache = wb->cache;

	kvfree(wb->buf);
	kfree(wb);

	mutex_lock(&cache->writeback_lock);
	writeback_ctx_end(cache, ret);
	mutex_unlock(&cache->writeback_lock);
}

static void cache_key_writeback_dax(struct pcache_cache *cache,
				     struct pcache_cache_key *key)
{
	struct pcache_backing_dev_req *writeback_req;
	struct pcache_backing_dev_req_opts writeback_req_opts = { 0 };
	struct pcache_cache_pos *pos = &key->cache_pos;
	void *addr;
	u32 req_len, done = 0;

next_req:
	addr = cache_pos_addr(pos) + done;
	req_len = backing_dev_req_coalesced_max_len(addr, key->len - done);

	writeback_req_opts.type = BACKING_DEV_REQ_TYPE_KMEM;
	writeback_req_opts.gfp_mask = GFP_NOIO;
	writeback_req_opts.end_fn = writeback_end_req;
	writeback_req_opts.priv_data = cache;

	writeback_req_opts.kmem.data = addr;
	writeback_req_opts.kmem.opf = REQ_OP_WRITE;
	writeback_req_opts.kmem.len = req_len;
	writeback_req_opts.kmem.backing_off = key->off + done;

	writeback_req = backing_dev_req_create(cache->backing_dev, &writeback_req_opts);

	atomic_inc(&cache->writeback_ctx.pending);
	backing_dev_req_submit(writeback_req, true);

	done += req_len;
	if (done < key->len)
		goto next_req;
}

static void cache_key_writeback_bio(struct pcache_cache *cache,
				     struct pcache_cache_key *key)
{
	struct pcache_cache_dev *cache_dev = cache->cache_dev;
	struct pcache_cache_pos *pos = &key->cache_pos;
	struct pcache_segment *segment = &pos->cache_seg->segment;
	struct pcache_backing_dev_req *writeback_req;
	struct pcache_backing_dev_req_opts writeback_req_opts = { 0 };
	struct writeback_kmem_buf *wb;
	u64 cdev_off;
	int ret;

	cdev_off = cmedium_off(cache_dev, segment->data) + pos->seg_off;

	wb = kmalloc_obj(*wb, GFP_NOIO);
	if (!wb)
		goto err;
	wb->cache = cache;
	wb->buf = kvmalloc(key->len, GFP_NOIO);
	if (!wb->buf)
		goto err_free_wb;

	ret = cmedium_read(cache_dev, wb->buf, cdev_off, key->len);
	if (ret)
		goto err_free_buf;

	writeback_req_opts.type = BACKING_DEV_REQ_TYPE_KMEM;
	writeback_req_opts.gfp_mask = GFP_NOIO;
	writeback_req_opts.end_fn = writeback_kmem_buf_end;
	writeback_req_opts.priv_data = wb;

	writeback_req_opts.kmem.data = wb->buf;
	writeback_req_opts.kmem.opf = REQ_OP_WRITE;
	writeback_req_opts.kmem.len = key->len;
	writeback_req_opts.kmem.backing_off = key->off;

	writeback_req = backing_dev_req_create(cache->backing_dev, &writeback_req_opts);
	if (!writeback_req)
		goto err_free_buf;

	atomic_inc(&cache->writeback_ctx.pending);
	backing_dev_req_submit(writeback_req, true);
	return;

err_free_buf:
	kvfree(wb->buf);
err_free_wb:
	kfree(wb);
err:
	/*
	 * No backing write was submitted, so take a pending reference to
	 * balance the writeback_ctx_end() drop below. This records the error
	 * (so dirty_tail is not advanced) without skewing the pending count.
	 */
	atomic_inc(&cache->writeback_ctx.pending);
	mutex_lock(&cache->writeback_lock);
	writeback_ctx_end(cache, -EIO);
	mutex_unlock(&cache->writeback_lock);
}

static void cache_key_writeback(struct pcache_cache *cache, struct pcache_cache_key *key)
{
	if (cache_key_clean(key))
		return;

	/*
	 * Cached data for a key never crosses a segment boundary, so the
	 * remaining space from the key's position must cover its length. If
	 * that invariant is somehow violated, fail this writeback cycle
	 * (recording the error keeps dirty_tail from advancing) rather than
	 * crashing.
	 */
	if (WARN_ON_ONCE(cache_seg_remain(&key->cache_pos) < key->len)) {
		atomic_inc(&cache->writeback_ctx.pending);
		mutex_lock(&cache->writeback_lock);
		writeback_ctx_end(cache, -EIO);
		mutex_unlock(&cache->writeback_lock);
		return;
	}

	if (cache_dev_is_bio(cache->cache_dev))
		cache_key_writeback_bio(cache, key);
	else
		cache_key_writeback_dax(cache, key);
}

static void cache_wb_tree_writeback(struct pcache_cache *cache, u32 advance)
{
	struct pcache_cache_tree *cache_tree = &cache->writeback_key_tree;
	struct pcache_cache_subtree *cache_subtree;
	struct rb_node *node;
	struct pcache_cache_key *key;
	u32 i;

	cache->writeback_ctx.ret = 0;
	cache->writeback_ctx.advance = advance;
	atomic_set(&cache->writeback_ctx.pending, 1);

	for (i = 0; i < cache_tree->n_subtrees; i++) {
		cache_subtree = &cache_tree->subtrees[i];

		node = rb_first(&cache_subtree->root);
		while (node) {
			key = CACHE_KEY(node);
			node = rb_next(node);

			cache_key_writeback(cache, key);
			cache_key_delete(key);
		}
	}
	writeback_ctx_end(cache, 0);
}

static int cache_kset_insert_tree(struct pcache_cache *cache, struct pcache_cache_kset_onmedia *kset_onmedia)
{
	struct pcache_cache_key_onmedia *key_onmedia;
	struct pcache_cache_subtree *cache_subtree;
	struct pcache_cache_key *key;
	int ret;
	u32 i;

	/* Iterate through all keys in the kset and write each back to storage */
	for (i = 0; i < kset_onmedia->key_num; i++) {
		key_onmedia = &kset_onmedia->data[i];

		key = cache_key_alloc(&cache->writeback_key_tree, GFP_NOIO);
		ret = cache_key_decode(cache, key_onmedia, key, true);
		if (ret) {
			cache_key_put(key);
			goto clear_tree;
		}

		cache_subtree = get_subtree(&cache->writeback_key_tree, key->off);
		spin_lock(&cache_subtree->tree_lock);
		cache_key_insert(&cache->writeback_key_tree, key, true);
		spin_unlock(&cache_subtree->tree_lock);
	}

	return 0;
clear_tree:
	cache_tree_clear(&cache->writeback_key_tree);
	return ret;
}

static int last_kset_writeback(struct pcache_cache *cache,
		struct pcache_cache_kset_onmedia *last_kset_onmedia)
{
	struct dm_pcache *pcache = CACHE_TO_PCACHE(cache);
	struct pcache_cache_segment *next_seg;

	if (!cache_seg_id_valid(cache, last_kset_onmedia->next_cache_seg_id)) {
		pcache_dev_err(pcache, "invalid next_cache_seg_id %u in writeback (n_segs %u)\n",
				last_kset_onmedia->next_cache_seg_id, cache->n_segs);
		return -EIO;
	}

	pcache_dev_debug(pcache, "last kset, next: %u\n", last_kset_onmedia->next_cache_seg_id);

	next_seg = &cache->segments[last_kset_onmedia->next_cache_seg_id];

	mutex_lock(&cache->dirty_tail_lock);
	cache->dirty_tail.cache_seg = next_seg;
	cache->dirty_tail.seg_off = 0;
	cache_encode_dirty_tail(cache);
	mutex_unlock(&cache->dirty_tail_lock);

	return 0;
}

void cache_writeback_fn(struct work_struct *work)
{
	struct pcache_cache *cache = container_of(work, struct pcache_cache, writeback_work.work);
	struct dm_pcache *pcache = CACHE_TO_PCACHE(cache);
	struct pcache_cache_pos dirty_tail;
	struct pcache_cache_kset_onmedia *kset_onmedia;
	u32 delay;
	int ret;

	mutex_lock(&cache->writeback_lock);
	if (atomic_read(&cache->writeback_ctx.pending))
		goto unlock;

	if (pcache_is_stopping(pcache))
		goto unlock;

	if (atomic_read(&cache->writeback_errors))
		goto unlock;

	kset_onmedia = (struct pcache_cache_kset_onmedia *)cache->wb_kset_onmedia_buf;

	mutex_lock(&cache->dirty_tail_lock);
	cache_pos_copy(&dirty_tail, &cache->dirty_tail);
	mutex_unlock(&cache->dirty_tail_lock);

	if (is_cache_clean(cache, &dirty_tail)) {
		delay = PCACHE_CACHE_WRITEBACK_INTERVAL;
		goto queue_work;
	}

	/*
	 * Lazy-writeback watermark: while the cache is below writeback_percent
	 * full, defer draining so write bursts are absorbed/coalesced and
	 * writeback's PMEM reads stay off the hot write path. Self-regulating:
	 * deferred dirty data keeps segs_used rising until it crosses the
	 * watermark, then we drain. 0 = eager (default).
	 */
	if (cache->writeback_percent) {
		u32 segs_used = bitmap_weight(cache->seg_map, cache->n_segs);
		u32 wb_threshold = cache->n_segs * cache->writeback_percent / 100;

		if (segs_used < wb_threshold) {
			delay = PCACHE_CACHE_WRITEBACK_INTERVAL;
			goto queue_work;
		}
	}

	if (kset_onmedia->flags & PCACHE_KSET_FLAGS_LAST) {
		ret = last_kset_writeback(cache, kset_onmedia);
		if (ret) {
			atomic_inc(&cache->writeback_errors);
			goto unlock;
		}
		delay = 0;
		goto queue_work;
	}

	if (get_kset_onmedia_size(kset_onmedia) > cache_seg_remain(&dirty_tail)) {
		atomic_inc(&cache->writeback_errors);
		goto unlock;
	}

	ret = cache_kset_insert_tree(cache, kset_onmedia);
	if (ret) {
		atomic_inc(&cache->writeback_errors);
		goto unlock;
	}

	cache_wb_tree_writeback(cache, get_kset_onmedia_size(kset_onmedia));
	delay = 0;
queue_work:
	queue_delayed_work(cache_get_wq(cache), &cache->writeback_work, delay);
unlock:
	mutex_unlock(&cache->writeback_lock);
}
