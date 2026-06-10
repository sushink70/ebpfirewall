#!/usr/bin/env bash
# Integration test: verifies that the XDP firewall correctly drops/passes
# packets according to the blocklist.
#
# Requires: hping3, root, a running ebpfirewall daemon on $IFACE.

set -euo pipefail
IFACE="${IFACE:-eth1}"
CTRL="sudo ./target/debug/ebpfirewall --iface $IFACE"
PASS=0; FAIL=0

log()  { echo "[TEST] $*"; }
ok()   { echo "  PASS: $*"; ((PASS++)); }
fail() { echo "  FAIL: $*"; ((FAIL++)); }

require_root() {
    [[ $EUID -eq 0 ]] || { echo "Run as root"; exit 1; }
}

# Read current drop counter from stats output
drop_count() {
    $CTRL stats 2>/dev/null | awk '/DROPPED/{print $2}'
}

# ── Test 1: Program loads and shows stats ─────────────────────────────────
test_stats_reachable() {
    log "Test: stats command reaches pinned maps"
    if $CTRL stats &>/dev/null; then
        ok "stats reachable"
    else
        fail "stats failed — is the daemon running? run: make run"
    fi
}

# ── Test 2: XDP mode is native (not generic) ──────────────────────────────
test_native_mode() {
    log "Test: XDP native mode on $IFACE"
    local output
    output=$(ip link show "$IFACE")
    if echo "$output" | grep -q " xdp " && ! echo "$output" | grep -q "xdpgeneric"; then
        ok "XDP native mode confirmed"
    elif echo "$output" | grep -q "xdpgeneric"; then
        fail "XDP is in generic (SKB) mode — native mode required for ENA perf"
    else
        fail "XDP not attached — daemon not running?"
    fi
}

# ── Test 3: block-ip → drop count increases ───────────────────────────────
test_block_ip_drops() {
    log "Test: block-ip causes drop counter to increase"
    local TEST_IP="10.99.99.99"
    
    $CTRL block-ip "$TEST_IP" &>/dev/null
    local before
    before=$(drop_count)
    
    # Send 5 spoofed packets from blocked IP
    sudo hping3 -S --spoof "$TEST_IP" -p 80 -c 5 \
        "$(ip -4 addr show $IFACE | awk '/inet/{print $2}' | cut -d/ -f1)" \
        &>/dev/null || true
    
    sleep 1
    local after
    after=$(drop_count)
    
    if (( after > before )); then
        ok "Drop counter increased: $before → $after"
    else
        fail "Drop counter did not increase (before=$before after=$after)"
        echo "       Hint: hping3 may require the same subnet; verify with tcpdump"
    fi
    
    $CTRL unblock-ip "$TEST_IP" &>/dev/null
}

# ── Test 4: unblock-ip → traffic flows again ──────────────────────────────
test_unblock_ip() {
    log "Test: unblock-ip removes entry from map"
    local TEST_IP="10.99.99.100"
    $CTRL block-ip   "$TEST_IP" &>/dev/null
    $CTRL unblock-ip "$TEST_IP" &>/dev/null
    
    # Verify the IP is gone from the list
    if ! $CTRL list-blocked-ips 2>/dev/null | grep -q "$TEST_IP"; then
        ok "IP removed from blocklist"
    else
        fail "IP still in blocklist after unblock"
    fi
}

# ── Test 5: block-port ────────────────────────────────────────────────────
test_block_port() {
    log "Test: block-port 9999 tcp"
    $CTRL block-port 9999 --proto tcp &>/dev/null
    local before
    before=$(drop_count)
    
    sudo hping3 -S -p 9999 -c 3 \
        "$(ip -4 addr show $IFACE | awk '/inet/{print $2}' | cut -d/ -f1)" \
        &>/dev/null || true
    
    sleep 1
    local after
    after=$(drop_count)
    
    if (( after > before )); then
        ok "Port block working: drops $before → $after"
    else
        fail "Port block not working — check hping3 is sending to $IFACE IP"
    fi
    
    $CTRL unblock-port 9999 &>/dev/null
}

# ── Main ──────────────────────────────────────────────────────────────────

require_root
echo "=== ebpfirewall integration tests ==="
echo "Interface: $IFACE"
echo

test_stats_reachable
test_native_mode
test_block_ip_drops
test_unblock_ip
test_block_port

echo
echo "Result: $PASS passed, $FAIL failed"
(( FAIL == 0 )) || exit 1