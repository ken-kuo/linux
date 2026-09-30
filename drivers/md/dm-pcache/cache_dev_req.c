// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/blkdev.h>
#include <linux/bio.h>

#include "../dm-core.h"
#include "pcache_internal.h"
#include "cache_dev.h"
#include "cache_dev_req.h"
#include "backing_dev.h"
#include "cache.h"
#include "dm_pcache.h"

static struct kmem_cache *cache_dev_req_cache;
static struct kmem_cache *cache_dev_bvec_cache;
static struct kmem_cache *cache_dev_meta_stage_cache;

void cache_dev_req_end(struct pcache_cache_dev_req *req)
{
	struct pcache_cache_dev *cache_dev = req->cache_dev;
	struct pcache_cache_dev_req_ctx *ctx = &cache_dev->req_ctx;

	if (req->end_req)
		req->end_req(req, req->ret);

	switch (req->type) {
	case CACHE_DEV_REQ_TYPE_REQ:
		if (req->req.upper_req)
			pcache_req_put(req->req.upper_req, req->ret);
		/* Account this data write for cache_dev_data_drain. */
		atomic64_inc(&ctx->nr_data_completed);
		wake_up(&ctx->data_drain_wq);
		break;
	case CACHE_DEV_REQ_TYPE_KMEM:
		if (req->kmem.bvecs != req->kmem.inline_bvecs)
			mempool_free(req->kmem.bvecs, &ctx->bvec_pool);
		break;
	default:
		BUG();
	}

	mempool_free(req, &ctx->req_pool);

	if (atomic_dec_and_test(&ctx->inflight_reqs))
		wake_up(&ctx->inflight_wq);
}

static void req_complete_fn(struct work_struct *work)
{
	struct pcache_cache_dev_req_ctx *ctx = container_of(work,
			struct pcache_cache_dev_req_ctx, req_complete_work);
	struct pcache_cache_dev_req *req;
	LIST_HEAD(tmp_list);

	spin_lock_irq(&ctx->complete_lock);
	list_splice_init(&ctx->complete_list, &tmp_list);
	spin_unlock_irq(&ctx->complete_lock);

	while (!list_empty(&tmp_list)) {
		req = list_first_entry(&tmp_list, struct pcache_cache_dev_req, node);
		list_del_init(&req->node);
		cache_dev_req_end(req);
	}
}

static void cache_dev_req_bio_end(struct bio *bio)
{
	struct pcache_cache_dev_req *req = bio->bi_private;
	struct pcache_cache_dev *cache_dev = req->cache_dev;
	struct pcache_cache_dev_req_ctx *ctx = &cache_dev->req_ctx;
	struct dm_pcache *pcache = CACHE_DEV_TO_PCACHE(cache_dev);
	unsigned long flags;

	req->ret = blk_status_to_errno(bio->bi_status);

	spin_lock_irqsave(&ctx->complete_lock, flags);
	list_move_tail(&req->node, &ctx->complete_list);
	queue_work(pcache->task_wq, &ctx->req_complete_work);
	spin_unlock_irqrestore(&ctx->complete_lock, flags);
}

static void req_submit_fn(struct work_struct *work)
{
	struct pcache_cache_dev_req_ctx *ctx = container_of(work,
			struct pcache_cache_dev_req_ctx, req_submit_work);
	struct pcache_cache_dev_req *req;
	LIST_HEAD(tmp_list);

	spin_lock(&ctx->submit_lock);
	list_splice_init(&ctx->submit_list, &tmp_list);
	spin_unlock(&ctx->submit_lock);

	while (!list_empty(&tmp_list)) {
		req = list_first_entry(&tmp_list, struct pcache_cache_dev_req, node);
		list_del_init(&req->node);
		submit_bio_noacct(&req->bio);
	}
}

void cache_dev_req_submit(struct pcache_cache_dev_req *req, bool direct)
{
	struct pcache_cache_dev *cache_dev = req->cache_dev;
	struct pcache_cache_dev_req_ctx *ctx = &cache_dev->req_ctx;
	struct dm_pcache *pcache = CACHE_DEV_TO_PCACHE(cache_dev);

	if (direct) {
		submit_bio_noacct(&req->bio);
		return;
	}

	spin_lock(&ctx->submit_lock);
	list_add_tail(&req->node, &ctx->submit_list);
	queue_work(pcache->task_wq, &ctx->req_submit_work);
	spin_unlock(&ctx->submit_lock);
}

static void bio_map(struct bio *bio, void *base, size_t size)
{
	struct page *page;
	unsigned int offset;
	unsigned int len;

	if (!is_vmalloc_addr(base)) {
		page = virt_to_page(base);
		offset = offset_in_page(base);

		BUG_ON(!bio_add_page(bio, page, size, offset));
		return;
	}

	flush_kernel_vmap_range(base, size);
	while (size) {
		page = vmalloc_to_page(base);
		offset = offset_in_page(base);
		len = min_t(size_t, PAGE_SIZE - offset, size);

		BUG_ON(!bio_add_page(bio, page, len, offset));
		size -= len;
		base += len;
	}
}

static struct pcache_cache_dev_req *
req_type_req_alloc(struct pcache_cache_dev *cache_dev,
		   struct pcache_cache_dev_req_opts *opts)
{
	struct pcache_cache_dev_req_ctx *ctx = &cache_dev->req_ctx;
	struct pcache_request *pcache_req = opts->req.upper_req;
	struct pcache_cache_dev_req *req;
	struct bio *orig = pcache_req->bio;

	req = mempool_alloc(&ctx->req_pool, opts->gfp_mask);
	if (!req)
		return NULL;

	memset(req, 0, sizeof(struct pcache_cache_dev_req));

	bio_init_clone(cache_dev->dm_dev->bdev, &req->bio, orig, opts->gfp_mask);

	req->type = CACHE_DEV_REQ_TYPE_REQ;
	req->cache_dev = cache_dev;
	atomic_inc(&ctx->inflight_reqs);
	/*
	 * Account this data write for cache_dev_data_drain. Increment at
	 * alloc (not at submit) so a req that is allocated but ends up
	 * failed before submission also contributes to the matching
	 * nr_data_completed bump in cache_dev_req_end.
	 */
	atomic64_inc(&ctx->nr_data_submitted);

	return req;
}

static struct pcache_cache_dev_req *
kmem_type_req_alloc(struct pcache_cache_dev *cache_dev,
		    struct pcache_cache_dev_req_opts *opts)
{
	struct pcache_cache_dev_req_ctx *ctx = &cache_dev->req_ctx;
	struct pcache_cache_dev_req *req;
	u32 n_vecs = bio_add_max_vecs(opts->kmem.data, opts->kmem.len);

	req = mempool_alloc(&ctx->req_pool, opts->gfp_mask);
	if (!req)
		return NULL;

	memset(req, 0, sizeof(struct pcache_cache_dev_req));

	if (n_vecs > CACHE_DEV_REQ_INLINE_BVECS) {
		req->kmem.bvecs = mempool_alloc(&ctx->bvec_pool, opts->gfp_mask);
		if (!req->kmem.bvecs)
			goto free_req;
	} else {
		req->kmem.bvecs = req->kmem.inline_bvecs;
	}

	req->kmem.n_vecs = n_vecs;
	req->type = CACHE_DEV_REQ_TYPE_KMEM;
	req->cache_dev = cache_dev;
	atomic_inc(&ctx->inflight_reqs);

	return req;

free_req:
	mempool_free(req, &ctx->req_pool);
	return NULL;
}

struct pcache_cache_dev_req *
cache_dev_req_alloc(struct pcache_cache_dev *cache_dev,
		    struct pcache_cache_dev_req_opts *opts)
{
	if (opts->type == CACHE_DEV_REQ_TYPE_REQ)
		return req_type_req_alloc(cache_dev, opts);

	if (opts->type == CACHE_DEV_REQ_TYPE_KMEM)
		return kmem_type_req_alloc(cache_dev, opts);

	BUG();
}

static void req_type_req_init(struct pcache_cache_dev_req *req,
			      struct pcache_cache_dev_req_opts *opts)
{
	struct pcache_request *pcache_req = opts->req.upper_req;
	struct bio *clone;
	u32 off = opts->req.req_off;
	u32 len = opts->req.len;

	clone = &req->bio;
	BUG_ON(off & SECTOR_MASK);
	BUG_ON(len & SECTOR_MASK);
	bio_trim(clone, off >> SECTOR_SHIFT, len >> SECTOR_SHIFT);

	clone->bi_iter.bi_sector = opts->cdev_off >> SECTOR_SHIFT;
	clone->bi_private = req;
	clone->bi_end_io = cache_dev_req_bio_end;

	INIT_LIST_HEAD(&req->node);
	req->end_req		= opts->end_fn;
	req->priv_data		= opts->priv_data;

	pcache_req_get(pcache_req);
	req->req.upper_req	= pcache_req;
	req->req.bio_off	= off;
}

static void kmem_type_req_init(struct pcache_cache_dev_req *req,
			       struct pcache_cache_dev_req_opts *opts)
{
	struct pcache_cache_dev *cache_dev = req->cache_dev;
	struct bio *cdev_bio;

	bio_init(&req->bio, cache_dev->dm_dev->bdev, req->kmem.bvecs,
		 req->kmem.n_vecs, opts->kmem.opf);

	cdev_bio = &req->bio;
	bio_map(cdev_bio, opts->kmem.data, opts->kmem.len);

	cdev_bio->bi_iter.bi_sector = opts->cdev_off >> SECTOR_SHIFT;
	cdev_bio->bi_private = req;
	cdev_bio->bi_end_io = cache_dev_req_bio_end;

	INIT_LIST_HEAD(&req->node);
	req->end_req	= opts->end_fn;
	req->priv_data	= opts->priv_data;
}

void cache_dev_req_init(struct pcache_cache_dev_req *req,
			struct pcache_cache_dev_req_opts *opts)
{
	if (opts->type == CACHE_DEV_REQ_TYPE_REQ)
		return req_type_req_init(req, opts);

	if (opts->type == CACHE_DEV_REQ_TYPE_KMEM)
		return kmem_type_req_init(req, opts);

	BUG();
}

struct pcache_cache_dev_req *
cache_dev_req_create(struct pcache_cache_dev *cache_dev,
		     struct pcache_cache_dev_req_opts *opts)
{
	struct pcache_cache_dev_req *req;

	req = cache_dev_req_alloc(cache_dev, opts);
	if (!req)
		return NULL;

	cache_dev_req_init(req, opts);

	return req;
}

void cache_dev_bdev_flush(struct pcache_cache_dev *cache_dev)
{
	blkdev_issue_flush(cache_dev->dm_dev->bdev);
}

/*
 * meta_stage_fn - drain staged metadata writes to the cache device.
 *
 * Runs in workqueue (sleepable) context. A single work_struct is never run
 * concurrently with itself, so the queue is drained FIFO by exactly one
 * worker — preserving the order metadata was staged in and avoiding RMW
 * races for sub-LBA writes that share a logical block.
 */
static void meta_stage_fn(struct work_struct *work)
{
	struct pcache_cache_dev_req_ctx *ctx = container_of(work,
			struct pcache_cache_dev_req_ctx, meta_stage_work);
	struct pcache_cache_dev *cache_dev = container_of(ctx,
			struct pcache_cache_dev, req_ctx);
	struct pcache_meta_stage *st;
	LIST_HEAD(tmp_list);

	spin_lock(&ctx->meta_stage_lock);
	list_splice_init(&ctx->meta_stage_list, &tmp_list);
	spin_unlock(&ctx->meta_stage_lock);

	while (!list_empty(&tmp_list)) {
		st = list_first_entry(&tmp_list, struct pcache_meta_stage, node);
		list_del_init(&st->node);

		cache_dev_sync_kmem_io(cache_dev, st->buf, st->off, st->len,
				       REQ_OP_WRITE);

		mempool_free(st, &ctx->meta_stage_pool);
		atomic64_inc(&ctx->nr_meta_completed);
		wake_up(&ctx->meta_drain_wq);
	}
}

void cache_dev_meta_stage_write(struct pcache_cache_dev *cache_dev, u64 off,
				const void *src, u32 len)
{
	struct pcache_cache_dev_req_ctx *ctx = &cache_dev->req_ctx;
	struct dm_pcache *pcache = CACHE_DEV_TO_PCACHE(cache_dev);
	struct pcache_meta_stage *st;

	if (WARN_ON_ONCE(len > PCACHE_META_STAGE_BUF_SIZE))
		return;

	st = mempool_alloc(&ctx->meta_stage_pool, GFP_ATOMIC);
	if (WARN_ONCE(!st, "pcache: metadata staging pool exhausted, write dropped\n"))
		return;

	memcpy(st->buf, src, len);
	st->off = off;
	st->len = len;

	/* Count before publishing so a drain that observes the item also waits on it. */
	atomic64_inc(&ctx->nr_meta_submitted);

	spin_lock(&ctx->meta_stage_lock);
	list_add_tail(&st->node, &ctx->meta_stage_list);
	queue_work(pcache->task_wq, &ctx->meta_stage_work);
	spin_unlock(&ctx->meta_stage_lock);
}

void cache_dev_meta_drain(struct pcache_cache_dev *cache_dev)
{
	struct pcache_cache_dev_req_ctx *ctx = &cache_dev->req_ctx;
	u64 target;

	/*
	 * Snapshot the staged-write count and wait for completions to catch
	 * up. Mirrors cache_dev_data_drain: writes staged after the snapshot
	 * belong to a later epoch and do not delay this drain.
	 */
	target = atomic64_read(&ctx->nr_meta_submitted);
	wait_event(ctx->meta_drain_wq,
		   atomic64_read(&ctx->nr_meta_completed) >= target);
}

void cache_dev_data_drain(struct pcache_cache_dev *cache_dev)
{
	struct pcache_cache_dev_req_ctx *ctx = &cache_dev->req_ctx;
	u64 target;

	/*
	 * Snapshot the number of data writes that have been submitted up
	 * to this point. Wait for the completion counter to catch up — any
	 * write submitted after the snapshot bumps nr_data_submitted past
	 * @target but does not delay this drain.
	 */
	target = atomic64_read(&ctx->nr_data_submitted);
	wait_event(ctx->data_drain_wq,
		   atomic64_read(&ctx->nr_data_completed) >= target);
}

struct cache_dev_sync_ctx {
	struct completion done;
	int ret;
};

static void cache_dev_sync_end(struct pcache_cache_dev_req *req, int ret)
{
	struct cache_dev_sync_ctx *sync = req->priv_data;

	sync->ret = ret;
	complete(&sync->done);
}

static int cache_dev_sync_kmem_io_raw(struct pcache_cache_dev *cache_dev,
				      void *data, u64 cdev_off, u32 len,
				      blk_opf_t opf)
{
	struct pcache_cache_dev_req_opts opts = { 0 };
	struct pcache_cache_dev_req *req;
	struct cache_dev_sync_ctx sync;

	init_completion(&sync.done);
	sync.ret = 0;

	opts.type = CACHE_DEV_REQ_TYPE_KMEM;
	opts.cdev_off = cdev_off;
	opts.kmem.data = data;
	opts.kmem.opf = opf;
	opts.kmem.len = len;
	opts.gfp_mask = GFP_NOIO;
	opts.end_fn = cache_dev_sync_end;
	opts.priv_data = &sync;

	req = cache_dev_req_create(cache_dev, &opts);
	if (!req)
		return -ENOMEM;

	cache_dev_req_submit(req, true);
	wait_for_completion(&sync.done);
	return sync.ret;
}

/*
 * Synchronous metadata I/O on the cache medium.
 *
 * Metadata accesses on dm-pcache are often sub-LBA (e.g. struct pcache_sb
 * is 28 bytes, a single pcache_cache_pos_onmedia is 12 bytes), but the
 * block layer rejects bios whose byte size is not a multiple of the
 * device's logical block size. Route any non-LBA-aligned access through
 * an LBA-aligned bounce buffer: for reads, fetch the covering blocks and
 * copy out the requested sub-range; for writes, read-modify-write —
 * read the head/tail blocks first, splice the new bytes, and write the
 * whole aligned region back.
 *
 * The metadata layout already places everything inside 4K-aligned
 * 4K-sized slots, so the bounce buffer is bounded (one or two LBAs).
 */
int cache_dev_sync_kmem_io(struct pcache_cache_dev *cache_dev, void *data,
			   u64 cdev_off, u32 len, blk_opf_t opf)
{
	u32 lba = bdev_logical_block_size(cache_dev->dm_dev->bdev);
	u32 lba_mask = lba - 1;
	u64 aligned_off;
	u32 head_skip;
	u32 aligned_len;
	void *buf;
	int ret;

	if (!(cdev_off & lba_mask) && !(len & lba_mask))
		return cache_dev_sync_kmem_io_raw(cache_dev, data,
						  cdev_off, len, opf);

	aligned_off = cdev_off & ~(u64)lba_mask;
	head_skip = cdev_off - aligned_off;
	aligned_len = round_up(head_skip + len, lba);

	buf = kmalloc(aligned_len, GFP_NOIO);
	if (!buf)
		return -ENOMEM;

	if (opf == REQ_OP_READ) {
		ret = cache_dev_sync_kmem_io_raw(cache_dev, buf, aligned_off,
						 aligned_len, REQ_OP_READ);
		if (!ret)
			memcpy(data, (char *)buf + head_skip, len);
	} else {
		/* RMW: read the covering blocks, splice in @data, write back. */
		ret = cache_dev_sync_kmem_io_raw(cache_dev, buf, aligned_off,
						 aligned_len, REQ_OP_READ);
		if (ret)
			goto out;
		memcpy((char *)buf + head_skip, data, len);
		ret = cache_dev_sync_kmem_io_raw(cache_dev, buf, aligned_off,
						 aligned_len, opf);
	}
out:
	kfree(buf);
	return ret;
}

int cache_dev_zeroout(struct pcache_cache_dev *cache_dev, u64 cdev_off, u64 len)
{
	return blkdev_issue_zeroout(cache_dev->dm_dev->bdev,
				    cdev_off >> SECTOR_SHIFT,
				    len >> SECTOR_SHIFT,
				    GFP_NOIO, 0);
}

int cache_dev_req_ctx_init(struct pcache_cache_dev *cache_dev)
{
	struct pcache_cache_dev_req_ctx *ctx = &cache_dev->req_ctx;
	int ret;

	ret = mempool_init_slab_pool(&ctx->req_pool, 128, cache_dev_req_cache);
	if (ret)
		return ret;

	ret = mempool_init_slab_pool(&ctx->bvec_pool, 128, cache_dev_bvec_cache);
	if (ret)
		goto req_pool_exit;

	ret = mempool_init_slab_pool(&ctx->meta_stage_pool, 64, cache_dev_meta_stage_cache);
	if (ret)
		goto bvec_pool_exit;

	INIT_LIST_HEAD(&ctx->submit_list);
	INIT_LIST_HEAD(&ctx->complete_list);
	spin_lock_init(&ctx->submit_lock);
	spin_lock_init(&ctx->complete_lock);
	INIT_WORK(&ctx->req_submit_work, req_submit_fn);
	INIT_WORK(&ctx->req_complete_work, req_complete_fn);
	atomic_set(&ctx->inflight_reqs, 0);
	init_waitqueue_head(&ctx->inflight_wq);

	atomic64_set(&ctx->nr_data_submitted, 0);
	atomic64_set(&ctx->nr_data_completed, 0);
	init_waitqueue_head(&ctx->data_drain_wq);

	INIT_LIST_HEAD(&ctx->meta_stage_list);
	spin_lock_init(&ctx->meta_stage_lock);
	INIT_WORK(&ctx->meta_stage_work, meta_stage_fn);
	atomic64_set(&ctx->nr_meta_submitted, 0);
	atomic64_set(&ctx->nr_meta_completed, 0);
	init_waitqueue_head(&ctx->meta_drain_wq);

	return 0;

bvec_pool_exit:
	mempool_exit(&ctx->bvec_pool);
req_pool_exit:
	mempool_exit(&ctx->req_pool);
	return ret;
}

void cache_dev_req_ctx_exit(struct pcache_cache_dev *cache_dev)
{
	struct pcache_cache_dev_req_ctx *ctx = &cache_dev->req_ctx;

	wait_event(ctx->inflight_wq, atomic_read(&ctx->inflight_reqs) == 0);

	/* Flush any metadata staged but not yet written before tearing down. */
	cache_dev_meta_drain(cache_dev);
	flush_work(&ctx->meta_stage_work);

	flush_work(&ctx->req_submit_work);
	flush_work(&ctx->req_complete_work);

	mempool_exit(&ctx->meta_stage_pool);
	mempool_exit(&ctx->bvec_pool);
	mempool_exit(&ctx->req_pool);
}

int pcache_cache_dev_req_init(void)
{
	u32 max_bvecs = (PCACHE_CACHE_SUBTREE_SIZE >> PAGE_SHIFT) + 1;
	int ret;

	cache_dev_req_cache = KMEM_CACHE(pcache_cache_dev_req, 0);
	if (!cache_dev_req_cache) {
		ret = -ENOMEM;
		goto err;
	}

	cache_dev_bvec_cache = kmem_cache_create("pcache-cdev-bvec-slab",
				max_bvecs * sizeof(struct bio_vec),
				0, 0, NULL);
	if (!cache_dev_bvec_cache) {
		ret = -ENOMEM;
		goto destroy_req_cache;
	}

	cache_dev_meta_stage_cache = KMEM_CACHE(pcache_meta_stage, 0);
	if (!cache_dev_meta_stage_cache) {
		ret = -ENOMEM;
		goto destroy_bvec_cache;
	}

	return 0;
destroy_bvec_cache:
	kmem_cache_destroy(cache_dev_bvec_cache);
destroy_req_cache:
	kmem_cache_destroy(cache_dev_req_cache);
err:
	return ret;
}

void pcache_cache_dev_req_exit(void)
{
	kmem_cache_destroy(cache_dev_meta_stage_cache);
	kmem_cache_destroy(cache_dev_bvec_cache);
	kmem_cache_destroy(cache_dev_req_cache);
}
