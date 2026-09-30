/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _PCACHE_INTERNAL_H
#define _PCACHE_INTERNAL_H

#include <linux/delay.h>
#include <linux/crc32c.h>

#define pcache_err(fmt, ...)							\
	pr_err("dm-pcache: %s:%u " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define pcache_info(fmt, ...)							\
	pr_info("dm-pcache: %s:%u " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define pcache_debug(fmt, ...)							\
	pr_debug("dm-pcache: %s:%u " fmt, __func__, __LINE__, ##__VA_ARGS__)
/* For per-request chatter that is expected in bulk -- see the AHEAD degrade. */
#define pcache_info_ratelimited(fmt, ...)					\
	pr_info_ratelimited("dm-pcache: %s:%u " fmt, __func__, __LINE__, ##__VA_ARGS__)

#define PCACHE_KB			(1024ULL)
#define PCACHE_MB			(1024 * PCACHE_KB)

/* Maximum number of metadata indices */
#define PCACHE_META_INDEX_MAX		2

#define PCACHE_CRC_SEED			0x3B15A
/*
 * struct pcache_meta_header - PCACHE metadata header structure
 * @crc: CRC checksum for validating metadata integrity.
 * @seq: Sequence number to track metadata updates.
 * @version: Metadata version.
 * @res: Reserved space for future use.
 */
struct pcache_meta_header {
	__u32 crc;
	__u8  seq;
	__u8  version;
	__u16 res;
};

/*
 * pcache_meta_crc - Calculate CRC for the given metadata header.
 * @header: Pointer to the metadata header.
 * @meta_size: Size of the metadata structure.
 *
 * Returns the CRC checksum calculated by excluding the CRC field itself.
 */
static inline u32 pcache_meta_crc(struct pcache_meta_header *header, u32 meta_size)
{
	return crc32c(PCACHE_CRC_SEED, (void *)header + 4, meta_size - 4);
}

/*
 * pcache_meta_seq_after - Check if a sequence number is more recent, accounting for overflow.
 * @seq1: First sequence number.
 * @seq2: Second sequence number.
 *
 * Determines if @seq1 is more recent than @seq2 by calculating the signed
 * difference between them. This approach allows handling sequence number
 * overflow correctly because the difference wraps naturally, and any value
 * greater than zero indicates that @seq1 is "after" @seq2. This method
 * assumes 8-bit unsigned sequence numbers, where the difference wraps
 * around if seq1 overflows past seq2.
 *
 * Returns:
 *   - true if @seq1 is more recent than @seq2, indicating it comes "after"
 *   - false otherwise.
 */
static inline bool pcache_meta_seq_after(u8 seq1, u8 seq2)
{
	return (s8)(seq1 - seq2) > 0;
}

#endif /* _PCACHE_INTERNAL_H */
