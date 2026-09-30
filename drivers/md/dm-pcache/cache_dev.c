// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/blkdev.h>
#include <linux/dax.h>
#include <linux/log2.h>
#include <linux/vmalloc.h>
#include <linux/parser.h>

#include "cache_dev.h"
#include "backing_dev.h"
#include "cache.h"
#include "dm_pcache.h"

static void cache_dev_dax_exit(struct pcache_cache_dev *cache_dev)
{
	if (cache_dev->use_vmap)
		vunmap(cache_dev->mapping);
}

static int build_vmap(struct dax_device *dax_dev, long total_pages, void **vaddr)
{
	struct page **pages;
	long i = 0, chunk;
	unsigned long pfn;
	int ret;

	pages = vmalloc_array(total_pages, sizeof(struct page *));
	if (!pages)
		return -ENOMEM;

	do {
		chunk = dax_direct_access(dax_dev, i, total_pages - i,
					  DAX_ACCESS, NULL, &pfn);
		if (chunk <= 0) {
			ret = chunk ? chunk : -EINVAL;
			goto out_free;
		}

		if (!pfn_valid(pfn)) {
			ret = -EOPNOTSUPP;
			goto out_free;
		}

		while (chunk-- && i < total_pages) {
			pages[i++] = pfn_to_page(pfn);
			pfn++;
			if (!(i & 15))
				cond_resched();
		}
	} while (i < total_pages);

	*vaddr = vmap(pages, total_pages, VM_MAP, PAGE_KERNEL);
	if (!*vaddr) {
		ret = -ENOMEM;
		goto out_free;
	}

	ret = 0;

out_free:
	vfree(pages);
	return ret;
}

static int cache_dev_dax_init(struct pcache_cache_dev *cache_dev)
{
	struct dm_pcache	*pcache = CACHE_DEV_TO_PCACHE(cache_dev);
	struct dax_device	*dax_dev;
	long			total_pages, mapped_pages;
	u64			bdev_size;
	void			*vaddr;
	int			ret;
	int			id;
	unsigned long		pfn;

	dax_dev	= cache_dev->dm_dev->dax_dev;
	/* total size check */
	bdev_size = bdev_nr_bytes(cache_dev->dm_dev->bdev);
	if (bdev_size < PCACHE_CACHE_DEV_SIZE_MIN) {
		pcache_dev_err(pcache, "dax device is too small, required at least %llu",
				PCACHE_CACHE_DEV_SIZE_MIN);
		ret = -ENOSPC;
		goto out;
	}

	total_pages = bdev_size >> PAGE_SHIFT;
	/* attempt: direct-map the whole range */
	id = dax_read_lock();
	mapped_pages = dax_direct_access(dax_dev, 0, total_pages,
					 DAX_ACCESS, &vaddr, &pfn);
	if (mapped_pages < 0) {
		pcache_dev_err(pcache, "dax_direct_access failed: %ld\n", mapped_pages);
		ret = mapped_pages;
		goto unlock;
	}

	if (!pfn_valid(pfn)) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}

	if (mapped_pages == total_pages) {
		/* success: contiguous direct mapping */
		cache_dev->mapping = vaddr;
	} else {
		/* need vmap fallback */
		ret = build_vmap(dax_dev, total_pages, &vaddr);
		if (ret) {
			pcache_dev_err(pcache, "vmap fallback failed: %d\n", ret);
			goto unlock;
		}

		cache_dev->mapping	= vaddr;
		cache_dev->use_vmap	= true;
	}
	dax_read_unlock(id);

	return 0;
unlock:
	dax_read_unlock(id);
out:
	return ret;
}

int pcache_meta_find_latest(struct pcache_cache_dev *cache_dev,
			    u64 header_off, u32 meta_size,
			    u32 meta_max_size, void *meta_ret)
{
	struct pcache_meta_header *meta = meta_ret;
	u32 i, seq_latest = 0;
	int latest = -1;
	u64 off;

	for (i = 0; i < PCACHE_META_INDEX_MAX; i++) {
		off = header_off + (u64)i * meta_max_size;
		if (cmedium_read(cache_dev, meta, off, meta_size)) {
			pcache_err("hardware memory error when copy meta");
			return -EIO;
		}

		/* Skip if CRC check fails, which means corrupted */
		if (meta->crc != pcache_meta_crc(meta, meta_size))
			continue;

		/* Update latest if a more recent sequence is found */
		if (latest < 0 || pcache_meta_seq_after(meta->seq, seq_latest)) {
			seq_latest = meta->seq;
			latest = i;
		}
	}

	if (latest < 0)
		return -ENOENT;

	off = header_off + (u64)latest * meta_max_size;
	if (cmedium_read(cache_dev, meta_ret, off, meta_size)) {
		pcache_err("hardware memory error");
		return -EIO;
	}

	return latest;
}

static int sb_read(struct pcache_cache_dev *cache_dev, struct pcache_sb *sb)
{
	return cmedium_read(cache_dev, sb, PCACHE_SB_OFF, sizeof(struct pcache_sb));
}

static void sb_write(struct pcache_cache_dev *cache_dev, struct pcache_sb *sb)
{
	cmedium_write(cache_dev, PCACHE_SB_OFF, sb, sizeof(struct pcache_sb));
	cmedium_flush(cache_dev);
}

static int sb_init(struct pcache_cache_dev *cache_dev, struct pcache_sb *sb)
{
	struct dm_pcache *pcache = CACHE_DEV_TO_PCACHE(cache_dev);
	struct block_device *bdev = file_bdev(cache_dev->dm_dev->bdev_file);
	u64 nr_segs;
	u64 cache_dev_size;
	u64 magic;
	u32 flags = 0;
	u32 block_size_sectors;

	magic = le64_to_cpu(sb->magic);
	if (magic)
		return -EEXIST;

	cache_dev_size = bdev_nr_bytes(bdev);
	if (cache_dev_size < PCACHE_CACHE_DEV_SIZE_MIN) {
		pcache_dev_err(pcache, "cache device is too small, required at least %llu",
				PCACHE_CACHE_DEV_SIZE_MIN);
		return -ENOSPC;
	}

	nr_segs = (cache_dev_size - PCACHE_SEGMENTS_OFF) / ((PCACHE_SEG_SIZE));
	block_size_sectors = max_t(u32, bdev_logical_block_size(bdev) >> SECTOR_SHIFT, 1);

#if defined(__BYTE_ORDER) ? (__BIG_ENDIAN == __BYTE_ORDER) : defined(__BIG_ENDIAN)
	flags |= PCACHE_SB_F_BIGENDIAN;
#endif
	sb->flags = cpu_to_le32(flags);
	sb->magic = cpu_to_le64(PCACHE_MAGIC);
	sb->seg_num = cpu_to_le32(nr_segs);
	sb->block_size = cpu_to_le32(block_size_sectors);
	sb->cache_type = cpu_to_le32(cache_dev->backend == PCACHE_CACHE_DEV_PMEM ?
				     PCACHE_CACHE_TYPE_PMEM : PCACHE_CACHE_TYPE_NVME);
	sb->crc = cpu_to_le32(crc32c(PCACHE_CRC_SEED, (void *)(sb) + 4, sizeof(struct pcache_sb) - 4));

	cmedium_zero(cache_dev, PCACHE_CACHE_INFO_OFF,
		     PCACHE_CACHE_INFO_SIZE * PCACHE_META_INDEX_MAX +
		     PCACHE_CACHE_CTRL_SIZE);

	return 0;
}

static int sb_validate(struct pcache_cache_dev *cache_dev, struct pcache_sb *sb)
{
	struct dm_pcache *pcache = CACHE_DEV_TO_PCACHE(cache_dev);
	struct block_device *bdev = file_bdev(cache_dev->dm_dev->bdev_file);
	u32 block_size_sectors;
	u32 bdev_lba;
	u32 cache_type;
	u32 flags;
	u32 crc;

	if (le64_to_cpu(sb->magic) != PCACHE_MAGIC) {
		pcache_dev_err(pcache, "unexpected magic: %llx\n",
				le64_to_cpu(sb->magic));
		return -EINVAL;
	}

	crc = crc32c(PCACHE_CRC_SEED, (void *)(sb) + 4, sizeof(struct pcache_sb) - 4);
	if (crc != le32_to_cpu(sb->crc)) {
		pcache_dev_err(pcache, "corrupted sb: %u, expected: %u\n", crc, le32_to_cpu(sb->crc));
		return -EINVAL;
	}

	flags = le32_to_cpu(sb->flags);
#if defined(__BYTE_ORDER) ? (__BIG_ENDIAN == __BYTE_ORDER) : defined(__BIG_ENDIAN)
	if (!(flags & PCACHE_SB_F_BIGENDIAN)) {
		pcache_dev_err(pcache, "cache_dev is not big endian\n");
		return -EINVAL;
	}
#else
	if (flags & PCACHE_SB_F_BIGENDIAN) {
		pcache_dev_err(pcache, "cache_dev is big endian\n");
		return -EINVAL;
	}
#endif

	cache_type = le32_to_cpu(sb->cache_type);
	switch (cache_type) {
	case PCACHE_CACHE_TYPE_PMEM:
		if (cache_dev->backend != PCACHE_CACHE_DEV_PMEM) {
			pcache_dev_err(pcache, "cache_dev was formatted as PMEM but device lacks DAX support\n");
			return -EINVAL;
		}
		break;
	case PCACHE_CACHE_TYPE_NVME:
		if (cache_dev->backend != PCACHE_CACHE_DEV_NVME) {
			pcache_dev_err(pcache, "cache_dev was formatted as NVMe but device exposes DAX; reformat or use a non-DAX device\n");
			return -EINVAL;
		}

		/*
		 * block_size constraints, NVMe only: the cache medium is
		 * reached through LBA-aligned bios, so the formatted block
		 * size must be a power of two, no smaller than the device's
		 * logical block size, and no larger than a host page. PMEM is
		 * byte-addressable (memcpy/DAX) and has no such constraint, so
		 * block_size carries no meaning there.
		 */
		block_size_sectors = le32_to_cpu(sb->block_size);
		bdev_lba = bdev_logical_block_size(bdev);
		if (!is_power_of_2(block_size_sectors)) {
			pcache_dev_err(pcache, "block_size %u sectors is not a power of two\n",
					block_size_sectors);
			return -EINVAL;
		}
		if (block_size_sectors > PAGE_SECTORS) {
			pcache_dev_err(pcache, "block_size %u sectors exceeds page size %u sectors\n",
					block_size_sectors, (unsigned int)PAGE_SECTORS);
			return -EINVAL;
		}
		if ((u64)block_size_sectors << SECTOR_SHIFT < bdev_lba) {
			pcache_dev_err(pcache, "block_size %u sectors is smaller than cache device LBA %u bytes\n",
					block_size_sectors, bdev_lba);
			return -EINVAL;
		}
		break;
	default:
		pcache_dev_err(pcache, "unknown cache_type: %u\n", cache_type);
		return -EINVAL;
	}

	return 0;
}

static int cache_dev_init(struct pcache_cache_dev *cache_dev, u32 seg_num)
{
	cache_dev->seg_num = seg_num;
	cache_dev->seg_bitmap = kvcalloc(BITS_TO_LONGS(cache_dev->seg_num), sizeof(unsigned long), GFP_KERNEL);
	if (!cache_dev->seg_bitmap)
		return -ENOMEM;

	return 0;
}

static void cache_dev_exit(struct pcache_cache_dev *cache_dev)
{
	kvfree(cache_dev->seg_bitmap);
}

void cache_dev_stop(struct dm_pcache *pcache)
{
	struct pcache_cache_dev *cache_dev = &pcache->cache_dev;

	cache_dev_exit(cache_dev);
	cache_dev_req_ctx_exit(cache_dev);
	cache_dev_dax_exit(cache_dev);
}

int cache_dev_start(struct dm_pcache *pcache)
{
	struct pcache_cache_dev *cache_dev = &pcache->cache_dev;
	struct pcache_sb sb;
	bool format = false;
	u32 seg_num;
	u64 max_segs;
	int ret;

	mutex_init(&cache_dev->seg_lock);

	/*
	 * Auto-detect backend from the device's DAX capability. A
	 * DAX-capable device is registered with the PMEM backend
	 * (byte-addressable mapping); a plain block device uses the NVMe
	 * backend (bio I/O via cache_dev_req). sb_validate later checks
	 * that the on-media cache_type matches this detection.
	 */
	cache_dev->backend = cache_dev->dm_dev->dax_dev ?
				PCACHE_CACHE_DEV_PMEM : PCACHE_CACHE_DEV_NVME;

	if (cache_dev->backend == PCACHE_CACHE_DEV_PMEM) {
		ret = cache_dev_dax_init(cache_dev);
		if (ret) {
			pcache_dev_err(pcache, "failed to init cache_dev %s via dax way: %d.",
				       cache_dev->dm_dev->name, ret);
			goto err;
		}
	}

	ret = cache_dev_req_ctx_init(cache_dev);
	if (ret) {
		pcache_dev_err(pcache, "failed to init cache_dev req ctx: %d", ret);
		goto dax_release;
	}

	ret = sb_read(cache_dev, &sb);
	if (ret)
		goto req_ctx_release;

	if (le64_to_cpu(sb.magic) == 0) {
		format = true;
		ret = sb_init(cache_dev, &sb);
		if (ret < 0)
			goto req_ctx_release;
	}

	ret = sb_validate(cache_dev, &sb);
	if (ret)
		goto req_ctx_release;

	cache_dev->sb_flags = le32_to_cpu(sb.flags);

	/*
	 * seg_num is read from the crc32c-only superblock, so whoever supplies
	 * the cache device controls it. It is the ceiling every later on-media
	 * segment id is validated against, so bound it against what the device
	 * physically holds before it is trusted, or a forged seg_num lets a
	 * segment id address past the DAX mapping.
	 */
	seg_num = le32_to_cpu(sb.seg_num);
	max_segs = (bdev_nr_bytes(cache_dev->dm_dev->bdev) - PCACHE_SEGMENTS_OFF) /
		   PCACHE_SEG_SIZE;
	if (seg_num == 0 || seg_num > max_segs || seg_num > PCACHE_CACHE_SEGS_MAX) {
		pcache_dev_err(pcache, "invalid seg_num %u from cache device (device holds %llu, max %u)\n",
			       seg_num, max_segs, (u32)PCACHE_CACHE_SEGS_MAX);
		ret = -EIO;
		goto dax_release;
	}

	ret = cache_dev_init(cache_dev, seg_num);
	if (ret)
		goto req_ctx_release;

	if (format)
		sb_write(cache_dev, &sb);

	return 0;

req_ctx_release:
	cache_dev_req_ctx_exit(cache_dev);
dax_release:
	cache_dev_dax_exit(cache_dev);
err:
	return ret;
}

int cache_dev_get_empty_segment_id(struct pcache_cache_dev *cache_dev, u32 *seg_id)
{
	int ret;

	mutex_lock(&cache_dev->seg_lock);
	*seg_id = find_next_zero_bit(cache_dev->seg_bitmap, cache_dev->seg_num, 0);
	if (*seg_id == cache_dev->seg_num) {
		ret = -ENOSPC;
		goto unlock;
	}

	__set_bit(*seg_id, cache_dev->seg_bitmap);
	ret = 0;
unlock:
	mutex_unlock(&cache_dev->seg_lock);
	return ret;
}
