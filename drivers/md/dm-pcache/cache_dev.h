/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _PCACHE_CACHE_DEV_H
#define _PCACHE_CACHE_DEV_H

#include <linux/device.h>
#include <linux/device-mapper.h>

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

/*
 * PCACHE SB flags configured during formatting
 *
 * The PCACHE_SB_F_xxx flags define registration requirements based on cache_dev
 * formatting. For a machine to register a cache_dev:
 * - PCACHE_SB_F_BIGENDIAN: Requires a big-endian machine.
 */
#define PCACHE_SB_F_BIGENDIAN			BIT(0)

struct pcache_sb {
	__le32 crc;
	__le32 flags;
	__le64 magic;

	__le32 seg_num;
};

struct pcache_cache_dev {
	u32				sb_flags;
	u32				seg_num;
	void				*mapping;
	bool				use_vmap;

	struct dm_dev			*dm_dev;

	struct mutex			seg_lock;
	unsigned long			*seg_bitmap;
};

struct dm_pcache;
int cache_dev_start(struct dm_pcache *pcache);
void cache_dev_stop(struct dm_pcache *pcache);

void cache_dev_zero_range(struct pcache_cache_dev *cache_dev, void *pos, u32 size);

int cache_dev_get_empty_segment_id(struct pcache_cache_dev *cache_dev, u32 *seg_id);

#endif /* _PCACHE_CACHE_DEV_H */
