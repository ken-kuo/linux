/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _PCACHE_CACHE_DEV_H
#define _PCACHE_CACHE_DEV_H

#include <linux/device.h>
#include <linux/device-mapper.h>
#include <linux/dax.h>
#include <linux/libnvdimm.h>
#include <linux/string.h>
#include <linux/uio.h>

#include "cache_dev_req.h"
#include "pcache_internal.h"

#define PCACHE_MAGIC				0x65B05EFA96C596EFULL

#define PCACHE_SB_OFF				(4 * PCACHE_KB)
#define PCACHE_SB_SIZE				(4 * PCACHE_KB)

#define PCACHE_CACHE_INFO_OFF			(PCACHE_SB_OFF + PCACHE_SB_SIZE)
#define PCACHE_CACHE_INFO_SIZE			(4 * PCACHE_KB)

#define PCACHE_CACHE_CTRL_OFF			(PCACHE_CACHE_INFO_OFF + (PCACHE_CACHE_INFO_SIZE * PCACHE_META_INDEX_MAX))
#define PCACHE_CACHE_CTRL_SIZE			(4 * PCACHE_KB)

/*
 * Phase 3 on-disk layout (breaking change vs Phase 2):
 *
 *   ... cache_ctrl ...
 *   peer registry region (8 KiB = 4 KiB × PCACHE_META_INDEX_MAX double-copy)
 *   bitmap region (1 GiB = PCACHE_BM_MAX_PEERS × PCACHE_BITMAP_SLOT_SIZE)
 *   segments ...
 *
 * Peer registry persists (node_id, addr, generation) per bitmap slot so that
 * slot ownership survives reboots. Sized to fit 32 entries comfortably in a
 * single 4 KiB page.
 *
 * Each bitmap slot is sized to cover up to ~4 TiB backing per peer (32 MiB /
 * 8 bits / 4 KiB block = 4 TiB). PCACHE_BM_MAX_PEERS bumped 2 → 32 to match
 * DRBD 9's DRBD_PEERS_MAX upper bound.
 *
 * Existing Phase 2 PMEM must be re-formatted (zero the SB) before loading a
 * Phase 3 module — both the registry insertion and the bitmap region growth
 * shift the segment base offset.
 */
#define PCACHE_PEER_REGISTRY_OFF		(PCACHE_CACHE_CTRL_OFF + PCACHE_CACHE_CTRL_SIZE)
#define PCACHE_PEER_REGISTRY_HDR_SIZE		(4 * PCACHE_KB)
#define PCACHE_PEER_REGISTRY_REGION_SIZE	(PCACHE_PEER_REGISTRY_HDR_SIZE * PCACHE_META_INDEX_MAX)

#define PCACHE_BM_MAX_PEERS			32
#define PCACHE_BITMAP_OFF			(PCACHE_PEER_REGISTRY_OFF + PCACHE_PEER_REGISTRY_REGION_SIZE)
#define PCACHE_BITMAP_SLOT_SIZE			(32 * PCACHE_MB)
#define PCACHE_BITMAP_REGION_SIZE		(PCACHE_BITMAP_SLOT_SIZE * PCACHE_BM_MAX_PEERS)
#define PCACHE_BITMAP_SLOT_HDR_SIZE		(4 * PCACHE_KB)
#define PCACHE_BITMAP_SLOT_DATA_OFF		(PCACHE_BITMAP_SLOT_HDR_SIZE * PCACHE_META_INDEX_MAX)
#define PCACHE_BITMAP_SLOT_DATA_SIZE		(PCACHE_BITMAP_SLOT_SIZE - PCACHE_BITMAP_SLOT_DATA_OFF)

#define PCACHE_SEGMENTS_OFF			(PCACHE_BITMAP_OFF + PCACHE_BITMAP_REGION_SIZE)
#define PCACHE_SEG_INFO_SIZE			(4 * PCACHE_KB)

#define CACHE_DEV_BITMAP_SLOT(cache_dev, slot)	\
	((void *)cache_dev->mapping + PCACHE_BITMAP_OFF + (slot) * PCACHE_BITMAP_SLOT_SIZE)

#define PCACHE_CACHE_DEV_SIZE_MIN		(512 * PCACHE_MB)	/* 512 MB */
#define PCACHE_SEG_SIZE				(16 * PCACHE_MB)	/* Size of each PCACHE segment (16 MB) */

#define CACHE_DEV_SB(cache_dev)			((struct pcache_sb *)(cache_dev->mapping + PCACHE_SB_OFF))
#define CACHE_DEV_CACHE_INFO(cache_dev)		((void *)cache_dev->mapping + PCACHE_CACHE_INFO_OFF)
#define CACHE_DEV_CACHE_CTRL(cache_dev)		((void *)cache_dev->mapping + PCACHE_CACHE_CTRL_OFF)
#define CACHE_DEV_PEER_REGISTRY(cache_dev)	((void *)cache_dev->mapping + PCACHE_PEER_REGISTRY_OFF)
#define CACHE_DEV_SEGMENTS(cache_dev)		((void *)cache_dev->mapping + PCACHE_SEGMENTS_OFF)
#define CACHE_DEV_SEGMENT(cache_dev, id)	((void *)CACHE_DEV_SEGMENTS(cache_dev) + (u64)id * PCACHE_SEG_SIZE)
#define CACHE_DEV_SEGMENT_OFF(id)		(PCACHE_SEGMENTS_OFF + (u64)(id) * PCACHE_SEG_SIZE)

/*
 * PCACHE SB flags configured during formatting
 *
 * The PCACHE_SB_F_xxx flags define registration requirements based on cache_dev
 * formatting. For a machine to register a cache_dev:
 * - PCACHE_SB_F_BIGENDIAN: Requires a big-endian machine.
 */
#define PCACHE_SB_F_BIGENDIAN			BIT(0)

/*
 * pcache_sb.cache_type — the access model the on-media layout assumes.
 * Recorded at format time, validated against the device on attach.
 */
#define PCACHE_CACHE_TYPE_PMEM			0
#define PCACHE_CACHE_TYPE_NVME			1

struct pcache_sb {
	__le32 crc;
	__le32 flags;
	__le64 magic;

	__le32 seg_num;
	__le32 block_size;	/* sector count; >= cache LBA, <= PAGE_SECTORS, power of 2 */
	__le32 cache_type;	/* PCACHE_CACHE_TYPE_PMEM | _NVME */
};

enum pcache_cache_dev_backend {
	PCACHE_CACHE_DEV_PMEM = 0,	/* byte-addressable DAX (default) */
	PCACHE_CACHE_DEV_NVME = 1,	/* block device via bio (cache_dev_req) */
};

struct pcache_cache_dev {
	u32				sb_flags;
	u32				seg_num;
	void				*mapping;
	bool				use_vmap;
	enum pcache_cache_dev_backend	backend;

	struct dm_dev			*dm_dev;

	struct mutex			seg_lock;
	unsigned long			*seg_bitmap;

	struct pcache_cache_dev_req_ctx	req_ctx;
};

struct dm_pcache;
int cache_dev_start(struct dm_pcache *pcache);
void cache_dev_stop(struct dm_pcache *pcache);

int cache_dev_get_empty_segment_id(struct pcache_cache_dev *cache_dev, u32 *seg_id);

/*
 * cmedium_* — byte-offset access to the cache medium.
 *
 * dm-pcache reaches the cache device today by computing a virtual address
 * inside the DAX whole-device kernel mapping and issuing pmem-aware
 * primitives (memcpy_flushcache, copy_mc_to_kernel, dax_flush). To allow
 * the same call sites to address a non-byte-addressable medium (an NVMe
 * block device) without leaking the access model into callers, primitives
 * below take a byte offset rather than a raw pointer and dispatch on
 * cache_dev->backend.
 *
 * The PMEM backend is the existing path: cmedium_write makes the bytes
 * cache-line durable but does not issue a store barrier, cmedium_flush is
 * the explicit ordering point (pmem_wmb / SFENCE on x86), and cmedium_zero
 * is also unfenced — callers must follow with cmedium_flush if a
 * dependent write requires the zeros to have landed first.
 *
 * The NVME backend routes each call through cache_dev_req. For now it
 * issues each metadata write synchronously and silently swallows I/O
 * errors at the cmedium_write level; later patches replace metadata
 * writes with a DRAM-staged path that batches device I/O at flush
 * boundaries. cmedium_read_iter and cmedium_write_iter (the per-bio data
 * copies) intentionally stay pmem-only here and return -EOPNOTSUPP on
 * NVMe; the async data path that supersedes them comes in the next
 * patches.
 */
static inline void *cmedium_addr(struct pcache_cache_dev *cache_dev, u64 off)
{
	return (char *)cache_dev->mapping + off;
}

static inline u64 cmedium_off(struct pcache_cache_dev *cache_dev, const void *addr)
{
	return (u64)((const char *)addr - (const char *)cache_dev->mapping);
}

static inline int cmedium_read(struct pcache_cache_dev *cache_dev, void *dst,
			       u64 off, size_t len)
{
	if (cache_dev->backend == PCACHE_CACHE_DEV_NVME)
		return cache_dev_sync_kmem_io(cache_dev, dst, off, len, REQ_OP_READ);

	if (copy_mc_to_kernel(dst, cmedium_addr(cache_dev, off), len))
		return -EIO;
	return 0;
}

static inline int cmedium_read_iter(struct pcache_cache_dev *cache_dev, u64 off,
				    size_t len, struct iov_iter *iter)
{
	/*
	 * Per-bio data copies are issued as asynchronous bios on the NVMe
	 * backend (cache_hit_read_submit / cache_data_write_submit submit a
	 * CACHE_DEV_REQ_TYPE_REQ directly), so this byte-copy helper is only
	 * reached on PMEM. Flag — rather than silently bounce — any future
	 * caller that routes an NVMe data copy through here.
	 */
	if (WARN_ON_ONCE(cache_dev->backend == PCACHE_CACHE_DEV_NVME))
		return -EOPNOTSUPP;

	if (_copy_mc_to_iter(cmedium_addr(cache_dev, off), len, iter) != len)
		return -EIO;
	return 0;
}

static inline void cmedium_write(struct pcache_cache_dev *cache_dev, u64 off,
				 const void *src, size_t len)
{
	if (cache_dev->backend == PCACHE_CACHE_DEV_NVME) {
		/*
		 * Cast away const: cache_dev_sync_kmem_io builds a bio over
		 * the buffer and does not modify it for REQ_OP_WRITE.
		 */
		cache_dev_sync_kmem_io(cache_dev, (void *)src, off, len, REQ_OP_WRITE);
		return;
	}
	memcpy_flushcache(cmedium_addr(cache_dev, off), src, len);
}

static inline int cmedium_write_iter(struct pcache_cache_dev *cache_dev, u64 off,
				     size_t len, struct iov_iter *iter)
{
	/* PMEM-only, like cmedium_read_iter; the NVMe data path is async. */
	if (WARN_ON_ONCE(cache_dev->backend == PCACHE_CACHE_DEV_NVME))
		return -EOPNOTSUPP;

	if (_copy_from_iter_flushcache(cmedium_addr(cache_dev, off), len, iter) != len)
		return -EIO;
	return 0;
}

static inline void cmedium_zero(struct pcache_cache_dev *cache_dev, u64 off,
				size_t len)
{
	void *addr;

	if (cache_dev->backend == PCACHE_CACHE_DEV_NVME) {
		cache_dev_zeroout(cache_dev, off, len);
		return;
	}
	addr = cmedium_addr(cache_dev, off);
	memset(addr, 0, len);
	dax_flush(cache_dev->dm_dev->dax_dev, addr, len);
}

/*
 * cmedium_stage_write - metadata write that is safe under a spinlock.
 *
 * PMEM: identical to cmedium_write (a non-sleeping cache-line copy).
 * NVMe: the synchronous bio of cmedium_write would sleep, so stage the
 *       bytes and let the metadata worker write them outside any lock.
 *
 * Callers must reach a cmedium_flush (which drains staged metadata) before
 * relying on the bytes being durable.
 */
static inline void cmedium_stage_write(struct pcache_cache_dev *cache_dev, u64 off,
				       const void *src, size_t len)
{
	if (cache_dev->backend == PCACHE_CACHE_DEV_NVME) {
		cache_dev_meta_stage_write(cache_dev, off, src, len);
		return;
	}
	memcpy_flushcache(cmedium_addr(cache_dev, off), src, len);
}

/*
 * cmedium_stage_barrier - ordering point for staged metadata, usable under a
 * spinlock.
 *
 * PMEM: pmem_wmb, the same store barrier cmedium_flush issues. NVMe: a no-op
 * — staged metadata is ordered by the FIFO worker and made durable at the
 * next cmedium_flush (the upper FLUSH boundary), so there is nothing to do
 * here and, crucially, nothing that sleeps.
 */
static inline void cmedium_stage_barrier(struct pcache_cache_dev *cache_dev)
{
	if (cache_dev->backend == PCACHE_CACHE_DEV_NVME)
		return;
	pmem_wmb();
}

static inline void cmedium_flush(struct pcache_cache_dev *cache_dev)
{
	if (cache_dev->backend == PCACHE_CACHE_DEV_NVME) {
		/* Make sure staged metadata has reached the device first. */
		cache_dev_meta_drain(cache_dev);
		cache_dev_bdev_flush(cache_dev);
		return;
	}
	pmem_wmb();
}

/*
 * pcache_meta_find_latest - Find the latest valid metadata copy on the cache
 *                           medium and load it into @meta_ret.
 * @cache_dev: cache device the metadata lives on.
 * @header_off: byte offset of the first metadata copy on the cache medium.
 * @meta_size: size of the metadata structure being read.
 * @meta_max_size: distance between consecutive copies (slot stride).
 * @meta_ret: caller-supplied buffer of at least @meta_size bytes.
 *
 * Reads each of PCACHE_META_INDEX_MAX copies via cmedium_read, validates
 * CRC, picks the highest sequence, and returns the chosen copy's index
 * (0..PCACHE_META_INDEX_MAX-1). Returns -EIO on a media read error and
 * -ENOENT if no valid copy was found.
 */
int __must_check pcache_meta_find_latest(struct pcache_cache_dev *cache_dev,
					 u64 header_off, u32 meta_size,
					 u32 meta_max_size, void *meta_ret);

#endif /* _PCACHE_CACHE_DEV_H */
