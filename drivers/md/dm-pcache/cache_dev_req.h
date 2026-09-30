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
 * Synchronous helpers built on the bio request lifecycle. Block the
 * caller until the bio completes. Intended for the NVMe-backed
 * cmedium dispatch where metadata I/O is rare and small.
 */
int cache_dev_sync_kmem_io(struct pcache_cache_dev *cache_dev, void *data,
			   u64 cdev_off, u32 len, blk_opf_t opf);
int cache_dev_zeroout(struct pcache_cache_dev *cache_dev, u64 cdev_off, u64 len);

int pcache_cache_dev_req_init(void);
void pcache_cache_dev_req_exit(void);

#endif /* _PCACHE_CACHE_DEV_REQ_H */
