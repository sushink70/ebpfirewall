// loader.rs — BPF object loading and XDP program attachment.
//
// Key design decisions:
//
//  1. We load the compiled C BPF ELF directly (no Aya / skeleton codegen).
//     This keeps the kernel/ directory in plain C and lets clang/LLVM drive
//     the BPF compilation, which gives the best verifier diagnostics.
//
//  2. `Firewall` is the single owner of the libbpf Object and the XDP Link.
//     Dropping `Firewall` detaches the program from the interface and
//     closes the BPF object — no dangling XDP programs left behind.
//
//  3. Map operations are methods on `Firewall` rather than a separate struct
//     to avoid Rust's self-referential-struct problem with libbpf-rs borrows.
//
//  4. We read /sys/class/net/<if>/ifindex to get the interface index — this
//     avoids pulling in nix / libc just for one syscall.

use anyhow::{anyhow, Context, Result};
use bytemuck::{bytes_of, Pod, Zeroable};
use libbpf_rs::{MapFlags, Object, ObjectBuilder};

use crate::rules::{
    Action, FwConfig, FwRuleKey, FwRuleVal, FwStat, LpmV4Key, PortRuleKey,
    Stats,
};

/* ─── stat index constants (must match kernel STAT_* defines) ─────────── */
const STAT_TOTAL:   u32 = 0;
const STAT_PASSED:  u32 = 1;
const STAT_DROPPED: u32 = 2;
const STAT_ERRORS:  u32 = 3;
const STAT_MAX:     u32 = 4;

/* ─── Firewall ────────────────────────────────────────────────────────── */

pub struct Firewall {
    /// The loaded BPF object. Must stay alive for as long as we want the
    /// maps to be accessible — dropping it closes all map fds.
    obj: Object,
    /// XDP Link handle. Dropping this detaches the XDP program.
    _link: libbpf_rs::Link,
}

// SAFETY: libbpf map/prog fds are just Linux file descriptors.
// BPF syscalls (BPF_MAP_UPDATE_ELEM, etc.) are thread-safe at the kernel
// level.  libbpf's C internals do some non-atomic pointer reads on the
// Object metadata, but those are only done once at load time before we
// share the struct across threads.
unsafe impl Send for Firewall {}

impl Firewall {
    /// Load the BPF ELF at `bpf_path` and attach the XDP program to
    /// the interface named `ifname`.
    ///
    /// `use_generic_mode`: if true, force XDP in SKB/generic mode.
    ///   Use this when the NIC driver does not support native XDP
    ///   (e.g. virtio-net without kernel patches, or t2.micro on AWS).
    ///   For ENA (t3+) with reduced channels and MTU ≤ 3498, use false.
    pub fn load(bpf_path: &str, ifname: &str, use_generic_mode: bool) -> Result<Self> {
        let ifindex = get_ifindex(ifname)
            .with_context(|| format!("Interface '{}' not found", ifname))?;

        // Open + load the BPF ELF. "load" runs the BPF verifier.
        let open_obj = ObjectBuilder::default()
            .open_file(bpf_path)
            .with_context(|| format!("Cannot open BPF object '{}'", bpf_path))?;
        let mut obj = open_obj
            .load()
            .context("BPF verifier rejected the program — check kernel version and object")?;

        // Find and attach the XDP program.
        let prog = obj
            .prog_mut("xdp_firewall_prog")
            .ok_or_else(|| anyhow!("'xdp_firewall_prog' not found in BPF object"))?;

        let flags = if use_generic_mode {
            libbpf_rs::XdpFlags::SKB_MODE   // generic / fallback
        } else {
            libbpf_rs::XdpFlags::DRV_MODE   // native — must reduce channels first on ENA
        };

        let link = prog
            .attach_xdp_with_flags(ifindex as i32, flags)
            .with_context(|| {
                if !use_generic_mode {
                    format!(
                        "Failed to attach XDP in native mode to '{}'.\n\
                         ENA requires MTU ≤ 3498 and reduced combined channels.\n\
                         Run: sudo ip link set {} mtu 3498 && \
                              sudo ethtool -L {} combined 2\n\
                         Or retry with --xdp-mode generic",
                        ifname, ifname, ifname
                    )
                } else {
                    format!("Failed to attach XDP to '{}'", ifname)
                }
            })?;

        log::info!(
            "XDP program attached to '{}' (ifindex={}) in {} mode",
            ifname,
            ifindex,
            if use_generic_mode { "generic" } else { "native" }
        );

        Ok(Self { obj, _link: link })
    }

    /* ── config ─────────────────────────────────────────────────────── */

    pub fn set_default_action(&mut self, action: Action) -> Result<()> {
        let val = FwConfig { default_action: action as u8, _pad: [0u8; 3] };
        let key = 0u32;
        map_update_mut(&mut self.obj, "fw_config", bytes_of(&key), bytes_of(&val))
    }

    /* ── blocklist ───────────────────────────────────────────────────── */

    pub fn blocklist_add(&mut self, key: &LpmV4Key, action: Action) -> Result<()> {
        let val: u8 = action as u8;
        map_update_mut(&mut self.obj, "blocklist_v4", bytes_of(key), &[val])
    }

    pub fn blocklist_del(&mut self, key: &LpmV4Key) -> Result<()> {
        map_delete_mut(&mut self.obj, "blocklist_v4", bytes_of(key))
    }

    /* ── port rules ──────────────────────────────────────────────────── */

    pub fn port_rule_add(&mut self, key: &PortRuleKey, action: Action) -> Result<()> {
        let val = FwRuleVal::new(action);
        map_update_mut(&mut self.obj, "port_rules", bytes_of(key), bytes_of(&val))
    }

    pub fn port_rule_del(&mut self, key: &PortRuleKey) -> Result<()> {
        map_delete_mut(&mut self.obj, "port_rules", bytes_of(key))
    }

    /* ── 5-tuple rules ───────────────────────────────────────────────── */

    pub fn rule_add(&mut self, key: &FwRuleKey, action: Action) -> Result<()> {
        let val = FwRuleVal::new(action);
        map_update_mut(&mut self.obj, "fw_rules", bytes_of(key), bytes_of(&val))
    }

    pub fn rule_del(&mut self, key: &FwRuleKey) -> Result<()> {
        map_delete_mut(&mut self.obj, "fw_rules", bytes_of(key))
    }

    /* ── stats ───────────────────────────────────────────────────────── */

    pub fn get_stats(&self) -> Result<Stats> {
        let mut stats = Stats::default();
        for (idx, field) in [
            (STAT_TOTAL,   &mut stats.total   as *mut FwStat),
            (STAT_PASSED,  &mut stats.passed  as *mut FwStat),
            (STAT_DROPPED, &mut stats.dropped as *mut FwStat),
            (STAT_ERRORS,  &mut stats.errors  as *mut FwStat),
        ] {
            if idx >= STAT_MAX { break; }
            let cpu_values = map_lookup_percpu(&self.obj, "fw_stats", bytes_of(&idx))?;
            // Sum across all CPUs.
            let mut agg = FwStat::default();
            for cpu_bytes in &cpu_values {
                if cpu_bytes.len() == std::mem::size_of::<FwStat>() {
                    // SAFETY: FwStat is Pod, length checked above.
                    let s: &FwStat = bytemuck::from_bytes(cpu_bytes);
                    agg.packets += s.packets;
                    agg.bytes   += s.bytes;
                }
            }
            // SAFETY: field is a valid *mut FwStat pointer into `stats`.
            unsafe { *field = agg; }
        }
        Ok(stats)
    }
}

/* ─── internal helpers ───────────────────────────────────────────────── */

fn get_ifindex(ifname: &str) -> Result<u32> {
    let path = format!("/sys/class/net/{}/ifindex", ifname);
    let raw = std::fs::read_to_string(&path)
        .with_context(|| format!("Cannot read {}", path))?;
    raw.trim()
        .parse::<u32>()
        .with_context(|| format!("Invalid ifindex in {}", path))
}

fn map_update_mut(obj: &mut Object, name: &str, key: &[u8], val: &[u8]) -> Result<()> {
    let map = obj
        .map_mut(name)
        .ok_or_else(|| anyhow!("BPF map '{}' not found", name))?;
    map.update(key, val, MapFlags::ANY)
        .with_context(|| format!("map_update failed on '{}'", name))
}

fn map_delete_mut(obj: &mut Object, name: &str, key: &[u8]) -> Result<()> {
    let map = obj
        .map_mut(name)
        .ok_or_else(|| anyhow!("BPF map '{}' not found", name))?;
    map.delete(key)
        .with_context(|| format!("map_delete failed on '{}'", name))
}

fn map_lookup_percpu(obj: &Object, name: &str, key: &[u8]) -> Result<Vec<Vec<u8>>> {
    let map = obj
        .map(name)
        .ok_or_else(|| anyhow!("BPF map '{}' not found", name))?;
    map.lookup_percpu(key, MapFlags::ANY)
        .with_context(|| format!("map_lookup_percpu failed on '{}'", name))
        .map(|opt| opt.unwrap_or_default())
}
