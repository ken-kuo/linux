/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _DM_PCACHE_H
#define _DM_PCACHE_H
#include <linux/device-mapper.h>

#include "../dm-core.h"
#include "cache_dev.h"
#include "backing_dev.h"
#include "cache.h"

#define CACHE_DEV_TO_PCACHE(cache_dev)		(container_of(cache_dev, struct dm_pcache, cache_dev))
#define BACKING_DEV_TO_PCACHE(backing_dev)	(container_of(backing_dev, struct dm_pcache, backing_dev))
#define CACHE_TO_PCACHE(cache)			(container_of(cache, struct dm_pcache, cache))

#define PCACHE_STATE_RUNNING			1
#define PCACHE_STATE_STOPPING			2

struct pcache_network;
struct pcache_peer_registry;

struct dm_pcache {
	struct dm_target *ti;
	struct pcache_cache_dev cache_dev;
	struct pcache_backing_dev backing_dev;
	struct pcache_cache cache;
	struct pcache_cache_options opts;

	spinlock_t			defered_req_list_lock;
	struct list_head		defered_req_list;
	struct workqueue_struct		*task_wq;

	struct work_struct		defered_req_work;

	atomic_t			state;
	atomic_t			inflight_reqs;
	wait_queue_head_t		inflight_wq;

	/* Phase 1 distributed networking */
	struct pcache_network		*net;
	bool				is_primary;
	int				node_id;	/* legacy: kept for compat; use my_node_id */

	/* Phase 3 device-level mesh config (parsed from dmsetup table line):
	 * - my_node_id: this node's id, persisted in registry
	 * - listen_port: TCP port for inbound peer connections (0 = no listen)
	 * - quorum_min: minimum quorum size for write/promote (1 = off; (N/2)+1
	 *   = majority where N=configured peers + self). Default 1 (= off). */
	u32				my_node_id;
	u32				listen_port;
	u32				quorum_min;
	bool				quorum_majority;	/* recompute on add/remove */

	/* Phase 3.5 — device-wide generation (was bm->generation).
	 * Each per-peer bitmap mirrors this for PMEM persistence.
	 * gen_dirty is set on first new dirty bit since last mark_synced
	 * by any peer; reset by any mark_synced. Bump happens only when
	 * gen_dirty transitions false → true. */
	u64				generation;
	bool				gen_dirty;
	spinlock_t			gen_lock;

	/* Phase 3 persistent peer registry (always present once cache_dev is up) */
	struct pcache_peer_registry	*registry;
};

static inline bool pcache_is_stopping(struct dm_pcache *pcache)
{
	return (atomic_read(&pcache->state) == PCACHE_STATE_STOPPING);
}

#define pcache_dev_err(pcache, fmt, ...)							\
	pcache_err("%s " fmt, pcache->ti->table->md->name, ##__VA_ARGS__)
#define pcache_dev_info(pcache, fmt, ...)							\
	pcache_info("%s " fmt, pcache->ti->table->md->name, ##__VA_ARGS__)
#define pcache_dev_debug(pcache, fmt, ...)							\
	pcache_debug("%s " fmt, pcache->ti->table->md->name, ##__VA_ARGS__)

struct pcache_request {
	struct dm_pcache	*pcache;
	struct bio		*bio;

	u64			off;
	u32			data_len;

	struct kref		ref;
	int			ret;

	struct list_head	list_node;
};

void pcache_req_get(struct pcache_request *pcache_req);
void pcache_req_put(struct pcache_request *pcache_req, int ret);

void pcache_defer_reqs_kick(struct dm_pcache *pcache);

/* network */
int pcache_net_init(struct dm_pcache *pcache, unsigned int argc, char **argv);
void pcache_net_stop(struct dm_pcache *pcache);
bool pcache_peer_connected(struct dm_pcache *pcache);
int pcache_net_submit_write(struct dm_pcache *pcache,
			    struct pcache_request *pcache_req,
			    u64 sector, u32 size);
int pcache_net_force_resync(struct dm_pcache *pcache);

/* Phase 3 — runtime peer management (dmsetup message add-peer / remove-peer /
 * list-peers). Caller passes target peer's node_id; for add, also the
 * "ip:port" string. list-peers writes a multi-line summary into @buf. */
int pcache_add_peer(struct dm_pcache *pcache, u32 peer_node_id, const char *addr);
int pcache_remove_peer(struct dm_pcache *pcache, u32 peer_node_id);
int pcache_list_peers(struct dm_pcache *pcache, char *buf, size_t buflen);

/* Phase 3 Step 9 — runtime role switch (dmsetup message set-role ...). */
int pcache_set_role(struct dm_pcache *pcache, bool primary, bool force);

/* Phase 3 Step 10 — quorum check (DRBD-style local check, no election).
 * Returns true if connected_peers + 1 (self) >= quorum_min. Single-host
 * mode (no net) always has quorum. */
bool pcache_has_quorum(struct dm_pcache *pcache);

/* Phase 3.5 — mark a [sector, size) range dirty in every active peer's
 * per-peer bitmap. Used when local write must succeed but replication can't
 * deliver (no connected peer, fan-out enqueue failure). */
void pcache_mark_all_peers_dirty(struct dm_pcache *pcache, u64 sector, u32 size);

/* Phase 3 Step 13 — split-brain recovery & resync control messages. */
int pcache_resolve_split_brain(struct dm_pcache *pcache, u32 peer_node_id,
			       bool victim);
int pcache_abort_resync(struct dm_pcache *pcache);
int pcache_reset_generation(struct dm_pcache *pcache);

#endif /* _DM_PCACHE_H */
