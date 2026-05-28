// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/dax.h>
#include <linux/libnvdimm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "cache_dev.h"
#include "dm_pcache.h"
#include "dm_pcache_registry.h"
#include "pcache_internal.h"

static void pcache_reg_flush_locked(struct pcache_peer_registry *reg);

static bool entry_in_use(const struct pcache_peer_entry *e)
{
	return (e->flags & PCACHE_PEER_F_IN_USE) &&
	       !(e->flags & PCACHE_PEER_F_TOMBSTONE) &&
	       e->node_id != PCACHE_NODE_ID_INVALID;
}

static void zero_entry(struct pcache_peer_entry *e)
{
	memset(e, 0, sizeof(*e));
	e->node_id = PCACHE_NODE_ID_INVALID;
}

/*
 * Persist the current in-memory state to the next (round-robin) PMEM copy.
 * Caller holds reg->lock. The on-media struct is ~3 KiB so we heap-allocate
 * rather than blow the kernel stack.
 */
static void pcache_reg_flush_locked(struct pcache_peer_registry *reg)
{
	struct pcache_peer_registry_onmedia *hdr;
	void *dst;
	u32 i;

	hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
	if (!hdr) {
		pcache_dev_err(reg->pcache,
			"registry: flush oom — skipping persist this round\n");
		return;
	}

	hdr->header.seq     = ++reg->meta_seq;
	hdr->header.version = PCACHE_PEER_REG_VERSION;
	hdr->magic          = cpu_to_le64(PCACHE_PEER_REG_MAGIC);
	hdr->nr_entries     = cpu_to_le32(PCACHE_BM_MAX_PEERS);
	hdr->my_node_id     = cpu_to_le32(reg->my_node_id);

	for (i = 0; i < PCACHE_BM_MAX_PEERS; i++) {
		const struct pcache_peer_entry *src = &reg->entries[i];
		struct pcache_peer_entry_onmedia *e = &hdr->entries[i];

		e->node_id          = cpu_to_le32(src->node_id);
		e->flags            = cpu_to_le32(src->flags);
		e->added_generation = cpu_to_le64(src->added_generation);
		BUILD_BUG_ON(sizeof(e->addr) != sizeof(src->addr));
		memcpy(e->addr, src->addr, sizeof(e->addr));
	}

	hdr->header.crc = pcache_meta_crc(&hdr->header, sizeof(*hdr));

	reg->meta_index = (reg->meta_index + 1) % PCACHE_META_INDEX_MAX;
	dst = (u8 *)reg->pmem_base + reg->meta_index * PCACHE_PEER_REGISTRY_HDR_SIZE;
	memcpy_flushcache(dst, hdr, sizeof(*hdr));
	pmem_wmb();

	kfree(hdr);
}

static int pcache_reg_load(struct pcache_peer_registry *reg)
{
	struct pcache_peer_registry_onmedia *hdr_array = reg->pmem_base;
	struct pcache_peer_registry_onmedia *buf;
	struct pcache_peer_registry_onmedia *latest;
	int ret = 0;
	u32 i;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	latest = pcache_meta_find_latest(&hdr_array->header, sizeof(*buf),
					 PCACHE_PEER_REGISTRY_HDR_SIZE, buf);
	if (IS_ERR(latest)) {
		ret = PTR_ERR(latest);
		goto out;
	}
	if (!latest) {
		ret = -ENOENT;
		goto out;
	}
	if (le64_to_cpu(buf->magic) != PCACHE_PEER_REG_MAGIC) {
		ret = -ENOENT;
		goto out;
	}
	if (le32_to_cpu(buf->nr_entries) != PCACHE_BM_MAX_PEERS) {
		ret = -EINVAL;
		goto out;
	}

	reg->my_node_id = le32_to_cpu(buf->my_node_id);
	reg->meta_seq   = buf->header.seq;
	/* Pick up which copy was latest so subsequent flushes start on the
	 * other copy. pcache_meta_find_latest returns a pointer into the
	 * mapped region; derive the index from byte distance. */
	reg->meta_index = ((u8 *)latest - (u8 *)hdr_array) /
			  PCACHE_PEER_REGISTRY_HDR_SIZE;

	for (i = 0; i < PCACHE_BM_MAX_PEERS; i++) {
		const struct pcache_peer_entry_onmedia *e = &buf->entries[i];
		struct pcache_peer_entry *dst = &reg->entries[i];

		dst->node_id          = le32_to_cpu(e->node_id);
		dst->flags            = le32_to_cpu(e->flags);
		dst->added_generation = le64_to_cpu(e->added_generation);
		memcpy(dst->addr, e->addr, sizeof(dst->addr));
		dst->addr[sizeof(dst->addr) - 1] = '\0';
	}
out:
	kfree(buf);
	return ret;
}

int pcache_registry_init(struct dm_pcache *pcache)
{
	struct pcache_cache_dev *cdev = &pcache->cache_dev;
	struct pcache_peer_registry *reg;
	bool fresh = false;
	u32 i, in_use = 0;
	int ret;

	reg = kzalloc(sizeof(*reg), GFP_KERNEL);
	if (!reg)
		return -ENOMEM;

	reg->pcache    = pcache;
	reg->pmem_base = CACHE_DEV_PEER_REGISTRY(cdev);
	reg->my_node_id = PCACHE_NODE_ID_INVALID;
	mutex_init(&reg->lock);
	for (i = 0; i < PCACHE_BM_MAX_PEERS; i++)
		zero_entry(&reg->entries[i]);

	ret = pcache_reg_load(reg);
	if (ret == -ENOENT) {
		fresh = true;
		ret = 0;
	} else if (ret) {
		pcache_dev_err(pcache,
			"registry: load failed (%d), starting fresh\n", ret);
		for (i = 0; i < PCACHE_BM_MAX_PEERS; i++)
			zero_entry(&reg->entries[i]);
		reg->my_node_id = PCACHE_NODE_ID_INVALID;
		reg->meta_seq = 0;
		reg->meta_index = 0;
		fresh = true;
		ret = 0;
	}

	if (fresh) {
		/* Zero both copies so any residual data from an earlier on-disk
		 * layout (e.g. Phase 2 bitmap region overlapping this offset)
		 * cannot accidentally pass CRC validation on future loads. */
		cache_dev_zero_range(cdev, reg->pmem_base,
				     PCACHE_PEER_REGISTRY_REGION_SIZE);
		mutex_lock(&reg->lock);
		pcache_reg_flush_locked(reg);
		mutex_unlock(&reg->lock);
		pcache_dev_info(pcache,
			"registry: fresh region — capacity=%u\n",
			PCACHE_BM_MAX_PEERS);
	} else {
		for (i = 0; i < PCACHE_BM_MAX_PEERS; i++)
			if (entry_in_use(&reg->entries[i]))
				in_use++;
		pcache_dev_info(pcache,
			"registry: loaded — my_node_id=%u, %u/%u peer slot(s) in use\n",
			reg->my_node_id, in_use, PCACHE_BM_MAX_PEERS);
	}

	pcache->registry = reg;
	return 0;
}

void pcache_registry_exit(struct dm_pcache *pcache)
{
	struct pcache_peer_registry *reg = pcache->registry;

	if (!reg)
		return;

	pcache->registry = NULL;
	mutex_destroy(&reg->lock);
	kfree(reg);
}

int pcache_registry_set_my_node_id(struct pcache_peer_registry *reg, u32 node_id)
{
	if (!reg)
		return -EINVAL;

	mutex_lock(&reg->lock);
	if (reg->my_node_id == node_id) {
		mutex_unlock(&reg->lock);
		return 0;
	}
	reg->my_node_id = node_id;
	pcache_reg_flush_locked(reg);
	mutex_unlock(&reg->lock);
	return 0;
}

int pcache_registry_find_slot_by_node_id(struct pcache_peer_registry *reg, u32 node_id)
{
	int slot = -ENOENT;
	u32 i;

	if (!reg || node_id == PCACHE_NODE_ID_INVALID)
		return -EINVAL;

	mutex_lock(&reg->lock);
	for (i = 0; i < PCACHE_BM_MAX_PEERS; i++) {
		if (entry_in_use(&reg->entries[i]) &&
		    reg->entries[i].node_id == node_id) {
			slot = (int)i;
			break;
		}
	}
	mutex_unlock(&reg->lock);
	return slot;
}

bool pcache_registry_slot_in_use(struct pcache_peer_registry *reg, int slot)
{
	bool ret;

	if (!reg || slot < 0 || slot >= PCACHE_BM_MAX_PEERS)
		return false;

	mutex_lock(&reg->lock);
	ret = entry_in_use(&reg->entries[slot]);
	mutex_unlock(&reg->lock);
	return ret;
}

int pcache_registry_add(struct pcache_peer_registry *reg, u32 node_id,
			const char *addr, u64 generation, int *out_slot)
{
	int slot = -ENOSPC;
	u32 i;

	if (!reg || !addr || node_id == PCACHE_NODE_ID_INVALID)
		return -EINVAL;
	if (strnlen(addr, PCACHE_PEER_ADDR_MAX) >= PCACHE_PEER_ADDR_MAX)
		return -ENAMETOOLONG;

	mutex_lock(&reg->lock);

	/* Reject duplicates */
	for (i = 0; i < PCACHE_BM_MAX_PEERS; i++) {
		if (entry_in_use(&reg->entries[i]) &&
		    reg->entries[i].node_id == node_id) {
			mutex_unlock(&reg->lock);
			return -EEXIST;
		}
	}

	/* Find first free slot. Tombstoned slots are reusable. */
	for (i = 0; i < PCACHE_BM_MAX_PEERS; i++) {
		struct pcache_peer_entry *e = &reg->entries[i];

		if (!(e->flags & PCACHE_PEER_F_IN_USE) ||
		    (e->flags & PCACHE_PEER_F_TOMBSTONE)) {
			zero_entry(e);
			e->node_id          = node_id;
			e->flags            = PCACHE_PEER_F_IN_USE;
			e->added_generation = generation;
			strscpy(e->addr, addr, sizeof(e->addr));
			slot = (int)i;
			break;
		}
	}

	if (slot >= 0)
		pcache_reg_flush_locked(reg);

	mutex_unlock(&reg->lock);

	if (slot >= 0 && out_slot)
		*out_slot = slot;
	return slot < 0 ? slot : 0;
}

int pcache_registry_remove(struct pcache_peer_registry *reg, u32 node_id)
{
	int slot = -ENOENT;
	u32 i;

	if (!reg || node_id == PCACHE_NODE_ID_INVALID)
		return -EINVAL;

	mutex_lock(&reg->lock);
	for (i = 0; i < PCACHE_BM_MAX_PEERS; i++) {
		struct pcache_peer_entry *e = &reg->entries[i];

		if (entry_in_use(e) && e->node_id == node_id) {
			/* Tombstone keeps slot reserved until next add so callers
			 * can detect "this peer was removed" rather than seeing
			 * the slot recycled mid-flight. */
			e->flags |= PCACHE_PEER_F_TOMBSTONE;
			slot = (int)i;
			break;
		}
	}
	if (slot >= 0)
		pcache_reg_flush_locked(reg);
	mutex_unlock(&reg->lock);

	return slot < 0 ? slot : 0;
}
