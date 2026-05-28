// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/bio.h>
#include <linux/wait.h>
#include <linux/sched/signal.h>
#include <net/tcp.h>
#include "dm_pcache.h"
#include "dm_pcache_bitmap.h"
#include "dm_pcache_net.h"
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
 * Phase 2 — decide which role this node plays after exchanging sync_params.
 * Only consults generation counters and the NEED_FULL flag; bitmap content
 * comparison is left to Step 7 (P_BITMAP exchange).
 */
static enum pcache_repl_state
pcache_decide_sync_role(struct pcache_peer *peer,
			const struct pcache_sync_params_pkt *peer_sp)
{
	struct pcache_bitmap *bm = peer->pcache->bitmap;
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
	struct pcache_bitmap *bm = peer->pcache->bitmap;
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

	spin_lock(&peer->pending_lock);
	list_for_each_entry(req, &peer->pending_list, list) {
		if (req->block_id == block_id) {
			list_del(&req->list);
			atomic_dec(&peer->pending_cnt);
			found = req;
			break;
		}
	}
	spin_unlock(&peer->pending_lock);

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
		pcache_bm_set_range(peer->pcache->bitmap,
				    found->sector, found->size);

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

	ret = pcache_recv_all(peer->sock, &rs, sizeof(rs));
	if (ret)
		return ret;

	sector = le64_to_cpu(hdr->sector);
	size   = le32_to_cpu(rs.size);
	if (!size)
		return 0;

	data = kvmalloc(size, GFP_NOIO);
	if (!data)
		return -ENOMEM;

	ret = pcache_recv_all(peer->sock, data, size);
	if (ret)
		goto free_data;

	ret = pcache_net_write_to_cache(&pcache->cache, sector, data, size);
	if (ret)
		pcache_err("secondary: RS_DATA write_to_cache failed: %d\n", ret);

	{
		struct pcache_packet_header ack;

		memset(&ack, 0, sizeof(ack));
		ack.type     = cpu_to_le16(PCACHE_P_RS_ACK);
		ack.sector   = hdr->sector;
		ack.block_id = hdr->block_id;
		ack.size     = rs.size;
		ret = pcache_send_packet(peer->sock, &ack);
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

	if (!size)
		return 0;

	data = kvmalloc(size, GFP_NOIO);
	if (!data)
		return -ENOMEM;

	ret = pcache_recv_all(peer->sock, data, size);
	if (ret)
		goto free_data;

	ret = pcache_net_write_to_cache(&pcache->cache, sector, data, size);
	if (ret)
		pcache_err("write to cache failed: %d (sector=%llu size=%u)\n",
			   ret, sector, size);

	if (flags & PCACHE_F_ACK) {
		memset(&reply, 0, sizeof(reply));
		reply.type = cpu_to_le16(ret ? PCACHE_P_NACK : PCACHE_P_ACK);
		reply.sector = hdr->sector;
		reply.block_id = hdr->block_id;
		reply.size = hdr->size;
		ret = pcache_send_packet(peer->sock, &reply);
		if (ret)
			pcache_err("secondary: ACK/NACK send failed: %d\n", ret);
	}

free_data:
	kvfree(data);
	return ret;
}

/*
 * Secondary worker thread.
 *
 * Wait for the listen socket, accept(), exchange handshake, then loop on
 * (recv header, dispatch). On disconnect, return to accept().
 */
static int pcache_secondary_worker(void *arg)
{
	struct pcache_peer *peer = arg;
	struct socket *sock = NULL;
	struct pcache_packet_header hdr;
	int ret;

	allow_signal(SIGTERM);

	if (!peer->listen_sock) {
		wait_event_interruptible(peer->wait_for_listen,
			peer->listen_sock || kthread_should_stop());
		if (kthread_should_stop())
			return 0;
	}

connect:
	while (!kthread_should_stop()) {
		if (sock) {
			sock_release(sock);
			sock = NULL;
		}

		ret = kernel_accept(peer->listen_sock, &sock, 0);
		if (ret < 0) {
			pcache_err("secondary: accept failed: %d\n", ret);
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
			continue;
		}

		/* Apply matching TCP tuning + SO_KEEPALIVE to the accepted sock
		 * so silent-failure zombie connections get RST'd in ~50 s
		 * instead of hanging the recv loop indefinitely. */
		pcache_apply_socket_opts(sock);

		peer->state = PCACHE_C_CONNECTING;
		ret = pcache_recv_handshake(sock, NULL);
		if (ret) {
			pcache_err("secondary: HANDSHAKE recv failed: %d\n", ret);
			continue;
		}

		ret = pcache_send_handshake(sock, peer->node_id);
		if (ret) {
			pcache_err("secondary: HANDSHAKE reply failed: %d\n", ret);
			continue;
		}

		/*
		 * Long timeout for data phase. kthread_should_stop() and a
		 * shutdown from pcache_net_stop() handle teardown.
		 */
		pcache_sock_set_timeout(sock, 300 * HZ);

		spin_lock(&peer->sock_lock);
		peer->sock = sock;
		peer->state = PCACHE_C_CONNECTED;
		spin_unlock(&peer->sock_lock);
		break;
	}

	if (kthread_should_stop()) {
		if (sock)
			sock_release(sock);
		return 0;
	}

	pcache_info("secondary: handshake complete, entering recv loop\n");

	/* Phase 2 Step 5+8: exchange P_SYNC_PARAMS, compute role.
	 * SYNC_TARGET stays in the recv loop and waits for P_RS_DATA followed
	 * by P_BITMAP_DONE; on receipt of DONE we mark_synced & ESTABLISHED. */
	pcache_set_repl_state(peer, PCACHE_R_NEGOTIATING);
	{
		struct pcache_sync_params_pkt peer_sp = {0};
		enum pcache_repl_state role;
		int sp_ret = pcache_exchange_sync_params(peer, sock, &peer_sp,
							 false);

		if (sp_ret) {
			pcache_err("secondary: sync_params exchange failed: %d\n",
				   sp_ret);
			peer->state = PCACHE_C_CONNECTING;
			pcache_set_repl_state(peer, PCACHE_R_OFF);
			spin_lock(&peer->sock_lock);
			peer->sock = NULL;
			spin_unlock(&peer->sock_lock);
			goto connect;
		}
		peer->peer_generation = le64_to_cpu(peer_sp.generation);
		role = pcache_decide_sync_role(peer, &peer_sp);
		pcache_info("secondary: decided role = %s\n",
			    pcache_repl_state_name(role));
		if (role == PCACHE_R_HALT) {
			pcache_set_repl_state(peer, PCACHE_R_HALT);
		} else if (role == PCACHE_R_SYNC_TARGET) {
			pcache_set_repl_state(peer, PCACHE_R_SYNC_TARGET);
			/* Recv loop below handles P_RS_DATA + P_BITMAP_DONE. */
		} else {
			pcache_bm_mark_synced(peer->pcache->bitmap,
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
			spin_lock(&peer->sock_lock);
			peer->sock = NULL;
			spin_unlock(&peer->sock_lock);
			goto connect;
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
			pcache_bm_mark_synced(peer->pcache->bitmap,
					      peer->peer_generation);
			pcache_set_repl_state(peer, PCACHE_R_ESTABLISHED);
			break;
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

	if (sock)
		sock_release(sock);
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
				wake_up(&peer->wait_for_work);
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
	struct pcache_bitmap *bm = pcache->bitmap;
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
		block_id = (u64)atomic64_inc_return(&peer->next_block_id);
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

	spin_lock(&peer->pending_lock);
	list_splice_init(&peer->pending_list, &drain);
	atomic_set(&peer->pending_cnt, 0);
	atomic_set(&peer->unsent_cnt, 0);
	spin_unlock(&peer->pending_lock);

	list_for_each_entry_safe(req, tmp, &drain, list) {
		list_del(&req->list);
		pcache_bm_set_range(peer->pcache->bitmap,
				    req->sector, req->size);
		pcache_req_put(req->pcache_req, err);
		pcache_pending_req_free(peer, req);
	}
}

/*
 * Primary worker thread.
 *
 * Establishes the TCP connection, performs handshake, spawns the ACK
 * receiver, then loops sending unsent pending requests. On any send error
 * it tears down and reconnects.
 */
static int pcache_primary_worker(void *arg)
{
	struct pcache_peer *peer = arg;
	struct socket *sock = NULL;
	int ret;

	allow_signal(SIGTERM);

connect:
	while (!kthread_should_stop()) {
		/* Whenever we re-enter connect:, replication is OFF until the
		 * next handshake completes. */
		pcache_set_repl_state(peer, PCACHE_R_OFF);

		if (peer->receiver) {
			if (sock)
				kernel_sock_shutdown(sock, SHUT_RDWR);
			kthread_stop(peer->receiver);
			peer->receiver = NULL;
		}
		if (sock) {
			sock_release(sock);
			sock = NULL;
			spin_lock(&peer->sock_lock);
			peer->sock = NULL;
			spin_unlock(&peer->sock_lock);
		}

		ret = pcache_create_peer_socket(peer);
		if (ret) {
			pcache_err("primary: connect failed: %d\n", ret);
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(3 * HZ);
			continue;
		}
		sock = peer->sock;

		ret = pcache_send_handshake(sock, peer->node_id);
		if (ret) {
			pcache_err("primary: HANDSHAKE send failed: %d\n", ret);
			sock_release(sock);
			sock = NULL;
			spin_lock(&peer->sock_lock);
			peer->sock = NULL;
			peer->state = PCACHE_C_CONNECTING;
			spin_unlock(&peer->sock_lock);
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
			continue;
		}

		ret = pcache_recv_handshake(sock, NULL);
		if (ret) {
			pcache_err("primary: HANDSHAKE reply failed: %d\n", ret);
			sock_release(sock);
			sock = NULL;
			spin_lock(&peer->sock_lock);
			peer->sock = NULL;
			peer->state = PCACHE_C_CONNECTING;
			spin_unlock(&peer->sock_lock);
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
			continue;
		}

		pcache_info("primary: handshake complete\n");

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
							     &peer_sp, true);
			if (sp_ret) {
				pcache_err("primary: sync_params exchange failed: %d\n",
					   sp_ret);
				sock_release(sock);
				sock = NULL;
				spin_lock(&peer->sock_lock);
				peer->sock = NULL;
				peer->state = PCACHE_C_CONNECTING;
				spin_unlock(&peer->sock_lock);
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
				pcache_set_repl_state(peer, PCACHE_R_HALT);
				while (!kthread_should_stop()) {
					set_current_state(TASK_INTERRUPTIBLE);
					schedule_timeout(HZ);
				}
				goto exit;
			}
			if (role == PCACHE_R_SYNC_SOURCE) {
				pcache_set_repl_state(peer, PCACHE_R_SYNC_SOURCE);
				sp_ret = pcache_do_resync(peer, sock, peer_gen);
				if (sp_ret) {
					pcache_err("primary: resync failed: %d, reconnecting\n",
						   sp_ret);
					sock_release(sock);
					sock = NULL;
					spin_lock(&peer->sock_lock);
					peer->sock = NULL;
					peer->state = PCACHE_C_CONNECTING;
					spin_unlock(&peer->sock_lock);
					pcache_set_repl_state(peer, PCACHE_R_OFF);
					set_current_state(TASK_INTERRUPTIBLE);
					schedule_timeout(HZ);
					continue;
				}
			} else {
				/* Clean reconnect — peer says we're already in sync. */
				pcache_bm_mark_synced(peer->pcache->bitmap,
						      peer_gen);
			}
			pcache_set_repl_state(peer, PCACHE_R_ESTABLISHED);
		}

		peer->receiver = kthread_run(pcache_primary_receiver, peer,
				"pcache-recv-%s",
				peer->pcache->ti->table->md->name);
		if (IS_ERR(peer->receiver)) {
			pcache_err("primary: failed to spawn receiver: %ld\n",
				   PTR_ERR(peer->receiver));
			peer->receiver = NULL;
			continue;
		}

		/* Re-mark unsent any requests that were in flight before the
		 * (re)connect so the new receiver can ACK them. */
		{
			struct pcache_pending_req *p;

			spin_lock(&peer->pending_lock);
			list_for_each_entry(p, &peer->pending_list, list) {
				if (p->sent) {
					p->sent = false;
					atomic_inc(&peer->unsent_cnt);
				}
			}
			spin_unlock(&peer->pending_lock);
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
		spin_lock(&peer->pending_lock);
		if (atomic_read(&peer->pending_cnt) > 0) {
			unsigned long deadline = jiffies -
				(PCACHE_ACK_TIMEOUT_SECS * HZ);

			list_for_each_entry_safe(req, tmp, &peer->pending_list, list) {
				if (!req->sent)
					continue;
				if (time_after_eq(req->send_time, deadline))
					continue;
				pcache_err("primary: request timeout sector=%llu size=%u\n",
					   req->sector, req->size);
				list_del(&req->list);
				atomic_dec(&peer->pending_cnt);
				/* Step 6: mark dirty before completing so next
				 * resync covers this range. */
				pcache_bm_set_range(peer->pcache->bitmap,
						    req->sector, req->size);
				pcache_req_put(req->pcache_req, -ETIMEDOUT);
				pcache_pending_req_free(peer, req);
				any_timed_out = true;
			}
		}
		spin_unlock(&peer->pending_lock);

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
			spin_lock(&peer->pending_lock);
			list_for_each_entry(req, &peer->pending_list, list) {
				if (!req->sent) {
					req->sent = true;
					req->send_time = jiffies;
					atomic_dec(&peer->unsent_cnt);
					found_unsent = true;
					break;
				}
			}
			spin_unlock(&peer->pending_lock);

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
		wait_event_interruptible_timeout(peer->wait_for_work,
			atomic_read(&peer->unsent_cnt) > 0 ||
			peer->state != PCACHE_C_CONNECTED ||
			kthread_should_stop(),
			5 * HZ);

		if (peer->state != PCACHE_C_CONNECTED)
			goto connect;
	}

exit:
	if (peer->receiver) {
		if (sock)
			kernel_sock_shutdown(sock, SHUT_RDWR);
		kthread_stop(peer->receiver);
		peer->receiver = NULL;
	}
	if (sock) {
		sock_release(sock);
		spin_lock(&peer->sock_lock);
		peer->sock = NULL;
		spin_unlock(&peer->sock_lock);
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
	peer->receiver = t;
	return 0;
}

static int pcache_start_primary(struct pcache_peer *peer)
{
	struct task_struct *t;

	t = kthread_run(pcache_primary_worker, peer, "pcache-primary-%s",
			peer->pcache->ti->table->md->name);
	if (IS_ERR(t))
		return PTR_ERR(t);
	peer->sender = t;
	return 0;
}

static int pcache_parse_net_args(struct pcache_network *net,
				 struct dm_pcache *pcache,
				 unsigned int argc, char **argv,
				 char **error)
{
	struct pcache_peer *peer;
	int node_id = -1;
	int is_primary = -1;
	char *peer_addr = NULL;
	int listen_port = 0;
	unsigned int i;

	for (i = 0; i < argc; i++) {
		char *arg = argv[i];
		char *val = strchr(arg, '=');

		if (!val)
			continue;
		*val++ = '\0';

		if (!strcmp(arg, "node_id")) {
			if (kstrtoint(val, 10, &node_id) < 0 || node_id < 0) {
				*error = "invalid node_id";
				return -EINVAL;
			}
		} else if (!strcmp(arg, "is_primary")) {
			if (kstrtoint(val, 10, &is_primary) < 0 ||
			    is_primary < 0 || is_primary > 1) {
				*error = "invalid is_primary (0 or 1)";
				return -EINVAL;
			}
		} else if (!strcmp(arg, "peer_addr")) {
			peer_addr = val;
		} else if (!strcmp(arg, "listen_port")) {
			if (kstrtoint(val, 10, &listen_port) < 0 || listen_port <= 0) {
				*error = "invalid listen_port";
				return -EINVAL;
			}
		}
	}

	if (node_id < 0 || is_primary < 0)
		return 0;

	peer = kzalloc(sizeof(*peer), GFP_KERNEL);
	if (!peer)
		return -ENOMEM;

	peer->pcache = pcache;
	peer->node_id = node_id;
	peer->is_primary = is_primary;
	peer->state = PCACHE_C_STANDALONE;
	peer->repl_state = PCACHE_R_OFF;
	if (peer_addr)
		strscpy(peer->remote_addr, peer_addr, sizeof(peer->remote_addr));

	atomic_set(&peer->pending_cnt, 0);
	atomic_set(&peer->unsent_cnt, 0);
	atomic64_set(&peer->next_block_id, 1);
	INIT_LIST_HEAD(&peer->pending_list);
	init_waitqueue_head(&peer->wait_for_work);
	init_waitqueue_head(&peer->wait_for_socket);
	init_waitqueue_head(&peer->wait_for_listen);
	spin_lock_init(&peer->sock_lock);
	spin_lock_init(&peer->pending_lock);
	__module_get(THIS_MODULE);

	if (!peer->is_primary && listen_port > 0) {
		int err = pcache_create_server_socket(peer, listen_port);

		if (err) {
			pcache_err("failed to create server socket on port %d: %d\n",
				   listen_port, err);
			module_put(THIS_MODULE);
			kfree(peer);
			return err;
		}
	}

	/* Step 2: legacy dmsetup-table path installs the single peer at
	 * slot 0. Step 4 will look up a real slot index in the registry. */
	net->peers[0] = peer;
	pcache->net = net;
	pcache->is_primary = peer->is_primary;
	pcache->node_id = peer->node_id;

	return 0;
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

	ret = pcache_parse_net_args(net, pcache, argc, argv, &error);
	if (ret) {
		if (error)
			pcache_err("%s\n", error);
		mempool_destroy(net->req_pool);
		kmem_cache_destroy(net->req_cache);
		kfree(net);
		return ret;
	}

	{
		struct pcache_peer *peer;
		int slot, started = 0;
		bool err_seen = false;

		for_each_active_peer(net, peer, slot) {
			if (peer->is_primary)
				ret = pcache_start_primary(peer);
			else
				ret = pcache_start_secondary(peer);
			if (ret) {
				err_seen = true;
				break;
			}
			started++;
		}

		if (err_seen) {
			/* Tear down whatever has already started. */
			for_each_active_peer(net, peer, slot)
				pcache_free_peer(peer);
			mempool_destroy(net->req_pool);
			kmem_cache_destroy(net->req_cache);
			kfree(net);
			return ret;
		}

		if (started == 0) {
			/* No peer slot populated — distributed-mode arguments
			 * were missing or incomplete. Tear down the empty net. */
			mempool_destroy(net->req_pool);
			kmem_cache_destroy(net->req_cache);
			kfree(net);
			return 0;
		}

		for_each_active_peer(net, peer, slot) {
			wake_up(&peer->wait_for_listen);
			pcache_info("network slot %d up (node_id=%d, is_primary=%d)\n",
				    slot, peer->node_id, peer->is_primary);
		}
	}

	return 0;
}

void pcache_net_stop(struct dm_pcache *pcache)
{
	struct pcache_network *net = pcache->net;
	struct pcache_peer *peer;
	int slot;

	if (!net)
		return;

	atomic_set(&net->stopping, 1);

	for_each_active_peer(net, peer, slot) {
		if (peer->sock)
			kernel_sock_shutdown(peer->sock, SHUT_RDWR);
		if (peer->listen_sock)
			kernel_sock_shutdown(peer->listen_sock, SHUT_RDWR);

		/*
		 * Each kthread must be stopped by exactly one thread.
		 * For Primary the sender owns its receiver and stops it in
		 * its exit path; we only stop the sender here.
		 * For Secondary there is only the secondary_worker
		 * (stored in peer->receiver).
		 */
		if (peer->sender) {
			kthread_stop(peer->sender);
			peer->sender = NULL;
			peer->receiver = NULL;
		} else if (peer->receiver) {
			kthread_stop(peer->receiver);
			peer->receiver = NULL;
		}
		pcache_flush_pending(peer, -EIO);
		pcache_free_peer(peer);
		net->peers[slot] = NULL;
		module_put(THIS_MODULE);
	}

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
int pcache_net_force_resync(struct dm_pcache *pcache)
{
	struct pcache_peer *peer;
	struct pcache_bitmap *bm = pcache->bitmap;
	int slot, kicked = 0;

	if (!pcache->net || !bm)
		return -ENOTCONN;

	spin_lock(&bm->lock);
	bm->flags |= PCACHE_BM_F_FULL_SYNC_NEEDED;
	if (!bm->gen_dirty) {
		bm->gen_dirty = true;
		bm->generation++;
	}
	spin_unlock(&bm->lock);
	pcache_bm_flush_sync(bm);

	/* Drop every connected peer socket so each connection rebuilds
	 * with NEED_FULL set on the next handshake. */
	for_each_active_peer(pcache->net, peer, slot) {
		spin_lock(&peer->sock_lock);
		if (peer->sock) {
			kernel_sock_shutdown(peer->sock, SHUT_RDWR);
			kicked++;
		}
		spin_unlock(&peer->sock_lock);
	}

	if (!kicked && pcache->net) {
		/* No connected sockets but we have configured peers — fine,
		 * they'll pick up NEED_FULL on next reconnect. */
		pcache_dev_info(pcache,
			"force-resync: gen=%llu full_sync flagged (no connected peers to kick)\n",
			bm->generation);
		return 0;
	}

	pcache_dev_info(pcache,
		"force-resync requested: gen=%llu full_sync flagged; %d socket(s) dropped\n",
		bm->generation, kicked);
	return 0;
}

/*
 * Queue a write replication request. Takes an additional reference on
 * @pcache_req so the bio survives until ACK arrives. Returns 0 on success
 * (caller must NOT call pcache_req_put on success path -- the ACK handler
 * does the final put), or negative errno on allocation failure (caller
 * still owns the ref and must put it).
 */
int pcache_net_submit_write(struct dm_pcache *pcache,
			    struct pcache_request *pcache_req,
			    u64 sector, u32 size)
{
	struct pcache_peer *peer;
	struct pcache_pending_req *req;

	if (!pcache->net)
		return -ENOTCONN;

	/* Step 2: still single-peer fan-out (slot 0 only). Step 7 will iterate
	 * over every connected slot and queue one pending_req per peer. */
	peer = pcache->net->peers[0];
	if (!peer || peer->state != PCACHE_C_CONNECTED)
		return -ENOTCONN;

	req = mempool_alloc(pcache->net->req_pool, GFP_NOIO);
	if (!req)
		return -ENOMEM;

	/* Caller (dm_pcache_map_bio) already took the extra reference. */
	req->pcache_req = pcache_req;
	req->sector = sector;
	req->size = size;
	req->block_id = (u64)atomic64_inc_return(&peer->next_block_id);
	req->sent = false;
	req->send_time = 0;

	spin_lock(&peer->pending_lock);
	list_add_tail(&req->list, &peer->pending_list);
	atomic_inc(&peer->pending_cnt);
	atomic_inc(&peer->unsent_cnt);
	spin_unlock(&peer->pending_lock);

	wake_up(&peer->wait_for_work);
	return 0;
}
