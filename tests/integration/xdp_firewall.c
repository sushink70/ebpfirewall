// SPDX-License-Identifier: GPL-2.0
/*
 * xdp_firewall.c — XDP stateless packet-filtering firewall.
 *
 * Compiled with Clang targeting the BPF virtual machine:
 *   clang -O2 -g -target bpf -c xdp_firewall.c -o xdp_firewall.o
 *
 * Decision pipeline (evaluated in order, first match wins):
 *
 *   1. Total stats counter.
 *   2. ETH parse  → non-IPv4 frame?          → XDP_PASS (let kernel handle)
 *   3. IPv4 parse → malformed header?         → XDP_PASS + error counter
 *   4. Blocklist  → src IP matches CIDR?      → ACTION from trie
 *   5. Blocklist  → dst IP matches CIDR?      → ACTION from trie
 *   6. L4 parse   → TCP / UDP / ICMP
 *   7. Port rule  → (proto, dst_port) hit?    → ACTION from hash
 *   8. Port rule  → (proto, 0) wildcard?      → ACTION from hash
 *   9. 5-tuple    → exact rule match?         → ACTION from hash
 *  10. Default    → fw_config[0].default_action
 *
 * Byte-order convention (follow this or you get silent wrong-drops):
 *   - IPs   in map keys: NETWORK byte order  (same as ip->saddr/daddr)
 *   - Ports in map keys: HOST byte order     (bpf_ntohs applied before key)
 *   The Rust loader must use to_be() for IPs and leave ports as-is.
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/icmp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#include "xdp_firewall.h"
#include "maps.h"

/* ─── helpers ────────────────────────────────────────────────────────── */

/* Hint the branch predictor — keeps the common (fast) path inlined. */
#ifndef likely
# define likely(x)   __builtin_expect(!!(x), 1)
# define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/*
 * stat_inc — increment a per-CPU stat counter.
 *
 * The PERCPU_ARRAY lookup can technically return NULL (e.g. if idx is out
 * of bounds), so guard it.  The compiler folds the constant-idx check and
 * the branch away, leaving zero overhead in the hot path.
 */
static __always_inline void
stat_inc(__u32 idx, __u32 pkt_len)
{
    if (idx >= STAT_MAX)
        return;
    struct fw_stat *s = bpf_map_lookup_elem(&fw_stats, &idx);
    if (likely(s)) {
        /* These are per-CPU so no need for atomics. */
        s->packets++;
        s->bytes += pkt_len;
    }
}

/*
 * get_default_action — read the global default from fw_config[0].
 *
 * ARRAY maps always have index 0 populated (zero-initialised), so
 * bpf_map_lookup_elem can only return NULL if the BPF verifier cannot
 * prove idx is in range — using a literal 0 guarantees it succeeds.
 */
static __always_inline __u8
get_default_action(void)
{
    __u32 zero = 0;
    struct fw_config *cfg = bpf_map_lookup_elem(&fw_config, &zero);
    /* Fallback to PASS so a misconfigured loader is safe, not a DoS. */
    return cfg ? cfg->default_action : ACTION_PASS;
}

/*
 * fw_verdict — update stats and return the matching XDP action.
 */
static __always_inline int
fw_verdict(__u8 action, __u32 pkt_len)
{
    if (action == ACTION_DROP) {
        stat_inc(STAT_DROPPED, pkt_len);
        return XDP_DROP;
    }
    stat_inc(STAT_PASSED, pkt_len);
    return XDP_PASS;
}

/* ─── XDP entry point ────────────────────────────────────────────────── */

SEC("xdp")
int xdp_firewall_prog(struct xdp_md *ctx)
{
    /*
     * ctx->data / ctx->data_end are __u32 offsets from the NIC DMA
     * buffer base.  Cast to (unsigned long) then to pointer — the BPF
     * verifier requires exactly this pattern to track packet bounds.
     */
    void *data_end = (void *)(unsigned long)ctx->data_end;
    void *data     = (void *)(unsigned long)ctx->data;
    __u32 pkt_len  = (__u32)(data_end - data);

    /* Count every packet the program sees. */
    stat_inc(STAT_TOTAL, pkt_len);

    /* ── Layer 2 ──────────────────────────────────────────────────── */

    struct ethhdr *eth = data;
    /*
     * CRITICAL: every pointer dereference must be preceded by a bounds
     * check against data_end.  The verifier rejects the program if any
     * load can go out of bounds — even if it would "never happen".
     */
    if (unlikely((void *)(eth + 1) > data_end)) {
        stat_inc(STAT_ERRORS, pkt_len);
        return XDP_PASS;   /* too short to be a valid frame */
    }

    /* Non-IPv4 traffic (ARP, IPv6, VLANs, …) — pass to the kernel stack. */
    if (bpf_ntohs(eth->h_proto) != ETH_P_IP)
        return XDP_PASS;

    /* ── Layer 3 ──────────────────────────────────────────────────── */

    struct iphdr *ip = (struct iphdr *)(eth + 1);
    if (unlikely((void *)(ip + 1) > data_end)) {
        stat_inc(STAT_ERRORS, pkt_len);
        return XDP_PASS;
    }

    /*
     * ip->ihl is a 4-bit field giving the header length in 32-bit words.
     * Minimum legal value is 5 (20 bytes, no options).
     * ihl < 5 → malformed; ihl > 5 → options present (skip them).
     */
    if (unlikely(ip->ihl < 5)) {
        stat_inc(STAT_ERRORS, pkt_len);
        return XDP_PASS;
    }
    __u32 ip_hdr_sz = (__u32)ip->ihl * 4u;

    /* Compute L4 start pointer and validate it stays inside the packet. */
    void *l4 = (void *)ip + ip_hdr_sz;
    if (unlikely(l4 > data_end)) {
        stat_inc(STAT_ERRORS, pkt_len);
        return XDP_PASS;
    }

    /* Extract fields we'll need for map lookups. */
    __u32 src_ip = ip->saddr;   /* network byte order */
    __u32 dst_ip = ip->daddr;   /* network byte order */
    __u8  proto  = ip->protocol;

    /* ── Blocklist: src IP ───────────────────────────────────────── */

    /*
     * LPM lookup: always provide prefixlen=32.  The trie returns the
     * entry with the longest matching prefix (e.g. /24 if /32 is absent).
     *
     * NOTE: we look up with the *network-byte-order* IP directly —
     *       that's the same representation stored by the loader.
     */
    struct lpm_v4_key lpm = { .prefixlen = 32, .ip = src_ip };
    __u8 *blk = bpf_map_lookup_elem(&blocklist_v4, &lpm);
    if (blk)
        return fw_verdict(*blk, pkt_len);

    /* ── Blocklist: dst IP ───────────────────────────────────────── */

    lpm.ip = dst_ip;
    blk = bpf_map_lookup_elem(&blocklist_v4, &lpm);
    if (blk)
        return fw_verdict(*blk, pkt_len);

    /* ── Layer 4 header parse ────────────────────────────────────── */

    __u16 src_port = 0;
    __u16 dst_port = 0;

    if (proto == IPPROTO_TCP) {
        struct tcphdr *tcp = (struct tcphdr *)l4;
        if (unlikely((void *)(tcp + 1) > data_end)) {
            stat_inc(STAT_ERRORS, pkt_len);
            return XDP_PASS;
        }
        /* Convert from network to host byte order for map keys. */
        src_port = bpf_ntohs(tcp->source);
        dst_port = bpf_ntohs(tcp->dest);
    } else if (proto == IPPROTO_UDP) {
        struct udphdr *udp = (struct udphdr *)l4;
        if (unlikely((void *)(udp + 1) > data_end)) {
            stat_inc(STAT_ERRORS, pkt_len);
            return XDP_PASS;
        }
        src_port = bpf_ntohs(udp->source);
        dst_port = bpf_ntohs(udp->dest);
    }
    /* ICMP — no ports, src_port and dst_port stay 0. */

    /* ── Port-level rule: exact proto+port ──────────────────────── */

    struct port_rule_key prk = {
        .proto    = proto,
        ._pad     = 0,
        .dst_port = dst_port,   /* host byte order */
    };
    struct fw_rule_val *prv = bpf_map_lookup_elem(&port_rules, &prk);
    if (prv)
        return fw_verdict(prv->action, pkt_len);

    /* ── Port-level rule: proto wildcard (dst_port = 0) ─────────── */

    prk.dst_port = 0;
    prv = bpf_map_lookup_elem(&port_rules, &prk);
    if (prv)
        return fw_verdict(prv->action, pkt_len);

    /* ── Full 5-tuple rule ───────────────────────────────────────── */

    /*
     * Zero-initialise the pad fields explicitly.  If pad contains
     * garbage the hash lookup will silently miss even a matching entry.
     */
    struct fw_rule_key rk = {
        .src_ip   = src_ip,
        .dst_ip   = dst_ip,
        .src_port = src_port,
        .dst_port = dst_port,
        .proto    = proto,
        ._pad     = { 0, 0, 0 },
    };
    struct fw_rule_val *rv = bpf_map_lookup_elem(&fw_rules, &rk);
    if (rv)
        return fw_verdict(rv->action, pkt_len);

    /* ── Default action ──────────────────────────────────────────── */

    return fw_verdict(get_default_action(), pkt_len);
}

char _license[] SEC("license") = "GPL";
