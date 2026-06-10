/* firewall.c
 *
 * XDP eBPF firewall — runs in kernel space, inside the ENA NIC driver.
 *
 * Processing pipeline per packet:
 *   1. Parse Ethernet header → check for IPv4
 *   2. Parse IPv4 header → extract src IP, protocol, total length
 *   3. Check src IP against blocklist_ipv4 LPM trie → DROP if matched
 *   4. Parse TCP/UDP header → extract destination port
 *   5. Check dest port against allow_ports hash map → DROP if not allowed
 *   6. Update per-IP statistics
 *   7. Return XDP_PASS or XDP_DROP
 *
 * Compilation:
 *   clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
 *         -I/usr/include/x86_64-linux-gnu \
 *         -c firewall.c -o firewall.o
 */

/* BPF CO-RE (Compile Once — Run Everywhere) approach.
 * vmlinux.h contains ALL kernel type definitions generated from BTF.
 * This replaces dozens of #include <linux/...> headers and makes the
 * program portable across kernel versions. */
#include "vmlinux.h"

/* libbpf BPF-side helpers — map definitions, helper functions */
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>    /* bpf_ntohs(), bpf_htons() for byte-order */

#include "xdp_firewall.h"

/* ─────────────────────────────────────────────────────────────────────────
 * BPF MAP DEFINITIONS
 *
 * Maps declared with SEC(".maps") are recognized by libbpf and the loader.
 * The kernel creates these maps when the program is loaded; the loader pins
 * them to /sys/fs/bpf/ so user space can open them by path.
 * ─────────────────────────────────────────────────────────────────────────
 */

/*
 * blocklist_ipv4 — LPM Trie for blocked IP ranges.
 *
 * BPF_MAP_TYPE_LPM_TRIE: designed specifically for CIDR lookups.
 * Lookup is O(prefix_length) — faster than a hash scan of all subnets.
 *
 * key_size = sizeof(struct lpm_key) = 8 bytes (4 prefixlen + 4 addr)
 * value_size = sizeof(__u8) = 1 byte (we only care if key exists)
 * max_entries = 10000: can hold 10k individual IPs or CIDR ranges
 * map_flags = BPF_F_NO_PREALLOC: LPM tries MUST use this flag;
 *   they allocate nodes on demand, not upfront.
 */
struct {
    __uint(type, BPF_MAP_TYPE_LPM_TRIE);
    __uint(key_size, sizeof(struct lpm_key));
    __uint(value_size, sizeof(__u8));
    __uint(max_entries, 10000);
    __uint(map_flags, BPF_F_NO_PREALLOC);
} blocklist_ipv4 SEC(".maps");

/*
 * allow_ports — Hash map of explicitly allowed destination ports.
 *
 * BPF_MAP_TYPE_HASH: O(1) average lookup.
 * Default policy = DROP (allowlist model).
 * If a port is NOT in this map, the packet is dropped.
 * This implements a default-deny firewall.
 *
 * key = __u16 destination port (network byte order)
 * value = struct port_rule (action + protocol)
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, __u16);
    __type(value, struct port_rule);
    __uint(max_entries, 1024);
} allow_ports SEC(".maps");

/*
 * stats_map — Per-source-IP packet and byte counters.
 *
 * BPF_MAP_TYPE_PERCPU_HASH: each CPU has its own copy of the value.
 * This eliminates contention on multi-core systems — no atomic ops needed
 * for the kernel side. User space sums across all CPUs when reading.
 *
 * key = __u32 source IP (network byte order)
 * value = struct ip_stats (per-cpu counters)
 */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_HASH);
    __type(key, __u32);
    __type(value, struct ip_stats);
    __uint(max_entries, 65536);
} stats_map SEC(".maps");

/*
 * config_map — Global firewall configuration flags.
 *
 * key 0 = global_enable (1 = firewall active, 0 = pass everything)
 * key 1 = default_policy (0 = default drop, 1 = default allow)
 * key 2 = log_level (0=off, 1=drops, 2=all)
 *
 * Using BPF_MAP_TYPE_ARRAY here because array maps have O(1) access
 * with no hashing overhead, perfect for a small fixed config set.
 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __type(key, __u32);
    __type(value, __u32);
    __uint(max_entries, 16);
} config_map SEC(".maps");

/* ─────────────────────────────────────────────────────────────────────────
 * HELPER MACROS
 * ─────────────────────────────────────────────────────────────────────────
 */

/*
 * BOUNDS_CHECK — The BPF verifier requires that EVERY pointer dereference
 * be proven safe. We cannot do `eth->h_proto` without first proving that
 * (char*)eth + sizeof(*eth) <= data_end.
 *
 * This macro performs that check. If it fails, we return XDP_PASS
 * (let the kernel handle malformed packets; don't silently drop them).
 */
#define BOUNDS_CHECK(ptr, end) \
    if ((void *)(ptr) > (void *)(end)) return XDP_PASS

/* ─────────────────────────────────────────────────────────────────────────
 * STATS UPDATE HELPER
 *
 * Updates per-IP stats. Separated into a helper to keep the main function
 * readable and within the verifier's complexity limit.
 *
 * __always_inline: the BPF verifier handles inlined functions better than
 * actual function calls (which require stack frame management in BPF).
 * ─────────────────────────────────────────────────────────────────────────
 */
static __always_inline void update_stats(__u32 src_ip, __u32 pkt_len, int dropped)
{
    struct ip_stats *stats;
    struct ip_stats new_stats = {};   /* zero-initialize — required by verifier */

    /* bpf_map_lookup_elem: returns a pointer into the map value, or NULL.
     * The pointer is valid for the duration of this XDP call.
     * We MUST check for NULL — the verifier enforces this. */
    stats = bpf_map_lookup_elem(&stats_map, &src_ip);
    if (!stats) {
        /* First packet from this IP — insert a new entry */
        if (dropped) {
            new_stats.packets_dropped = 1;
            new_stats.bytes_dropped   = pkt_len;
        } else {
            new_stats.packets_passed = 1;
            new_stats.bytes_passed   = pkt_len;
        }
        /* bpf_map_update_elem with BPF_NOEXIST: insert only if key absent.
         * We use BPF_ANY here to handle the race where another CPU inserted
         * between our lookup and this update. */
        bpf_map_update_elem(&stats_map, &src_ip, &new_stats, BPF_ANY);
        return;
    }

    /* Atomic increment: __sync_fetch_and_add is the BPF-safe way to do
     * atomic adds on map values. Essential for PERCPU_HASH maps. */
    if (dropped) {
        __sync_fetch_and_add(&stats->packets_dropped, 1);
        __sync_fetch_and_add(&stats->bytes_dropped, pkt_len);
    } else {
        __sync_fetch_and_add(&stats->packets_passed, 1);
        __sync_fetch_and_add(&stats->bytes_passed, pkt_len);
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * MAIN XDP FUNCTION
 *
 * SEC("xdp") tells libbpf this is an XDP program.
 * The function receives an xdp_md context containing:
 *   ctx->data      = pointer to start of packet (Ethernet header)
 *   ctx->data_end  = pointer to one byte past the end of packet
 *   ctx->ingress_ifindex = interface index the packet arrived on
 * ─────────────────────────────────────────────────────────────────────────
 */
SEC("xdp")
int firewall_main(struct xdp_md *ctx)
{
    /* Cast data pointers to void* for arithmetic.
     * The verifier tracks these as "packet data" pointers with special rules. */
    void *data     = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    /* ── Check global enable flag ─────────────────────────────────────── */
    __u32 config_key = 0;    /* key 0 = global_enable */
    __u32 *enabled = bpf_map_lookup_elem(&config_map, &config_key);
    /* If config map lookup fails or firewall disabled, pass everything */
    if (!enabled || *enabled == 0)
        return XDP_PASS;

    /* ── Parse Ethernet header ───────────────────────────────────────── */
    struct ethhdr *eth = data;

    /* BOUNDS_CHECK: prove to verifier that the full ethhdr is within packet */
    BOUNDS_CHECK(eth + 1, data_end);

    /* bpf_ntohs: converts 16-bit network byte order to host byte order.
     * ETH_P_IP = 0x0800: IPv4 Ethernet type.
     * Non-IPv4 packets (ARP, IPv6, etc.) are passed without inspection. */
    if (bpf_ntohs(eth->h_proto) != ETH_P_IP)
        return XDP_PASS;

    /* ── Parse IPv4 header ───────────────────────────────────────────── */
    /* eth + 1 advances past the Ethernet header (14 bytes) to the IP header */
    struct iphdr *ip = (struct iphdr *)(eth + 1);
    BOUNDS_CHECK(ip + 1, data_end);

    /* Extract source IP (already in network byte order — we store it as-is
     * in maps because the LPM key also uses network byte order) */
    __u32 src_ip  = ip->saddr;
    __u32 pkt_len = bpf_ntohs(ip->tot_len);  /* total IP packet length */
    __u8  proto   = ip->protocol;

    /* ── Check IP blocklist (LPM Trie lookup) ────────────────────────── */
    struct lpm_key key = {
        .prefixlen = 32,   /* exact match: treat src IP as a /32 host route */
        .addr      = src_ip,
    };

    /* bpf_map_lookup_elem on an LPM trie does prefix matching automatically.
     * A /32 key will match /32, /24, /16 entries in order of longest match.
     * Returns non-NULL if ANY prefix in the trie covers this source IP. */
    if (bpf_map_lookup_elem(&blocklist_ipv4, &key)) {
        update_stats(src_ip, pkt_len, 1 /* dropped */);
        return XDP_DROP;    /* silently drop — blocked IP */
    }

    /* ── Parse transport header (TCP or UDP) ─────────────────────────── */
    /* ihl = IP header length in 32-bit words; multiply by 4 for bytes.
     * Standard IPv4 header is 20 bytes (ihl=5), but options can extend it. */
    __u16 dest_port = 0;

    if (proto == IPPROTO_TCP) {
        /* Advance past IP header using ihl field */
        struct tcphdr *tcp = (struct tcphdr *)((char *)ip + (ip->ihl * 4));
        BOUNDS_CHECK(tcp + 1, data_end);
        dest_port = bpf_ntohs(tcp->dest);

    } else if (proto == IPPROTO_UDP) {
        struct udphdr *udp = (struct udphdr *)((char *)ip + (ip->ihl * 4));
        BOUNDS_CHECK(udp + 1, data_end);
        dest_port = bpf_ntohs(udp->dest);

    } else if (proto == IPPROTO_ICMP) {
        /* ICMP has no ports — check if ICMP is globally allowed */
        __u16 icmp_key = 0;   /* use port 0 as a sentinel for ICMP */
        struct port_rule *icmp_rule = bpf_map_lookup_elem(&allow_ports, &icmp_key);
        if (!icmp_rule || icmp_rule->action == ACTION_DROP) {
            update_stats(src_ip, pkt_len, 1);
            return XDP_DROP;
        }
        update_stats(src_ip, pkt_len, 0);
        return XDP_PASS;
    } else {
        /* Unknown protocol — check default policy */
        __u32 policy_key = 1;
        __u32 *default_policy = bpf_map_lookup_elem(&config_map, &policy_key);
        if (!default_policy || *default_policy == 0) {
            /* Default deny */
            update_stats(src_ip, pkt_len, 1);
            return XDP_DROP;
        }
        update_stats(src_ip, pkt_len, 0);
        return XDP_PASS;
    }

    /* ── Check port allowlist ─────────────────────────────────────────── */
    /* Convert dest_port back to network byte order for map lookup.
     * We store ports in network byte order in the map (matches what
     * comes off the wire) to avoid conversion overhead at the hot path. */
    __u16 port_key = bpf_htons(dest_port);
    struct port_rule *rule = bpf_map_lookup_elem(&allow_ports, &port_key);

    if (!rule) {
        /* Port not in allowlist — default deny */
        update_stats(src_ip, pkt_len, 1);
        return XDP_DROP;
    }

    /* Check if the rule's protocol matches (or is ANY) */
    if (rule->protocol != PROTO_ANY && rule->protocol != proto) {
        /* Protocol mismatch — e.g., rule says TCP but packet is UDP */
        update_stats(src_ip, pkt_len, 1);
        return XDP_DROP;
    }

    if (rule->action == ACTION_DROP) {
        update_stats(src_ip, pkt_len, 1);
        return XDP_DROP;
    }

    /* ── Packet passed all checks ─────────────────────────────────────── */
    update_stats(src_ip, pkt_len, 0);
    return XDP_PASS;
}

/*
 * License declaration — MANDATORY for BPF programs.
 * The kernel refuses to load GPL-restricted helpers (like bpf_trace_printk)
 * without this. Most production XDP programs use "GPL".
 */
char LICENSE[] SEC("license") = "GPL";