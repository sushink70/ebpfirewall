#!/usr/bin/env bash
# Pre-flight checks for running XDP native mode on AWS ENA.
# Run this before `make build` on a fresh instance.

set -euo pipefail
IFACE="${IFACE:-eth1}"
PASS=0; WARN=0; FAIL=0

ok()   { echo "  [OK]   $*";   ((PASS++)); }
warn() { echo "  [WARN] $*";   ((WARN++)); }
fail() { echo "  [FAIL] $*";   ((FAIL++)); }

echo "=== ebpfirewall pre-flight check ==="
echo "Interface: $IFACE"
echo

# ── 1. Kernel version ──────────────────────────────────────────────────────
KVER=$(uname -r)
KMAJ=$(echo "$KVER" | cut -d. -f1)
KMIN=$(echo "$KVER" | cut -d. -f2)
if (( KMAJ > 5 )) || (( KMAJ == 5 && KMIN >= 9 )); then
    ok "Kernel $KVER (≥ 5.9 required for ENA native XDP)"
else
    fail "Kernel $KVER is too old — need ≥ 5.9"
fi

# ── 2. ENA driver version ──────────────────────────────────────────────────
ENA_VER=$(modinfo ena 2>/dev/null | awk '/^version:/{print $2}')
if [[ -z "$ENA_VER" ]]; then
    fail "ENA driver not loaded — not an AWS Nitro instance?"
else
    ENA_MAJ=$(echo "$ENA_VER" | cut -d. -f1)
    ENA_MIN=$(echo "$ENA_VER" | cut -d. -f2)
    if (( ENA_MAJ > 2 )) || (( ENA_MAJ == 2 && ENA_MIN >= 2 )); then
        ok "ENA driver $ENA_VER (≥ 2.2.0 required for native XDP)"
    else
        fail "ENA driver $ENA_VER — native XDP requires ≥ 2.2.0"
    fi
fi

# ── 3. Interface exists ────────────────────────────────────────────────────
if ip link show "$IFACE" &>/dev/null; then
    ok "Interface $IFACE exists"
else
    fail "Interface $IFACE not found — attach a second ENI in EC2 console"
fi

# ── 4. MTU ─────────────────────────────────────────────────────────────────
MTU=$(ip link show "$IFACE" 2>/dev/null | awk '/mtu/{for(i=1;i<=NF;i++) if($i=="mtu") print $(i+1)}')
if [[ -n "$MTU" ]]; then
    if (( MTU <= 3498 )); then
        ok "MTU $MTU (≤ 3498 required for XDP on ENA)"
    else
        warn "MTU $MTU is too large for XDP native on ENA"
        echo "       Fix: sudo ip link set $IFACE mtu 3498"
    fi
fi

# ── 5. Channel count ───────────────────────────────────────────────────────
if command -v ethtool &>/dev/null; then
    COMBINED=$(ethtool -l "$IFACE" 2>/dev/null | awk '/Combined:/{c++; if(c==2) print $2}')
    if [[ -n "$COMBINED" ]]; then
        if (( COMBINED <= 4 )); then
            ok "Combined channels: $COMBINED"
        else
            warn "Combined channels: $COMBINED — consider halving for XDP native"
            echo "       Fix: sudo ethtool -L $IFACE combined $((COMBINED / 2))"
        fi
    fi
else
    warn "ethtool not installed — cannot verify channel count"
fi

# ── 6. BPF filesystem ─────────────────────────────────────────────────────
if mount | grep -q "bpf on /sys/fs/bpf"; then
    ok "BPFfs mounted at /sys/fs/bpf"
else
    fail "BPFfs not mounted"
    echo "       Fix: sudo mount -t bpf none /sys/fs/bpf"
fi

# ── 7. Required tools ─────────────────────────────────────────────────────
for tool in clang cargo bpftool ip hping3; do
    if command -v "$tool" &>/dev/null; then
        ok "$tool available"
    else
        warn "$tool not found"
    fi
done

# ── 8. Rust nightly + bpf target ──────────────────────────────────────────
if rustup toolchain list 2>/dev/null | grep -q nightly; then
    ok "Rust nightly toolchain present"
else
    fail "Rust nightly not installed: rustup toolchain install nightly"
fi

if rustup target list --toolchain nightly --installed 2>/dev/null \
        | grep -q "bpfel-unknown-none"; then
    ok "bpfel-unknown-none target installed"
else
    warn "bpfel-unknown-none target not installed"
    echo "       Fix: rustup target add bpfel-unknown-none --toolchain nightly"
fi

# ── Summary ────────────────────────────────────────────────────────────────
echo
echo "Result: $PASS passed, $WARN warnings, $FAIL failed"
(( FAIL == 0 )) && echo "Ready to build." || { echo "Fix failures before building."; exit 1; }