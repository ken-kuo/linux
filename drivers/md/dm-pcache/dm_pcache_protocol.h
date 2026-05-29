/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _DM_PCACHE_PROTOCOL_H
#define _DM_PCACHE_PROTOCOL_H

#include <linux/types.h>

/*
 * Protocol version history:
 *   1 — Phase 1: HANDSHAKE / DATA / ACK / NACK only, full-resync on every reconnect
 *   2 — Phase 2: + SYNC_PARAMS / BITMAP / RS_DATA / RS_ACK / OUT_OF_SYNC / PING
 *   3 — Phase 3.5: + DEMOTE_REQ / DEMOTE_ACK for coordinated promote/demote
 */
#define PCACHE_PROTO_VERSION	3
#define PCACHE_DEFAULT_PORT	9764

enum pcache_conn_state {
	PCACHE_C_STANDALONE	= 0,
	PCACHE_C_CONNECTING	= 1,
	PCACHE_C_CONNECTED	= 2,
	PCACHE_C_DISCONNECTING	= 3,	/* reserved; not written by Phase 1 */
};

/*
 * Replication state — separate dimension from conn_state.
 * Tracks the data-sync subprotocol while the underlying TCP is C_CONNECTED.
 */
enum pcache_repl_state {
	PCACHE_R_OFF              = 0,	/* not connected */
	PCACHE_R_NEGOTIATING      = 1,	/* P_SYNC_PARAMS in flight */
	PCACHE_R_BITMAP_EXCHANGE  = 2,	/* swapping bitmaps */
	PCACHE_R_SYNC_SOURCE      = 3,	/* this node sending resync data */
	PCACHE_R_SYNC_TARGET      = 4,	/* this node receiving resync data */
	PCACHE_R_ESTABLISHED      = 5,	/* fully in sync, normal I/O */
	PCACHE_R_HALT             = 6,	/* split-brain or fatal mismatch */
};

enum pcache_packet_type {
	/* Phase 1 packets (proto v1+) */
	PCACHE_P_HANDSHAKE	= 0x00,
	PCACHE_P_DATA		= 0x01,
	PCACHE_P_ACK		= 0x02,
	PCACHE_P_NACK		= 0x03,

	/* Phase 2 packets (proto v2+) */
	PCACHE_P_SYNC_PARAMS	= 0x10,	/* exchange generation + bitmap geometry */
	PCACHE_P_BITMAP		= 0x11,	/* compressed bitmap chunk */
	PCACHE_P_BITMAP_DONE	= 0x12,	/* end-of-bitmap marker */
	PCACHE_P_RS_DATA	= 0x13,	/* resync payload block */
	PCACHE_P_RS_ACK		= 0x14,	/* resync chunk acked */
	PCACHE_P_OUT_OF_SYNC	= 0x15,	/* mark region dirty on peer */
	PCACHE_P_PING		= 0x20,	/* keep-alive request */
	PCACHE_P_PING_ACK	= 0x21,	/* keep-alive reply */

	/* Phase 3.5 — coordinated demote handshake (B1) */
	PCACHE_P_DEMOTE_REQ	= 0x30,	/* peer asks me to demote (it wants to promote) */
	PCACHE_P_DEMOTE_ACK	= 0x31,	/* I have demoted (or was already Secondary) */
};

/* Phase 3.5 — body for P_DEMOTE_REQ / P_DEMOTE_ACK (carried inline after
 * pcache_packet_header). */
struct pcache_demote_pkt {
	__le32	initiator_node_id;	/* who is about to promote */
	__le32	flags;			/* reserved for force / status hints */
	__le32	status;			/* on ACK: 0 = OK, !=0 = errno */
	__le32	reserved;
} __packed;

#define PCACHE_DM_PKT_SIZE	sizeof(struct pcache_demote_pkt)

#define PCACHE_F_ACK		BIT(0)
#define PCACHE_F_HANDSHAKE	BIT(1)

/*
 * Wire format: all multi-byte fields are little-endian on the wire.
 * Producers fill via cpu_to_le*(); consumers read via le*_to_cpu().
 */
struct pcache_packet_header {
	__le16	type;
	__le16	length;		/* payload bytes following the header */
	__le64	sector;
	__le64	block_id;	/* opaque request id echoed back in ACK */
	__le32	size;		/* logical I/O size in bytes */
	__le32	flags;
} __packed;

struct pcache_handshake_pkt {
	__le16	protocol_version;
	__le16	node_id;
	__le32	stream_id;	/* Cut C: 0 = control stream, 1.. = data-only */
} __packed;

/* Phase 2 — sent immediately after handshake to negotiate resync direction */
struct pcache_sync_params_pkt {
	__le64	generation;		/* my current data generation */
	__le64	peer_gen_at_sync;	/* peer's gen recorded at my last clean sync */
	__le64	bm_nr_bits;
	__le32	bm_block_shift;		/* expected to match on both sides */
	__le32	flags;
} __packed;

#define PCACHE_SP_F_NEED_FULL	BIT(0)	/* request full bitmap (fresh peer) */

/* Phase 2 — bitmap chunk in BITMAP_EXCHANGE phase */
struct pcache_bitmap_pkt {
	__le64	start_bit;
	__le32	nr_bits;
	__le32	encoding;		/* 0 = raw, 1 = RLE */
	/* followed by encoded payload */
} __packed;

#define PCACHE_BM_ENC_RAW	0
#define PCACHE_BM_ENC_RLE	1
#define PCACHE_BM_ENC_RUN	2	/* Step 6: contiguous set-bits run, no payload */

/* Phase 2 — single resync data block (header is reused: pcache_packet_header) */
struct pcache_rs_data_pkt {
	__le64	start_bit;		/* first bit covered */
	__le32	nr_bits;		/* contiguous bits being synced */
	__le32	size;			/* payload bytes (= nr_bits * BLOCK_SIZE) */
	__le64	block_id;
	/* followed by `size` bytes of payload */
} __packed;

#define PCACHE_PKT_HEADER_SIZE	sizeof(struct pcache_packet_header)
#define PCACHE_HS_PKT_SIZE	sizeof(struct pcache_handshake_pkt)
#define PCACHE_SP_PKT_SIZE	sizeof(struct pcache_sync_params_pkt)
#define PCACHE_BM_PKT_SIZE	sizeof(struct pcache_bitmap_pkt)
#define PCACHE_RS_PKT_SIZE	sizeof(struct pcache_rs_data_pkt)

#endif /* _DM_PCACHE_PROTOCOL_H */
