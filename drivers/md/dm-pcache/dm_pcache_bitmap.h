/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _DM_PCACHE_BITMAP_H
#define _DM_PCACHE_BITMAP_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include "pcache_internal.h"

struct dm_pcache;
struct pcache_peer;

#define PCACHE_BM_BLOCK_SHIFT		12U			/* 4 KiB / bit */
#define PCACHE_BM_BLOCK_SIZE		(1U << PCACHE_BM_BLOCK_SHIFT)
#define PCACHE_BM_SECTORS_PER_BIT	(PCACHE_BM_BLOCK_SIZE >> SECTOR_SHIFT)

/* Phase 3.5 bumps magic — on-disk layout breaking change. Old Phase 2/3
 * PMEM (PCACMB1\2 in slot 0 only) must be zero'd before mounting under 3.5. */
#define PCACHE_BM_MAGIC			0x504341434d423103ULL	/* "PCACMB1\3" */

/*
 * On-disk per-peer-slot header.  PCACHE_META_INDEX_MAX copies live at the
 * head of each slot; the latest valid (CRC-match, highest seq) wins.
 * Sized to 4 KiB to match other dm-pcache metadata blocks.
 */
struct pcache_bm_header_onmedia {
	struct pcache_meta_header	header;		/* crc + seq + version */
	__le64	magic;					/* = PCACHE_BM_MAGIC */
	__le64	generation;				/* this node's current gen */
	__le64	peer_gen_at_sync;			/* peer's gen at last clean sync */
	__le64	bm_nr_bits;
	__le32	bm_block_shift;
	__le32	flags;
	__le64	dirty_count;				/* informational only */
	__le64	reserved[6];
} __packed;

#define PCACHE_BM_F_FULL_SYNC_NEEDED	BIT(0)
#define PCACHE_BM_F_RESYNC_IN_PROGRESS	BIT(1)

/*
 * Phase 3.5 — Per-peer dirty bitmap.
 *
 * Each pcache_peer owns its own pcache_bitmap instance; the bitmap tracks
 * regions THIS peer is currently out of sync on. Replaces Phase 2/3's
 * single shared bitmap on pcache->bitmap.
 *
 * Per-peer PMEM persistence: bm->slot_idx is the registry slot index
 * (0..PCACHE_BM_MAX_PEERS-1), and pmem_base/pmem_data point at this
 * slot's reserved region on PMEM.
 */
struct pcache_bitmap {
	struct pcache_peer	*peer;		/* owning peer (back-ref) */
	struct dm_pcache	*pcache;	/* cached for convenience: peer->pcache */
	unsigned long		*cache;		/* RAM working copy */
	u64			nr_bits;
	u64			nr_words;	/* DIV_ROUND_UP(nr_bits, BITS_PER_LONG) */
	spinlock_t		lock;
	atomic64_t		dirty_count;

	/* PMEM mapping — slot decided by registry slot index */
	int			slot_idx;
	void			*pmem_base;	/* CACHE_DEV_BITMAP_SLOT(cdev, slot_idx) */
	void			*pmem_data;	/* pmem_base + DATA_OFF */
	u32			nr_pages;	/* number of 4 KiB pages in pmem_data */

	/* Dirty-page tracking for hinted writeback */
	unsigned long		*dirty_pages;	/* bit i = RAM page i differs from PMEM */
	u8			meta_seq;	/* monotonic seq for header double-copy */
	u8			meta_index;	/* which header copy is current */

	/* Per-peer state mirrored to/from on-disk header.
	 * `generation` is a snapshot of pcache->generation at last header flush
	 * (replicated across all peers' slots for recovery redundancy). */
	u64			generation;
	u64			peer_gen_at_sync;
	u32			flags;
};

int  pcache_peer_bitmap_init(struct pcache_peer *peer, u64 backing_sectors,
			     int slot_idx);
void pcache_peer_bitmap_exit(struct pcache_peer *peer);

void pcache_bm_set_range(struct pcache_bitmap *bm, u64 sector, u32 size);
void pcache_bm_clear_range(struct pcache_bitmap *bm, u64 sector, u32 size);

bool pcache_bm_test_bit(struct pcache_bitmap *bm, u64 bit);
void pcache_bm_set_all(struct pcache_bitmap *bm);
void pcache_bm_clear_all(struct pcache_bitmap *bm);

static inline u64 pcache_bm_dirty_count(struct pcache_bitmap *bm)
{
	return (u64)atomic64_read(&bm->dirty_count);
}

bool pcache_bm_find_next_dirty(struct pcache_bitmap *bm, u64 *start_bit,
			       u32 *nr_bits, u32 max_run);

/* Phase 2 Step 2 — persistence */
int  pcache_bm_flush_sync(struct pcache_bitmap *bm);

/* Phase 3.5 — generation bookkeeping (was Phase 2 Step 9).
 * Called on transition into ESTABLISHED to record the peer's gen and
 * (if no other peer is still ahead) reset the device-wide gen_dirty flag. */
void pcache_bm_mark_synced(struct pcache_bitmap *bm, u64 peer_gen);

#endif /* _DM_PCACHE_BITMAP_H */
