// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/bio.h>
#include <linux/wait.h>
#include <linux/sched/signal.h>
#include <linux/timekeeping.h>
#include <net/tcp.h>
#include "dm_pcache.h"
#include "dm_pcache_bitmap.h"
#include "dm_pcache_net.h"
#include "dm_pcache_registry.h"
#include "cache.h"
#include "pcache_internal.h"

static int pcache_primary_worker(void *arg);
static int pcache_primary_receiver(void *arg);
static int pcache_secondary_worker(void *arg);
static void pcache_flush_pending(struct pcache_peer *peer, int err);

static const char *pcache_repl_state_name(enum pcache_repl_state s)
{
	switch (s) {
	case PCACHE_R_OFF:		return "OFF";
	case PCACHE_R_NEGOTIATING:	return "NEGOTIATING";
	case PCACHE_R_BITMAP_EXCHANGE:	return "BITMAP_EXCHANGE";
	case PCACHE_R_SYNC_SOURCE:	return "SYNC_SOURCE";
	case PCACHE_R_SYNC_TARGET:	return "SYNC_TARGET";
	case PCACHE_R_ESTABLISHED:	return "ESTABLISHED";
	case PCACHE_R_HALT:		return "HALT";
	}
	return "?";
}

static void pcache_set_repl_state(struct pcache_peer *peer,
				  enum pcache_repl_state new_state)
{
	enum pcache_repl_state old = peer->repl_state;

	if (old == new_state)
		return;

	peer->repl_state = new_state;
	pcache_info("%s: repl_state %s -> %s\n",
		    peer->is_primary ? "primary" : "secondary",
		    pcache_repl_state_name(old),
		    pcache_repl_state_name(new_state));
}

/*
 * Phase 3.5 Step A3 — id-based connection direction.
 * Lower node_id initiates the outbound TCP connect; higher node_id waits
 * for the accept_thread to dispatch the inbound socket. Symmetric across
 * the pair, so each connection is opened exactly once regardless of which
 * side is Primary.
 */
static inline bool pcache_peer_is_initiator(struct pcache_peer *peer)
{
	return peer->pcache->my_node_id < (u32)peer->node_id;
}

/*
 * Phase 3 Step 6 — bilateral bitmap exchange helpers.
 *
 * After sync_params + role decision but before any P_RS_DATA flows, each
 * side dumps its dirty bitmap to the peer. The peer ORs the received bits
 * into its local bitmap so the SYNC_SOURCE drain in do_resync covers
 * regions either side marked out-of-sync.
 *
 * Wire encoding: PCACHE_BM_ENC_RUN — one P_BITMAP packet per contiguous
 * set-bits run with header-only (no payload). End-of-stream marker is a
 * P_BITMAP_DONE header (length=0), reused from the resync protocol.
 */
#define PCACHE_BM_EXCHANGE_MAX_RUN_BITS  (1U << 20)	/* up to 1M bits / packet */

static int pcache_send_bitmap_run(struct socket *sock,
				  u64 start_bit, u32 nr_bits)
{
	struct pcache_packet_header hdr = {
		.type	= cpu_to_le16(PCACHE_P_BITMAP),
		.length	= cpu_to_le16(PCACHE_BM_PKT_SIZE),
	};
	struct pcache_bitmap_pkt bp = {
		.start_bit = cpu_to_le64(start_bit),
		.nr_bits   = cpu_to_le32(nr_bits),
		.encoding  = cpu_to_le32(PCACHE_BM_ENC_RUN),
	};
	u8 buf[sizeof(hdr) + sizeof(bp)];

	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &bp, sizeof(bp));
	return pcache_send_all(sock, buf, sizeof(buf));
}

static int pcache_send_bitmap_full(struct pcache_peer *peer, struct socket *sock)
{
	struct pcache_bitmap *bm = peer->bm;
	struct pcache_packet_header done = {
		.type   = cpu_to_le16(PCACHE_P_BITMAP_DONE),
		.length = 0,
	};
	u64 cursor = 0;
	u32 nr_bits;
	int ret, runs_sent = 0;

	if (bm) {
		while (pcache_bm_find_next_dirty(bm, &cursor, &nr_bits,
						 PCACHE_BM_EXCHANGE_MAX_RUN_BITS)) {
			ret = pcache_send_bitmap_run(sock, cursor, nr_bits);
			if (ret)
				return ret;
			runs_sent++;
			cursor += nr_bits;
		}
	}
	ret = pcache_send_packet(sock, &done);
	if (ret)
		return ret;

	pcache_info("bitmap-exchange: sent %d run(s)\n", runs_sent);
	return 0;
}

static int pcache_recv_bitmap_merge(struct pcache_peer *peer, struct socket *sock)
{
	struct pcache_bitmap *bm = peer->bm;
	struct pcache_packet_header hdr;
	struct pcache_bitmap_pkt bp;
	int ret, runs_recv = 0;

	while (1) {
		ret = pcache_recv_all(sock, &hdr, sizeof(hdr));
		if (ret)
			return ret;

		if (le16_to_cpu(hdr.type) == PCACHE_P_BITMAP_DONE)
			break;
		if (le16_to_cpu(hdr.type) != PCACHE_P_BITMAP)
			return -EPROTO;

		ret = pcache_recv_all(sock, &bp, sizeof(bp));
		if (ret)
			return ret;

		if (le32_to_cpu(bp.encoding) != PCACHE_BM_ENC_RUN)
			return -EPROTONOSUPPORT;

		if (bm) {
			u64 start = le64_to_cpu(bp.start_bit);
			u32 nbits = le32_to_cpu(bp.nr_bits);
			u64 sector = start * PCACHE_BM_SECTORS_PER_BIT;
			u32 size = nbits * PCACHE_BM_BLOCK_SIZE;

			pcache_bm_set_range(bm, sector, size);
		}
		runs_recv++;
	}

	pcache_info("bitmap-exchange: received %d run(s)\n", runs_recv);
	return 0;
}

/* Symmetric send-then-recv (initiator) / recv-then-send (non-initiator). */
static int pcache_exchange_bitmaps(struct pcache_peer *peer,
				   struct socket *sock, bool initiator)
{
	int ret;

	if (initiator) {
		ret = pcache_send_bitmap_full(peer, sock);
		if (ret)
			return ret;
		return pcache_recv_bitmap_merge(peer, sock);
	}
	ret = pcache_recv_bitmap_merge(peer, sock);
	if (ret)
		return ret;
	return pcache_send_bitmap_full(peer, sock);
}

/*
 * Phase 2 — decide which role this node plays after exchanging sync_params.
 * Only consults generation counters and the NEED_FULL flag; bitmap content
 * comparison is left to Step 7 (P_BITMAP exchange).
 */
static enum pcache_repl_state
pcache_decide_sync_role(struct pcache_peer *peer,
			const struct pcache_sync_params_pkt *peer_sp)
{
	struct pcache_bitmap *bm = peer->bm;
	u64 my_gen, my_peer_gen, peer_gen, peer_my_gen;
	u32 my_flags, peer_flags;
	bool primary = peer->is_primary;
	bool my_need, peer_need;

	if (!bm)
		return PCACHE_R_ESTABLISHED;

	/* Geometry check first — incompatible setups halt immediately. */
	if (le64_to_cpu(peer_sp->bm_nr_bits) != bm->nr_bits ||
	    le32_to_cpu(peer_sp->bm_block_shift) != PCACHE_BM_BLOCK_SHIFT) {
		pcache_err("sync_params: geometry mismatch (my %llu bits @ shift %u, peer %llu bits @ shift %u)\n",
			   bm->nr_bits, PCACHE_BM_BLOCK_SHIFT,
			   le64_to_cpu(peer_sp->bm_nr_bits),
			   le32_to_cpu(peer_sp->bm_block_shift));
		return PCACHE_R_HALT;
	}

	my_gen      = bm->generation;
	my_peer_gen = bm->peer_gen_at_sync;
	peer_gen    = le64_to_cpu(peer_sp->generation);
	peer_my_gen = le64_to_cpu(peer_sp->peer_gen_at_sync);
	my_flags    = bm->flags;
	peer_flags  = le32_to_cpu(peer_sp->flags);
	my_need     = !!(my_flags   & PCACHE_BM_F_FULL_SYNC_NEEDED);
	peer_need   = !!(peer_flags & PCACHE_SP_F_NEED_FULL);

	pcache_info("sync_params: my(gen=%llu,peer_at=%llu,full=%d) peer(gen=%llu,peer_at=%llu,full=%d) primary=%d\n",
		    my_gen, my_peer_gen, my_need,
		    peer_gen, peer_my_gen, peer_need, primary);

	/* Fresh boot — Primary owns the initial full sync. */
	if (my_need || peer_need)
		return primary ? PCACHE_R_SYNC_SOURCE : PCACHE_R_SYNC_TARGET;

	/* Clean reconnect: both gens line up reciprocally. */
	if (my_gen == peer_gen &&
	    my_peer_gen == peer_gen && peer_my_gen == my_gen)
		return PCACHE_R_ESTABLISHED;

	/* I am newer: peer's view of itself == my recorded snapshot of it. */
	if (my_gen > peer_gen && peer_gen == my_peer_gen)
		return PCACHE_R_SYNC_SOURCE;

	/* Peer is newer: my view of myself == peer's recorded snapshot of me. */
	if (peer_gen > my_gen && my_gen == peer_my_gen)
		return PCACHE_R_SYNC_TARGET;

	/* Same gen but reciprocity broken, or unrelated chains — split-brain. */
	pcache_err("sync_params: incompatible generations, halting replication\n");
	return PCACHE_R_HALT;
}

/*
 * Exchange P_SYNC_PARAMS in two flavors. Initiator (Primary) sends first;
 * Secondary recvs then sends. After this call returns success, the caller
 * has filled *peer_sp with the peer's view.
 */
static int pcache_exchange_sync_params(struct pcache_peer *peer,
				       struct socket *sock,
				       struct pcache_sync_params_pkt *peer_sp,
				       bool initiator)
{
	struct pcache_bitmap *bm = peer->bm;
	u64 my_gen = 0, my_peer_gen = 0, my_nr_bits = 0;
	u32 my_flags = 0;
	int ret;

	if (bm) {
		my_gen      = bm->generation;
		my_peer_gen = bm->peer_gen_at_sync;
		my_nr_bits  = bm->nr_bits;
		if (bm->flags & PCACHE_BM_F_FULL_SYNC_NEEDED)
			my_flags |= PCACHE_SP_F_NEED_FULL;
	}

	if (initiator) {
		ret = pcache_send_sync_params(sock, my_gen, my_peer_gen,
					      my_nr_bits, my_flags);
		if (ret)
			return ret;
		ret = pcache_recv_sync_params(sock, peer_sp);
		if (ret)
			return ret;
	} else {
		ret = pcache_recv_sync_params(sock, peer_sp);
		if (ret)
			return ret;
		ret = pcache_send_sync_params(sock, my_gen, my_peer_gen,
					      my_nr_bits, my_flags);
		if (ret)
			return ret;
	}
	return 0;
}

static void pcache_pending_req_free(struct pcache_peer *peer,
				    struct pcache_pending_req *req)
{
	if (peer->pcache->net && peer->pcache->net->req_pool)
		mempool_free(req, peer->pcache->net->req_pool);
	else
		kfree(req);
}

/*
 * Primary: an ACK/NACK has been received from the peer. Find the matching
 * pending request by block_id, drop it from the pending list, complete the
 * underlying pcache_request (which triggers bio_endio and inflight_reqs--).
 */
static void pcache_handle_ack(struct pcache_peer *peer,
			      struct pcache_packet_header *hdr,
			      bool success)
{
	struct pcache_pending_req *req, *found = NULL;
	u64 block_id = le64_to_cpu(hdr->block_id);

	spin_lock(&peer->streams[0].pending_lock);
	list_for_each_entry(req, &peer->streams[0].pending_list, list) {
		if (req->block_id == block_id) {
			list_del(&req->list);
			atomic_dec(&peer->streams[0].pending_cnt);
			found = req;
			break;
		}
	}
	spin_unlock(&peer->streams[0].pending_lock);

	if (!found) {
		pcache_err("%s for unknown block_id=%llu sector=%llu\n",
			   success ? "ACK" : "NACK", block_id,
			   le64_to_cpu(hdr->sector));
		return;
	}

	/* Phase 2 Step 6: NACK ⇒ peer rejected the write, mark dirty so the
	 * next resync covers it. (ACK path: bitmap should already be clean
	 * for this region; nothing to do.) */
	if (!success)
		pcache_bm_set_range(peer->bm,
				    found->sector, found->size);

#if PCACHE_LAT_TRACE
	/* Decompose the primary-side per-IO latency. All fields read here are
	 * written before the data send (t_map/t_enqueue/t_send_start), so they
	 * are stable by the time this ACK arrives — no race with the sender.
	 * sendack folds in send-push + wire RTT + secondary commit + ack RTT;
	 * subtract the secondary trace's total to isolate the wire/sched cost. */
	if (success && found->t_send_start && found->pcache_req) {
		u64 t_ack = ktime_get_ns();
		u64 t_map = found->pcache_req->t_map;

		trace_printk("P blk=%llu sz=%u local=%llu qwait=%llu sendack=%llu total=%llu\n",
			     found->block_id, found->size,
			     found->t_enqueue - t_map,
			     found->t_send_start - found->t_enqueue,
			     t_ack - found->t_send_start,
			     t_ack - t_map);
	}
#endif

	pcache_req_put(found->pcache_req, success ? 0 : -EIO);
	pcache_pending_req_free(peer, found);
}

/*
 * Phase 2 Step 8 — Secondary handler for P_RS_DATA (resync chunk).
 * Symmetric to pcache_handle_data_recv but parses the rs_data_pkt body,
 * persists the payload via the cache layer, and replies with P_RS_ACK.
 */
static int pcache_handle_rs_data_recv(struct pcache_peer *peer,
				      struct pcache_packet_header *hdr)
{
	struct dm_pcache *pcache = peer->pcache;
	struct pcache_rs_data_pkt rs;
	u64 sector;
	u32 size;
	void *data;
	int ret;

	ret = pcache_recv_all(peer->streams[0].sock, &rs, sizeof(rs));
	if (ret)
		return ret;

	sector = le64_to_cpu(hdr->sector);
	size   = le32_to_cpu(rs.size);
	if (!size)
		return 0;

	data = kvmalloc(size, GFP_NOIO);
	if (!data)
		return -ENOMEM;

	ret = pcache_recv_all(peer->streams[0].sock, data, size);
	if (ret)
		goto free_data;

	ret = pcache_net_write_to_cache(&pcache->cache, sector, data, size);
	if (ret)
		pcache_err("secondary: RS_DATA write_to_cache failed: %d\n", ret);

	/* Step 6 fix — clear the dirty bits we just brought into sync. Without
	 * this, bits OR'd in during bilateral bitmap exchange (Step 6) would
	 * stay set after resync, making sync_params at the next handshake see
	 * Secondary as having unsynced data and decide HALT on gen mismatch. */
	if (!ret && peer->bm)
		pcache_bm_clear_range(peer->bm, sector, size);

	{
		struct pcache_packet_header ack;

		memset(&ack, 0, sizeof(ack));
		ack.type     = cpu_to_le16(PCACHE_P_RS_ACK);
		ack.sector   = hdr->sector;
		ack.block_id = hdr->block_id;
		ack.size     = rs.size;
		ret = pcache_send_packet(peer->streams[0].sock, &ack);
		if (ret)
			pcache_err("secondary: RS_ACK send failed: %d\n", ret);
	}

free_data:
	kvfree(data);
	return ret;
}

/*
 * Secondary: pull payload bytes off the socket and persist them via the
 * cache layer, then reply with an ACK or NACK.
 */
static int pcache_handle_data_recv(struct pcache_peer *peer,
				   struct pcache_packet_header *hdr)
{
	struct dm_pcache *pcache = peer->pcache;
	u64 sector = le64_to_cpu(hdr->sector);
	u32 size = le32_to_cpu(hdr->size);
	u32 flags = le32_to_cpu(hdr->flags);
	struct pcache_packet_header reply;
	void *data;
	int ret;
#if PCACHE_LAT_TRACE
	u64 t0, t_data, t_commit;
#endif

	if (!size)
		return 0;

#if PCACHE_LAT_TRACE
	t0 = ktime_get_ns();		/* entry; recv segment includes kvmalloc */
#endif
	data = kvmalloc(size, GFP_NOIO);
	if (!data)
		return -ENOMEM;

	ret = pcache_recv_all(peer->streams[0].sock, data, size);
	if (ret)
		goto free_data;
#if PCACHE_LAT_TRACE
	t_data = ktime_get_ns();
#endif

	ret = pcache_net_write_to_cache(&pcache->cache, sector, data, size);
	if (ret)
		pcache_err("write to cache failed: %d (sector=%llu size=%u)\n",
			   ret, sector, size);
#if PCACHE_LAT_TRACE
	t_commit = ktime_get_ns();
#endif

	if (flags & PCACHE_F_ACK) {
		memset(&reply, 0, sizeof(reply));
		reply.type = cpu_to_le16(ret ? PCACHE_P_NACK : PCACHE_P_ACK);
		reply.sector = hdr->sector;
		reply.block_id = hdr->block_id;
		reply.size = hdr->size;
		ret = pcache_send_packet(peer->streams[0].sock, &reply);
		if (ret)
			pcache_err("secondary: ACK/NACK send failed: %d\n", ret);
	}

#if PCACHE_LAT_TRACE
	{
		u64 t_ack = ktime_get_ns();

		trace_printk("S blk=%llu sz=%u recv=%llu commit=%llu acksend=%llu total=%llu\n",
			     le64_to_cpu(hdr->block_id), size,
			     t_data - t0, t_commit - t_data,
			     t_ack - t_commit, t_ack - t0);
	}
#endif

free_data:
	kvfree(data);
	return ret;
}

/*
 * Secondary worker thread.
 *
 * Phase 3.5 Step A3 — direction-aware socket acquisition:
 *   - If we are the initiator (my_node_id < peer.node_id), open the
 *     outbound TCP connection and do the handshake ourselves.
 *   - Otherwise, wait for accept_thread to install peer->streams[0].sock (handshake
 *     already done there).
 * After that the role-based recv loop runs unchanged.
 */
static int pcache_secondary_worker(void *arg)
{
	struct pcache_peer *peer = arg;
	struct socket *sock = NULL;
	struct pcache_packet_header hdr;
	bool initiator = pcache_peer_is_initiator(peer);
	int ret;

	allow_signal(SIGTERM);

wait_sock:
	if (sock) {
		sock_release(sock);
		sock = NULL;
	}

	if (initiator) {
		/* outbound connect retry loop */
		while (!kthread_should_stop()) {
			ret = pcache_create_peer_socket(peer);
			if (!ret)
				break;
			pcache_err("secondary[initiator]: connect failed: %d\n", ret);
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(3 * HZ);
		}
		if (kthread_should_stop())
			return 0;
		sock = peer->streams[0].sock;

		ret = pcache_send_handshake(sock, (int)peer->pcache->my_node_id, 0);
		if (ret) {
			pcache_err("secondary[initiator]: handshake send failed: %d\n", ret);
			spin_lock(&peer->streams[0].sock_lock);
			peer->streams[0].sock = NULL;
			peer->state = PCACHE_C_CONNECTING;
			spin_unlock(&peer->streams[0].sock_lock);
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
			goto wait_sock;
		}
		ret = pcache_recv_handshake(sock, NULL, NULL);
		if (ret) {
			pcache_err("secondary[initiator]: handshake recv failed: %d\n", ret);
			spin_lock(&peer->streams[0].sock_lock);
			peer->streams[0].sock = NULL;
			peer->state = PCACHE_C_CONNECTING;
			spin_unlock(&peer->streams[0].sock_lock);
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
			goto wait_sock;
		}
	} else {
		wait_event_interruptible(peer->streams[0].wait_for_socket,
			(peer->streams[0].sock && peer->state == PCACHE_C_CONNECTED) ||
			kthread_should_stop());
		if (kthread_should_stop())
			return 0;

		spin_lock(&peer->streams[0].sock_lock);
		sock = peer->streams[0].sock;
		spin_unlock(&peer->streams[0].sock_lock);
		if (!sock)
			goto wait_sock;
	}

	pcache_info("secondary: handshake complete (%s), entering recv loop\n",
		    initiator ? "initiator" : "responder");

	/* Phase 2 Step 5+8: exchange P_SYNC_PARAMS, compute role.
	 * SYNC_TARGET stays in the recv loop and waits for P_RS_DATA followed
	 * by P_BITMAP_DONE; on receipt of DONE we mark_synced & ESTABLISHED. */
	pcache_set_repl_state(peer, PCACHE_R_NEGOTIATING);
	{
		struct pcache_sync_params_pkt peer_sp = {0};
		enum pcache_repl_state role;
		int sp_ret = pcache_exchange_sync_params(peer, sock, &peer_sp,
							 initiator);

		if (sp_ret) {
			pcache_err("secondary: sync_params exchange failed: %d\n",
				   sp_ret);
			peer->state = PCACHE_C_CONNECTING;
			pcache_set_repl_state(peer, PCACHE_R_OFF);
			spin_lock(&peer->streams[0].sock_lock);
			peer->streams[0].sock = NULL;
			spin_unlock(&peer->streams[0].sock_lock);
			sock_release(sock);
			sock = NULL;
			goto wait_sock;
		}
		peer->peer_generation = le64_to_cpu(peer_sp.generation);
		role = pcache_decide_sync_role(peer, &peer_sp);
		pcache_info("secondary: decided role = %s\n",
			    pcache_repl_state_name(role));

		/* Phase 3.5 Step A3 — bilateral bitmap exchange using id-based
		 * initiator (was hardcoded false in Phase 3). */
		if (role == PCACHE_R_SYNC_SOURCE ||
		    role == PCACHE_R_SYNC_TARGET) {
			int ex_ret;

			pcache_set_repl_state(peer, PCACHE_R_BITMAP_EXCHANGE);
			ex_ret = pcache_exchange_bitmaps(peer, sock, initiator);
			if (ex_ret) {
				pcache_err("secondary: bitmap-exchange failed: %d\n",
					   ex_ret);
				peer->state = PCACHE_C_CONNECTING;
				pcache_set_repl_state(peer, PCACHE_R_OFF);
				spin_lock(&peer->streams[0].sock_lock);
				peer->streams[0].sock = NULL;
				spin_unlock(&peer->streams[0].sock_lock);
				sock_release(sock);
				sock = NULL;
				goto wait_sock;
			}
		}

		if (role == PCACHE_R_HALT) {
			/* Phase 3 Step 12 — split-brain: stay in HALT until
			 * operator runs dmsetup message resolve-split-brain.
			 * Do NOT fall through to the recv loop (which would
			 * accept P_DATA from a peer we've declared diverged). */
			peer->split_brain = true;
			pcache_set_repl_state(peer, PCACHE_R_HALT);
			pcache_dev_err(peer->pcache,
				"split-brain detected for peer node_id=%d; halted (run dmsetup message resolve-split-brain to recover)\n",
				peer->node_id);
			while (!kthread_should_stop()) {
				set_current_state(TASK_INTERRUPTIBLE);
				schedule_timeout(HZ);
			}
			if (sock) {
				spin_lock(&peer->streams[0].sock_lock);
				peer->streams[0].sock = NULL;
				spin_unlock(&peer->streams[0].sock_lock);
				sock_release(sock);
			}
			return 0;
		} else if (role == PCACHE_R_SYNC_TARGET) {
			pcache_set_repl_state(peer, PCACHE_R_SYNC_TARGET);
			/* Recv loop below handles P_RS_DATA + P_BITMAP_DONE. */
		} else {
			pcache_bm_mark_synced(peer->bm,
					      peer->peer_generation);
			pcache_set_repl_state(peer, PCACHE_R_ESTABLISHED);
		}
	}

	while (!kthread_should_stop()) {
		ret = pcache_recv_all(sock, &hdr, sizeof(hdr));
		if (ret == -EAGAIN || ret == -EINTR) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ / 10);
			__set_current_state(TASK_RUNNING);
			continue;
		}
		if (ret) {
			pcache_err("secondary: connection lost: %d\n", ret);
			peer->state = PCACHE_C_CONNECTING;
			pcache_set_repl_state(peer, PCACHE_R_OFF);
			spin_lock(&peer->streams[0].sock_lock);
			peer->streams[0].sock = NULL;
			spin_unlock(&peer->streams[0].sock_lock);
			sock_release(sock);
			sock = NULL;
			goto wait_sock;
		}

		switch (le16_to_cpu(hdr.type)) {
		case PCACHE_P_HANDSHAKE:
			/* re-handshake on an active stream is unexpected */
			break;
		case PCACHE_P_DATA:
			ret = pcache_handle_data_recv(peer, &hdr);
			if (ret)
				pcache_err("secondary: data recv failed: %d\n", ret);
			break;
		case PCACHE_P_RS_DATA:
			ret = pcache_handle_rs_data_recv(peer, &hdr);
			if (ret)
				pcache_err("secondary: RS_DATA recv failed: %d\n", ret);
			break;
		case PCACHE_P_BITMAP_DONE:
			/* Phase 2 Step 8: Primary finished sending resync.
			 * Mark synced and transition to ESTABLISHED. */
			pcache_bm_mark_synced(peer->bm,
					      peer->peer_generation);
			pcache_set_repl_state(peer, PCACHE_R_ESTABLISHED);
			break;
		case PCACHE_P_DEMOTE_REQ: {
			/* Phase 3.5 B1 — peer is about to promote; we should
			 * demote (already Secondary here, so no-op state-wise)
			 * and ACK so the promoter can proceed. */
			struct pcache_demote_pkt body;
			int rc = pcache_recv_demote_body(sock, &body);

			if (rc) {
				pcache_err("secondary: DEMOTE_REQ body recv: %d\n", rc);
				break;
			}
			pcache_dev_info(peer->pcache,
				"DEMOTE_REQ from node_id=%u acknowledged\n",
				le32_to_cpu(body.initiator_node_id));
			pcache_send_demote_ack(sock, 0);
			break;
		}
		case PCACHE_P_DEMOTE_ACK: {
			struct pcache_demote_pkt body;
			int rc = pcache_recv_demote_body(sock, &body);

			if (rc)
				break;
			pcache_dev_info(peer->pcache,
				"DEMOTE_ACK status=%u\n",
				le32_to_cpu(body.status));
			break;
		}
		case PCACHE_P_ACK:
		case PCACHE_P_NACK:
			/* Phase 1: secondary never sends DATA, so ACK/NACK
			 * arriving here is a protocol violation. Log and skip. */
			pcache_err("secondary: unexpected ACK/NACK\n");
			break;
		default:
			pcache_err("secondary: unknown packet type: %u\n",
				   le16_to_cpu(hdr.type));
			break;
		}
	}

	if (sock) {
		spin_lock(&peer->streams[0].sock_lock);
		peer->streams[0].sock = NULL;
		spin_unlock(&peer->streams[0].sock_lock);
		sock_release(sock);
	}
	return 0;
}

/*
 * Primary: dedicated ACK receiver kthread. Reads packet headers off the
 * connected socket and routes ACK/NACK to pcache_handle_ack().
 */
static int pcache_primary_receiver(void *arg)
{
	struct pcache_peer *peer = arg;
	struct pcache_packet_header hdr;
	int ret;

	allow_signal(SIGTERM);

	while (!kthread_should_stop()) {
		struct socket *sock = pcache_sock_get(peer);

		if (!sock || peer->state != PCACHE_C_CONNECTED) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ / 10);
			__set_current_state(TASK_RUNNING);
			continue;
		}

		ret = pcache_recv_all(sock, &hdr, sizeof(hdr));
		if (ret) {
			if (ret != -EAGAIN && ret != -EINTR) {
				pcache_err("primary recv: ACK recv failed: %d\n", ret);
				peer->state = PCACHE_C_CONNECTING;
				pcache_set_repl_state(peer, PCACHE_R_OFF);
				/*
				 * Link is dead — complete every in-flight write
				 * locally (bio_endio with success) and let the
				 * bitmap drive the resync once we reconnect.
				 * Without this, the bios sit in pending_list
				 * indefinitely because the timeout reaping only
				 * runs from inside the main send loop, which we
				 * left as soon as the sender saw state change.
				 */
				pcache_flush_pending(peer, 0);
				wake_up(&peer->streams[0].wait_for_work);
			}
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ / 10);
			__set_current_state(TASK_RUNNING);
			continue;
		}

		switch (le16_to_cpu(hdr.type)) {
		case PCACHE_P_ACK:
			pcache_handle_ack(peer, &hdr, true);
			break;
		case PCACHE_P_NACK:
			pcache_handle_ack(peer, &hdr, false);
			break;
		case PCACHE_P_DEMOTE_REQ: {
			/* Phase 3.5 B1 — peer wants to promote; we (currently
			 * Primary) ACK + flag for operator visibility. Actual
			 * demote left to operator (split-brain detect will
			 * catch divergence if it stays both-primary). */
			struct pcache_demote_pkt body;
			int rc = pcache_recv_demote_body(sock, &body);

			if (rc) {
				pcache_err("primary recv: DEMOTE_REQ body: %d\n", rc);
				break;
			}
			pcache_dev_info(peer->pcache,
				"DEMOTE_REQ from node_id=%u (we are Primary); ACKing — operator should set-role secondary\n",
				le32_to_cpu(body.initiator_node_id));
			pcache_send_demote_ack(sock, 0);
			break;
		}
		case PCACHE_P_DEMOTE_ACK: {
			struct pcache_demote_pkt body;
			int rc = pcache_recv_demote_body(sock, &body);

			if (rc)
				break;
			pcache_dev_info(peer->pcache,
				"DEMOTE_ACK status=%u\n",
				le32_to_cpu(body.status));
			break;
		}
		default:
			pcache_err("primary recv: unexpected packet type: %u\n",
				   le16_to_cpu(hdr.type));
			break;
		}
	}
	return 0;
}

/*
 * Phase 2 Step 8 — Primary SYNC_SOURCE resync.
 * Walks the dirty bitmap chunk by chunk, reads each dirty range from the
 * backing device, sends it as P_RS_DATA, waits synchronously for P_RS_ACK,
 * then clears the bit. Terminates with a P_BITMAP_DONE marker.
 *
 * Runs in primary_worker context BEFORE the primary_receiver kthread is
 * spawned, so we own all socket reads/writes for the duration.
 */
#define PCACHE_RS_BITS_PER_CHUNK	64u	/* 64 bits × 4 KiB = 256 KiB */
#define PCACHE_RS_CHUNK_SIZE		(PCACHE_RS_BITS_PER_CHUNK * PCACHE_BM_BLOCK_SIZE)
#define PCACHE_RS_CHUNK_PAGES		(PCACHE_RS_CHUNK_SIZE / PAGE_SIZE)

static int pcache_send_pages_inline(struct socket *sock, struct page **pages,
				    u32 nr_pages, u32 total_size)
{
	u32 off = 0;
	u32 i;
	int ret;

	for (i = 0; i < nr_pages && off < total_size; i++) {
		u32 page_left = min_t(u32, PAGE_SIZE, total_size - off);
		u32 page_off  = 0;

		while (page_left > 0) {
			struct msghdr msg = {
				.msg_flags = MSG_NOSIGNAL | MSG_SPLICE_PAGES,
			};
			struct bio_vec bvec;

			bvec_set_page(&bvec, pages[i], page_left, page_off);
			iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1,
				      page_left);
			ret = sock_sendmsg(sock, &msg);
			if (ret <= 0)
				return ret ? ret : -ECONNRESET;
			page_left -= ret;
			page_off  += ret;
			off       += ret;
		}
	}
	return 0;
}

static int pcache_do_resync(struct pcache_peer *peer, struct socket *sock,
			    u64 peer_gen)
{
	struct dm_pcache *pcache = peer->pcache;
	struct pcache_bitmap *bm = peer->bm;

	/* Phase 3.5 Step B2 — mark RESYNC_IN_PROGRESS so a crash mid-resync
	 * is detectable at next bitmap_init load (which forces FULL_SYNC). */
	if (bm) {
		spin_lock(&bm->lock);
		bm->flags |= PCACHE_BM_F_RESYNC_IN_PROGRESS;
		spin_unlock(&bm->lock);
		pcache_bm_flush_sync(bm);
	}
	struct block_device *backing_bdev;
	struct page **pages = NULL;
	u64 cursor = 0;
	u32 nr_bits;
	int ret = 0;
	u32 i;

	if (!bm)
		return 0;

	backing_bdev = pcache->backing_dev.dm_dev->bdev;

	/* Fresh-start full sync — mark every bit dirty so the loop covers the
	 * entire device on the very first connect. */
	if (bm->flags & PCACHE_BM_F_FULL_SYNC_NEEDED) {
		pcache_info("primary: full-sync needed, marking all %llu bits\n",
			    bm->nr_bits);
		pcache_bm_set_all(bm);
	}

	/* Flush PMEM cache → backing so we can read the latest data from the
	 * backing device during resync. */
	ret = pcache_cache_flush(&pcache->cache);
	if (ret) {
		pcache_err("primary: cache_flush failed before resync: %d\n", ret);
		return ret;
	}

	pages = kcalloc(PCACHE_RS_CHUNK_PAGES, sizeof(*pages), GFP_NOIO);
	if (!pages)
		return -ENOMEM;
	for (i = 0; i < PCACHE_RS_CHUNK_PAGES; i++) {
		pages[i] = alloc_page(GFP_NOIO);
		if (!pages[i]) {
			ret = -ENOMEM;
			goto out_free;
		}
	}

	pcache_info("primary: resync starting (dirty=%llu / %llu bits)\n",
		    pcache_bm_dirty_count(bm), bm->nr_bits);

	while (!kthread_should_stop() &&
	       pcache_bm_find_next_dirty(bm, &cursor, &nr_bits,
					 PCACHE_RS_BITS_PER_CHUNK)) {
		u64 sector = cursor * PCACHE_BM_SECTORS_PER_BIT;
		u32 size   = nr_bits * PCACHE_BM_BLOCK_SIZE;
		u32 nr_pages = DIV_ROUND_UP(size, PAGE_SIZE);
		u64 block_id;
		struct bio *rbio;
		struct pcache_packet_header rh, ah;
		struct pcache_rs_data_pkt rs;
		u8 buf[sizeof(rh) + sizeof(rs)];

		/* 1. Read this dirty range from backing into our page array. */
		rbio = bio_alloc(backing_bdev, nr_pages, REQ_OP_READ, GFP_NOIO);
		if (!rbio) {
			ret = -ENOMEM;
			goto out_free;
		}
		rbio->bi_iter.bi_sector = sector;
		for (i = 0; i < nr_pages; i++) {
			u32 page_len = min_t(u32, PAGE_SIZE,
					     size - i * PAGE_SIZE);
			if (!bio_add_page(rbio, pages[i], page_len, 0)) {
				bio_put(rbio);
				pcache_err("primary: bio_add_page failed\n");
				ret = -EIO;
				goto out_free;
			}
		}
		ret = submit_bio_wait(rbio);
		bio_put(rbio);
		if (ret) {
			pcache_err("primary: resync read failed sector=%llu size=%u ret=%d\n",
				   sector, size, ret);
			goto out_free;
		}

		/* 2. Send P_RS_DATA header + payload. */
		block_id = (u64)atomic64_inc_return(&peer->streams[0].next_block_id);
		memset(&rh, 0, sizeof(rh));
		rh.type     = cpu_to_le16(PCACHE_P_RS_DATA);
		rh.length   = cpu_to_le16(sizeof(rs));
		rh.sector   = cpu_to_le64(sector);
		rh.block_id = cpu_to_le64(block_id);
		rh.size     = cpu_to_le32(size);
		rs.start_bit = cpu_to_le64(cursor);
		rs.nr_bits   = cpu_to_le32(nr_bits);
		rs.size      = cpu_to_le32(size);
		rs.block_id  = cpu_to_le64(block_id);
		memcpy(buf, &rh, sizeof(rh));
		memcpy(buf + sizeof(rh), &rs, sizeof(rs));
		ret = pcache_send_all(sock, buf, sizeof(buf));
		if (ret)
			goto out_free;

		ret = pcache_send_pages_inline(sock, pages, nr_pages, size);
		if (ret)
			goto out_free;

		/* 3. Wait for P_RS_ACK. */
		ret = pcache_recv_all(sock, &ah, sizeof(ah));
		if (ret)
			goto out_free;
		if (le16_to_cpu(ah.type) != PCACHE_P_RS_ACK ||
		    le64_to_cpu(ah.block_id) != block_id) {
			pcache_err("primary: bad RS_ACK type=%u block_id=%llu expected=%llu\n",
				   le16_to_cpu(ah.type),
				   le64_to_cpu(ah.block_id), block_id);
			ret = -EPROTO;
			goto out_free;
		}

		/* 4. Clear bitmap range. */
		pcache_bm_clear_range(bm, sector, size);
		cursor += nr_bits;
	}

	if (!kthread_should_stop()) {
		struct pcache_packet_header done = {
			.type   = cpu_to_le16(PCACHE_P_BITMAP_DONE),
			.length = 0,
		};
		ret = pcache_send_packet(sock, &done);
		if (ret)
			goto out_free;

		pcache_bm_mark_synced(bm, peer_gen);
		pcache_info("primary: resync done (dirty=%llu)\n",
			    pcache_bm_dirty_count(bm));
	}

out_free:
	if (pages) {
		for (i = 0; i < PCACHE_RS_CHUNK_PAGES; i++) {
			if (pages[i])
				__free_page(pages[i]);
		}
		kfree(pages);
	}
	return ret;
}

/*
 * Drop all in-flight pending requests, mark their sector range as out-of-sync
 * (Phase 2 Step 6 — so they get re-sent on the next resync), and complete
 * the user bios with @err.
 */
static void pcache_flush_pending(struct pcache_peer *peer, int err)
{
	struct pcache_pending_req *req, *tmp;
	LIST_HEAD(drain);

	spin_lock(&peer->streams[0].pending_lock);
	list_splice_init(&peer->streams[0].pending_list, &drain);
	atomic_set(&peer->streams[0].pending_cnt, 0);
	atomic_set(&peer->streams[0].unsent_cnt, 0);
	spin_unlock(&peer->streams[0].pending_lock);

	list_for_each_entry_safe(req, tmp, &drain, list) {
		list_del(&req->list);
		pcache_bm_set_range(peer->bm,
				    req->sector, req->size);
		pcache_req_put(req->pcache_req, err);
		pcache_pending_req_free(peer, req);
	}
}

/*
 * Primary worker thread.
 *
 * Phase 3.5 Step A3 — direction-aware socket acquisition (was always
 * outbound-connect in Phase 3):
 *   - If initiator (my_node_id < peer.node_id): connect + handshake.
 *   - Else: wait for accept_thread to install the socket; handshake done.
 */
static int pcache_primary_worker(void *arg)
{
	struct pcache_peer *peer = arg;
	struct socket *sock = NULL;
	bool initiator = pcache_peer_is_initiator(peer);
	int ret;

	allow_signal(SIGTERM);

connect:
	while (!kthread_should_stop()) {
		pcache_set_repl_state(peer, PCACHE_R_OFF);

		if (peer->streams[0].receiver) {
			if (sock)
				kernel_sock_shutdown(sock, SHUT_RDWR);
			kthread_stop(peer->streams[0].receiver);
			peer->streams[0].receiver = NULL;
		}
		if (sock) {
			sock_release(sock);
			sock = NULL;
			spin_lock(&peer->streams[0].sock_lock);
			peer->streams[0].sock = NULL;
			spin_unlock(&peer->streams[0].sock_lock);
		}

		if (initiator) {
			ret = pcache_create_peer_socket(peer);
			if (ret) {
				pcache_err("primary[initiator]: connect failed: %d\n", ret);
				set_current_state(TASK_INTERRUPTIBLE);
				schedule_timeout(3 * HZ);
				continue;
			}
			sock = peer->streams[0].sock;

			ret = pcache_send_handshake(sock, (int)peer->pcache->my_node_id, 0);
			if (ret) {
				pcache_err("primary[initiator]: HANDSHAKE send failed: %d\n", ret);
				sock_release(sock); sock = NULL;
				spin_lock(&peer->streams[0].sock_lock);
				peer->streams[0].sock = NULL;
				peer->state = PCACHE_C_CONNECTING;
				spin_unlock(&peer->streams[0].sock_lock);
				set_current_state(TASK_INTERRUPTIBLE);
				schedule_timeout(HZ);
				continue;
			}

			ret = pcache_recv_handshake(sock, NULL, NULL);
			if (ret) {
				pcache_err("primary[initiator]: HANDSHAKE reply failed: %d\n", ret);
				sock_release(sock); sock = NULL;
				spin_lock(&peer->streams[0].sock_lock);
				peer->streams[0].sock = NULL;
				peer->state = PCACHE_C_CONNECTING;
				spin_unlock(&peer->streams[0].sock_lock);
				set_current_state(TASK_INTERRUPTIBLE);
				schedule_timeout(HZ);
				continue;
			}
		} else {
			/* Wait for accept_thread to deliver socket (it already
			 * performed the handshake exchange). */
			wait_event_interruptible(peer->streams[0].wait_for_socket,
				(peer->streams[0].sock && peer->state == PCACHE_C_CONNECTED) ||
				kthread_should_stop());
			if (kthread_should_stop())
				goto exit;

			spin_lock(&peer->streams[0].sock_lock);
			sock = peer->streams[0].sock;
			spin_unlock(&peer->streams[0].sock_lock);
			if (!sock) {
				set_current_state(TASK_INTERRUPTIBLE);
				schedule_timeout(HZ / 4);
				continue;
			}
		}

		pcache_info("primary: handshake complete (%s)\n",
			    initiator ? "initiator" : "responder");

		/* Phase 2 Step 5: exchange P_SYNC_PARAMS BEFORE spawning the
		 * receiver, so the SP reply doesn't race with ACK reads.
		 * Step 8: if role == SYNC_SOURCE, drive a synchronous resync
		 * over the same socket before entering ESTABLISHED. */
		pcache_set_repl_state(peer, PCACHE_R_NEGOTIATING);
		{
			struct pcache_sync_params_pkt peer_sp = {0};
			enum pcache_repl_state role;
			int sp_ret;
			u64 peer_gen;

			sp_ret = pcache_exchange_sync_params(peer, sock,
							     &peer_sp, initiator);
			if (sp_ret) {
				pcache_err("primary: sync_params exchange failed: %d\n",
					   sp_ret);
				sock_release(sock);
				sock = NULL;
				spin_lock(&peer->streams[0].sock_lock);
				peer->streams[0].sock = NULL;
				peer->state = PCACHE_C_CONNECTING;
				spin_unlock(&peer->streams[0].sock_lock);
				pcache_set_repl_state(peer, PCACHE_R_OFF);
				set_current_state(TASK_INTERRUPTIBLE);
				schedule_timeout(HZ);
				continue;
			}
			peer_gen = le64_to_cpu(peer_sp.generation);
			peer->peer_generation = peer_gen;
			role = pcache_decide_sync_role(peer, &peer_sp);
			pcache_info("primary: decided role = %s\n",
				    pcache_repl_state_name(role));
			if (role == PCACHE_R_HALT) {
				/* Phase 3 Step 12 — split-brain marker on this
				 * pair. Operator recovers via dmsetup message
				 * resolve-split-brain. */
				peer->split_brain = true;
				pcache_set_repl_state(peer, PCACHE_R_HALT);
				pcache_dev_err(peer->pcache,
					"split-brain detected for peer node_id=%d; halted\n",
					peer->node_id);
				while (!kthread_should_stop()) {
					set_current_state(TASK_INTERRUPTIBLE);
					schedule_timeout(HZ);
				}
				goto exit;
			}
			/* Phase 3 Step 6 — bilateral bitmap exchange BEFORE
			 * any resync data flows. Both sides OR the peer's
			 * dirty bitmap into their local one so the SOURCE
			 * drain covers regions either side marked dirty. */
			if (role == PCACHE_R_SYNC_SOURCE ||
			    role == PCACHE_R_SYNC_TARGET) {
				pcache_set_repl_state(peer,
						      PCACHE_R_BITMAP_EXCHANGE);
				sp_ret = pcache_exchange_bitmaps(peer, sock,
								 initiator);
				if (sp_ret) {
					pcache_err("primary: bitmap-exchange failed: %d\n",
						   sp_ret);
					sock_release(sock);
					sock = NULL;
					spin_lock(&peer->streams[0].sock_lock);
					peer->streams[0].sock = NULL;
					peer->state = PCACHE_C_CONNECTING;
					spin_unlock(&peer->streams[0].sock_lock);
					pcache_set_repl_state(peer, PCACHE_R_OFF);
					set_current_state(TASK_INTERRUPTIBLE);
					schedule_timeout(HZ);
					continue;
				}
			}

			if (role == PCACHE_R_SYNC_SOURCE) {
				pcache_set_repl_state(peer, PCACHE_R_SYNC_SOURCE);
				sp_ret = pcache_do_resync(peer, sock, peer_gen);
				if (sp_ret) {
					pcache_err("primary: resync failed: %d, reconnecting\n",
						   sp_ret);
					sock_release(sock);
					sock = NULL;
					spin_lock(&peer->streams[0].sock_lock);
					peer->streams[0].sock = NULL;
					peer->state = PCACHE_C_CONNECTING;
					spin_unlock(&peer->streams[0].sock_lock);
					pcache_set_repl_state(peer, PCACHE_R_OFF);
					set_current_state(TASK_INTERRUPTIBLE);
					schedule_timeout(HZ);
					continue;
				}
			} else {
				/* Clean reconnect — peer says we're already in sync. */
				pcache_bm_mark_synced(peer->bm,
						      peer_gen);
			}
			pcache_set_repl_state(peer, PCACHE_R_ESTABLISHED);
		}

		peer->streams[0].receiver = kthread_run(pcache_primary_receiver, peer,
				"pcache-recv-%s",
				peer->pcache->ti->table->md->name);
		if (IS_ERR(peer->streams[0].receiver)) {
			pcache_err("primary: failed to spawn receiver: %ld\n",
				   PTR_ERR(peer->streams[0].receiver));
			peer->streams[0].receiver = NULL;
			continue;
		}

		/* Re-mark unsent any requests that were in flight before the
		 * (re)connect so the new receiver can ACK them. */
		{
			struct pcache_pending_req *p;

			spin_lock(&peer->streams[0].pending_lock);
			list_for_each_entry(p, &peer->streams[0].pending_list, list) {
				if (p->sent) {
					p->sent = false;
					atomic_inc(&peer->streams[0].unsent_cnt);
				}
			}
			spin_unlock(&peer->streams[0].pending_lock);
		}
		break;
	}

	if (kthread_should_stop()) {
		if (sock)
			sock_release(sock);
		return 0;
	}

	while (!kthread_should_stop()) {
		struct pcache_pending_req *req, *tmp;
		bool found_unsent;
		bool any_timed_out = false;

		/* Reap timed-out requests. */
		spin_lock(&peer->streams[0].pending_lock);
		if (atomic_read(&peer->streams[0].pending_cnt) > 0) {
			unsigned long deadline = jiffies -
				(PCACHE_ACK_TIMEOUT_SECS * HZ);

			list_for_each_entry_safe(req, tmp, &peer->streams[0].pending_list, list) {
				if (!req->sent)
					continue;
				if (time_after_eq(req->send_time, deadline))
					continue;
				pcache_err("primary: request timeout sector=%llu size=%u\n",
					   req->sector, req->size);
				list_del(&req->list);
				atomic_dec(&peer->streams[0].pending_cnt);
				/* Step 6: mark dirty before completing so next
				 * resync covers this range. */
				pcache_bm_set_range(peer->bm,
						    req->sector, req->size);
				pcache_req_put(req->pcache_req, -ETIMEDOUT);
				pcache_pending_req_free(peer, req);
				any_timed_out = true;
			}
		}
		spin_unlock(&peer->streams[0].pending_lock);

		/*
		 * If any in-flight req timed out the link is presumed dead
		 * even though TCP hasn't declared so. Shut down the socket
		 * here — that wakes the receiver with -ECONNRESET, which
		 * flips state to CONNECTING and sends us back to connect:.
		 */
		if (any_timed_out) {
			struct socket *bad = pcache_sock_get(peer);
			pcache_err("primary: ACK timeout — forcing reconnect\n");
			if (bad)
				kernel_sock_shutdown(bad, SHUT_RDWR);
		}

		/* Send all currently-unsent requests. */
		do {
			struct pcache_packet_header phdr;

			found_unsent = false;
			spin_lock(&peer->streams[0].pending_lock);
			list_for_each_entry(req, &peer->streams[0].pending_list, list) {
				if (!req->sent) {
					req->sent = true;
					req->send_time = jiffies;
					atomic_dec(&peer->streams[0].unsent_cnt);
					found_unsent = true;
					break;
				}
			}
			spin_unlock(&peer->streams[0].pending_lock);

			if (!found_unsent)
				break;

			sock = pcache_sock_get(peer);
			if (!sock || peer->state != PCACHE_C_CONNECTED)
				goto connect;

			memset(&phdr, 0, sizeof(phdr));
			phdr.type	= cpu_to_le16(PCACHE_P_DATA);
			phdr.sector	= cpu_to_le64(req->sector);
			phdr.block_id	= cpu_to_le64(req->block_id);
			phdr.size	= cpu_to_le32(req->size);
			phdr.flags	= cpu_to_le32(PCACHE_F_ACK);

#if PCACHE_LAT_TRACE
			req->t_send_start = ktime_get_ns();
#endif
			ret = pcache_send_packet(sock, &phdr);
			if (ret) {
				pcache_err("primary: send header failed: %d\n", ret);
				goto connect;
			}

			if (req->pcache_req && req->pcache_req->bio) {
				ret = pcache_send_bio_pages(sock,
					req->pcache_req->bio);
				if (ret) {
					pcache_err("primary: send data failed: %d\n", ret);
					goto connect;
				}
			}
		} while (!kthread_should_stop());

		/*
		 * Wait for new work OR a state change OR stop OR a 5 s tick.
		 * The periodic tick is essential: when every queued req has
		 * already been sent (unsent_cnt == 0) and we're waiting for
		 * ACKs that may never arrive (silent network failure where TCP
		 * hasn't yet declared the link dead), nothing else would wake
		 * us — so the 30 s ACK-timeout reaping at the top of this loop
		 * would never run. The 5 s heartbeat bounds detection latency.
		 */
		wait_event_interruptible_timeout(peer->streams[0].wait_for_work,
			atomic_read(&peer->streams[0].unsent_cnt) > 0 ||
			peer->state != PCACHE_C_CONNECTED ||
			kthread_should_stop(),
			5 * HZ);

		if (peer->state != PCACHE_C_CONNECTED)
			goto connect;
	}

exit:
	if (peer->streams[0].receiver) {
		if (sock)
			kernel_sock_shutdown(sock, SHUT_RDWR);
		kthread_stop(peer->streams[0].receiver);
		peer->streams[0].receiver = NULL;
	}
	if (sock) {
		sock_release(sock);
		spin_lock(&peer->streams[0].sock_lock);
		peer->streams[0].sock = NULL;
		spin_unlock(&peer->streams[0].sock_lock);
	}
	pcache_flush_pending(peer, -EIO);
	return 0;
}

static int pcache_start_secondary(struct pcache_peer *peer)
{
	struct task_struct *t;

	t = kthread_run(pcache_secondary_worker, peer, "pcache-secondary-%s",
			peer->pcache->ti->table->md->name);
	if (IS_ERR(t))
		return PTR_ERR(t);
	peer->streams[0].receiver = t;
	return 0;
}

static int pcache_start_primary(struct pcache_peer *peer)
{
	struct task_struct *t;

	t = kthread_run(pcache_primary_worker, peer, "pcache-primary-%s",
			peer->pcache->ti->table->md->name);
	if (IS_ERR(t))
		return PTR_ERR(t);
	peer->streams[0].sender = t;
	return 0;
}

/*
 * Phase 3 Step 3+4 — device-level args parser.
 *
 * Recognises:
 *   my_node_id=N    -- this node's id (also accepted as legacy node_id=)
 *   listen_port=P   -- TCP port to accept inbound peer connections on
 *   is_primary=B    -- data-role hint (0 or 1); peers added later inherit this
 *
 * Per-peer args (peer_addr / per-peer node_id) are no longer accepted in the
 * dmsetup table line — peers are exclusively added via the runtime
 * `dmsetup message <dev> add-peer <node_id> <ip:port>` message (Step 4).
 */
static int pcache_parse_dev_args(struct dm_pcache *pcache,
				 unsigned int argc, char **argv,
				 char **error)
{
	int my_node_id = -1;
	int listen_port = 0;
	int is_primary = -1;
	unsigned int i;

	for (i = 0; i < argc; i++) {
		char *arg = argv[i];
		char *val = strchr(arg, '=');

		if (!val)
			continue;
		*val++ = '\0';

		if (!strcmp(arg, "my_node_id") || !strcmp(arg, "node_id")) {
			if (kstrtoint(val, 10, &my_node_id) < 0 ||
			    my_node_id < 0) {
				*error = "invalid my_node_id";
				return -EINVAL;
			}
		} else if (!strcmp(arg, "is_primary")) {
			if (kstrtoint(val, 10, &is_primary) < 0 ||
			    is_primary < 0 || is_primary > 1) {
				*error = "invalid is_primary (0 or 1)";
				return -EINVAL;
			}
		} else if (!strcmp(arg, "listen_port")) {
			if (kstrtoint(val, 10, &listen_port) < 0 ||
			    listen_port <= 0 || listen_port > 65535) {
				*error = "invalid listen_port";
				return -EINVAL;
			}
		} else if (!strcmp(arg, "quorum")) {
			if (!strcmp(val, "off")) {
				pcache->quorum_min = 1;
				pcache->quorum_majority = false;
			} else if (!strcmp(val, "majority")) {
				pcache->quorum_majority = true;
				/* quorum_min recomputed dynamically: (active+1)/2 + 1 */
			} else {
				unsigned int q;

				if (kstrtouint(val, 10, &q) < 0 || q < 1) {
					*error = "invalid quorum (off|majority|N>=1)";
					return -EINVAL;
				}
				pcache->quorum_min = q;
				pcache->quorum_majority = false;
			}
		} else if (!strcmp(arg, "streams")) {
			int n;

			if (kstrtoint(val, 10, &n) < 0 ||
			    n < 1 || n > PCACHE_MAX_STREAMS) {
				*error = "invalid streams (1..PCACHE_MAX_STREAMS)";
				return -EINVAL;
			}
			pcache->nr_streams = n;
		} else if (!strcmp(arg, "peer_addr")) {
			pcache_dev_info(pcache,
				"peer_addr= in table line is deprecated; use dmsetup message add-peer\n");
		}
	}

	if (my_node_id < 0 || is_primary < 0)
		return 0;	/* not distributed mode */

	pcache->my_node_id = (u32)my_node_id;
	pcache->listen_port = (u32)listen_port;
	pcache->is_primary = !!is_primary;
	pcache->node_id = my_node_id;	/* legacy mirror */

	/* Default quorum to "off" (= 1) unless explicitly set above. */
	if (pcache->quorum_min == 0)
		pcache->quorum_min = 1;
	return 1;	/* distributed-mode args present */
}

bool pcache_has_quorum(struct dm_pcache *pcache)
{
	struct pcache_network *net = pcache->net;
	struct pcache_peer *peer;
	int slot, total, connected, threshold;

	if (!net)
		return true;	/* standalone mode */

	mutex_lock(&net->peers_lock);
	total = 1;	/* self */
	connected = 1;	/* self */
	for_each_active_peer(net, peer, slot) {
		total++;
		if (peer->state == PCACHE_C_CONNECTED)
			connected++;
	}
	mutex_unlock(&net->peers_lock);

	threshold = pcache->quorum_majority ? (total / 2) + 1
					    : (int)pcache->quorum_min;
	return connected >= threshold;
}

/*
 * Phase 3 Step 3 — accept thread.
 * Single per-network kthread that owns the listen socket. Accepts inbound
 * peer connections, completes handshake-recv + handshake-send, looks up the
 * matching peer slot by handshake.node_id, and hands off the connected
 * socket to that peer's worker via wait_for_socket.
 */
static int pcache_accept_thread(void *arg)
{
	struct dm_pcache *pcache = arg;
	struct pcache_network *net = pcache->net;
	struct socket *sock;
	struct pcache_peer *peer;
	int ret, peer_node_id, slot, stream_id = 0;

	allow_signal(SIGTERM);

	while (!kthread_should_stop() && !atomic_read(&net->stopping)) {
		sock = NULL;
		ret = kernel_accept(net->listen_sock, &sock, 0);
		if (ret < 0) {
			if (ret == -EAGAIN || ret == -EINTR)
				continue;
			pcache_dev_err(pcache, "accept: %d\n", ret);
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
			continue;
		}

		pcache_apply_socket_opts(sock);

		peer_node_id = -1;
		ret = pcache_recv_handshake(sock, &peer_node_id, &stream_id);
		if (ret) {
			pcache_dev_err(pcache,
				"accept: handshake recv failed: %d\n", ret);
			sock_release(sock);
			continue;
		}

		ret = pcache_send_handshake(sock, (int)pcache->my_node_id, stream_id);
		if (ret) {
			pcache_dev_err(pcache,
				"accept: handshake reply failed: %d\n", ret);
			sock_release(sock);
			continue;
		}

		mutex_lock(&net->peers_lock);
		peer = NULL;
		for_each_active_peer(net, peer, slot) {
			if (peer->node_id == peer_node_id)
				break;
			peer = NULL;
		}
		if (!peer) {
			mutex_unlock(&net->peers_lock);
			pcache_dev_err(pcache,
				"accept: no peer slot for node_id=%d; drop\n",
				peer_node_id);
			sock_release(sock);
			continue;
		}
		if (stream_id < 0 || stream_id >= peer->nr_streams) {
			mutex_unlock(&net->peers_lock);
			pcache_dev_err(pcache,
				"accept: bad stream_id=%d (nr_streams=%d) node_id=%d; drop\n",
				stream_id, peer->nr_streams, peer_node_id);
			sock_release(sock);
			continue;
		}

		/*
		 * Cut C — route the accepted socket to its stream slot. stream 0
		 * is the control stream and owns peer->state; data streams
		 * (stream_id > 0) only attach their socket and wake their own
		 * worker. At nr_streams == 1 this is the legacy path.
		 */
		spin_lock(&peer->streams[stream_id].sock_lock);
		if (peer->streams[stream_id].sock) {
			/* Race-loser: lower-id outbound got there first. */
			spin_unlock(&peer->streams[stream_id].sock_lock);
			mutex_unlock(&net->peers_lock);
			pcache_dev_info(pcache,
				"accept: peer node_id=%d stream=%d already connected; drop dup\n",
				peer_node_id, stream_id);
			sock_release(sock);
			continue;
		}
		peer->streams[stream_id].sock = sock;
		if (stream_id == 0)
			peer->state = PCACHE_C_CONNECTED;
		spin_unlock(&peer->streams[stream_id].sock_lock);
		mutex_unlock(&net->peers_lock);

		pcache_sock_set_timeout(sock, 300 * HZ);
		wake_up(&peer->streams[stream_id].wait_for_socket);
		pcache_dev_info(pcache,
			"accept: handed inbound sock to slot=%d node_id=%d stream=%d\n",
			slot, peer_node_id, stream_id);
	}
	return 0;
}

/* Cut C — initialise one stream slot. */
static void pcache_stream_init(struct pcache_stream *st,
			       struct pcache_peer *peer, int stream_id)
{
	st->peer	= peer;
	st->stream_id	= stream_id;
	st->sock	= NULL;
	st->sender	= NULL;
	st->receiver	= NULL;
	atomic_set(&st->pending_cnt, 0);
	atomic_set(&st->unsent_cnt, 0);
	atomic64_set(&st->next_block_id, 1);
	INIT_LIST_HEAD(&st->pending_list);
	init_waitqueue_head(&st->wait_for_work);
	init_waitqueue_head(&st->wait_for_socket);
	spin_lock_init(&st->sock_lock);
	spin_lock_init(&st->pending_lock);
}

/*
 * Phase 3 Step 4 — allocate and initialise a peer struct.
 * Caller still needs to install it at net->peers[slot] and start its worker.
 */
static struct pcache_peer *
pcache_alloc_peer(struct dm_pcache *pcache, u32 peer_node_id, const char *addr)
{
	struct pcache_peer *peer;
	int i;

	peer = kzalloc(sizeof(*peer), GFP_KERNEL);
	if (!peer)
		return NULL;

	peer->pcache	  = pcache;
	peer->node_id	  = (int)peer_node_id;
	peer->is_primary  = pcache->is_primary;
	peer->state	  = PCACHE_C_STANDALONE;
	peer->repl_state  = PCACHE_R_OFF;
	peer->split_brain = false;
	strscpy(peer->remote_addr, addr, sizeof(peer->remote_addr));

	/* Cut C — N parallel streams (default 1 = legacy single connection). */
	peer->nr_streams = pcache->nr_streams >= 1 ? pcache->nr_streams : 1;
	for (i = 0; i < peer->nr_streams; i++)
		pcache_stream_init(&peer->streams[i], peer, i);
	init_waitqueue_head(&peer->wait_for_listen);

	return peer;
}

int pcache_net_init(struct dm_pcache *pcache, unsigned int argc, char **argv)
{
	struct pcache_network *net;
	char *error = NULL;
	int ret;

	if (argc == 0)
		return 0;

	net = kzalloc(sizeof(*net), GFP_KERNEL);
	if (!net)
		return -ENOMEM;

	mutex_init(&net->peers_lock);
	mutex_init(&net->connection_mutex);
	net->protocol_version = PCACHE_PROTO_VERSION;
	atomic_set(&net->stopping, 0);

	net->req_cache = kmem_cache_create("pcache_req_cache",
					sizeof(struct pcache_pending_req),
					0, 0, NULL);
	if (!net->req_cache) {
		kfree(net);
		return -ENOMEM;
	}

	net->req_pool = mempool_create_slab_pool(1024, net->req_cache);
	if (!net->req_pool) {
		kmem_cache_destroy(net->req_cache);
		kfree(net);
		return -ENOMEM;
	}

	ret = pcache_parse_dev_args(pcache, argc, argv, &error);
	if (ret < 0) {
		if (error)
			pcache_err("%s\n", error);
		mempool_destroy(net->req_pool);
		kmem_cache_destroy(net->req_cache);
		kfree(net);
		return ret;
	}
	if (ret == 0) {
		/* No distributed-mode args — tear down the empty net. */
		mempool_destroy(net->req_pool);
		kmem_cache_destroy(net->req_cache);
		kfree(net);
		return 0;
	}

	pcache->net = net;
	pcache_registry_set_my_node_id(pcache->registry, pcache->my_node_id);

	/* Start listen socket + accept_thread if a port was configured. Even
	 * pure-initiator nodes (lowest id in the cluster) keep a no-op listen
	 * so peers can be added later that connect *to* them. */
	if (pcache->listen_port) {
		ret = pcache_create_listen_socket(&net->listen_sock,
						  (int)pcache->listen_port);
		if (ret) {
			pcache_dev_err(pcache,
				"listen on :%u failed: %d\n",
				pcache->listen_port, ret);
			goto fail;
		}

		net->accept_thread = kthread_run(pcache_accept_thread, pcache,
				"pcache-accept-%s",
				pcache->ti->table->md->name);
		if (IS_ERR(net->accept_thread)) {
			ret = PTR_ERR(net->accept_thread);
			net->accept_thread = NULL;
			pcache_dev_err(pcache,
				"failed to start accept thread: %d\n", ret);
			goto fail;
		}
	}

	pcache_dev_info(pcache,
		"network up: my_node_id=%u listen_port=%u is_primary=%d\n",
		pcache->my_node_id, pcache->listen_port, pcache->is_primary);
	return 0;

fail:
	if (net->listen_sock) {
		sock_release(net->listen_sock);
		net->listen_sock = NULL;
	}
	mempool_destroy(net->req_pool);
	kmem_cache_destroy(net->req_cache);
	pcache->net = NULL;
	kfree(net);
	return ret;
}

void pcache_net_stop(struct dm_pcache *pcache)
{
	struct pcache_network *net = pcache->net;
	struct pcache_peer *peer;
	int slot;

	if (!net)
		return;

	atomic_set(&net->stopping, 1);

	/* Stop accept_thread first so no new inbound sockets land on peers
	 * while we're tearing them down. */
	if (net->accept_thread) {
		if (net->listen_sock)
			kernel_sock_shutdown(net->listen_sock, SHUT_RDWR);
		kthread_stop(net->accept_thread);
		net->accept_thread = NULL;
	}
	if (net->listen_sock) {
		sock_release(net->listen_sock);
		net->listen_sock = NULL;
	}

	mutex_lock(&net->peers_lock);
	for_each_active_peer(net, peer, slot) {
		if (peer->streams[0].sock)
			kernel_sock_shutdown(peer->streams[0].sock, SHUT_RDWR);

		/*
		 * Each kthread must be stopped by exactly one thread. For
		 * Primary the sender owns its receiver and stops it in its
		 * exit path; for Secondary there is only secondary_worker
		 * (stored in peer->streams[0].receiver).
		 */
		if (peer->streams[0].sender) {
			kthread_stop(peer->streams[0].sender);
			peer->streams[0].sender = NULL;
			peer->streams[0].receiver = NULL;
		} else if (peer->streams[0].receiver) {
			kthread_stop(peer->streams[0].receiver);
			peer->streams[0].receiver = NULL;
		}
		pcache_flush_pending(peer, -EIO);
		pcache_free_peer(peer);
		net->peers[slot] = NULL;
		module_put(THIS_MODULE);
	}
	mutex_unlock(&net->peers_lock);

	if (net->req_pool)
		mempool_destroy(net->req_pool);
	if (net->req_cache)
		kmem_cache_destroy(net->req_cache);

	kfree(net);
	pcache->net = NULL;
}

bool pcache_peer_connected(struct dm_pcache *pcache)
{
	struct pcache_peer *peer;
	int slot;

	if (!pcache->net)
		return false;

	/* Step 2: "connected" means at least one populated peer slot is
	 * currently connected. This already matches the multi-peer fan-out
	 * semantic (we only suppress the offline-bitmap-dirty branch when
	 * we have somebody to replicate to). */
	for_each_active_peer(pcache->net, peer, slot) {
		if (peer->state == PCACHE_C_CONNECTED)
			return true;
	}
	return false;
}

/*
 * Phase 2 Step 11 — Operator-driven full resync.
 * Set FULL_SYNC_NEEDED on the local bitmap, bump generation, then drop the
 * current socket to force a reconnect. The next handshake's sync_params
 * exchange will see NEED_FULL and elect Primary as SYNC_SOURCE.
 */
/*
 * Phase 3 Step 4 — runtime peer add/remove.
 *
 * add-peer reserves a slot in the on-PMEM registry, allocates the in-memory
 * peer struct, installs it at net->peers[slot], and starts a primary or
 * secondary worker depending on this device's role. Direction of the
 * actual TCP connection is decided per-pair: lower node_id initiates via
 * primary_worker's kernel_connect path, higher node_id waits for the
 * accept_thread to deliver an inbound socket. (For Step 3+4 with Primary
 * always at the lowest id, the role-based kthread choice naturally aligns
 * with the id-based direction rule.)
 */
int pcache_add_peer(struct dm_pcache *pcache, u32 peer_node_id, const char *addr)
{
	struct pcache_network *net = pcache->net;
	struct pcache_peer *peer;
	u64 gen;
	int slot, ret;

	if (!net)
		return -ENOTCONN;
	if (peer_node_id == pcache->my_node_id)
		return -EINVAL;
	if (!addr || !*addr)
		return -EINVAL;

	mutex_lock(&net->peers_lock);

	/* Idempotent: if the registry already has this peer (persisted from a
	 * prior session), reuse its slot. Otherwise allocate a fresh one. */
	slot = pcache_registry_find_slot_by_node_id(pcache->registry,
						    peer_node_id);
	if (slot >= 0) {
		if (net->peers[slot]) {
			mutex_unlock(&net->peers_lock);
			return -EEXIST;	/* already fully alive */
		}
		/* registry has slot but no in-memory peer — recreate it */
	} else {
		gen = pcache->generation;
		ret = pcache_registry_add(pcache->registry, peer_node_id, addr,
					  gen, &slot);
		if (ret) {
			mutex_unlock(&net->peers_lock);
			pcache_dev_err(pcache,
				"add-peer: registry_add(node_id=%u) failed: %d\n",
				peer_node_id, ret);
			return ret;
		}
	}

	peer = pcache_alloc_peer(pcache, peer_node_id, addr);
	if (!peer) {
		pcache_registry_remove(pcache->registry, peer_node_id);
		mutex_unlock(&net->peers_lock);
		return -ENOMEM;
	}

	/* Phase 3.5 — per-peer bitmap on its registry-assigned slot. */
	ret = pcache_peer_bitmap_init(peer, pcache->backing_dev.dev_size, slot);
	if (ret) {
		pcache_free_peer(peer);
		pcache_registry_remove(pcache->registry, peer_node_id);
		mutex_unlock(&net->peers_lock);
		pcache_dev_err(pcache,
			"add-peer: peer_bitmap_init failed: %d\n", ret);
		return ret;
	}

	__module_get(THIS_MODULE);
	net->peers[slot] = peer;

	if (peer->is_primary)
		ret = pcache_start_primary(peer);
	else
		ret = pcache_start_secondary(peer);
	if (ret) {
		net->peers[slot] = NULL;
		pcache_peer_bitmap_exit(peer);
		pcache_free_peer(peer);
		pcache_registry_remove(pcache->registry, peer_node_id);
		module_put(THIS_MODULE);
		mutex_unlock(&net->peers_lock);
		pcache_dev_err(pcache,
			"add-peer: worker spawn failed: %d\n", ret);
		return ret;
	}

	mutex_unlock(&net->peers_lock);
	pcache_dev_info(pcache,
		"add-peer: slot=%d node_id=%u addr=%s is_primary=%d\n",
		slot, peer_node_id, addr, peer->is_primary);
	return 0;
}

/*
 * Phase 3 Step 11 — stop existing per-peer workers and restart them under
 * the new role. Caller holds net->peers_lock. Each peer's pending_list and
 * registry slot are preserved; only the kthread + connection are recycled.
 */
static void pcache_set_role_swap_workers(struct pcache_network *net,
					 bool to_primary)
{
	struct pcache_peer *peer;
	int slot;

	for_each_active_peer(net, peer, slot) {
		peer->is_primary = to_primary;

		/* Shut down the socket to wake the worker from any blocking
		 * recv/send and unblock kthread_stop. */
		spin_lock(&peer->streams[0].sock_lock);
		if (peer->streams[0].sock)
			kernel_sock_shutdown(peer->streams[0].sock, SHUT_RDWR);
		spin_unlock(&peer->streams[0].sock_lock);

		if (peer->streams[0].sender) {
			kthread_stop(peer->streams[0].sender);
			peer->streams[0].sender = NULL;
			peer->streams[0].receiver = NULL;	/* primary_receiver owned by sender */
		} else if (peer->streams[0].receiver) {
			kthread_stop(peer->streams[0].receiver);
			peer->streams[0].receiver = NULL;
		}

		/* Drop any lingering socket pointer so the new worker starts
		 * with a clean state. */
		spin_lock(&peer->streams[0].sock_lock);
		if (peer->streams[0].sock) {
			sock_release(peer->streams[0].sock);
			peer->streams[0].sock = NULL;
		}
		peer->state = PCACHE_C_CONNECTING;
		peer->repl_state = PCACHE_R_OFF;
		spin_unlock(&peer->streams[0].sock_lock);

		if (to_primary)
			pcache_start_primary(peer);
		else
			pcache_start_secondary(peer);
	}
}

/*
 * Phase 3 Step 9 — runtime role switch.
 *
 * promote (Secondary → Primary):
 *   - Without `force`: refuse if any peer is currently CONNECTED (operator
 *     should demote the existing Primary first, or pass `force`). This is a
 *     conservative local check; the cluster-wide quorum-based safety arrives
 *     in Step 10, and split-brain detection in Step 12.
 *   - Bumps our generation so the next handshake forces a re-sync decision.
 *
 * demote (Primary → Secondary):
 *   - Flip is_primary first (map_bio guard now rejects new WRITEs).
 *   - Drop sockets so peers' workers re-handshake with the new role view.
 *   - Flush the bitmap so on-disk state matches our in-memory view.
 *   - Currently does NOT wait for inflight bios to fully complete — they
 *     proceed normally (already enqueued under the old role). A coordinated
 *     demote-handshake (P_DEMOTE_REQ/ACK) is a Step 11/12 follow-up.
 */
int pcache_set_role(struct dm_pcache *pcache, bool primary, bool force)
{
	struct pcache_network *net = pcache->net;
	struct pcache_peer *peer;
	int slot;

	if (!net)
		return -ENOTCONN;

	mutex_lock(&net->peers_lock);

	if (primary && !pcache->is_primary) {
		mutex_unlock(&net->peers_lock);
		if (!force && !pcache_has_quorum(pcache)) {
			pcache_dev_err(pcache,
				"set-role primary refused: no quorum (use force to override)\n");
			return -EBUSY;
		}
		mutex_lock(&net->peers_lock);

		/* Phase 3.5 B1 — broadcast P_DEMOTE_REQ on every connected
		 * peer socket (best-effort notification). Peers that ARE the
		 * current Primary will see this in their primary_receiver and
		 * log it; operator's coordinated demote workflow + split-brain
		 * detection cover the actual demote. */
		if (!force) {
			int sent = 0;
			for_each_active_peer(net, peer, slot) {
				spin_lock(&peer->streams[0].sock_lock);
				if (peer->streams[0].sock) {
					int rc = pcache_send_demote_req(peer->streams[0].sock,
						pcache->my_node_id, 0);
					if (!rc)
						sent++;
				}
				spin_unlock(&peer->streams[0].sock_lock);
			}
			if (sent)
				pcache_dev_info(pcache,
					"set-role primary: broadcast DEMOTE_REQ to %d peer(s)\n",
					sent);
		}

		pcache->is_primary = true;

		/* Phase 3.5 — promote-triggered FULL resync, per-peer.
		 * Each peer's bm gets FULL_SYNC_NEEDED + set_all. Device gen
		 * bumps once (gen_dirty trigger). */
		spin_lock(&pcache->gen_lock);
		if (!pcache->gen_dirty) {
			pcache->gen_dirty = true;
			pcache->generation++;
		}
		spin_unlock(&pcache->gen_lock);

		for_each_active_peer(net, peer, slot) {
			if (!peer->bm)
				continue;
			spin_lock(&peer->bm->lock);
			peer->bm->flags |= PCACHE_BM_F_FULL_SYNC_NEEDED;
			spin_unlock(&peer->bm->lock);
			pcache_bm_set_all(peer->bm);
			pcache_bm_flush_sync(peer->bm);
		}

		pcache_set_role_swap_workers(net, true);

		pcache_dev_info(pcache,
			"set-role: promoted to PRIMARY (force=%d, gen=%llu, full-resync flagged for %d peer(s))\n",
			force, pcache->generation, 0);
	} else if (!primary && pcache->is_primary) {
		pcache->is_primary = false;
		pcache_set_role_swap_workers(net, false);

		for_each_active_peer(net, peer, slot)
			if (peer->bm)
				pcache_bm_flush_sync(peer->bm);

		pcache_dev_info(pcache,
			"set-role: demoted to SECONDARY (force=%d)\n", force);
	}

	mutex_unlock(&net->peers_lock);
	return 0;
}

/*
 * Phase 3 Step 13 — split-brain & resync control messages.
 *
 * resolve-split-brain victim:
 *   - Local side surrenders. Clear our bitmap, set FULL_SYNC_NEEDED, then
 *     restart the peer worker so it re-handshakes and takes the peer's
 *     full data via SYNC_TARGET.
 * resolve-split-brain survivor:
 *   - Local side wins. Bump generation so peer's decide_sync_role on
 *     reconnect sees our gen as newer, then restart worker to push a
 *     full resync to peer (which should have run "victim" on its end).
 */
int pcache_resolve_split_brain(struct dm_pcache *pcache, u32 peer_node_id,
			       bool victim)
{
	struct pcache_network *net = pcache->net;
	struct pcache_peer *peer;
	struct pcache_bitmap *bm;
	int slot;

	if (!net)
		return -ENOTCONN;

	mutex_lock(&net->peers_lock);
	slot = pcache_registry_find_slot_by_node_id(pcache->registry,
						    peer_node_id);
	if (slot < 0 || !net->peers[slot]) {
		mutex_unlock(&net->peers_lock);
		return -ENOENT;
	}
	peer = net->peers[slot];
	bm = peer->bm;

	peer->split_brain = false;

	if (victim && bm) {
		pcache_bm_clear_all(bm);
		spin_lock(&bm->lock);
		bm->flags |= PCACHE_BM_F_FULL_SYNC_NEEDED;
		spin_unlock(&bm->lock);
		spin_lock(&pcache->gen_lock);
		pcache->gen_dirty = false;	/* peer is authoritative */
		spin_unlock(&pcache->gen_lock);
		pcache_bm_flush_sync(bm);
	} else if (!victim && bm) {
		spin_lock(&pcache->gen_lock);
		if (!pcache->gen_dirty) {
			pcache->gen_dirty = true;
			pcache->generation++;
		}
		spin_unlock(&pcache->gen_lock);
		spin_lock(&bm->lock);
		bm->flags |= PCACHE_BM_F_FULL_SYNC_NEEDED;
		spin_unlock(&bm->lock);
		pcache_bm_set_all(bm);
		pcache_bm_flush_sync(bm);
	}

	/* Break the HALT loop by stopping + restarting the kthread. */
	{
		struct pcache_peer *p2 = peer;
		bool was_primary = p2->is_primary;

		spin_lock(&p2->streams[0].sock_lock);
		if (p2->streams[0].sock)
			kernel_sock_shutdown(p2->streams[0].sock, SHUT_RDWR);
		spin_unlock(&p2->streams[0].sock_lock);

		if (p2->streams[0].sender) {
			kthread_stop(p2->streams[0].sender);
			p2->streams[0].sender = NULL;
			p2->streams[0].receiver = NULL;
		} else if (p2->streams[0].receiver) {
			kthread_stop(p2->streams[0].receiver);
			p2->streams[0].receiver = NULL;
		}

		spin_lock(&p2->streams[0].sock_lock);
		if (p2->streams[0].sock) {
			sock_release(p2->streams[0].sock);
			p2->streams[0].sock = NULL;
		}
		p2->state = PCACHE_C_CONNECTING;
		p2->repl_state = PCACHE_R_OFF;
		spin_unlock(&p2->streams[0].sock_lock);

		if (was_primary)
			pcache_start_primary(p2);
		else
			pcache_start_secondary(p2);
	}

	mutex_unlock(&net->peers_lock);

	pcache_dev_info(pcache,
		"resolve-split-brain: peer node_id=%u, role=%s, worker restarted\n",
		peer_node_id, victim ? "victim" : "survivor");
	return 0;
}

int pcache_abort_resync(struct dm_pcache *pcache)
{
	struct pcache_network *net = pcache->net;
	struct pcache_peer *peer;
	int slot, dropped = 0;

	if (!net)
		return -ENOTCONN;

	mutex_lock(&net->peers_lock);
	for_each_active_peer(net, peer, slot) {
		if (peer->bm) {
			spin_lock(&peer->bm->lock);
			peer->bm->flags &= ~PCACHE_BM_F_FULL_SYNC_NEEDED;
			spin_unlock(&peer->bm->lock);
			pcache_bm_flush_sync(peer->bm);
		}
		spin_lock(&peer->streams[0].sock_lock);
		if (peer->streams[0].sock) {
			kernel_sock_shutdown(peer->streams[0].sock, SHUT_RDWR);
			dropped++;
		}
		spin_unlock(&peer->streams[0].sock_lock);
	}
	mutex_unlock(&net->peers_lock);

	pcache_dev_info(pcache,
		"abort-resync: cleared FULL_SYNC_NEEDED on all peers, dropped %d socket(s)\n",
		dropped);
	return 0;
}

int pcache_reset_generation(struct dm_pcache *pcache)
{
	struct pcache_network *net = pcache->net;
	struct pcache_peer *peer;
	int slot;
	u64 new_gen;

	spin_lock(&pcache->gen_lock);
	pcache->generation++;
	pcache->gen_dirty = true;
	new_gen = pcache->generation;
	spin_unlock(&pcache->gen_lock);

	/* Flush every active peer's header so the new device gen lands in
	 * PMEM (replicated across all slots). */
	if (net) {
		mutex_lock(&net->peers_lock);
		for_each_active_peer(net, peer, slot) {
			if (peer->bm)
				pcache_bm_flush_sync(peer->bm);
			spin_lock(&peer->streams[0].sock_lock);
			if (peer->streams[0].sock)
				kernel_sock_shutdown(peer->streams[0].sock, SHUT_RDWR);
			spin_unlock(&peer->streams[0].sock_lock);
		}
		mutex_unlock(&net->peers_lock);
	}

	pcache_dev_info(pcache, "reset-generation: bumped to gen=%llu\n", new_gen);
	return 0;
}

int pcache_remove_peer(struct dm_pcache *pcache, u32 peer_node_id)
{
	struct pcache_network *net = pcache->net;
	struct pcache_peer *peer;
	int slot;

	if (!net)
		return -ENOTCONN;

	mutex_lock(&net->peers_lock);
	slot = pcache_registry_find_slot_by_node_id(pcache->registry,
						    peer_node_id);
	if (slot < 0) {
		mutex_unlock(&net->peers_lock);
		return -ENOENT;
	}
	peer = net->peers[slot];
	if (!peer) {
		mutex_unlock(&net->peers_lock);
		return -ENOENT;
	}

	/* Shut down the socket first to wake any blocking recv/send in the
	 * worker so kthread_stop returns promptly. */
	if (peer->streams[0].sock)
		kernel_sock_shutdown(peer->streams[0].sock, SHUT_RDWR);

	if (peer->streams[0].sender) {
		kthread_stop(peer->streams[0].sender);
		peer->streams[0].sender = NULL;
		peer->streams[0].receiver = NULL;
	} else if (peer->streams[0].receiver) {
		kthread_stop(peer->streams[0].receiver);
		peer->streams[0].receiver = NULL;
	}
	pcache_flush_pending(peer, -EIO);
	pcache_peer_bitmap_exit(peer);
	pcache_free_peer(peer);
	net->peers[slot] = NULL;
	module_put(THIS_MODULE);

	pcache_registry_remove(pcache->registry, peer_node_id);
	mutex_unlock(&net->peers_lock);

	pcache_dev_info(pcache, "remove-peer: slot=%d node_id=%u removed\n",
			slot, peer_node_id);
	return 0;
}

int pcache_list_peers(struct dm_pcache *pcache, char *buf, size_t buflen)
{
	struct pcache_peer *peer;
	int slot;
	int sz = 0;

	if (!pcache->net)
		return scnprintf(buf, buflen, "no network");

	sz += scnprintf(buf + sz, buflen - sz, "my_node_id=%u listen_port=%u\n",
			pcache->my_node_id, pcache->listen_port);
	for_each_active_peer(pcache->net, peer, slot) {
		sz += scnprintf(buf + sz, buflen - sz,
				"slot=%d node_id=%d addr=%s state=%d repl=%d split_brain=%d\n",
				slot, peer->node_id, peer->remote_addr,
				peer->state, peer->repl_state,
				peer->split_brain ? 1 : 0);
		if (sz >= (int)buflen)
			break;
	}
	return sz;
}

int pcache_net_force_resync(struct dm_pcache *pcache)
{
	struct pcache_peer *peer;
	int slot, kicked = 0;

	if (!pcache->net)
		return -ENOTCONN;

	spin_lock(&pcache->gen_lock);
	if (!pcache->gen_dirty) {
		pcache->gen_dirty = true;
		pcache->generation++;
	}
	spin_unlock(&pcache->gen_lock);

	/* Phase 3.5 — set FULL_SYNC_NEEDED on EVERY peer's bm + drop sockets. */
	for_each_active_peer(pcache->net, peer, slot) {
		if (peer->bm) {
			spin_lock(&peer->bm->lock);
			peer->bm->flags |= PCACHE_BM_F_FULL_SYNC_NEEDED;
			spin_unlock(&peer->bm->lock);
			pcache_bm_flush_sync(peer->bm);
		}
		spin_lock(&peer->streams[0].sock_lock);
		if (peer->streams[0].sock) {
			kernel_sock_shutdown(peer->streams[0].sock, SHUT_RDWR);
			kicked++;
		}
		spin_unlock(&peer->streams[0].sock_lock);
	}

	pcache_dev_info(pcache,
		"force-resync requested: gen=%llu full_sync flagged on all peers; %d socket(s) dropped\n",
		pcache->generation, kicked);
	return 0;
}

/* Phase 3.5 — public helper used by map_bio when fan-out cannot deliver. */
void pcache_mark_all_peers_dirty(struct dm_pcache *pcache, u64 sector, u32 size)
{
	struct pcache_peer *peer;
	int slot;

	if (!pcache->net)
		return;

	mutex_lock(&pcache->net->peers_lock);
	for_each_active_peer(pcache->net, peer, slot) {
		if (peer->bm)
			pcache_bm_set_range(peer->bm, sector, size);
	}
	mutex_unlock(&pcache->net->peers_lock);
}

/*
 * Phase 3 Step 7 — Primary fan-out broadcast write.
 *
 * Enqueue one pending_req per *connected* peer so the same bio gets
 * replicated to every Secondary currently online. Disconnected peer slots
 * are skipped; the dirty bitmap is set for the range so the next resync
 * after they reconnect covers it.
 *
 * Reference counting:
 *   caller (map_bio) already did 1 pcache_req_get before calling us.
 *   First enqueued peer reuses that ref.
 *   For each additional connected peer, we pcache_req_get one more time.
 *   Each peer's ACK eventually calls pcache_req_put once. After M ACKs the
 *   kref reaches 0 and the bio is completed (Protocol C "all-connected
 *   ACK" semantic).
 *
 * Returns 0 if at least one peer was enqueued, -ENOTCONN if no peer slot
 * is currently in PCACHE_C_CONNECTED state (caller still owns the extra ref
 * and must drop it).
 */
int pcache_net_submit_write(struct dm_pcache *pcache,
			    struct pcache_request *pcache_req,
			    u64 sector, u32 size)
{
	struct pcache_network *net;
	struct pcache_peer *peer;
	struct pcache_pending_req *req;
	int slot, enqueued = 0;
	bool any_skipped = false;

	net = pcache->net;
	if (!net)
		return -ENOTCONN;

	mutex_lock(&net->peers_lock);
	for_each_active_peer(net, peer, slot) {
		if (peer->state != PCACHE_C_CONNECTED) {
			/* Phase 3.5 — mark THIS peer's bitmap so its next
			 * resync covers the range. */
			if (peer->bm)
				pcache_bm_set_range(peer->bm, sector, size);
			any_skipped = true;
			continue;
		}

		req = mempool_alloc(net->req_pool, GFP_NOIO);
		if (!req) {
			if (peer->bm)
				pcache_bm_set_range(peer->bm, sector, size);
			any_skipped = true;
			continue;
		}

		/* First enqueue reuses the caller's extra ref; subsequent
		 * peers need additional refs so each can independently put. */
		if (enqueued > 0)
			pcache_req_get(pcache_req);

		req->pcache_req = pcache_req;
		req->sector = sector;
		req->size = size;
		req->block_id = (u64)atomic64_inc_return(&peer->streams[0].next_block_id);
		req->sent = false;
		req->send_time = 0;
#if PCACHE_LAT_TRACE
		req->t_enqueue = ktime_get_ns();
		req->t_send_start = 0;
#endif

		spin_lock(&peer->streams[0].pending_lock);
		list_add_tail(&req->list, &peer->streams[0].pending_list);
		atomic_inc(&peer->streams[0].pending_cnt);
		atomic_inc(&peer->streams[0].unsent_cnt);
		spin_unlock(&peer->streams[0].pending_lock);

		wake_up(&peer->streams[0].wait_for_work);
		enqueued++;
	}
	mutex_unlock(&net->peers_lock);

	(void)any_skipped;

	if (enqueued == 0)
		return -ENOTCONN;
	return 0;
}
