/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _DM_PCACHE_REGISTRY_H
#define _DM_PCACHE_REGISTRY_H

#include <linux/mutex.h>
#include <linux/types.h>
#include "cache_dev.h"
#include "pcache_internal.h"

struct dm_pcache;

/* "PCACMPR1\3" — distinguishes Phase 3 registry from prior layouts. */
#define PCACHE_PEER_REG_MAGIC		0x504341434d505233ULL
#define PCACHE_PEER_REG_VERSION		1U

#define PCACHE_PEER_ADDR_MAX		64
#define PCACHE_NODE_ID_INVALID		((u32)~0U)

/* Per-entry flags */
#define PCACHE_PEER_F_IN_USE		BIT(0)
#define PCACHE_PEER_F_TOMBSTONE		BIT(1)

/* Slot index 0..PCACHE_BM_MAX_PEERS-1 also indexes the bitmap region. */
struct pcache_peer_entry_onmedia {
	__le32	node_id;
	__le32	flags;
	__le64	added_generation;
	__le64	reserved[2];
	char	addr[PCACHE_PEER_ADDR_MAX];	/* "ip:port", NUL-terminated */
} __packed;	/* 96 bytes */

struct pcache_peer_registry_onmedia {
	struct pcache_meta_header	header;		/* crc + seq + version */
	__le64	magic;					/* = PCACHE_PEER_REG_MAGIC */
	__le32	nr_entries;				/* = PCACHE_BM_MAX_PEERS */
	__le32	my_node_id;				/* this node's id (INVALID until set) */
	__le64	reserved[4];
	struct pcache_peer_entry_onmedia entries[PCACHE_BM_MAX_PEERS];
} __packed;	/* < 4 KiB */

/* In-memory shadow of a single entry. */
struct pcache_peer_entry {
	u32	node_id;
	u32	flags;
	u64	added_generation;
	char	addr[PCACHE_PEER_ADDR_MAX];
};

struct pcache_peer_registry {
	struct dm_pcache	*pcache;
	void			*pmem_base;	/* CACHE_DEV_PEER_REGISTRY(...) */
	struct mutex		lock;		/* serialise add/remove/flush */
	u32			my_node_id;
	u8			meta_seq;
	u8			meta_index;
	struct pcache_peer_entry entries[PCACHE_BM_MAX_PEERS];
};

int  pcache_registry_init(struct dm_pcache *pcache);
void pcache_registry_exit(struct dm_pcache *pcache);

/* Stamp the local node_id into the registry (and persist) when it
 * differs from the on-disk value. Idempotent. */
int  pcache_registry_set_my_node_id(struct pcache_peer_registry *reg, u32 node_id);

/* Slot management — Step 4 wires dmsetup messages onto these. Step 1
 * provides the storage layer; callers come later. */
int  pcache_registry_add(struct pcache_peer_registry *reg, u32 node_id,
			 const char *addr, u64 generation, int *out_slot);
int  pcache_registry_remove(struct pcache_peer_registry *reg, u32 node_id);
int  pcache_registry_find_slot_by_node_id(struct pcache_peer_registry *reg, u32 node_id);
bool pcache_registry_slot_in_use(struct pcache_peer_registry *reg, int slot);

#endif /* _DM_PCACHE_REGISTRY_H */
