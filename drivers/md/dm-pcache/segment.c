// SPDX-License-Identifier: GPL-2.0-or-later

#include "pcache_internal.h"
#include "cache_dev.h"
#include "segment.h"

int segment_copy_to_bio(struct pcache_segment *segment,
		u32 data_off, u32 data_len, struct bio *bio, u32 bio_off)
{
	struct pcache_cache_dev *cache_dev = segment->cache_dev;
	struct iov_iter iter;
	u64 off;

	iov_iter_bvec(&iter, ITER_DEST, &bio->bi_io_vec[bio->bi_iter.bi_idx],
			bio_segments(bio), bio->bi_iter.bi_size);
	iter.iov_offset = bio->bi_iter.bi_offset;
	if (bio_off)
		iov_iter_advance(&iter, bio_off);

	off = cmedium_off(cache_dev, segment->data) + data_off;
	return cmedium_read_iter(cache_dev, off, data_len, &iter);
}

int segment_copy_from_bio(struct pcache_segment *segment,
		u32 data_off, u32 data_len, struct bio *bio, u32 bio_off)
{
	struct pcache_cache_dev *cache_dev = segment->cache_dev;
	struct iov_iter iter;
	u64 off;
	int ret;

	iov_iter_bvec(&iter, ITER_SOURCE, &bio->bi_io_vec[bio->bi_iter.bi_idx],
			bio_segments(bio), bio->bi_iter.bi_size);
	iter.iov_offset = bio->bi_iter.bi_offset;
	if (bio_off)
		iov_iter_advance(&iter, bio_off);

	off = cmedium_off(cache_dev, segment->data) + data_off;
	ret = cmedium_write_iter(cache_dev, off, data_len, &iter);
	if (ret)
		return ret;
	cmedium_flush(cache_dev);

	return 0;
}

void pcache_segment_init(struct pcache_cache_dev *cache_dev, struct pcache_segment *segment,
		      struct pcache_segment_init_options *options)
{
	segment->seg_info = options->seg_info;
	segment_info_set_type(segment->seg_info, options->type);

	segment->cache_dev = cache_dev;
	segment->seg_id = options->seg_id;
	segment->data_size = PCACHE_SEG_SIZE - options->data_off;
	segment->data = CACHE_DEV_SEGMENT(cache_dev, options->seg_id) + options->data_off;
}
