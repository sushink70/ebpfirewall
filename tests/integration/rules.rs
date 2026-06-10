// rules.rs — Rust-side mirror of the structs defined in xdp_firewall.h.
//
// CRITICAL: every struct here MUST match the C layout exactly.
//   - #[repr(C)]          — no field reordering, no Rust ABI.
//   - bytemuck::{Pod, Zeroable} — safe cast to/from &[u8].
//   - Explicit _pad fields — never rely on implicit padding.
//
// Byte-order conventions (mirror the kernel comment):
//   - IPs   stored as NETWORK byte order  → use u32::to_be() before storing
//   - Ports stored as HOST byte order      → use plain u16

use bytemuck::{Pod, Zeroable};
use serde::{Deserialize, Serialize};
use std::net::Ipv4Addr;

/* ─── action ────────────────────────────────────────────────────────── */

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Action {
    Pass = 0,
    Drop = 1,
}

impl Action {
    pub fn from_u8(v: u8) -> Self {
        if v == 1 { Action::Drop } else { Action::Pass }
    }
}

/* ─── LPM trie key ───────────────────────────────────────────────────── */

/// Key for `blocklist_v4`. Mirrors `struct lpm_v4_key` in xdp_firewall.h.
/// `ip` must be in **network** (big-endian) byte order.
#[repr(C)]
#[derive(Debug, Clone, Copy, Pod, Zeroable)]
pub struct LpmV4Key {
    pub prefixlen: u32,
    pub ip: u32, // network byte order
}

impl LpmV4Key {
    /// Build from a dotted-decimal string and prefix length.
    /// E.g. ("10.0.0.0", 24) → key that matches 10.0.0.0/24.
    pub fn new(ip: Ipv4Addr, prefixlen: u32) -> Self {
        Self {
            prefixlen,
            ip: u32::from(ip).to_be(), // to_be() → network byte order
        }
    }

    /// Lookup key: always use prefixlen=32 so the trie finds the
    /// longest matching stored prefix.
    pub fn lookup(ip: Ipv4Addr) -> Self {
        Self::new(ip, 32)
    }
}

/* ─── port rule key ─────────────────────────────────────────────────── */

/// Key for `port_rules`. Mirrors `struct port_rule_key`.
/// `dst_port` in **host** byte order (same as XDP program).
#[repr(C)]
#[derive(Debug, Clone, Copy, Pod, Zeroable)]
pub struct PortRuleKey {
    pub proto: u8,
    pub _pad: u8,
    pub dst_port: u16, // host byte order
}

impl PortRuleKey {
    pub fn new(proto: u8, dst_port: u16) -> Self {
        Self { proto, _pad: 0, dst_port }
    }
    /// Wildcard variant: matches all ports of the given protocol.
    pub fn wildcard(proto: u8) -> Self {
        Self::new(proto, 0)
    }
}

/* ─── 5-tuple rule key ──────────────────────────────────────────────── */

/// Key for `fw_rules`. Mirrors `struct fw_rule_key`.
#[repr(C)]
#[derive(Debug, Clone, Copy, Pod, Zeroable)]
pub struct FwRuleKey {
    pub src_ip: u32,   // network byte order
    pub dst_ip: u32,   // network byte order
    pub src_port: u16, // host byte order
    pub dst_port: u16, // host byte order
    pub proto: u8,
    pub _pad: [u8; 3],
}

impl FwRuleKey {
    pub fn new(
        src_ip: Ipv4Addr,
        dst_ip: Ipv4Addr,
        src_port: u16,
        dst_port: u16,
        proto: u8,
    ) -> Self {
        Self {
            src_ip: u32::from(src_ip).to_be(),
            dst_ip: u32::from(dst_ip).to_be(),
            src_port,
            dst_port,
            proto,
            _pad: [0u8; 3],
        }
    }
}

/* ─── rule value ─────────────────────────────────────────────────────── */

/// Stored value for both `port_rules` and `fw_rules`.
/// Mirrors `struct fw_rule_val`.
#[repr(C)]
#[derive(Debug, Clone, Copy, Pod, Zeroable)]
pub struct FwRuleVal {
    pub action: u8,
    pub _pad: [u8; 3],
}

impl FwRuleVal {
    pub fn new(action: Action) -> Self {
        Self { action: action as u8, _pad: [0u8; 3] }
    }
}

/* ─── stats ─────────────────────────────────────────────────────────── */

/// Per-CPU stat entry. Mirrors `struct fw_stat`.
#[repr(C)]
#[derive(Debug, Default, Clone, Copy, Pod, Zeroable)]
pub struct FwStat {
    pub packets: u64,
    pub bytes: u64,
}

/// Aggregated (cross-CPU summed) statistics.
#[derive(Debug, Default, Serialize)]
pub struct Stats {
    pub total: FwStat,
    pub passed: FwStat,
    pub dropped: FwStat,
    pub errors: FwStat,
}

/* ─── config ─────────────────────────────────────────────────────────── */

/// Mirrors `struct fw_config`.
#[repr(C)]
#[derive(Debug, Clone, Copy, Pod, Zeroable)]
pub struct FwConfig {
    pub default_action: u8,
    pub _pad: [u8; 3],
}

/* ─── protocol helpers ───────────────────────────────────────────────── */

pub fn proto_from_str(s: &str) -> anyhow::Result<u8> {
    match s.to_lowercase().as_str() {
        "tcp"  => Ok(6),
        "udp"  => Ok(17),
        "icmp" => Ok(1),
        "any" | "" => Ok(0),
        _ => anyhow::bail!("Unknown protocol '{}'. Use tcp/udp/icmp/any.", s),
    }
}

pub fn proto_to_str(p: u8) -> &'static str {
    match p {
        6  => "tcp",
        17 => "udp",
        1  => "icmp",
        0  => "any",
        _  => "unknown",
    }
}
