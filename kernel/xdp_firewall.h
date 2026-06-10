/* firewall.h
 *
 * Shared header: map key/value structs used by both the XDP C program
 * and by user-space tools (via vmlinux.h or manual struct mirroring).
 *
 * All structs that go into BPF maps must be:
 *   - Fixed-size (no pointers, no flexible arrays)
 *   - Packed or naturally aligned (no hidden padding bytes)
 *   - Mirrored exactly in Rust/Go user-space code
 */

#ifndef XDP_FIREWALL_H
#define XDP_FIREWALL_H

#include <linux/types.h>    /* __u8, __u16, __u32, __u64 — kernel integer types */

/* ─────────────────────────────────────────────────────────────────────────
 * LPM (Longest Prefix Match) key for IPv4 CIDR blocking.
 *
 * The BPF_MAP_TYPE_LPM_TRIE key format is MANDATORY:
 *   - First 4 bytes = prefix length in bits (e.g., 24 for /24)
 *   - Remaining bytes = the data (the IP address, big-endian)
 *
 * This struct mirrors the kernel's struct bpf_lpm_trie_key exactly.
 * ─────────────────────────────────────────────────────────────────────────
 */
struct lpm_key {
    __u32 prefixlen;    /* prefix length: 0–32 for IPv4 */
    __u32 addr;         /* IPv4 address in network byte order (big-endian) */
};

/* ─────────────────────────────────────────────────────────────────────────
 * Per-IP statistics entry stored in the stats map.
 *
 * The XDP program increments these atomically using __sync_fetch_and_add.
 * User space reads them periodically for monitoring dashboards.
 * ─────────────────────────────────────────────────────────────────────────
 */
struct ip_stats {
    __u64 packets_dropped;   /* total packets dropped from this source IP */
    __u64 packets_passed;    /* total packets passed from this source IP */
    __u64 bytes_dropped;     /* total bytes dropped */
    __u64 bytes_passed;      /* total bytes passed */
};

/* ─────────────────────────────────────────────────────────────────────────
 * Port rule value stored in the allow_ports map.
 *
 * Key = __u16 port number (big-endian).
 * Value = this struct.
 * ─────────────────────────────────────────────────────────────────────────
 */
struct port_rule {
    __u8  action;     /* 0 = drop, 1 = allow */
    __u8  protocol;   /* 0 = any, 6 = TCP, 17 = UDP */
    __u16 pad;        /* explicit padding to avoid struct holes */
};

/* Action constants — used in port_rule.action and XDP return codes */
#define ACTION_DROP  0
#define ACTION_ALLOW 1

/* Protocol constants */
#define PROTO_ANY  0
#define PROTO_TCP  6
#define PROTO_UDP  17
#define PROTO_ICMP 1

#endif /* XDP_FIREWALL_H */