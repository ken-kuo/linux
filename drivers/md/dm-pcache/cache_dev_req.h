/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _PCACHE_CACHE_DEV_REQ_H
#define _PCACHE_CACHE_DEV_REQ_H

#include <linux/bio.h>
#include <linux/blk_types.h>
#include <linux/mempool.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "pcache_internal.h"

struct pcache_cache_dev;
struct pcache_cache_dev_req;
struct pcache_request;

typedef void (*cache_dev_req_end_fn_t)(struct pcache_cache_dev_req *req, int ret);

#define CACHE_DEV_REQ_TYPE_REQ		1
#define CACHE_DEV_REQ_TYPE_KMEM		2

#define CACHE_DEV_REQ_INLINE_BVECS	4

/*
 * pcache_cache_dev_req - a bio request issued against the cache device.
 *
 * Two flavours, mirroring pcache_backing_dev_req:
 *  - TYPE_REQ:  clone of an upper-layer bio, retargeted to the cache
 *               device and trimmed to a sub-range. Data path use
 *               (zero-copy of the upper bio's pages).
 *  - TYPE_KMEM: bio built around a kernel buffer for metadata I/O.
 */
struct pcache_cache_dev_req {
	u8				type;
	/*
	 * Drain class, orthogonal to @type (which only describes the buffer
	 * shape: TYPE_REQ = clone of an upper bio, TYPE_KMEM = a kernel
	 * buffer). When set, this request is a cache-device data-path I/O that
	 * a FLUSH must wait for via cache_dev_data_drain(). Always true for
	 * TYPE_REQ; opt-in for TYPE_KMEM (e.g. the read-miss promote write,
	 * as opposed to metadata I/O which is not data-drained).
	 */
	bool				count_data;
	struct bio			bio;
	struct pcache_cache_dev		*cache_dev;
	int				ret;

	void				*priv_data;
	cache_dev_req_end_fn_t		end_req;

	struct list_head		node;

	union {
		struct {
			struct pcache_request	*upper_req;
			u32			bio_off;
		} req;
		struct {
			struct bio_vec	inline_bvecs[CACHE_DEV_REQ_INLINE_BVECS];
			struct bio_vec	*bvecs;
			u32		n_vecs;
		} kmem;
	};
};

/* Per-cache-device request lifecycle state. Embedded in struct pcache_cache_dev. */
struct pcache_cache_dev_req_ctx {
	mempool_t			req_pool;
	mempool_t			bvec_pool;

	struct list_head		submit_list;
	spinlock_t			submit_lock;
	struct work_struct		req_submit_work;

	struct list_head		complete_list;
	spinlock_t			complete_lock;
	struct work_struct		req_complete_work;

	atomic_t			inflight_reqs;
	wait_queue_head_t		inflight_wq;

	/*
	 * Monotonic counters for cache-device data-path I/O (any request with
	 * ->count_data set: all TYPE_REQ, plus the promote TYPE_KMEM write).
	 * Used by cache_dev_data_drain at the FLUSH boundary to wait for I/O
	 * submitted before the FLUSH to complete, without blocking I/O
	 * submitted after it. nr_data_submitted is incremented when the bio is
	 * allocated; nr_data_completed when the bio completes.
	 */
	atomic64_t			nr_data_submitted;
	atomic64_t			nr_data_completed;
	wait_queue_head_t		data_drain_wq;

	/*
	 * DRAM-staged metadata writes (BIO backend only).
	 *
	 * Several metadata writers (cache_kset_close, cache_seg_ctrl_write)
	 * run with a spinlock held, where the synchronous bio of
	 * cache_dev_sync_kmem_io would sleep. On BIO those writers instead
	 * copy their bytes into a staging object under the lock (a non-
	 * sleeping memcpy) and enqueue it here; meta_stage_work drains the
	 * queue with real device I/O outside any lock. Durability is enforced
	 * at the upper FLUSH boundary via cache_dev_meta_drain.
	 */
	mempool_t			meta_stage_pool;
	struct list_head		meta_stage_list;
	spinlock_t			meta_stage_lock;
	struct work_struct		meta_stage_work;
	atomic64_t			nr_meta_submitted;
	atomic64_t			nr_meta_completed;
	wait_queue_head_t		meta_drain_wq;
};

/* Upper bound on a single staged metadata write (largest is a full kset). */
#define PCACHE_META_STAGE_BUF_SIZE	8192

struct pcache_meta_stage {
	struct list_head	node;
	u64			off;
	u32			len;
	char			buf[PCACHE_META_STAGE_BUF_SIZE];
};

struct pcache_cache_dev_req_opts {
	u32 type;
	u64 cdev_off;
	union {
		struct {
			struct pcache_request	*upper_req;
			u32			req_off;
			u32			len;
		} req;
		struct {
			void		*data;
			blk_opf_t	opf;
			u32		len;
		} kmem;
	};

	gfp_t				gfp_mask;
	/*
	 * Mark a TYPE_KMEM request as a cache-device data write so it is
	 * accounted in cache_dev_data_drain (used by the read-miss promote).
	 * Ignored for TYPE_REQ, which is always data-drained.
	 */
	bool				count_data;
	cache_dev_req_end_fn_t		end_fn;
	void				*priv_data;
};

int cache_dev_req_ctx_init(struct pcache_cache_dev *cache_dev);
void cache_dev_req_ctx_exit(struct pcache_cache_dev *cache_dev);

struct pcache_cache_dev_req *
cache_dev_req_alloc(struct pcache_cache_dev *cache_dev,
		    struct pcache_cache_dev_req_opts *opts);
void cache_dev_req_init(struct pcache_cache_dev_req *req,
			struct pcache_cache_dev_req_opts *opts);
struct pcache_cache_dev_req *
cache_dev_req_create(struct pcache_cache_dev *cache_dev,
		     struct pcache_cache_dev_req_opts *opts);

void cache_dev_req_submit(struct pcache_cache_dev_req *req, bool direct);
void cache_dev_req_end(struct pcache_cache_dev_req *req);

void cache_dev_bdev_flush(struct pcache_cache_dev *cache_dev);

/*
 * Stage a metadata write for the BIO backend. Copies @len bytes from @src
 * into a staging object and queues it; the actual device write happens
 * asynchronously in meta_stage_work. Safe to call with a spinlock held (no
 * sleeping). @len must not exceed PCACHE_META_STAGE_BUF_SIZE.
 */
void cache_dev_meta_stage_write(struct pcache_cache_dev *cache_dev, u64 off,
				const void *src, u32 len);

/*
 * Wait for all metadata writes staged before this call to reach the device.
 * Must be called from a sleepable context (no spinlock held). Pair with
 * cache_dev_bdev_flush to make the staged metadata durable on media.
 */
void cache_dev_meta_drain(struct pcache_cache_dev *cache_dev);

/*
 * Wait for all cache-device data writes (TYPE_REQ) submitted before the
 * call to complete. Writes submitted after the call do not block this
 * wait. Used by pcache_cache_flush to enforce data-before-kset
 * durability ordering at the upper FLUSH boundary.
 */
void cache_dev_data_drain(struct pcache_cache_dev *cache_dev);

/*
 * Synchronous helpers built on the bio request lifecycle. Block the
 * caller until the bio completes. Intended for the BIO-backed
 * cmedium dispatch where metadata I/O is rare and small.
 */
int cache_dev_sync_kmem_io(struct pcache_cache_dev *cache_dev, void *data,
			   u64 cdev_off, u32 len, blk_opf_t opf);
int cache_dev_zeroout(struct pcache_cache_dev *cache_dev, u64 cdev_off, u64 len);

int pcache_cache_dev_req_init(void);
void pcache_cache_dev_req_exit(void);

#endif /* _PCACHE_CACHE_DEV_REQ_H */
