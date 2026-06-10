/* SPDX-License-Identifier: GPL-2.0
 *
 * maps.h — BPF map declarations for the XDP firewall.
 *
 * Every map here is:
 *   1. Pinned under /sys/fs/bpf/ebpfirewall/<name> by the loader so that
 *      the maps survive ctrl-plane restarts.
 *   2. Accessed from user space (Rust) via map handles derived from the
 *      pinned path or from the loaded object.
 *
 * DO NOT include this file from user-space code.
 * The shared types come from xdp_firewall.h.
 */
#pragma once

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include "xdp_firewall.h"

/*
 * blocklist_v4 — IPv4 CIDR blocklist using an LPM trie.
 *
 * Key  : struct lpm_v4_key  (prefixlen + 32-bit IP)
 * Value: __u8               (ACTION_PASS or ACTION_DROP)
 *
 * BPF_F_NO_PREALLOC is *mandatory* for LPM_TRIE; without it the
 * verifier rejects the program with EINVAL.
 *
 * A lookup with prefixlen=32 finds the most specific stored prefix
 * that matches the host address — correct CIDR semantics.
 */
struct {
    __uint(type,       BPF_MAP_TYPE_LPM_TRIE);
    __uint(max_entries, FW_MAX_BLOCKLIST);
    __type(key,        struct lpm_v4_key);
    __type(value,      __u8);
    __uint(map_flags,  BPF_F_NO_PREALLOC);
} blocklist_v4 SEC(".maps");

/*
 * port_rules — protocol + destination-port ACL.
 *
 * Key  : struct port_rule_key  (proto, dst_port)
 * Value: struct fw_rule_val    (action)
 *
 * Two entries per logical rule are inserted by user space:
 *   1. (proto, exact_port)  — matches that specific port.
 *   2. (proto, 0)           — wildcard: matches all ports of the proto.
 * The XDP program tries (proto, exact_port) first, then (proto, 0).
 */
struct {
    __uint(type,        BPF_MAP_TYPE_HASH);
    __uint(max_entries, FW_MAX_PORT_RULES);
    __type(key,         struct port_rule_key);
    __type(value,       struct fw_rule_val);
} port_rules SEC(".maps");

/*
 * fw_rules — full 5-tuple firewall rules.
 *
 * Key  : struct fw_rule_key  (src_ip, dst_ip, src_port, dst_port, proto)
 * Value: struct fw_rule_val  (action)
 */
struct {
    __uint(type,        BPF_MAP_TYPE_HASH);
    __uint(max_entries, FW_MAX_RULES);
    __type(key,         struct fw_rule_key);
    __type(value,       struct fw_rule_val);
} fw_rules SEC(".maps");

/*
 * fw_stats — per-CPU packet / byte counters.
 *
 * Key  : __u32   (STAT_TOTAL, STAT_PASSED, STAT_DROPPED, STAT_ERRORS)
 * Value: struct fw_stat {packets, bytes}
 *
 * Per-CPU arrays allow lock-free updates from multiple cores.
 * User space must sum all per-CPU values to get totals.
 */
struct {
    __uint(type,        BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, STAT_MAX);
    __type(key,         __u32);
    __type(value,       struct fw_stat);
} fw_stats SEC(".maps");

/*
 * fw_config — single-entry global config array.
 *
 * Key  : __u32 (always 0)
 * Value: struct fw_config {default_action}
 *
 * Using an ARRAY (not a map) means index 0 always exists, so the
 * XDP program can safely assume the lookup never returns NULL after
 * the loader has set the value.
 */
struct {
    __uint(type,        BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key,         __u32);
    __type(value,       struct fw_config);
} fw_config SEC(".maps");
