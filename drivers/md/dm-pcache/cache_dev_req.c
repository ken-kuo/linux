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

	INIT_LIST_HEAD(&ctx->submit_list);
	INIT_LIST_HEAD(&ctx->complete_list);
	spin_lock_init(&ctx->submit_lock);
	spin_lock_init(&ctx->complete_lock);
	INIT_WORK(&ctx->req_submit_work, req_submit_fn);
	INIT_WORK(&ctx->req_complete_work, req_complete_fn);
	atomic_set(&ctx->inflight_reqs, 0);
	init_waitqueue_head(&ctx->inflight_wq);

	return 0;

req_pool_exit:
	mempool_exit(&ctx->req_pool);
	return ret;
}

void cache_dev_req_ctx_exit(struct pcache_cache_dev *cache_dev)
{
	struct pcache_cache_dev_req_ctx *ctx = &cache_dev->req_ctx;

	wait_event(ctx->inflight_wq, atomic_read(&ctx->inflight_reqs) == 0);

	flush_work(&ctx->req_submit_work);
	flush_work(&ctx->req_complete_work);

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

	return 0;
destroy_req_cache:
	kmem_cache_destroy(cache_dev_req_cache);
err:
	return ret;
}

void pcache_cache_dev_req_exit(void)
{
	kmem_cache_destroy(cache_dev_bvec_cache);
	kmem_cache_destroy(cache_dev_req_cache);
}
