// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/net.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/socket.h>
#include <linux/uio.h>
#include <linux/bvec.h>
#include <linux/bio.h>
#include <net/sock.h>
#include <net/inet_common.h>
#include <net/tcp.h>
#include "dm_pcache.h"
#include "dm_pcache_bitmap.h"
#include "dm_pcache_net.h"

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

	/*
	 * Publish the connected socket under sock_lock so concurrent
	 * pcache_sock_get() callers observe a stable pointer.
	 */
	spin_lock(&peer->sock_lock);
	peer->sock = sock;
	peer->state = PCACHE_C_CONNECTED;
	spin_unlock(&peer->sock_lock);
	wake_up(&peer->wait_for_socket);
	return 0;

out_release:
	sock_release(sock);
	return err;
}

int pcache_create_server_socket(struct pcache_peer *peer, int port)
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

	err = sock->ops->listen(sock, 1);
	if (err)
		goto out_release;

	/* The listen socket's rcvbuf seeds accepted sockets. */
	sock_setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
			KERNEL_SOCKPTR(&bufsz), sizeof(bufsz));

	/* accept() should not time out while waiting for the peer. */
	sock->sk->sk_rcvtimeo = 60 * HZ;

	peer->listen_sock = sock;
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

int pcache_send_handshake(struct socket *sock, int node_id)
{
	struct pcache_packet_header hdr = {
		.type	= cpu_to_le16(PCACHE_P_HANDSHAKE),
		.length	= cpu_to_le16(PCACHE_HS_PKT_SIZE),
		.flags	= cpu_to_le32(PCACHE_F_HANDSHAKE),
	};
	struct pcache_handshake_pkt hs = {
		.protocol_version	= cpu_to_le16(PCACHE_PROTO_VERSION),
		.node_id		= cpu_to_le16(node_id),
	};
	u8 buf[sizeof(hdr) + sizeof(hs)];

	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &hs, sizeof(hs));
	return pcache_send_all(sock, buf, sizeof(buf));
}

int pcache_recv_handshake(struct socket *sock, int *peer_node_id)
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

/**
 * pcache_send_bio_pages - Zero-copy transmission of a BIO payload.
 *
 * Walks the bio segment-by-segment and pushes each page directly into the
 * TCP send queue via MSG_SPLICE_PAGES. No intermediate buffer, no memcpy.
 * Mirrors DRBD's _drbd_send_page() pattern.
 */
int pcache_send_bio_pages(struct socket *sock, struct bio *bio)
{
	struct bio_vec bvec;
	struct bvec_iter iter;

	bio_for_each_segment(bvec, bio, iter) {
		int len = bvec.bv_len;
		int offset = bvec.bv_offset;
		struct page *page = bvec.bv_page;

		while (len > 0) {
			struct msghdr msg = {
				.msg_flags = MSG_NOSIGNAL | MSG_SPLICE_PAGES,
			};
			struct bio_vec out;
			int sent;

			bvec_set_page(&out, page, len, offset);
			iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &out, 1, len);

			sent = sock_sendmsg(sock, &msg);
			if (sent <= 0)
				return sent ? sent : -ECONNRESET;

			len    -= sent;
			offset += sent;
		}
	}
	return 0;
}

void pcache_free_peer(struct pcache_peer *peer)
{
	struct pcache_pending_req *req, *tmp;

	if (peer->sock) {
		sock_release(peer->sock);
		peer->sock = NULL;
	}
	if (peer->listen_sock) {
		sock_release(peer->listen_sock);
		peer->listen_sock = NULL;
	}

	list_for_each_entry_safe(req, tmp, &peer->pending_list, list) {
		list_del(&req->list);
		if (peer->pcache->net && peer->pcache->net->req_pool)
			mempool_free(req, peer->pcache->net->req_pool);
		else
			kfree(req);
	}

	kfree(peer);
}

/**
 * pcache_sock_get - Snapshot the peer socket pointer under sock_lock.
 *
 * No additional reference is taken on the socket; the lock just makes the
 * read atomic with respect to writers swapping the pointer (connect /
 * disconnect). Lifetime is managed by the worker that owns @peer->sock.
 */
struct socket *pcache_sock_get(struct pcache_peer *peer)
{
	struct socket *sock;

	spin_lock(&peer->sock_lock);
	sock = peer->sock;
	spin_unlock(&peer->sock_lock);

	return sock;
}

void pcache_sock_put(struct pcache_peer *peer)
{
}
