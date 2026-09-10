// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/socket.h>
#include <linux/uio.h>
#include <linux/bvec.h>
#include <linux/bio.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/inet_common.h>
#include <net/tcp.h>
#include "dm_pcache.h"
#include "dm_pcache_bitmap.h"
#include "dm_pcache_net.h"

void pcache_peer_bitmap_exit(struct pcache_peer *peer);	/* forward decl */

/*
 * Fully transmit @size bytes from @buf over @sock.
 *
 * Returns 0 on success, negative errno on hard failure. Loops over partial
 * sends. Caller must be in a sleepable context.
 */
int pcache_send_all(struct socket *sock, const void *buf, int size)
{
	struct msghdr msg = { .msg_flags = MSG_NOSIGNAL };
	struct kvec iov;
	int ret, left = size;

	while (left > 0) {
		iov.iov_base = (void *)buf + (size - left);
		iov.iov_len  = left;
		ret = kernel_sendmsg(sock, &msg, &iov, 1, left);
		if (ret < 0)
			return ret;
		if (ret == 0)
			return -ECONNRESET;
		left -= ret;
	}
	return 0;
}

/*
 * Fully receive @size bytes into @buf from @sock.
 *
 * Returns 0 on full success, -ECONNRESET if peer closed mid-stream, or the
 * underlying errno (e.g. -EAGAIN if nothing has been read yet). Once a
 * partial read has begun, blocks until completion (treating EAGAIN/EINTR as
 * a transient backoff).
 */
int pcache_recv_all(struct socket *sock, void *buf, int size)
{
	struct msghdr msg = { .msg_flags = MSG_NOSIGNAL };
	struct kvec iov;
	int ret, left = size;

	while (left > 0) {
		iov.iov_base = buf + (size - left);
		iov.iov_len  = left;
		iov_iter_kvec(&msg.msg_iter, ITER_DEST, &iov, 1, left);
		ret = sock_recvmsg(sock, &msg, msg.msg_flags);
		if (ret == 0)
			return -ECONNRESET;
		if (ret < 0) {
			if (ret == -EAGAIN || ret == -EWOULDBLOCK || ret == -EINTR) {
				if (left == size)
					return ret;
				/* mid-packet: brief backoff and continue */
				set_current_state(TASK_INTERRUPTIBLE);
				schedule_timeout(HZ / 4);
				__set_current_state(TASK_RUNNING);
				continue;
			}
			return ret;
		}
		left -= ret;
	}
	return 0;
}

static int pcache_set_sockaddr(struct sockaddr_in *addr, const char *ip_port,
			       int default_port)
{
	char *colon, ip[64];
	int port = default_port;

	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;

	strscpy(ip, ip_port, sizeof(ip));
	colon = strchr(ip, ':');
	if (colon) {
		*colon = '\0';
		if (kstrtoint(colon + 1, 10, &port) < 0 || port <= 0)
			return -EINVAL;
	}

	addr->sin_port = htons(port);
	if (in4_pton(ip, strlen(ip), (u8 *)&addr->sin_addr.s_addr, -1, NULL) <= 0)
		return -EINVAL;
	return 0;
}

void pcache_apply_socket_opts(struct socket *sock)
{
	int one = 1;
	int bufsz = PCACHE_SOCK_BUF_BYTES;
	/*
	 * Aggressive keepalive timings so silent-failure connections (e.g.
	 * peer's iptables/nft drops packets without RST, leaving us with a
	 * TCP retransmit queue that never completes) get RST within ~50 s.
	 *
	 * Total worst-case death detection = KEEPIDLE + KEEPCNT × KEEPINTVL.
	 */
	int keep_idle  = 20;	/* seconds idle before first probe */
	int keep_intvl = 10;	/* seconds between probes */
	int keep_cnt   = 3;	/* probes before dropping the connection */

	sock->ops->setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
			      KERNEL_SOCKPTR(&one), sizeof(one));
	sock->ops->setsockopt(sock, IPPROTO_TCP, TCP_QUICKACK,
			      KERNEL_SOCKPTR(&one), sizeof(one));
	sock_setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
			KERNEL_SOCKPTR(&bufsz), sizeof(bufsz));
	sock_setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
			KERNEL_SOCKPTR(&bufsz), sizeof(bufsz));

	sock_setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE,
			KERNEL_SOCKPTR(&one), sizeof(one));
	sock->ops->setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE,
			      KERNEL_SOCKPTR(&keep_idle), sizeof(keep_idle));
	sock->ops->setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL,
			      KERNEL_SOCKPTR(&keep_intvl), sizeof(keep_intvl));
	sock->ops->setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,
			      KERNEL_SOCKPTR(&keep_cnt), sizeof(keep_cnt));
}

int pcache_create_peer_socket(struct pcache_peer *peer)
{
	struct sockaddr_in addr;
	struct socket *sock = NULL;
	int err;

	pcache_info("connecting to %s\n", peer->remote_addr);

	err = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (err)
		return err;

	err = pcache_set_sockaddr(&addr, peer->remote_addr, 0);
	if (err)
		goto out_release;

	sock->sk->sk_rcvtimeo = 10 * HZ;
	sock->sk->sk_sndtimeo = 10 * HZ;

	err = kernel_connect(sock, (struct sockaddr_unsized *)&addr, sizeof(addr), 0);
	if (err) {
		pcache_err("connect to %s failed: %d\n", peer->remote_addr, err);
		goto out_release;
	}

	pcache_apply_socket_opts(sock);

	/* Publish the connected socket as a refcounted conn. */
	err = pcache_conn_install(&peer->streams[0], sock);
	if (err)
		goto out_release;
	peer->state = PCACHE_C_CONNECTED;
	wake_up(&peer->streams[0].wait_for_socket);
	return 0;

out_release:
	sock_release(sock);
	return err;
}

/*
 * Cut C — connect one extra data stream (stream_id > 0) to the peer and run
 * the handshake carrying its stream_id. Stores the socket in st->sock; unlike
 * the control stream it does not touch peer->state. Initiator side only.
 */
int pcache_connect_data_stream(struct pcache_stream *st)
{
	struct pcache_peer *peer = st->peer;
	struct sockaddr_in addr;
	struct socket *sock = NULL;
	int err;

	err = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (err)
		return err;

	err = pcache_set_sockaddr(&addr, peer->remote_addr, 0);
	if (err)
		goto out_release;

	sock->sk->sk_rcvtimeo = 10 * HZ;
	sock->sk->sk_sndtimeo = 10 * HZ;

	err = kernel_connect(sock, (struct sockaddr_unsized *)&addr, sizeof(addr), 0);
	if (err)
		goto out_release;

	pcache_apply_socket_opts(sock);

	err = pcache_send_handshake(sock, (int)peer->pcache->my_node_id,
				    st->stream_id);
	if (err)
		goto out_release;
	err = pcache_recv_handshake(sock, NULL, NULL);
	if (err)
		goto out_release;

	pcache_sock_set_timeout(sock, 300 * HZ);

	err = pcache_conn_install(st, sock);
	if (err)
		goto out_release;
	return 0;

out_release:
	sock_release(sock);
	return err;
}

int pcache_create_listen_socket(struct socket **sock_out, int port)
{
	struct sockaddr_in addr;
	struct socket *sock = NULL;
	int err, one = 1;
	int bufsz = PCACHE_SOCK_BUF_BYTES;

	err = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (err)
		return err;

	err = sock_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
			      KERNEL_SOCKPTR(&one), sizeof(one));
	if (err)
		goto out_release;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	err = kernel_bind(sock, (struct sockaddr_unsized *)&addr, sizeof(addr));
	if (err)
		goto out_release;

	/* Phase 3 — backlog large enough for N=32 mesh (multiple peers might
	 * race to connect during a coordinated restart). */
	err = sock->ops->listen(sock, PCACHE_BM_MAX_PEERS);
	if (err)
		goto out_release;

	/* The listen socket's rcvbuf seeds accepted sockets. */
	sock_setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
			KERNEL_SOCKPTR(&bufsz), sizeof(bufsz));

	/* Bounded accept timeout so the accept_thread can react to
	 * kthread_should_stop() without long stalls. */
	sock->sk->sk_rcvtimeo = 5 * HZ;

	*sock_out = sock;
	return 0;

out_release:
	sock_release(sock);
	return err;
}

void pcache_sock_set_timeout(struct socket *sock, unsigned long timeout_jiffies)
{
	sock->sk->sk_rcvtimeo = timeout_jiffies;
	sock->sk->sk_sndtimeo = timeout_jiffies;
}

int pcache_send_packet(struct socket *sock, struct pcache_packet_header *hdr)
{
	return pcache_send_all(sock, hdr, sizeof(*hdr));
}

int pcache_send_handshake(struct socket *sock, int node_id, int stream_id)
{
	struct pcache_packet_header hdr = {
		.type	= cpu_to_le16(PCACHE_P_HANDSHAKE),
		.length	= cpu_to_le16(PCACHE_HS_PKT_SIZE),
		.flags	= cpu_to_le32(PCACHE_F_HANDSHAKE),
	};
	struct pcache_handshake_pkt hs = {
		.protocol_version	= cpu_to_le16(PCACHE_PROTO_VERSION),
		.node_id		= cpu_to_le16(node_id),
		.stream_id		= cpu_to_le32(stream_id),
	};
	u8 buf[sizeof(hdr) + sizeof(hs)];

	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &hs, sizeof(hs));
	return pcache_send_all(sock, buf, sizeof(buf));
}

int pcache_recv_handshake(struct socket *sock, int *peer_node_id, int *stream_id)
{
	struct pcache_packet_header hdr;
	struct pcache_handshake_pkt hs;
	int ret;

	ret = pcache_recv_all(sock, &hdr, sizeof(hdr));
	if (ret)
		return ret;
	if (le16_to_cpu(hdr.type) != PCACHE_P_HANDSHAKE ||
	    !(le32_to_cpu(hdr.flags) & PCACHE_F_HANDSHAKE) ||
	    le16_to_cpu(hdr.length) != PCACHE_HS_PKT_SIZE)
		return -EINVAL;

	ret = pcache_recv_all(sock, &hs, sizeof(hs));
	if (ret)
		return ret;
	if (le16_to_cpu(hs.protocol_version) != PCACHE_PROTO_VERSION)
		return -EPROTONOSUPPORT;

	if (peer_node_id)
		*peer_node_id = le16_to_cpu(hs.node_id);
	if (stream_id)
		*stream_id = le32_to_cpu(hs.stream_id);
	return 0;
}

/* Phase 2 — exchange data-replication parameters right after handshake. */
int pcache_send_sync_params(struct socket *sock, u64 generation,
			    u64 peer_gen_at_sync, u64 bm_nr_bits, u32 flags)
{
	struct pcache_packet_header hdr = {
		.type	= cpu_to_le16(PCACHE_P_SYNC_PARAMS),
		.length	= cpu_to_le16(PCACHE_SP_PKT_SIZE),
	};
	struct pcache_sync_params_pkt sp = {
		.generation        = cpu_to_le64(generation),
		.peer_gen_at_sync  = cpu_to_le64(peer_gen_at_sync),
		.bm_nr_bits        = cpu_to_le64(bm_nr_bits),
		.bm_block_shift    = cpu_to_le32(PCACHE_BM_BLOCK_SHIFT),
		.flags             = cpu_to_le32(flags),
	};
	u8 buf[sizeof(hdr) + sizeof(sp)];

	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &sp, sizeof(sp));
	return pcache_send_all(sock, buf, sizeof(buf));
}

int pcache_recv_sync_params(struct socket *sock,
			    struct pcache_sync_params_pkt *out)
{
	struct pcache_packet_header hdr;
	int ret;

	ret = pcache_recv_all(sock, &hdr, sizeof(hdr));
	if (ret)
		return ret;
	if (le16_to_cpu(hdr.type) != PCACHE_P_SYNC_PARAMS ||
	    le16_to_cpu(hdr.length) != PCACHE_SP_PKT_SIZE)
		return -EINVAL;

	return pcache_recv_all(sock, out, sizeof(*out));
}

/*
 * Zero-copy completion context, one per bio transmission.
 *
 * MSG_ZEROCOPY is the only flag that gives a kernel sender both halves of what
 * this path needs:
 *
 *   - it takes a real page reference. Its fill routine is
 *     zerocopy_fill_skb_from_iter() -> iov_iter_get_pages2(), whose bvec branch
 *     does folio_get() on every page. MSG_SPLICE_PAGES goes through
 *     iov_iter_extract_pages() instead, whose bvec branch only copies the
 *     pointers -- that is fix-history #20;
 *
 *   - it says when transmission is done, through ubuf_info::ops->complete().
 *     That is the signal this driver never had: sock_sendmsg() returning says
 *     nothing about whether the NIC is finished with the pages.
 *
 * We hold a pcache_request reference for the whole transmission and drop it in
 * the completion, so bio_endio() cannot run -- and the pages cannot be handed
 * back to the submitter and rewritten -- while an skb still points at them.
 * Every early-completion path (ACK timeout, flush on link death, teardown) is
 * then safe by construction rather than by timing.
 *
 * Refcount contract, following io_uring's notif.c: initialise to 1 for
 * ourselves, let the stack take its own reference per skb, and decrement in
 * complete() -- net_zcopy_put() calls complete() directly, it does not
 * decrement.
 */
struct pcache_zc {
	struct ubuf_info	uarg;
	struct pcache_request	*preq;
};

static void pcache_zc_complete(struct sk_buff *skb, struct ubuf_info *uarg,
			       bool success)
{
	struct pcache_zc *zc = container_of(uarg, struct pcache_zc, uarg);

	if (!refcount_dec_and_test(&uarg->refcnt))
		return;

	/*
	 * Status 0 even when success is false: a failed transmission is not a
	 * failed write. The payload is already in the local cache and the range
	 * is on the dirty bitmap, so the resync carries it -- the same reasoning
	 * as every other completion path here (fix-history #4, #13).
	 */
	pcache_req_put(zc->preq, 0);
	kfree(zc);
}

static const struct ubuf_info_ops pcache_zc_ops = {
	.complete = pcache_zc_complete,
};

/*
 * Slab pages get no reference from iov_iter_get_pages2() -- it skips
 * folio_get() for them -- so they must not go out zero-copy. DRBD checks the
 * same thing in drbd_send_bio(): "e.g. XFS meta- & log-data is in slab pages".
 */
static bool pcache_bio_zc_ok(struct bio *bio)
{
	struct bvec_iter iter;
	struct bio_vec bvec;

	bio_for_each_segment(bvec, bio, iter)
		if (!sendpage_ok(bvec.bv_page))
			return false;
	return true;
}

/**
 * pcache_send_bio_pages - Transmit a BIO payload.
 *
 * Walks the bio segment-by-segment and hands each page to TCP, zero-copy when
 * the bio allows it.
 *
 * This once passed MSG_SPLICE_PAGES, which is only legal while the caller keeps
 * the pages alive until the skb is released -- with an ITER_BVEC source the
 * network stack takes no reference of its own (iov_iter_extract_bvec_pages()
 * just copies the page pointers). These pages belong to a bio that is completed
 * as soon as the request is retired, which can happen the moment sock_sendmsg()
 * returns, so the NIC could be left transmitting from freed or reused memory
 * (fix-history #20).
 *
 * That is not theoretical: regression 12 caught a single
 * "enp1s0: bad gso: type: 1b, size: 0" 10 ms before the peer started reading
 * the byte stream as garbage, and a VM_BUG_ON_PAGE(page_ref_count == 0) 37 s
 * later.
 *
 * MSG_ZEROCOPY fixes both halves of that: the stack references the pages, and
 * it tells us when it is done with them. A bio that cannot go out zero-copy
 * (slab pages, or no request to pin) falls back to letting TCP copy, which is
 * always safe because the pages are ours again as soon as the call returns.
 */
int pcache_send_bio_pages(struct socket *sock, struct bio *bio,
			  struct pcache_request *preq)
{
	struct bio_vec bvec;
	struct bvec_iter iter;
	struct pcache_zc *zc = NULL;
	unsigned int zc_flag = 0;
	int err = 0;

	/*
	 * Zero-copy only when we can both pin the pages and be told when the
	 * NIC is done with them. Anything else falls back to letting TCP copy,
	 * which is always safe because the pages are ours again as soon as
	 * sock_sendmsg() returns.
	 */
	if (preq && pcache_bio_zc_ok(bio)) {
		zc = kzalloc(sizeof(*zc), GFP_NOIO);
		if (zc) {
			zc->uarg.ops = &pcache_zc_ops;
			/*
			 * skb_zcopy_init() does `shinfo->flags |= uarg->flags`,
			 * and skb_zcopy() -- the test skb_zcopy_clear() uses on
			 * release -- keys off SKBFL_ZEROCOPY_ENABLE in those
			 * flags. Leaving uarg.flags at 0 (kzalloc) means the
			 * skb never looks zero-copy, so complete() is never
			 * called even though net_zcopy_get() already took a
			 * reference: the request is pinned forever and the bio
			 * never ends. Same value io_uring uses for its notif
			 * (IO_NOTIF_UBUF_FLAGS in io_uring/notif.h).
			 */
			zc->uarg.flags = SKBFL_ZEROCOPY_FRAG | SKBFL_DONT_ORPHAN;
			refcount_set(&zc->uarg.refcnt, 1);
			zc->preq = preq;
			pcache_req_get(preq);
			zc_flag = MSG_ZEROCOPY;
		}
	}

	bio_for_each_segment(bvec, bio, iter) {
		int len = bvec.bv_len;
		int offset = bvec.bv_offset;
		struct page *page = bvec.bv_page;
		/*
		 * MSG_MORE on every segment but the last, as DRBD's
		 * dtt_send_bio() does. Without it each segment is pushed on its
		 * own, which for a 128 KiB bio means a separate skb per page --
		 * and with MSG_ZEROCOPY each of those skbs takes its own uarg
		 * reference.
		 */
		unsigned int more = bio_iter_last(bvec, iter) ? 0 : MSG_MORE;

		while (len > 0) {
			struct msghdr msg = {
				.msg_flags = MSG_NOSIGNAL | zc_flag | more,
				.msg_ubuf = zc ? &zc->uarg : NULL,
			};
			struct bio_vec out;
			int sent;

			bvec_set_page(&out, page, len, offset);
			iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &out, 1, len);

			sent = sock_sendmsg(sock, &msg);
			if (sent <= 0) {
				err = sent ? sent : -ECONNRESET;
				goto out;
			}

			len    -= sent;
			offset += sent;
		}
	}
out:
	/*
	 * Drop the reference we initialised the uarg with. The stack holds its
	 * own per-skb references, so complete() -- and with it the request's
	 * release -- fires only once the last skb carrying these pages is gone.
	 * On the error path this is equally correct: whatever the stack already
	 * queued still keeps the pages pinned until it is done.
	 */
	if (zc)
		net_zcopy_put(&zc->uarg);
	return err;
}

/*
 * Phase 3.5 B1 — coordinated demote handshake.
 *
 * Promoter sends P_DEMOTE_REQ to each connected peer; each peer that is
 * currently Primary demotes locally then replies P_DEMOTE_ACK with
 * status=0. Already-Secondary peers ACK immediately (no-op).
 */
int pcache_send_demote_req(struct socket *sock, u32 initiator_node_id, u32 flags)
{
	struct pcache_packet_header hdr = {
		.type	= cpu_to_le16(PCACHE_P_DEMOTE_REQ),
		.length	= cpu_to_le16(PCACHE_DM_PKT_SIZE),
	};
	struct pcache_demote_pkt body = {
		.initiator_node_id = cpu_to_le32(initiator_node_id),
		.flags             = cpu_to_le32(flags),
	};
	u8 buf[sizeof(hdr) + sizeof(body)];

	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &body, sizeof(body));
	return pcache_send_all(sock, buf, sizeof(buf));
}

int pcache_send_demote_ack(struct socket *sock, u32 status)
{
	struct pcache_packet_header hdr = {
		.type	= cpu_to_le16(PCACHE_P_DEMOTE_ACK),
		.length	= cpu_to_le16(PCACHE_DM_PKT_SIZE),
	};
	struct pcache_demote_pkt body = {
		.status = cpu_to_le32(status),
	};
	u8 buf[sizeof(hdr) + sizeof(body)];

	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &body, sizeof(body));
	return pcache_send_all(sock, buf, sizeof(buf));
}

int pcache_recv_demote_body(struct socket *sock, struct pcache_demote_pkt *out)
{
	return pcache_recv_all(sock, out, sizeof(*out));
}

void pcache_free_peer(struct pcache_peer *peer)
{
	struct pcache_pending_req *req, *tmp;
	int i, leaked = 0;

	/*
	 * Last-resort drain. Every caller is supposed to have run
	 * pcache_flush_pending() after stopping this peer's threads, so these
	 * lists are normally empty -- but if one ever is not, the reqs each
	 * hold a reference on a pcache_request, and freeing them without
	 * putting it strands the bio (never completed, inflight_reqs never
	 * decremented, so dtr then hangs on inflight_wq).
	 *
	 * Done before pcache_peer_bitmap_exit() so the range can still be
	 * marked dirty, and across every stream -- the old code only walked
	 * streams[0], leaking the data streams' lists outright at nr_streams > 1.
	 *
	 * Status 0, not -EIO, for the same reason as the flush paths in
	 * dm_pcache_state.c: the payload is already committed to the local
	 * cache and the range is marked dirty, so the resync carries it.
	 */
	for (i = 0; i < peer->nr_streams; i++) {
		struct pcache_stream *st = &peer->streams[i];
		LIST_HEAD(drain);

		spin_lock(&st->pending_lock);
		list_splice_init(&st->pending_list, &drain);
		atomic_set(&st->pending_cnt, 0);
		atomic_set(&st->unsent_cnt, 0);
		spin_unlock(&st->pending_lock);

		list_for_each_entry_safe(req, tmp, &drain, list) {
			list_del(&req->list);
			pcache_bm_set_range(peer->bm, req->sector, req->size);
			pcache_req_put(req->pcache_req, 0);
			if (peer->pcache->net && peer->pcache->net->req_pool)
				mempool_free(req, peer->pcache->net->req_pool);
			else
				kfree(req);
			leaked++;
		}
	}
	if (leaked)
		pcache_err("free_peer: drained %d pending req(s) the caller should have flushed\n",
			   leaked);

	/* Phase 3.5 — defensive bitmap release in case the explicit
	 * pcache_peer_bitmap_exit() in remove-peer / net_stop was skipped. */
	pcache_peer_bitmap_exit(peer);

	/* Defensively drop any conn still attached to any stream. */
	for (i = 0; i < peer->nr_streams; i++)
		pcache_conn_drop(&peer->streams[i]);

	kfree(peer);
}

/*
 * Cut C — refcounted per-stream socket lifetime.
 *
 * The whole class of teardown use-after-frees came from threads holding a raw
 * socket pointer across a blocking recv/send while another thread freed it.
 * Now a user takes a reference for the duration of its I/O; the socket is
 * released only when the last reference is dropped.
 */
static void pcache_conn_release(struct kref *kref)
{
	struct pcache_conn *c = container_of(kref, struct pcache_conn, ref);

	sock_release(c->sock);
	kfree(c);
}

/* Take a reference on the stream's current connection (or NULL if none). */
struct pcache_conn *pcache_conn_get(struct pcache_stream *st)
{
	struct pcache_conn *c;

	spin_lock(&st->sock_lock);
	c = st->conn;
	if (c)
		kref_get(&c->ref);
	spin_unlock(&st->sock_lock);

	return c;
}

void pcache_conn_put(struct pcache_conn *c)
{
	if (c)
		kref_put(&c->ref, pcache_conn_release);
}

/*
 * Install a freshly connected socket on the stream. The conn starts with one
 * reference (the "installed" ref, held by the stream until pcache_conn_drop).
 * On -ENOMEM the caller still owns @sock and must release it.
 */
int pcache_conn_install(struct pcache_stream *st, struct socket *sock)
{
	struct pcache_conn *c;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	kref_init(&c->ref);	/* install ref */
	c->sock = sock;

	spin_lock(&st->sock_lock);
	st->conn = c;
	spin_unlock(&st->sock_lock);
	return 0;
}

/*
 * Detach the current connection from the stream, shut its socket down to wake
 * any blocked recv/send, and drop the install reference. Threads currently
 * using the socket hold their own references, so it survives until they put.
 */
void pcache_conn_drop(struct pcache_stream *st)
{
	struct pcache_conn *c;

	spin_lock(&st->sock_lock);
	c = st->conn;
	st->conn = NULL;
	spin_unlock(&st->sock_lock);

	if (c) {
		kernel_sock_shutdown(c->sock, SHUT_RDWR);
		pcache_conn_put(c);	/* drop install ref */
	}
}

/* Shut the current connection's socket down without detaching it. */
void pcache_conn_shutdown(struct pcache_stream *st)
{
	struct pcache_conn *c = pcache_conn_get(st);

	if (c) {
		kernel_sock_shutdown(c->sock, SHUT_RDWR);
		pcache_conn_put(c);
	}
}

bool pcache_stream_has_conn(struct pcache_stream *st)
{
	bool has;

	spin_lock(&st->sock_lock);
	has = st->conn != NULL;
	spin_unlock(&st->sock_lock);

	return has;
}
