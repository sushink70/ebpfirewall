/* SPDX-License-Identifier: GPL-2.0 */
/*
 * maps.h — Shared data structures between the XDP kernel program and the
 *           Rust control-plane.
 *
 * IMPORTANT: This header is included only from BPF C programs.
 * The Rust control plane (ctrl/src/maps.rs) defines bit-for-bit mirror
 * types manually using #[repr(C)].  Any change here must be mirrored there.
 *
 * Byte-order contract
 * -------------------
 * All multi-byte packet-derived fields (IP addresses, ports) are stored in
 * NETWORK byte order (big-endian) inside BPF maps.  This matches exactly
 * what the kernel provides from struct iphdr / tcphdr / udphdr.
 * The userspace must convert host-byte-order values before inserting keys.
 *
 * Padding contract
 * ----------------
 * Every struct has EXPLICIT padding fields (_pad[]).  The compiler must NOT
 * insert implicit padding.  The Rust mirror types replicate these exactly.
 * Verify with:  pahole kernel/xdp_firewall.bpf.o
 */

#pragma once

#include <linux/types.h>   /* __u8, __u16, __u32, __u64 */

/* ── Map capacity limits ───────────────────────────────────────────── */

#define FW_MAX_RULES        4096    /* max entries in fw_rules hash map    */
#define FW_MAX_BLOCKED_IPS  65536   /* max entries in blocked_ips LRU hash */
#define FW_STATS_ENTRIES    1       /* single per-CPU stats entry          */

/* ── Actions (stored as __u8 in map values) ─────────────────────────── */

#define FW_ACTION_PASS  0
#define FW_ACTION_DROP  1

/* ── Wildcard sentinel values ─────────────────────────────────────── */

#define FW_PROTO_ANY    0   /* proto == 0 → match any IP protocol  */
#define FW_PORT_ANY     0   /* port  == 0 → match any port         */
#define FW_IP_ANY       0   /* ip    == 0 → match any IP address   */

/*
 * rule_key — 5-tuple key for the fw_rules hash map.
 *
 * Fields set to the FW_*_ANY sentinel are treated as wildcards by the
 * rule-matching logic in xdp_firewall.c.  The BPF program performs up to
 * five ordered hash lookups (most-specific → least-specific); see the
 * xdp_fw_match_rule() comment for the full order.
 *
 * All IP and port fields: NETWORK byte order.
 * Size: 16 bytes, no compiler-inserted padding.
 */
struct rule_key {
    __u32 src_ip;       /* source IPv4,        0 = any       */
    __u32 dst_ip;       /* destination IPv4,   0 = any       */
    __u16 src_port;     /* L4 source port,     0 = any       */
    __u16 dst_port;     /* L4 destination port, 0 = any      */
    __u8  proto;        /* IP protocol number, 0 = any       */
    __u8  _pad[3];      /* explicit padding to 16-byte total */
};

/*
 * rule_val — Value stored per fw_rules entry.
 *
 * Size: 4 bytes.
 */
struct rule_val {
    __u8  action;   /* FW_ACTION_PASS or FW_ACTION_DROP */
    __u8  _pad[3];
};

/*
 * fw_stats — Per-CPU packet statistics.
 *
 * Stored in a BPF_MAP_TYPE_PERCPU_ARRAY at index 0.
 * Userspace reads all per-CPU copies and sums them.
 *
 * Using per-CPU eliminates atomic increments; each CPU writes its own
 * counters — the hot path never stalls on a cache line owned by another CPU.
 *
 * Size: 24 bytes.
 */
struct fw_stats {
    __u64 rx_packets;   /* every IPv4 packet that entered the program */
    __u64 passed;       /* packets returned XDP_PASS                  */
    __u64 dropped;      /* packets returned XDP_DROP                  */
};

/*
 * fw_config — Global run-time configuration.
 *
 * Stored in a BPF_MAP_TYPE_ARRAY at index 0 (single entry, all CPUs share).
 * Userspace writes this via map update; BPF reads it every packet.
 *
 * Size: 4 bytes.
 */
struct fw_config {
    __u8 default_action;  /* FW_ACTION_PASS or FW_ACTION_DROP         */
    __u8 enabled;         /* 0 = pass-through (bypass), 1 = active    */
    __u8 _pad[2];
};
