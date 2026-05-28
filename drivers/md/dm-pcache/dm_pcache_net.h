/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _DM_PCACHE_NET_H
#define _DM_PCACHE_NET_H

#include <linux/kthread.h>
#include <linux/net.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/mempool.h>
#include <linux/slab.h>
#include "cache_dev.h"
#include "dm_pcache_protocol.h"

struct dm_pcache;
struct pcache_request;

/*
 * One in-flight write replication request tracked by the Primary.
 * The pending_req borrows a reference on the original pcache_request, so the
 * bio stays alive until ACK/NACK/timeout completes it.
 */
struct pcache_pending_req {
	struct list_head	list;
	struct pcache_request	*pcache_req;
	u64			sector;
	u32			size;
	u64			block_id;	/* unique id echoed by peer in ACK */
	bool			sent;
	unsigned long		send_time;
};

struct pcache_peer {
	struct dm_pcache	*pcache;
	struct socket		*sock;
	struct socket		*listen_sock;
	enum pcache_conn_state	state;
	enum pcache_repl_state	repl_state;	/* Phase 2 — data sync sub-state */
	struct task_struct	*sender;
	struct task_struct	*receiver;

	/* Protects sock pointer + state during receiver handoff */
	spinlock_t		sock_lock;
	/* Protects pending_list and pending_cnt */
	spinlock_t		pending_lock;
	struct list_head	pending_list;
	atomic_t		pending_cnt;	/* in-flight (queued + sent, not yet acked) */
	atomic_t		unsent_cnt;	/* queued but not yet pushed to socket */
	atomic64_t		next_block_id;
	wait_queue_head_t	wait_for_work;
	wait_queue_head_t	wait_for_listen;
	wait_queue_head_t	wait_for_socket;

	char			remote_addr[64];
	int			node_id;
	int			peer_node_id;
	bool			is_primary;

	/* Phase 2 — peer's reported generation at handshake (captured in SP exchange) */
	u64			peer_generation;
};

struct pcache_network {
	/* Phase 3 — peer slots indexed 0..PCACHE_BM_MAX_PEERS-1.
	 * Step 2: only peers[0] is populated by the legacy dmsetup-table
	 * single-peer path. Step 4 wires dmsetup messages onto the rest
	 * and starts using the registry to pick slot indices. */
	struct pcache_peer	*peers[PCACHE_BM_MAX_PEERS];
	struct mutex		connection_mutex;
	u32			protocol_version;
	atomic_t		stopping;
	struct kmem_cache	*req_cache;
	mempool_t		*req_pool;
};

/*
 * Iterate over every populated peer slot. Safe during ctr/dtr or any path
 * that knows no concurrent peer add/remove can occur (Step 2 baseline).
 * Step 4 introduces an RCU-protected variant for runtime add/remove.
 */
#define for_each_active_peer(net, _peer, _slot)				\
	for ((_slot) = 0; (_slot) < PCACHE_BM_MAX_PEERS; (_slot)++)	\
		if (((_peer) = (net)->peers[_slot]))

/* ACK timeout in seconds */
#define PCACHE_ACK_TIMEOUT_SECS		30
/* Socket buffer sizes (4 MiB each is plenty for PMEM-class RTT) */
#define PCACHE_SOCK_BUF_BYTES		(4 * 1024 * 1024)

/* dm_pcache_net.c */
int pcache_send_all(struct socket *sock, const void *buf, int size);
int pcache_recv_all(struct socket *sock, void *buf, int size);
int pcache_create_peer_socket(struct pcache_peer *peer);
int pcache_create_server_socket(struct pcache_peer *peer, int port);
void pcache_sock_set_timeout(struct socket *sock, unsigned long timeout_jiffies);
void pcache_apply_socket_opts(struct socket *sock);
int pcache_send_packet(struct socket *sock, struct pcache_packet_header *hdr);
int pcache_send_handshake(struct socket *sock, int node_id);
int pcache_recv_handshake(struct socket *sock, int *peer_node_id);
int pcache_send_sync_params(struct socket *sock, u64 generation,
			    u64 peer_gen_at_sync, u64 bm_nr_bits, u32 flags);
int pcache_recv_sync_params(struct socket *sock,
			    struct pcache_sync_params_pkt *out);
int pcache_send_bio_pages(struct socket *sock, struct bio *bio);
void pcache_free_peer(struct pcache_peer *peer);
struct socket *pcache_sock_get(struct pcache_peer *peer);
void pcache_sock_put(struct pcache_peer *peer);

#endif /* _DM_PCACHE_NET_H */
