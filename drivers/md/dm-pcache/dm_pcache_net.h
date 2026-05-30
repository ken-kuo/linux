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
struct pcache_bitmap;

/* Latency instrumentation toggle — canonical definition in dm_pcache.h.
 * Guarded so this header is self-contained even if included first. */
#ifndef PCACHE_LAT_TRACE
#define PCACHE_LAT_TRACE 1
#endif

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
#if PCACHE_LAT_TRACE
	u64			t_enqueue;	/* ns when pending_req created */
	u64			t_send_start;	/* ns before header send */
#endif
};

/*
 * Cut C — one TCP connection (stream) of a peer. streams[0] is the control
 * stream (carries handshake / sync-params / resync as well as its own data
 * shard); streams[1..nr_streams-1] are data-only. Writes are demuxed across
 * streams by hash(sector) so a given region always uses the same stream
 * (preserves per-region ordering). At nr_streams == 1 this reproduces the
 * legacy single-connection behavior byte-for-byte.
 */
#define PCACHE_MAX_STREAMS	4

/*
 * Cut C — refcounted socket holder. A thread that wants to use a stream's
 * socket takes a reference (pcache_conn_get) for the duration of its blocking
 * recv/send; teardown (pcache_conn_drop) detaches the conn from the stream,
 * shuts the socket down to wake blocked users, and drops the install ref. The
 * socket is released only when the last reference is put, so no thread can ever
 * touch a freed socket.
 */
struct pcache_conn {
	struct kref		ref;
	struct socket		*sock;
};

struct pcache_stream {
	struct pcache_peer	*peer;		/* owning peer */
	int			stream_id;	/* 0 = control, 1.. = data-only */
	struct pcache_conn	*conn;		/* current connection (refcounted) */
	struct task_struct	*sender;
	struct task_struct	*receiver;

	/* Protects the conn pointer during install/drop/get */
	spinlock_t		sock_lock;
	/* Protects pending_list and pending_cnt */
	spinlock_t		pending_lock;
	struct list_head	pending_list;
	atomic_t		pending_cnt;	/* in-flight (queued + sent, not yet acked) */
	atomic_t		unsent_cnt;	/* queued but not yet pushed to socket */
	atomic64_t		next_block_id;	/* per-stream; ACK returns on same stream */
	wait_queue_head_t	wait_for_work;
	wait_queue_head_t	wait_for_socket;
};

struct pcache_peer {
	struct dm_pcache	*pcache;
	struct socket		*listen_sock;
	enum pcache_conn_state	state;
	enum pcache_repl_state	repl_state;	/* Phase 2 — data sync sub-state */

	int			nr_streams;
	struct pcache_stream	streams[PCACHE_MAX_STREAMS];

	wait_queue_head_t	wait_for_listen;

	char			remote_addr[64];
	int			node_id;
	int			peer_node_id;
	bool			is_primary;

	/* Phase 2 — peer's reported generation at handshake (captured in SP exchange) */
	u64			peer_generation;

	/* Phase 3 Step 12 — per-pair split-brain marker. Set when
	 * decide_sync_role returns HALT for this pair; cleared by
	 * dmsetup message resolve-split-brain. Just for status visibility. */
	bool			split_brain;

	/* Phase 3.5 — per-peer dirty bitmap (replaces pcache->bitmap).
	 * Tracks regions out of sync with THIS specific peer. Owns one slot
	 * in the PMEM bitmap region (slot_idx = registry slot index). */
	struct pcache_bitmap	*bm;
};

struct pcache_network {
	/* Phase 3 — peer slots indexed 0..PCACHE_BM_MAX_PEERS-1.
	 * Slots are populated by dmsetup message add-peer (Step 4) or by the
	 * legacy dmsetup-table single-peer path (peers[0]). Index in this
	 * array matches the bitmap slot index in the registry. */
	struct pcache_peer	*peers[PCACHE_BM_MAX_PEERS];
	struct mutex		peers_lock;	/* protects add/remove of peers[] */
	struct mutex		connection_mutex;
	u32			protocol_version;
	atomic_t		stopping;
	struct kmem_cache	*req_cache;
	mempool_t		*req_pool;

	/* Phase 3 — one listen socket per network (Step 3): accept_thread
	 * services it and dispatches accepted sockets to the matching peer
	 * slot via handshake.node_id lookup. */
	struct socket		*listen_sock;
	struct task_struct	*accept_thread;
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
int pcache_connect_data_stream(struct pcache_stream *st);
int pcache_create_listen_socket(struct socket **sock_out, int port);
void pcache_sock_set_timeout(struct socket *sock, unsigned long timeout_jiffies);
void pcache_apply_socket_opts(struct socket *sock);
int pcache_send_packet(struct socket *sock, struct pcache_packet_header *hdr);
int pcache_send_handshake(struct socket *sock, int node_id, int stream_id);
int pcache_recv_handshake(struct socket *sock, int *peer_node_id, int *stream_id);
int pcache_send_sync_params(struct socket *sock, u64 generation,
			    u64 peer_gen_at_sync, u64 bm_nr_bits, u32 flags);
int pcache_recv_sync_params(struct socket *sock,
			    struct pcache_sync_params_pkt *out);
int pcache_send_bio_pages(struct socket *sock, struct bio *bio);

/* Phase 3.5 — B1 coordinated demote */
int pcache_send_demote_req(struct socket *sock, u32 initiator_node_id, u32 flags);
int pcache_send_demote_ack(struct socket *sock, u32 status);
int pcache_recv_demote_body(struct socket *sock, struct pcache_demote_pkt *out);
void pcache_free_peer(struct pcache_peer *peer);

/* Cut C — refcounted per-stream socket lifetime. */
struct pcache_conn *pcache_conn_get(struct pcache_stream *st);
void pcache_conn_put(struct pcache_conn *c);
int pcache_conn_install(struct pcache_stream *st, struct socket *sock);
void pcache_conn_drop(struct pcache_stream *st);
void pcache_conn_shutdown(struct pcache_stream *st);
bool pcache_stream_has_conn(struct pcache_stream *st);

#endif /* _DM_PCACHE_NET_H */
