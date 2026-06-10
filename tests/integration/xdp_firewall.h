/* SPDX-License-Identifier: GPL-2.0
 *
 * xdp_firewall.h — types shared between the XDP kernel program and the
 * Rust control plane.  Every struct here is #[repr(C)] on the Rust side;
 * adding or reordering fields breaks the ABI silently, so treat this file
 * as the single source of truth.
 */
#pragma once

#include <linux/types.h>

/* ─── tuneable limits ─────────────────────────────────────────────────── */
#define FW_MAX_RULES      1024
#define FW_MAX_BLOCKLIST  65536
#define FW_MAX_PORT_RULES 512

/* ─── stat-array indices ─────────────────────────────────────────────── */
#define STAT_TOTAL   0   /* every packet seen */
#define STAT_PASSED  1   /* explicit PASS or default-pass */
#define STAT_DROPPED 2   /* explicit DROP or blocklist hit */
#define STAT_ERRORS  3   /* malformed / truncated headers */
#define STAT_MAX     4   /* array size – must equal number of STAT_* above */

/* ─── action values ──────────────────────────────────────────────────── */
#define ACTION_PASS  0
#define ACTION_DROP  1

/* ─── protocol shortcuts (equal to IPPROTO_* values) ────────────────── */
#define PROTO_ANY  0    /* wildcard – matches any L4 protocol */
#define PROTO_ICMP 1
#define PROTO_TCP  6
#define PROTO_UDP  17

/* ─── map key / value structs ────────────────────────────────────────── */

/*
 * LPM-trie key for the IPv4 blocklist.
 *
 * The BPF LPM-trie key format is:
 *   { __u32 prefixlen;  __u8 data[]; }
 *
 * For IPv4 the "data" field is a 4-byte address in *network* byte order.
 * prefixlen counts bits from the MSB of data[0], so a /24 means
 * bits 0-23 must match.
 *
 * Lookup key must always use prefixlen = 32 (exact host address);
 * the trie returns the longest matching stored prefix.
 */
struct lpm_v4_key {
    __u32 prefixlen;   /* 0-32 */
    __u32 ip;          /* network byte order */
};

/*
 * Port-level rule key.
 * Allows blocking/allowing an entire protocol (e.g. all TCP:22)
 * without specifying src/dst IPs.
 *
 * dst_port == 0 is a wildcard that matches all ports of the given proto.
 * All fields in **host** byte order to match the XDP program logic.
 */
struct port_rule_key {
    __u8  proto;       /* PROTO_* */
    __u8  _pad;        /* explicit pad — never leave implicit holes */
    __u16 dst_port;    /* host byte order; 0 = any */
};

/*
 * Full 5-tuple rule key.
 *
 * Convention (same as port_rule_key):
 *  - IPs in network byte order
 *  - ports in host byte order
 *  - 0 in any field is NOT a wildcard at this level; wildcards are
 *    handled by the user-space layer inserting multiple hash entries.
 */
struct fw_rule_key {
    __u32 src_ip;      /* network byte order */
    __u32 dst_ip;      /* network byte order */
    __u16 src_port;    /* host byte order */
    __u16 dst_port;    /* host byte order */
    __u8  proto;       /* PROTO_* */
    __u8  _pad[3];     /* explicit pad to reach 16 bytes, cache-line friendly */
};

/* Value stored for every rule */
struct fw_rule_val {
    __u8  action;      /* ACTION_PASS or ACTION_DROP */
    __u8  _pad[3];
};

/*
 * Per-CPU statistics entry.
 * Stored in a PERCPU_ARRAY so updates need no atomic ops.
 * User space must aggregate across all CPUs.
 */
struct fw_stat {
    __u64 packets;
    __u64 bytes;
};

/*
 * Global firewall config (ARRAY map, single entry at index 0).
 */
struct fw_config {
    __u8  default_action;  /* ACTION_PASS or ACTION_DROP */
    __u8  _pad[3];
};
