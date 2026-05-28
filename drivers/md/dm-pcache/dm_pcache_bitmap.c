// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/dax.h>
#include <linux/libnvdimm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include "cache_dev.h"
#include "dm_pcache.h"
#include "dm_pcache_bitmap.h"
#include "dm_pcache_net.h"
#include "pcache_internal.h"

static void pcache_bm_header_flush(struct pcache_bitmap *bm);

/* Mark all 4 KiB PMEM pages overlapping [start_bit, end_bit) as dirty,
 * so the next flush_sync writes them back. */
static void pcache_bm_mark_range_pages_dirty(struct pcache_bitmap *bm,
					     u64 start_bit, u64 end_bit)
{
	u64 sb = start_bit / BITS_PER_BYTE / PCACHE_BM_BLOCK_SIZE;
	u64 eb = (end_bit + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
	u32 first_page = (u32)sb;
	u32 last_page  = (u32)((eb + PCACHE_BM_BLOCK_SIZE - 1) >> PCACHE_BM_BLOCK_SHIFT);

	if (last_page > bm->nr_pages)
		last_page = bm->nr_pages;
	if (first_page < last_page)
		bitmap_set(bm->dirty_pages, first_page, last_page - first_page);
}

/*
 * Convert a [sector, sector + nsec) byte-aligned-to-PCACHE_BM_BLOCK_SIZE
 * range into a [start_bit, end_bit) half-open bit range, rounding the
 * start DOWN and the end UP so we never under-mark a partially dirty
 * 4 KiB block.
 */
static void pcache_bm_range_to_bits(struct pcache_bitmap *bm,
				    u64 sector, u32 size,
				    u64 *start_bit, u64 *end_bit)
{
	u64 first_sector = sector;
	u64 last_sector  = sector + (size >> SECTOR_SHIFT);

	*start_bit = first_sector / PCACHE_BM_SECTORS_PER_BIT;
	*end_bit   = DIV_ROUND_UP_ULL(last_sector, PCACHE_BM_SECTORS_PER_BIT);

	if (*end_bit > bm->nr_bits)
		*end_bit = bm->nr_bits;
	if (*start_bit > bm->nr_bits)
		*start_bit = bm->nr_bits;
}

void pcache_bm_set_range(struct pcache_bitmap *bm, u64 sector, u32 size)
{
	u64 start_bit, end_bit, b;
	long delta = 0;
	bool bumped_gen = false;
	struct dm_pcache *pcache;

	if (!bm || !size)
		return;
	pcache = bm->pcache;

	pcache_bm_range_to_bits(bm, sector, size, &start_bit, &end_bit);

	spin_lock(&bm->lock);
	for (b = start_bit; b < end_bit; b++) {
		if (!test_bit(b, bm->cache)) {
			__set_bit(b, bm->cache);
			delta++;
		}
	}
	pcache_bm_mark_range_pages_dirty(bm, start_bit, end_bit);
	spin_unlock(&bm->lock);

	if (delta)
		atomic64_add(delta, &bm->dirty_count);

	/* Phase 3.5 — device-wide gen bump (was per-bitmap in Phase 2/3).
	 * First new dirty bit anywhere since last mark_synced bumps gen so
	 * the next handshake puts us as SYNC_SOURCE for any peer behind. */
	if (delta && pcache) {
		spin_lock(&pcache->gen_lock);
		if (!pcache->gen_dirty) {
			pcache->gen_dirty = true;
			pcache->generation++;
			bumped_gen = true;
		}
		bm->generation = pcache->generation;	/* mirror for PMEM persist */
		spin_unlock(&pcache->gen_lock);
	}

	if (bumped_gen)
		pcache_bm_header_flush(bm);
}

void pcache_bm_clear_range(struct pcache_bitmap *bm, u64 sector, u32 size)
{
	u64 start_bit, end_bit, b;
	long delta = 0;

	if (!bm || !size)
		return;

	pcache_bm_range_to_bits(bm, sector, size, &start_bit, &end_bit);

	spin_lock(&bm->lock);
	for (b = start_bit; b < end_bit; b++) {
		if (test_bit(b, bm->cache)) {
			__clear_bit(b, bm->cache);
			delta++;
		}
	}
	pcache_bm_mark_range_pages_dirty(bm, start_bit, end_bit);
	spin_unlock(&bm->lock);

	if (delta)
		atomic64_sub(delta, &bm->dirty_count);
}

bool pcache_bm_test_bit(struct pcache_bitmap *bm, u64 bit)
{
	bool ret;

	if (!bm || bit >= bm->nr_bits)
		return false;

	spin_lock(&bm->lock);
	ret = test_bit(bit, bm->cache);
	spin_unlock(&bm->lock);
	return ret;
}

void pcache_bm_set_all(struct pcache_bitmap *bm)
{
	if (!bm)
		return;

	spin_lock(&bm->lock);
	bitmap_set(bm->cache, 0, bm->nr_bits);
	spin_unlock(&bm->lock);
	atomic64_set(&bm->dirty_count, (s64)bm->nr_bits);
}

void pcache_bm_clear_all(struct pcache_bitmap *bm)
{
	if (!bm)
		return;

	spin_lock(&bm->lock);
	bitmap_zero(bm->cache, bm->nr_bits);
	spin_unlock(&bm->lock);
	atomic64_set(&bm->dirty_count, 0);
}

bool pcache_bm_find_next_dirty(struct pcache_bitmap *bm, u64 *start_bit,
			       u32 *nr_bits, u32 max_run)
{
	u64 begin, end, scan;

	if (!bm || !start_bit || !nr_bits || !max_run)
		return false;
	if (*start_bit >= bm->nr_bits)
		return false;

	spin_lock(&bm->lock);
	begin = find_next_bit(bm->cache, bm->nr_bits, *start_bit);
	if (begin >= bm->nr_bits) {
		spin_unlock(&bm->lock);
		return false;
	}
	scan = min_t(u64, begin + max_run, bm->nr_bits);
	end  = find_next_zero_bit(bm->cache, scan, begin);
	spin_unlock(&bm->lock);

	*start_bit = begin;
	*nr_bits   = (u32)(end - begin);
	return true;
}

/*
 * Look at both on-disk header copies, return the latest valid one (or NULL).
 * Also fills *which_idx with the copy index that was picked.
 */
static struct pcache_bm_header_onmedia *
pcache_bm_header_load(struct pcache_bitmap *bm,
		      struct pcache_bm_header_onmedia *out, u32 *which_idx)
{
	struct pcache_bm_header_onmedia *hdr_array = bm->pmem_base;

	return pcache_meta_find_latest(&hdr_array->header,
				       sizeof(*out),
				       PCACHE_BITMAP_SLOT_HDR_SIZE,
				       out);
	/* Note: pcache_meta_find_latest doesn't return the index it picked,
	 * but we re-derive it from out->header.seq below. */
	(void)which_idx;
}

/* Write the current in-memory header state to the next (round-robin) PMEM slot. */
static void pcache_bm_header_flush(struct pcache_bitmap *bm)
{
	struct pcache_bm_header_onmedia hdr = {0};
	void *dst;

	hdr.header.seq     = ++bm->meta_seq;
	hdr.header.version = 0;
	hdr.magic          = cpu_to_le64(PCACHE_BM_MAGIC);
	hdr.generation     = cpu_to_le64(bm->generation);
	hdr.peer_gen_at_sync = cpu_to_le64(bm->peer_gen_at_sync);
	hdr.bm_nr_bits     = cpu_to_le64(bm->nr_bits);
	hdr.bm_block_shift = cpu_to_le32(PCACHE_BM_BLOCK_SHIFT);
	hdr.flags          = cpu_to_le32(bm->flags);
	hdr.dirty_count    = cpu_to_le64(pcache_bm_dirty_count(bm));
	hdr.header.crc     = pcache_meta_crc(&hdr.header, sizeof(hdr));

	bm->meta_index = (bm->meta_index + 1) % PCACHE_META_INDEX_MAX;
	dst = bm->pmem_base + bm->meta_index * PCACHE_BITMAP_SLOT_HDR_SIZE;
	memcpy_flushcache(dst, &hdr, sizeof(hdr));
	pmem_wmb();
}

/*
 * Flush all dirty bitmap pages from RAM to PMEM, then re-stamp the header.
 * Safe to call from sleepable context. Caller doesn't need to hold bm->lock;
 * we take it briefly to snapshot dirty pages.
 */
/*
 * Record that we are now in sync with the peer at its reported generation.
 * Caller should already have cleared the in-RAM bitmap if appropriate.
 */
void pcache_bm_mark_synced(struct pcache_bitmap *bm, u64 peer_gen)
{
	bool changed = false;
	struct dm_pcache *pcache;

	if (!bm)
		return;
	pcache = bm->pcache;

	spin_lock(&bm->lock);
	if (bm->peer_gen_at_sync != peer_gen) {
		bm->peer_gen_at_sync = peer_gen;
		changed = true;
	}
	if (bm->flags & PCACHE_BM_F_FULL_SYNC_NEEDED) {
		bm->flags &= ~PCACHE_BM_F_FULL_SYNC_NEEDED;
		changed = true;
	}
	if (bm->flags & PCACHE_BM_F_RESYNC_IN_PROGRESS) {
		bm->flags &= ~PCACHE_BM_F_RESYNC_IN_PROGRESS;
		changed = true;
	}
	spin_unlock(&bm->lock);

	/* Phase 3.5 — gen_dirty is device-wide; reset on any peer's sync.
	 * Other peers behind on generation are still flagged via their
	 * own peer_gen_at_sync < pcache->generation. */
	if (pcache) {
		spin_lock(&pcache->gen_lock);
		if (pcache->gen_dirty) {
			pcache->gen_dirty = false;
			changed = true;
		}
		bm->generation = pcache->generation;	/* mirror */
		spin_unlock(&pcache->gen_lock);
	}

	if (changed)
		pcache_bm_header_flush(bm);
}

int pcache_bm_flush_sync(struct pcache_bitmap *bm)
{
	u32 page_idx;
	size_t page_bytes = PCACHE_BM_BLOCK_SIZE;
	u64 cache_bytes;

	if (!bm)
		return 0;

	cache_bytes = bm->nr_words * sizeof(unsigned long);

	spin_lock(&bm->lock);
	for_each_set_bit(page_idx, bm->dirty_pages, bm->nr_pages) {
		size_t off = (size_t)page_idx << PCACHE_BM_BLOCK_SHIFT;
		size_t n   = page_bytes;

		if (off >= cache_bytes)
			break;
		if (off + n > cache_bytes)
			n = (size_t)(cache_bytes - off);

		memcpy_flushcache((u8 *)bm->pmem_data + off,
				  (u8 *)bm->cache + off, n);
		/* Zero any tail bytes within the PMEM page that aren't covered
		 * by RAM cache (only matters on the last page). */
		if (n < page_bytes)
			memset_io((u8 *)bm->pmem_data + off + n, 0,
				  page_bytes - n);
	}
	bitmap_zero(bm->dirty_pages, bm->nr_pages);
	spin_unlock(&bm->lock);

	pmem_wmb();
	pcache_bm_header_flush(bm);
	return 0;
}

/*
 * Phase 3.5 — Allocate this peer's bitmap on its registry-assigned PMEM
 * slot. Slot index comes from peer->bm->slot_idx which the caller (Add 4 in
 * pcache_add_peer) sets before invoking us; we read pmem_base from the
 * cache_dev region for that slot.
 *
 * Loads existing on-disk state if present, else creates a fresh bitmap
 * with FULL_SYNC_NEEDED flagged.
 */
int pcache_peer_bitmap_init(struct pcache_peer *peer, u64 backing_sectors,
			    int slot_idx)
{
	struct dm_pcache *pcache = peer->pcache;
	struct pcache_cache_dev *cdev = &pcache->cache_dev;
	struct pcache_bitmap *bm;
	struct pcache_bm_header_onmedia hdr_buf;
	struct pcache_bm_header_onmedia *hdr;
	u64 nr_bits, nr_words, cache_bytes;
	u32 nr_pages;
	size_t cache_alloc;
	int slot = slot_idx;
	bool fresh = false;

	if (!backing_sectors) {
		pcache_dev_err(pcache, "peer_bitmap_init: backing_sectors == 0\n");
		return -EINVAL;
	}
	if (slot < 0 || slot >= PCACHE_BM_MAX_PEERS) {
		pcache_dev_err(pcache, "peer_bitmap_init: slot %d out of range\n", slot);
		return -EINVAL;
	}

	bm = kzalloc(sizeof(*bm), GFP_KERNEL);
	if (!bm)
		return -ENOMEM;
	bm->slot_idx = slot;

	nr_bits     = DIV_ROUND_UP_ULL(backing_sectors, PCACHE_BM_SECTORS_PER_BIT);
	nr_words    = DIV_ROUND_UP_ULL(nr_bits, BITS_PER_LONG);
	cache_bytes = nr_words * sizeof(unsigned long);
	cache_alloc = (size_t)cache_bytes;
	nr_pages    = (u32)DIV_ROUND_UP_ULL(cache_bytes, PCACHE_BM_BLOCK_SIZE);

	if (cache_bytes > PCACHE_BITMAP_SLOT_DATA_SIZE) {
		pcache_dev_err(pcache,
			"bitmap: backing too large (%llu bytes) for slot (%lu bytes)\n",
			cache_bytes, (unsigned long)PCACHE_BITMAP_SLOT_DATA_SIZE);
		kfree(bm);
		return -EFBIG;
	}

	bm->cache = kvmalloc(cache_alloc, GFP_KERNEL | __GFP_ZERO);
	if (!bm->cache)
		goto err_free_bm;

	bm->dirty_pages = kvmalloc(BITS_TO_LONGS(nr_pages) * sizeof(unsigned long),
				    GFP_KERNEL | __GFP_ZERO);
	if (!bm->dirty_pages)
		goto err_free_cache;

	bm->peer      = peer;
	bm->pcache    = pcache;
	bm->nr_bits   = nr_bits;
	bm->nr_words  = nr_words;
	bm->nr_pages  = nr_pages;
	bm->pmem_base = CACHE_DEV_BITMAP_SLOT(cdev, slot);
	bm->pmem_data = (u8 *)bm->pmem_base + PCACHE_BITMAP_SLOT_DATA_OFF;
	spin_lock_init(&bm->lock);
	atomic64_set(&bm->dirty_count, 0);

	hdr = pcache_bm_header_load(bm, &hdr_buf, NULL);
	if (hdr && le64_to_cpu(hdr_buf.magic) == PCACHE_BM_MAGIC &&
	    le64_to_cpu(hdr_buf.bm_nr_bits) == nr_bits &&
	    le32_to_cpu(hdr_buf.bm_block_shift) == PCACHE_BM_BLOCK_SHIFT) {
		u64 cnt, load_gen;

		if (copy_mc_to_kernel(bm->cache, bm->pmem_data, cache_bytes)) {
			pcache_dev_err(pcache,
				"bitmap: hardware memory error reading PMEM\n");
			goto err_free_dirty;
		}
		load_gen             = le64_to_cpu(hdr_buf.generation);
		bm->generation       = load_gen;
		bm->peer_gen_at_sync = le64_to_cpu(hdr_buf.peer_gen_at_sync);
		bm->flags            = le32_to_cpu(hdr_buf.flags);
		bm->meta_seq         = hdr_buf.header.seq;
		cnt = bitmap_weight(bm->cache, nr_bits);
		atomic64_set(&bm->dirty_count, (s64)cnt);

		/* Phase 3.5 Step B2 — RESYNC_IN_PROGRESS sticky on disk means
		 * last session crashed mid-resync. Force a fresh FULL_SYNC
		 * to ensure consistency, then clear the in-progress flag. */
		if (bm->flags & PCACHE_BM_F_RESYNC_IN_PROGRESS) {
			pcache_dev_info(pcache,
				"bitmap[slot=%d node_id=%d]: previous resync was interrupted; forcing FULL_SYNC\n",
				slot, peer->node_id);
			bm->flags |= PCACHE_BM_F_FULL_SYNC_NEEDED;
			bm->flags &= ~PCACHE_BM_F_RESYNC_IN_PROGRESS;
		}

		/* Reconcile device-wide gen across peers: max wins so the
		 * latest seen across any PMEM slot is what we carry. */
		spin_lock(&pcache->gen_lock);
		if (load_gen > pcache->generation)
			pcache->generation = load_gen;
		spin_unlock(&pcache->gen_lock);

		pcache_dev_info(pcache,
			"bitmap[slot=%d node_id=%d]: loaded gen=%llu peer_gen=%llu dirty=%llu/%llu\n",
			slot, peer->node_id, bm->generation,
			bm->peer_gen_at_sync, cnt, nr_bits);
	} else {
		fresh = true;
		spin_lock(&pcache->gen_lock);
		if (pcache->generation == 0)
			pcache->generation = 1;
		bm->generation = pcache->generation;
		spin_unlock(&pcache->gen_lock);
		bm->peer_gen_at_sync = 0;
		bm->flags            = PCACHE_BM_F_FULL_SYNC_NEEDED;
		bm->meta_seq         = 0;
		pcache_dev_info(pcache,
			"bitmap[slot=%d node_id=%d]: fresh — nr_bits=%llu, FULL_SYNC flagged\n",
			slot, peer->node_id, nr_bits);
	}

	peer->bm = bm;

	if (fresh) {
		cache_dev_zero_range(cdev, bm->pmem_data, PCACHE_BITMAP_SLOT_DATA_SIZE);
		pcache_bm_header_flush(bm);
	}
	return 0;

err_free_dirty:
	kvfree(bm->dirty_pages);
err_free_cache:
	kvfree(bm->cache);
err_free_bm:
	kfree(bm);
	return -ENOMEM;
}

void pcache_peer_bitmap_exit(struct pcache_peer *peer)
{
	struct pcache_bitmap *bm;

	if (!peer)
		return;
	bm = peer->bm;
	if (!bm)
		return;

	pcache_bm_flush_sync(bm);

	peer->bm = NULL;
	kvfree(bm->dirty_pages);
	kvfree(bm->cache);
	kfree(bm);
}
