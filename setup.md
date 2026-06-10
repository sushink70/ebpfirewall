Now I have everything I need. Let me build a comprehensive, deeply structured answer.This is a rich systems topic. Let me lay the complete ground truth before a single command runs.Now the AWS reality check — the answer to your core question about NIC support.

---

## Concept glossary before we proceed

**ENA (Elastic Network Adapter)** — AWS's custom virtual NIC driver. All Nitro-based EC2 instances use it. It is not a physical high-end NIC (like an Intel X710 or Mellanox ConnectX-6) — it is a paravirtualized device backed by AWS's Nitro hypervisor. The physical SR-IOV hardware is hidden from you.

**XDP Native mode (XDP_DRV)** — the XDP program runs inside the NIC driver, before the kernel allocates an SKB (Socket Buffer) struct. This is the fast path. Think of it as intercepting a package at the warehouse loading dock before it enters the building.

**XDP Generic mode (XDP_SKB)** — the XDP program runs after SKB allocation. Slower. Works on ANY NIC driver. No performance advantage over iptables for a firewall use case.

**SKB (Socket Kernel Buffer / `sk_buff`)** — a large kernel struct (~200 bytes of metadata + packet data) that the kernel creates for every packet to carry it through the networking stack. Allocating it is expensive — XDP native avoids this entirely.

**NAPI (New API)** — the Linux kernel mechanism for batching NIC interrupt processing. Instead of one interrupt per packet, the driver polls a ring buffer in a tight loop when traffic is heavy.

**ENI (Elastic Network Interface)** — AWS's virtual NIC that you attach to an EC2 instance. You can attach multiple ENIs to one instance.

**BPF Map** — a key-value data structure shared between kernel space (your XDP C program) and user space (your Rust/Go control plane). This is how your firewall stores rules, IP blocklists, and per-flow statistics.

---

## The NIC reality on AWS — your core question answered

AWS ENA supports XDP natively, allowing eBPF programs to be integrated at the driver level. But here is the full truth you need before writing a single line of code:

**AWS does NOT give you a physical high-end NIC.** What you get is a virtualized function backed by a Nitro card. However, XDP support was introduced to the ENA driver in version 2.2.0. If the NIC driver does not support XDP, you can only use XDP in generic mode.

For your firewall lab, the key insight is: **you only need `XDP_DROP` and `XDP_PASS`**. These work perfectly on ENA native mode. The known limitation is with `XDP_TX` (sending packets back out the same interface), which has a TX ring size issue on ENA:

The TX ring size is always 1024, which is very small for XDP programs using XDP_TX. Even on large instances like c5n.9xlarge, the PPS of XDP_TX packets is lower than XDP generic/skb mode.

For a firewall: you do not care about this. You drop or pass — no TX needed.

Additionally, the ENA Linux driver supports native AF_XDP zero-copy, so if you later want to build packet capture or DPI (Deep Packet Inspection) into your firewall, the foundation is there.

---

## Instance selection for your 6-month free plan

If you created your AWS account on or after July 15, 2025, you can use t3.micro, t3.small, t4g.micro, t4g.small, c7i-flex.large, and m7i-flex.large for 6 months or until your credits are used up.

**Critical distinction:**

| Instance | Hypervisor | ENA | XDP Native | Recommendation |
|---|---|---|---|---|
| t2.micro | Xen | No | No (virtio) | Avoid — generic XDP only |
| t3.micro | Nitro | Yes v2.2+ | Yes | Minimum viable |
| t3.small | Nitro | Yes v2.2+ | Yes | Recommended for this lab |
| c7i-flex.large | Nitro v5 | Yes | Yes + ntuple steering | Best (if credits allow) |

**Use t3.small** (2 vCPU, 2 GB RAM). Compiling eBPF programs with Clang/LLVM and running `bpftool`, `libbpf`, and Rust toolchains simultaneously is memory-heavy. A t3.micro (1 GB RAM) will swap-thrash during builds.

Launch the instance using a Nitro instance type (T3, M5, C5, etc.). Nitro instances have Enhanced Networking, which allows you to use the ENA driver, which has XDP support.

---

## Critical XDP prerequisites on ENA — before loading any program

Two mandatory configuration steps that will cause silent failures if skipped.

**1. MTU reduction**

The default MTU in EC2 is 9001, so you will have to reduce this MTU to use XDP. ENA's XDP implementation requires that a packet fits in a single RX buffer. Jumbo frames violate this.

```bash
# On your XDP interface (eth1, the lab interface — NOT eth0)
sudo ip link set eth1 mtu 3498
```

**2. Channel (queue pair) reduction**

XDP native mode on ENA requires that you free up combined channels. The driver needs headroom. You can use the following command to see how the channels are allocated on your NIC and what the maximum available channel count is.

```bash
# Check current channels
ethtool -l eth1

# Set to half the combined maximum (e.g. if max combined=4, set to 2)
sudo ethtool -L eth1 combined 2
```

Without this step, the kernel will refuse to load the XDP program in native mode and silently fall back to generic (or reject entirely).

---

## AWS lab setup — step-by-step

```
Step 0: Account + region
  └─ Use us-east-1 (cheapest, most services)
  └─ Ubuntu 24.04 LTS AMI (kernel 6.8+, ENA v2.x bundled)

Step 1: Launch t3.small
  └─ Enable "Enhanced networking (ENA)" — on by default for Nitro
  └─ Storage: 20 GB gp3 (free tier: 30 GB)
  └─ Security Group: allow port 22 (SSH) from your IP only
  └─ NO Elastic IP for now (use Session Manager or SSH through private IP)

Step 2: Add a second ENI (eth1) — CRITICAL
  └─ EC2 Console → Networking → Attach Network Interface
  └─ Create new ENI in same subnet, same Security Group
  └─ Attach to your instance
  └─ This is your XDP lab interface — experiments run here
  └─ If your XDP program crashes eth1, you still have eth0 for SSH

Step 3: Ubuntu 24.04 toolchain install
  └─ See commands below

Step 4: Configure eth1 for XDP
  └─ Reduce MTU: ip link set eth1 mtu 3498
  └─ Reduce channels: ethtool -L eth1 combined 2
  └─ Verify ENA driver version: modinfo ena | grep ^version

Step 5: Build + load your first XDP program
  └─ Start with XDP_DROP everything on eth1
  └─ Verify with: ip link show eth1 (look for "xdp" in output)
```

**Toolchain install on Ubuntu 24.04:**

```bash
sudo apt update && sudo apt install -y \
  clang llvm \
  libbpf-dev linux-headers-$(uname -r) \
  bpftool iproute2 \
  pkg-config libelf-dev \
  linux-tools-$(uname -r) linux-tools-generic

# Rust (for Aya-based control plane)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env
cargo install bpf-linker    # Aya's BPF backend

# Aya project template
cargo install cargo-generate
cargo generate --git https://github.com/aya-rs/aya-template

# Go (for management plane)
wget https://go.dev/dl/go1.22.linux-amd64.tar.gz
sudo tar -C /usr/local -xzf go1.22.linux-amd64.tar.gz
export PATH=$PATH:/usr/local/go/bin

# Verify ENA native XDP capability
modinfo ena | grep version
# Must be >= 2.2.0 (Ubuntu 24.04 ships 2.x)
```

**Verify native mode loaded correctly:**

```bash
# After loading your XDP prog:
ip link show eth1
# You should see "xdp" in the output (NOT "xdpgeneric")
# xdp      = native mode  ← what we want
# xdpgeneric = generic mode ← fallback, no perf benefit
```

# eBPFirewall — AWS XDP/eBPF Lab Setup Guide

> **Architecture:** C (kernel datapath) · Rust/Aya (userspace control plane) · Go (management API & CLI)  
> **Platform:** AWS EC2 · Ubuntu 24.04 LTS · ENA v2.x · Kernel 6.8+

---

## Table of Contents

1. [The NIC Question — Answered](#1-the-nic-question--answered)
2. [AWS Free Tier Reality Check (2025)](#2-aws-free-tier-reality-check-2025)
3. [Instance Selection Matrix](#3-instance-selection-matrix)
4. [Why Three Languages?](#4-why-three-languages)
5. [System Architecture Overview](#5-system-architecture-overview)
6. [AWS Console Setup — Step by Step](#6-aws-console-setup--step-by-step)
7. [Ubuntu 24.04 Toolchain Installation](#7-ubuntu-2404-toolchain-installation)
8. [ENA XDP Prerequisites (Critical)](#8-ena-xdp-prerequisites-critical)
9. [Project Repository Structure](#9-project-repository-structure)
10. [Verification Checklist](#10-verification-checklist)
11. [Known ENA Limitations & Workarounds](#11-known-ena-limitations--workarounds)
12. [Future: Adding a Public IP for Traffic Testing](#12-future-adding-a-public-ip-for-traffic-testing)
13. [Glossary](#13-glossary)

---

## 1. The NIC Question — Answered

> **"Not all NICs support XDP. Does AWS give us a high-end NIC for this?"**

This is the most important question before writing a single line of code. The direct answer is:

**AWS does NOT give you a physical NIC.** What you get is a paravirtualized device called **ENA (Elastic Network Adapter)** — a virtual NIC backed by AWS's Nitro hypervisor. The physical SR-IOV hardware underneath (Nitro cards) is completely abstracted from you.

However, **this does NOT mean you cannot use XDP natively.** Here is the full picture:

### What ENA Supports

| XDP Feature | ENA Native Support | Notes |
|---|---|---|
| `XDP_DROP` | ✅ Full native | Perfect for a firewall — our primary use case |
| `XDP_PASS` | ✅ Full native | Allow traffic through cleanly |
| `XDP_REDIRECT` | ✅ Supported | Inter-interface redirects work |
| `XDP_TX` | ⚠️ Native but limited | TX ring size = 1024 only; PPS lower than generic mode on small instances |
| `AF_XDP` zero-copy | ✅ Supported | Available, with queue count restrictions |
| `XDP_ABORTED` | ✅ Tracked via stats | Counter visible via `ethtool -S` |

### XDP Mode Comparison on ENA

```
XDP Native Mode (XDP_DRV)         XDP Generic Mode (XDP_SKB)
─────────────────────────         ──────────────────────────
Program runs INSIDE the driver    Program runs AFTER SKB allocation
No SKB allocation = fast          SKB allocated = same cost as iptables
Pre-stack interception            Post-stack processing
Requires ENA driver >= 2.2.0      Works on ANY driver (even old virtio)
MTU must be <= 3498               MTU not restricted
Channels must be halved           No channel restriction

For a FIREWALL (DROP/PASS only):
XDP native is the clear winner.
```

### The One Limitation That Doesn't Affect Us

The TX ring size on ENA is hardcoded at 1024. This causes `XDP_TX` (bouncing packets back out the same interface) to perform **worse** than generic mode on small instances like c5n.xlarge. For a firewall, we only need `XDP_DROP` and `XDP_PASS` — we never retransmit on the same interface. **This limitation is irrelevant to our use case.**

### Driver Version Requirement

XDP support was introduced to the ENA driver at version **2.2.0**. Ubuntu 24.04 ships with ENA **2.x** by default — this requirement is already met without manual driver installation.

```bash
# Verify after launch:
modinfo ena | grep ^version
# Must show >= 2.2.0
```

### Bottom Line

You do not need a "high-end physical NIC" for this lab. The ENA virtual NIC on any Nitro-based EC2 instance gives you true XDP native mode for DROP and PASS operations. The Nitro hypervisor handles the hardware abstraction transparently, and from the kernel's perspective the ENA driver exposes a fully capable XDP hook point.

---

## 2. AWS Free Tier Reality Check (2025)

AWS changed its free tier structure on **July 15, 2025**. Your plan depends on when your account was created.

### Account Created Before July 15, 2025

- **Duration:** 12 months from account creation
- **Free instance:** t2.micro (750 hours/month) or t3.micro where t2 unavailable
- **Problem for this project:** t2.micro uses the **Xen hypervisor** — no ENA, no XDP native mode. Only generic mode available. t3.micro uses Nitro but has only 1 GB RAM, which will swap-thrash during Rust/LLVM builds.
- **Recommendation:** Use t3.micro but create a 4 GB swap file. Or spend a few cents per hour on t3.small.

### Account Created On or After July 15, 2025

- **Duration:** 6 months OR until $200 in credits exhausted (whichever comes first)
- **Free instances:** t3.micro, **t3.small**, t4g.micro, t4g.small, c7i-flex.large, m7i-flex.large
- **Recommendation:** **Use t3.small** — 2 vCPU, 2 GB RAM, Nitro hypervisor, ENA native XDP. Free under the new plan.

> ⚠️ **Important:** The new 6-month plan uses a credit system ($100 at signup + $100 earned via exploration activities). Credits run out before 6 months if you leave instances running 24/7. Stop instances when not actively working.

---

## 3. Instance Selection Matrix

| Instance | vCPU | RAM | Hypervisor | ENA | XDP Native | Free Tier | Recommendation |
|---|---|---|---|---|---|---|---|
| t2.micro | 1 | 1 GB | Xen | ❌ | ❌ | Pre-Jul 2025 (12mo) | **Avoid** — no native XDP |
| t3.micro | 2 | 1 GB | Nitro | ✅ | ✅ | Both plans | Usable but RAM is tight |
| **t3.small** | **2** | **2 GB** | **Nitro** | **✅** | **✅** | **Post-Jul 2025** | **✅ Recommended** |
| t4g.small | 2 | 2 GB | Nitro | ✅ | ✅ | Post-Jul 2025 | ARM64 — toolchain differs |
| c7i-flex.large | 2 | 4 GB | Nitro v5 | ✅ | ✅ + ntuple | Post-Jul 2025 | Best if credits allow |

**Use t3.small.** Reasons:

1. 2 GB RAM handles simultaneous Clang/LLVM compilation + Rust toolchain without swap
2. Nitro hypervisor guarantees ENA >= 2.x and native XDP support
3. x86_64 — all eBPF tooling, libbpf, bpftool packages are well tested on this architecture
4. Free under the post-July 2025 plan

---

## 4. Why Three Languages?

The three-language split follows a natural architectural boundary. Each language does what it does best.

### C — Kernel Datapath (XDP Program)

```
WHERE: Runs inside the Linux kernel, attached to the NIC driver
WHY C:
  - Only language with official, stable eBPF kernel support
  - Compiled to eBPF bytecode via clang -target bpf
  - Direct access to kernel helper functions (bpf_map_lookup_elem, etc.)
  - The eBPF verifier understands C-generated BPF bytecode best
  - All production references (Cilium, Katran, XDP-firewall) are in C
  - CO-RE (Compile Once Run Everywhere) via BTF is most mature in C/libbpf

WHAT IT DOES IN OUR FIREWALL:
  - Parses Ethernet/IP/TCP/UDP headers at line rate
  - Looks up source IP in a BPF_MAP_TYPE_LPM_TRIE (CIDR blocklist)
  - Looks up (src_ip, dst_port) in a BPF_MAP_TYPE_HASH (rule table)
  - Returns XDP_DROP or XDP_PASS
  - Updates per-rule counters in a BPF_MAP_TYPE_PERCPU_ARRAY
```

### Rust (Aya / libbpf-rs) — Userspace Control Plane

```
WHERE: Userspace process that loads/manages the eBPF program
WHY RUST:
  - Memory safety for map manipulation (no UAF, no buffer overflows)
  - Aya provides idiomatic Rust APIs for BPF map read/write
  - Type-safe map key/value structs shared between kernel and userspace
  - Async support (Tokio) for event-driven rule reloading
  - libbpf-rs = safe Rust wrapper around libbpf for CO-RE portability

WHAT IT DOES IN OUR FIREWALL:
  - Loads the compiled .o BPF object and attaches it to eth1 via XDP
  - Reads/writes BPF maps: inserts/removes firewall rules
  - Reads per-CPU counters and aggregates packet statistics
  - Watches a rule file for changes and hot-reloads rules without detaching XDP
  - Exposes a Unix domain socket for the Go management API to call
```

### Go — Management Plane (API Server & CLI)

```
WHERE: Userspace service + CLI tool
WHY GO:
  - Excellent HTTP/gRPC server libraries (standard library + chi/grpc)
  - Fast protobuf/JSON marshaling for rule serialization
  - Goroutines make concurrent rule application trivial
  - Easy to build a CLI tool (cobra/flag)
  - Familiar to DevOps/SRE teams for operational tooling
  - Not suitable for kernel BPF work — that's why Rust handles it

WHAT IT DOES IN OUR FIREWALL:
  - REST/gRPC API: POST /rules, DELETE /rules/{id}, GET /stats
  - CLI: ebpfw add --src 10.0.0.0/8 --port 443 --action drop
  - Rule persistence: load rules from YAML/JSON on startup
  - Metrics endpoint: Prometheus-compatible /metrics (packet counters)
  - Audit log: log every rule add/remove with timestamp + operator
```

### Language Boundary Summary

```
┌─────────────────────────────────────────────────────────────┐
│  Network Interface (eth1)                                   │
│  ↓ packets arrive at driver level                           │
├─────────────────────────────────────────────────────────────┤
│  C / XDP Program (kernel space)                             │
│  - Header parsing, map lookup, XDP_DROP / XDP_PASS          │
│  - BPF maps: LPM trie, hash map, per-CPU counters           │
├─────────────────────────────────────────────────────────────┤
│  Rust / Aya Control Plane (userspace)                       │
│  - Loads BPF object, attaches XDP hook                      │
│  - Reads/writes BPF maps (the "bridge" between planes)      │
│  - Unix socket server for management commands               │
├─────────────────────────────────────────────────────────────┤
│  Go / Management API + CLI (userspace)                      │
│  - REST/gRPC API for operators                              │
│  - CLI tool, rule persistence, Prometheus metrics           │
└─────────────────────────────────────────────────────────────┘
```

---

## 5. System Architecture Overview

```
Operator / Admin
     │
     │ HTTP/gRPC (REST API)
     ▼
┌─────────────────────────────┐
│  Go: ebpfw-api              │   ← Management plane
│  Port 8080 (internal only)  │     Rule CRUD, metrics, audit
│  ebpfw CLI tool             │
└────────────┬────────────────┘
             │ Unix socket (/run/ebpfw.sock)
             ▼
┌─────────────────────────────┐
│  Rust: ebpfw-ctrl           │   ← Control plane
│  (Aya / libbpf-rs)          │     BPF object loader
│  - map: BLOCKLIST (LPM)     │     XDP attacher
│  - map: RULES (hash)        │     Rule→map translator
│  - map: COUNTERS (percpu)   │     Stats aggregator
└────────────┬────────────────┘
             │ BPF syscall (bpf_map_update_elem)
             ▼
┌─────────────────────────────┐
│  C: xdp_firewall.c          │   ← Kernel datapath
│  Compiled → xdp_firewall.o  │     Runs in ENA driver hook
│                             │     Zero SKB allocation
│  parse eth → ip → tcp/udp   │     Line-rate DROP/PASS
│  lookup map → action        │
│  update counter             │
└────────────┬────────────────┘
             │ XDP hook (native mode)
             ▼
┌─────────────────────────────┐
│  ENA driver (eth1)          │   ← AWS virtual NIC
│  XDP native mode            │     Nitro-backed
│  MTU: 3498, channels: ½max  │
└─────────────────────────────┘
```

---

## 6. AWS Console Setup — Step by Step

### Step 0: Account & Region

- Use **us-east-1** (N. Virginia) — cheapest region, all instance types available
- Confirm free tier: go to **AWS Console → Billing → Free Tier** and check your plan

### Step 1: Create a VPC (optional but recommended for lab isolation)

> If you skip this, use the default VPC. For a lab, the default VPC is fine.

```
VPC → Create VPC
  Name: ebpfirewall-lab
  IPv4 CIDR: 10.10.0.0/16
  Subnet: 10.10.1.0/24 (us-east-1a)
  Enable DNS hostnames: Yes
```

### Step 2: Create a Security Group

```
EC2 → Security Groups → Create
  Name: ebpfirewall-sg
  Description: Lab SG for eBPF firewall dev
  
Inbound rules:
  SSH (22) — My IP only     (for initial access)
  
Outbound rules:
  All traffic — 0.0.0.0/0   (for apt installs)
```

> ⚠️ Do NOT open ports to 0.0.0.0/0 on inbound. Your XDP firewall is the experiment — not the security group.

### Step 3: Launch the EC2 Instance

```
EC2 → Launch Instances

  Name: ebpfirewall-dev
  AMI: Ubuntu Server 24.04 LTS (HVM), SSD Volume Type
       (search "ubuntu 24.04" — free tier eligible)
  
  Architecture: x86_64 (not ARM — toolchain is simpler)
  
  Instance type: t3.small
    (post-Jul 2025 accounts: free)
    (pre-Jul 2025 accounts: use t3.micro, ~$0.01/hr difference)
  
  Key pair: Create new → ebpfirewall-key (RSA, .pem)
            Save the .pem file securely — you cannot re-download it
  
  Network settings:
    VPC: ebpfirewall-lab (or default)
    Subnet: public subnet (us-east-1a)
    Auto-assign public IP: Enable (for SSH during setup)
    Security group: ebpfirewall-sg
  
  Storage:
    1x gp3 volume, 20 GB
    (free tier: 30 GB gp3 — keep some headroom)
  
  Advanced → User data: (leave blank for now)

→ Launch Instance
```

### Step 4: Attach a Second ENI (eth1) — Critical for Safety

This is the interface you will attach XDP programs to. If your XDP program crashes `eth1`, you retain SSH access through `eth0`.

```
EC2 → Network Interfaces → Create Network Interface
  Description: XDP lab interface
  Subnet: same subnet as your instance
  Security group: ebpfirewall-sg
  → Create

EC2 → Instances → select your instance
  → Actions → Networking → Attach network interface
  → Select the ENI you just created
  → Attach
```

After attaching, SSH into the instance and bring up `eth1`:

```bash
# Check that eth1 appeared
ip link show

# Bring it up (it will have no IP — that's fine for XDP lab)
sudo ip link set eth1 up

# Optionally assign a test IP
sudo ip addr add 10.10.1.100/24 dev eth1
```

### Step 5: (Optional) Disable Public IP Later

Once your toolchain is set up, you can:
1. Snapshot the instance (create an AMI)
2. Move to a private subnet
3. Use AWS Session Manager (SSM) for SSH-less access without a public IP

For now, keep the public IP for easy SSH access during setup.

---

## 7. Ubuntu 24.04 Toolchain Installation

SSH into your instance and run the following in order.

### System Update

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y git curl wget build-essential
```

### eBPF / BPF Core Toolchain

```bash
sudo apt install -y \
  clang \
  llvm \
  libbpf-dev \
  linux-headers-$(uname -r) \
  linux-tools-$(uname -r) \
  linux-tools-generic \
  bpftool \
  iproute2 \
  pkg-config \
  libelf-dev \
  zlib1g-dev \
  ethtool \
  tcpdump \
  perf-tools-unstable
```

Verify the BPF stack:

```bash
# Check kernel BPF support
grep CONFIG_BPF /boot/config-$(uname -r)
# Must show: CONFIG_BPF=y, CONFIG_BPF_SYSCALL=y, CONFIG_XDP_SOCKETS=y

# Check bpftool works
sudo bpftool prog list

# Check ENA driver version (must be >= 2.2.0)
modinfo ena | grep ^version
```

### Rust Toolchain (for Aya control plane)

```bash
# Install rustup
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source ~/.cargo/env

# Add the BPF LLVM backend (required by Aya for kernel-space programs)
cargo install bpf-linker

# Add nightly toolchain (Aya kernel programs require it)
rustup toolchain install nightly
rustup component add rust-src --toolchain nightly

# Add BPF compilation target
rustup target add bpfel-unknown-none

# Aya project generator
cargo install cargo-generate

# Verify
cargo --version
rustc --version
bpf-linker --version
```

### Go Toolchain (for management API + CLI)

```bash
# Download Go 1.22 (check https://go.dev/dl/ for latest stable)
wget https://go.dev/dl/go1.22.4.linux-amd64.tar.gz
sudo tar -C /usr/local -xzf go1.22.4.linux-amd64.tar.gz
rm go1.22.4.linux-amd64.tar.gz

# Add to PATH permanently
echo 'export PATH=$PATH:/usr/local/go/bin' >> ~/.bashrc
echo 'export GOPATH=$HOME/go' >> ~/.bashrc
source ~/.bashrc

# Verify
go version
```

### Additional Libraries

```bash
# libxdp (xdp-tools) — for xdp-loader and multiprog support
sudo apt install -y libxdp-dev xdp-tools

# libbpf headers (development)
sudo apt install -y libbpf-dev

# Verify AF_XDP support
grep XDP_SOCKETS /boot/config-$(uname -r)
```

---

## 8. ENA XDP Prerequisites (Critical)

These two steps **must** be done before loading any XDP program in native mode. Skipping either will cause silent fallback to generic mode or outright rejection by the driver.

### 8.1 Reduce MTU on eth1

The default EC2 MTU is 9001 bytes (jumbo frames). ENA's XDP implementation requires that every packet fits in a **single RX buffer**. Jumbo frames violate this constraint.

```bash
# Set MTU to 3498 (XDP-safe maximum for ENA)
sudo ip link set eth1 mtu 3498

# Verify
ip link show eth1 | grep mtu
# Should show: mtu 3498

# Make it persistent (Ubuntu 24.04 uses netplan)
# Edit /etc/netplan/50-cloud-init.yaml or create a new file:
cat << 'EOF' | sudo tee /etc/netplan/60-eth1-xdp.yaml
network:
  version: 2
  ethernets:
    eth1:
      mtu: 3498
      dhcp4: false
EOF
```

> **Why 3498?** ENA's XDP RX buffer is sized to fit one page. With driver overhead, 3498 is the safe maximum. Some references use 3000 — both work, 3498 gives slightly more headroom.

### 8.2 Reduce Queue Channels on eth1

XDP native mode on ENA requires that the number of combined channels be reduced to less than or equal to half the maximum. This frees up queue pairs for XDP use.

```bash
# Check current channel configuration
ethtool -l eth1
# Example output for t3.small:
#   Pre-set maximums:
#     Combined: 2
#   Current hardware settings:
#     Combined: 2

# Set to half of "Pre-set maximums: Combined" value
# For t3.small (max 2), set to 1:
sudo ethtool -L eth1 combined 1

# For larger instances (e.g., max combined=8), set to 4:
# sudo ethtool -L eth1 combined 4

# Verify
ethtool -l eth1
```

### 8.3 Verify Native Mode After Loading

After loading your XDP program, confirm it's running in native (not generic) mode:

```bash
ip link show eth1
# CORRECT:   ... xdp ...           ← native mode
# WRONG:     ... xdpgeneric ...    ← generic mode (fallback)
```

### 8.4 Quick Sanity Test — XDP_DROP Everything

Before building the full firewall, load a trivial "drop all" program to confirm native XDP works end-to-end:

```c
// test_drop.c
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

SEC("xdp")
int xdp_drop_all(struct xdp_md *ctx) {
    return XDP_DROP;
}

char _license[] SEC("license") = "GPL";
```

```bash
# Compile
clang -O2 -target bpf -c test_drop.c -o test_drop.o

# Load in native mode (no -S flag = native mode attempt)
sudo ip link set eth1 xdp obj test_drop.o sec xdp

# Confirm native mode
ip link show eth1
# Look for "xdp" NOT "xdpgeneric"

# Check XDP drop counters
sudo ethtool -S eth1 | grep xdp_drop

# Unload
sudo ip link set eth1 xdp off
```

---

## 9. Project Repository Structure

```
ebpfirewall/
├── README.md
├── Makefile                    # Top-level build orchestration
│
├── kernel/                     # C — XDP datapath programs
│   ├── xdp_firewall.c          # Main XDP program (DROP/PASS logic)
│   ├── xdp_firewall.h          # Shared structs: rule_key, rule_val, counters
│   ├── maps.h                  # BPF map definitions (LPM trie, hash, percpu)
│   └── Makefile                # clang -target bpf build rules
│
├── ctrl/                       # Rust — userspace control plane (Aya)
│   ├── Cargo.toml
│   ├── src/
│   │   ├── main.rs             # Entry: load BPF obj, attach XDP, start socket
│   │   ├── loader.rs           # Load xdp_firewall.o, pin maps to /sys/fs/bpf/
│   │   ├── maps.rs             # Type-safe wrappers around BPF map operations
│   │   ├── rules.rs            # Rule struct, serialization, map translation
│   │   └── socket.rs           # Unix domain socket server (for Go API calls)
│   └── build.rs                # Aya codegen: generate Rust types from BTF
│
├── mgmt/                       # Go — management API + CLI
│   ├── go.mod
│   ├── cmd/
│   │   ├── ebpfw-api/
│   │   │   └── main.go         # REST/gRPC API server
│   │   └── ebpfw/
│   │       └── main.go         # CLI: ebpfw add/delete/list/stats
│   ├── internal/
│   │   ├── api/                # HTTP handlers (rules CRUD, stats, health)
│   │   ├── client/             # Unix socket client to Rust ctrl
│   │   ├── model/              # Rule struct, JSON tags, validation
│   │   └── metrics/            # Prometheus metrics collector
│   └── rules/
│       └── default.yaml        # Default rules loaded on startup
│
├── setup/
│   ├── setup.md                # This document (AWS setup guide)
│   └── scripts/
│       ├── bootstrap.sh        # Full toolchain install (idempotent)
│       ├── configure-ena.sh    # MTU + channel reduction for eth1
│       └── verify-xdp.sh       # Sanity checks: driver version, mode
│
└── tests/
    ├── unit/                   # Rust unit tests for map logic
    ├── integration/            # Go integration tests against API
    └── traffic/                # pktgen / scapy traffic generation scripts
```

---

## 10. Verification Checklist

Run this checklist after completing setup. Every item must pass before writing firewall logic.

```bash
#!/bin/bash
# verify-xdp.sh

echo "=== ENA Driver Version ==="
modinfo ena | grep ^version
# PASS: version >= 2.2.0

echo ""
echo "=== Kernel BPF Support ==="
grep -E "CONFIG_BPF=|CONFIG_BPF_SYSCALL=|CONFIG_XDP_SOCKETS=" \
  /boot/config-$(uname -r)
# PASS: all three show =y

echo ""
echo "=== eth1 MTU ==="
ip link show eth1 | grep -o 'mtu [0-9]*'
# PASS: mtu 3498

echo ""
echo "=== eth1 Channels ==="
ethtool -l eth1
# PASS: Current combined <= half of Pre-set maximum combined

echo ""
echo "=== bpftool Available ==="
sudo bpftool version
# PASS: any version output

echo ""
echo "=== Clang BPF Target ==="
clang --print-targets | grep bpf
# PASS: bpf listed as target

echo ""
echo "=== Rust BPF Target ==="
rustup target list --installed | grep bpf
# PASS: bpfel-unknown-none listed

echo ""
echo "=== Go Available ==="
go version
# PASS: go 1.22.x or newer
```

---

## 11. Known ENA Limitations & Workarounds

### Limitation 1: XDP_TX Performance (Does NOT Affect Firewall)

**What:** The ENA TX ring has only 1024 entries. Programs using `XDP_TX` to bounce packets back on the same interface see lower PPS than generic mode on small instances.

**Impact on this project:** **None.** A firewall only uses `XDP_DROP` and `XDP_PASS`. We never retransmit on the same interface.

**Workaround if you later need XDP_TX:** Use `XDP_REDIRECT` to a separate `veth` pair or use a larger instance (c5n.9xlarge reduces the gap significantly).

### Limitation 2: AF_XDP Zero-Copy Queue Constraints

**What:** AF_XDP zero-copy (for future packet capture/DPI features) requires the channel index to be less than half the maximum channel count.

**Impact:** Limits the number of zero-copy RX queues on small instances.

**Workaround:** On t3.small (max 2 channels, set to 1), only channel 0 supports zero-copy. Adequate for a lab.

### Limitation 3: Large LLQ + XDP Incompatibility

**What:** Large LLQ (Large LLQ entry size of 256 bytes, enabled by default on Nitro v4+ instances) is disabled when an XDP program is loaded. The TX queue size reduces to 512 in this mode.

**Impact:** Minor TX performance reduction when XDP is active.

**Workaround:** No action needed for a firewall (we're primarily doing RX processing). The driver handles this automatically.

### Limitation 4: LPC (Local Page Cache) Disabled with XDP

**What:** ENA's Local Page Cache (LPC) mechanism is disabled when an XDP program is loaded or when using fewer than 16 queue pairs.

**Impact:** Slightly higher memory pressure for RX buffer management.

**Workaround:** None needed for a lab setup. Monitor with `ethtool -S eth1 | grep lpc`.

### Limitation 5: Kernel Version Sensitivity

**What:** AF_XDP zero-copy performance can vary significantly between kernel versions. Kernel 6.11 showed half the throughput of 6.8 in some test scenarios.

**Impact:** Potential for inconsistent benchmark results if the kernel is upgraded.

**Workaround:** Pin the kernel version on your AMI once performance testing begins. Ubuntu 24.04 ships kernel **6.8** — stay on it for the lab.

```bash
# Pin kernel version (prevent auto-upgrades)
sudo apt-mark hold linux-image-$(uname -r) linux-headers-$(uname -r)
```

---

## 12. Future: Adding a Public IP for Traffic Testing

When you're ready to test with real external traffic:

### Option A: Elastic IP (EIP) — Simplest

```
EC2 → Elastic IPs → Allocate Elastic IP address
→ Associate with your instance (eth0 or eth1)
```

Attach to `eth1` (the XDP interface) for end-to-end testing. Update your security group to allow test traffic inbound.

### Option B: Second Instance as Traffic Generator

A cleaner lab setup uses two instances in the same VPC:

```
Instance 1: ebpfirewall-dev    (your XDP firewall, private only)
Instance 2: traffic-generator  (scapy / pktgen / iperf, private only)

Both in same subnet → private communication only
Traffic generator sends crafted packets → firewall XDP processes them
No public IP needed for either
```

```bash
# On traffic-generator instance, install scapy:
sudo apt install -y python3-scapy

# Example: send 10,000 packets to test DROP rule
python3 -c "
from scapy.all import *
pkts = [IP(src='10.0.0.1', dst='10.10.1.100')/TCP(dport=80) for _ in range(10000)]
send(pkts, iface='eth0', verbose=False)
print('sent 10000 packets')
"
```

### Option C: AWS VPC Traffic Mirroring

For production-like testing, AWS VPC Traffic Mirroring can duplicate real traffic to your XDP firewall instance for analysis without impacting the source traffic.

---

## 13. Glossary

| Term | Definition |
|---|---|
| **ENA** | Elastic Network Adapter — AWS's custom virtual NIC driver for Nitro instances |
| **Nitro** | AWS's hypervisor platform (replaces Xen). Required for ENA and XDP native mode |
| **XDP** | eXpress Data Path — Linux kernel subsystem for high-performance packet processing at the NIC driver level |
| **XDP Native (XDP_DRV)** | XDP program runs inside the NIC driver before SKB allocation. Fastest mode |
| **XDP Generic (XDP_SKB)** | XDP program runs after SKB allocation. Slower but works on any driver |
| **SKB / sk_buff** | Socket Kernel Buffer — kernel struct (~200 bytes) allocated per packet in the normal networking stack. XDP native avoids this |
| **BPF Map** | Kernel data structure (hash, array, LPM trie, etc.) shared between kernel XDP program and userspace. The "shared memory" of our firewall |
| **LPM Trie** | Longest Prefix Match Trie — BPF map type used for CIDR-based IP matching (e.g., block 192.168.0.0/16) |
| **CO-RE** | Compile Once, Run Everywhere — eBPF portability mechanism using BTF that allows BPF programs to run across different kernel versions |
| **BTF** | BPF Type Format — kernel debug info used for CO-RE and introspection |
| **Aya** | Rust library for writing and loading eBPF programs. Used for our control plane |
| **libbpf-rs** | Safe Rust wrapper around the C libbpf library. Alternative to Aya with full CO-RE support |
| **ENI** | Elastic Network Interface — AWS's virtual NIC attachment. Multiple ENIs can be attached to one instance |
| **NAPI** | New API — Linux kernel mechanism for batching NIC interrupt processing. ENA uses NAPI queues (channels) |
| **AF_XDP** | Address Family XDP — kernel socket type for zero-copy packet processing between kernel and userspace |
| **bpftool** | CLI utility to introspect and interact with BPF programs, maps, and BTF |
| **XDP_DROP** | XDP action: drop the packet immediately, before SKB allocation |
| **XDP_PASS** | XDP action: pass the packet up to the normal Linux network stack |
| **XDP_TX** | XDP action: send the packet back out the same interface |
| **XDP_REDIRECT** | XDP action: redirect the packet to another interface or AF_XDP socket |

---

## References

- [AWS ENA Driver Repository](https://github.com/amzn/amzn-drivers/tree/master/kernel/linux/ena) — official driver with XDP implementation
- [ENA Linux Best Practices](https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/ENA_Linux_Best_Practices.rst) — performance tuning guide
- [Aya eBPF Book](https://aya-rs.dev/book/) — getting started with Rust eBPF
- [XDP Tutorial (xdp-project)](https://github.com/xdp-project/xdp-tutorial) — reference C XDP programs
- [AWS EC2 Free Tier](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/ec2-free-tier-usage.html) — current free tier details
- [libbpf-bootstrap](https://github.com/libbpf/libbpf-bootstrap) — C eBPF project scaffolding
- [ENA XDP TX performance issue](https://github.com/amzn/amzn-drivers/issues/264) — known XDP_TX limitation documented

Now I have everything I need. Let me build the comprehensive setup document.Here's your complete setup guide, built from your existing findings plus fresh research. Key things I confirmed and added:

**On the NIC question** — AWS will not give you a physical high-end NIC (no Intel X710, no Mellanox ConnectX). What you get is the **ENA virtual NIC**, backed by the Nitro hypervisor. The good news: XDP support was introduced to the ENA driver in version 2.2.0, and Ubuntu 24.04 ships with ENA 2.x already. For a firewall that only needs `XDP_DROP` and `XDP_PASS`, this is entirely sufficient. The known XDP_TX performance limitation (TX ring size locked at 1024) only affects programs that bounce packets back on the same interface — a pure firewall never does this.

**On the free tier** — if your account was created on or after July 15, 2025, you can use t3.micro, t3.small, t4g.micro, t4g.small, c7i-flex.large, and m7i-flex.large for 6 months or until your $200 credits are used up. **t3.small is your target** — 2 GB RAM handles simultaneous Clang/LLVM + Rust toolchain builds without swap pressure.

**On the three-language architecture** — writing the eBPF program in C aligns with most production-grade open source projects like Cilium and Tracee, and because much of the eBPF ecosystem documentation assumes C, it makes it easier to understand, reuse, and extend existing examples. The userspace logic can remain in Rust, giving safety and modern tooling while relying on a stable foundation for kernel interaction. Go then handles the operator-facing management layer where HTTP APIs and concurrency patterns shine.

The two critical ENA prerequisites (MTU → 3498, halve the channel count) are fully documented with persistence commands for Ubuntu 24.04's netplan.

# eBPFirewall — GCP vs AWS for XDP/eBPF Lab

> **Short answer:** Yes, GCP can run XDP/eBPF. But for this specific lab on a free plan, **AWS is the better choice.** This document explains exactly why, with no hand-waving.

---

## Table of Contents

1. [GCP's Virtual NIC Landscape](#1-gcps-virtual-nic-landscape)
2. [XDP Support Status per GCP NIC Type](#2-xdp-support-status-per-gcp-nic-type)
3. [GCP Free Tier Reality](#3-gcp-free-tier-reality)
4. [The Core Problem: e2-micro vs t3.small](#4-the-core-problem-e2-micro-vs-t3small)
5. [GCP Setup Path (if you still choose GCP)](#5-gcp-setup-path-if-you-still-choose-gcp)
6. [Side-by-Side Comparison: AWS vs GCP](#6-side-by-side-comparison-aws-vs-gcp)
7. [Final Verdict](#7-final-verdict)

---

## 1. GCP's Virtual NIC Landscape

GCP has **two** virtual NIC types, and which one your VM gets depends entirely on the machine generation. This matters a lot for XDP.

### virtio-net (1st and 2nd generation machines)

The classic Linux `virtio-net` driver. This is what e2-micro, n1-standard, and other older machine series use. Virtio-net **does** support XDP native mode in the upstream Linux kernel — but with a painful prerequisite: GRO hardware offload (`GRO_HW`) and checksum offload (`CSUM`) must be disabled before the kernel will allow an XDP program to attach.

```bash
# Required before loading XDP on virtio-net
sudo ethtool -K ens4 gro off
sudo ethtool -K ens4 tx off rx off

# If GCP's hypervisor doesn't allow this, you get:
# "virtio_net: Can't set XDP while host is implementing GRO_HW/CSUM"
```

The catch: GCP's Andromeda hypervisor controls what offloads are exposed to the guest. In practice, disabling these offloads is not always permitted on all GCP VM types, and the error message above is a common failure report from users attempting native XDP on standard GCP VMs with virtio-net.

### gVNIC / gve (3rd generation and newer machines)

GCP's own virtual NIC, called **gVNIC**, with the Linux kernel driver named `gve`. This is the successor to virtio-net and is mandatory for 3rd-generation and later machine series (N2, C2, C3, N4, C4, etc.).

The good news: **gVNIC explicitly added native XDP support.** From the official Google driver repository:

> Driver-mode XDP support was introduced in release 1.3.4 for the GQI QPL queue format and 1.4.6 for the DQO RDA queue format.

The XDP queue requirement is identical to AWS ENA: RX and TX queue counts must each be set to no more than half their maximum values before attaching an XDP program.

```bash
# Check current queues on gVNIC interface
ethtool -l ens4

# Reduce to half maximum (example: max 4, set to 2)
sudo ethtool -L ens4 rx 2 tx 2
```

**However** — gVNIC machines (N2, C2, C3, etc.) are **not in the always-free tier.** More on this below.

---

## 2. XDP Support Status per GCP NIC Type

| NIC Driver | Machine Generation | XDP Native | Condition | Free Tier |
|---|---|---|---|---|
| `virtio-net` | E2, N1, N2 (1st/2nd gen) | ⚠️ Conditional | Must disable GRO_HW + CSUM offloads — GCP hypervisor may block this | e2-micro = Always Free |
| `gve` (gVNIC) | N2, C2, C3, N4, C4 (3rd gen+) | ✅ Native | Queue halving required (same as ENA). Driver >= 1.3.4 needed | **NOT free** |

### The virtio-net XDP native situation in detail

The Linux kernel's virtio-net driver does have XDP native support, but it requires the host (GCP's hypervisor) to configure the TAP device with enough queues: `queues = 2 × vCPUs`. If the hypervisor doesn't expose the right queue configuration, the kernel will refuse native XDP and emit:

```
libbpf: Kernel error message: virtio_net: XDP expects header/data 
in single page, any_header_sg required
```

or:

```
virtio_net: Can't set XDP while host is implementing GRO_HW/CSUM, 
disable GRO_HW/CSUM first
```

On the e2-micro (GCP's free VM), these errors are commonly encountered because the shared-core hypervisor configuration does not expose the required queue/offload setup for native XDP. XDP generic mode works fine on e2-micro — but that offers no performance advantage over iptables.

---

## 3. GCP Free Tier Reality

GCP has two distinct free tiers. They are very different from each other and from AWS.

### Always Free — Permanent, no expiry

```
Instance:  e2-micro only (2 shared vCPU, 1 GB RAM)
Duration:  Forever — does not expire
Region:    ONLY us-west1, us-central1, or us-east1
Storage:   30 GB standard persistent disk
Egress:    1 GB outbound per month to internet
```

Key facts about e2-micro:
- It is a **shared-core** machine: each vCPU gets only 12.5% of a physical core's time
- 1 GB RAM is tight even for basic development; Rust + LLVM compilation will OOM-kill
- Uses **virtio-net** — native XDP is problematic (GRO_HW/CSUM issue)
- Only the e2-micro qualifies — e2-small, n1-standard-1, and all other types charge immediately

### Free Trial — $300 credits, 90 days

```
Credits:   $300 USD
Duration:  90 days from account creation (NOT 6 months)
Instances: Any type, including N2, C2 (gVNIC, native XDP capable)
Limit:     Credits exhausted or 90 days — whichever comes first
```

The trial gives you access to proper machines with gVNIC, but 90 days is half the time you'd get on AWS's 6-month plan. For a learn-at-your-own-pace lab, this is a real constraint.

---

## 4. The Core Problem: e2-micro vs t3.small

This is the crux of why AWS wins for this specific project.

```
                        GCP e2-micro            AWS t3.small
                        (Always Free)           (Free, post-Jul 2025)
────────────────────────────────────────────────────────────────────
Free duration           Forever                 6 months
vCPU                    2 (shared, 12.5% ea)    2 (dedicated burst)
RAM                     1 GB                    2 GB  ← critical
Hypervisor              KVM (shared-core)       Nitro
NIC driver              virtio-net              ENA (gve analog)
XDP native mode         ⚠️ Problematic          ✅ Confirmed working
XDP generic mode        ✅ Works                ✅ Works
Rust build (aya)        ❌ OOM likely           ✅ Fits in 2 GB
LLVM/clang build        ❌ OOM likely           ✅ Fits in 2 GB
MTU issue for XDP       No                      Yes (fix: set to 3498)
Queue halving needed    Yes (virtio-net)        Yes (ENA)
Cost after free period  ~$6/month               ~$17/month (paid)
```

### RAM is the blocking issue

Compiling eBPF programs with Clang/LLVM and running the Rust toolchain simultaneously consumes 1.5–2 GB of RAM during a build. On the e2-micro's 1 GB, the kernel OOM killer will terminate your build processes. You would need to create a large swap file on the standard persistent disk — which is slow HDD-backed storage on e2-micro's "standard persistent disk" — making every Rust compile extremely slow.

On AWS t3.small with 2 GB, builds complete normally.

---

## 5. GCP Setup Path (if you still choose GCP)

If you have reasons to prefer GCP (familiarity, existing $300 trial credits, Google ecosystem), here is the correct path to get native XDP working.

### Option A: Use the $300 trial with a proper gVNIC machine

**Recommended instance: `n2-standard-2`**

```
vCPU: 2 (dedicated)
RAM:  8 GB
NIC:  gVNIC (gve driver, native XDP supported)
Cost: ~$0.097/hour → ~$70/month if running 24/7
      With $300 trial: ~4 months of continuous use, or longer if you stop it
```

```bash
# Create the instance with gVNIC explicitly enabled
gcloud compute instances create ebpfirewall-dev \
  --zone=us-central1-a \
  --machine-type=n2-standard-2 \
  --image-family=ubuntu-2404-lts \
  --image-project=ubuntu-os-cloud \
  --boot-disk-size=30GB \
  --boot-disk-type=pd-ssd \
  --network-interface=nic-type=GVNIC
```

> ⚠️ Stop the instance when not working to conserve trial credits. At ~$0.097/hr, 8 hours/day for 90 days ≈ $70, well within $300.

### Option B: e2-micro with XDP generic mode (free forever, limited learning)

If you want the truly free path and can accept XDP generic mode:

```bash
# e2-micro: always free in us-central1
gcloud compute instances create ebpfirewall-dev \
  --zone=us-central1-a \
  --machine-type=e2-micro \
  --image-family=ubuntu-2404-lts \
  --image-project=ubuntu-os-cloud \
  --boot-disk-size=30GB \
  --boot-disk-type=pd-standard
```

Then set up a large swap file to survive builds:

```bash
# 4 GB swap file on the standard persistent disk
sudo fallocate -l 4G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab

# Reduce swappiness so RAM is used first
echo 'vm.swappiness=10' | sudo tee -a /etc/sysctl.conf
sudo sysctl -p
```

**What you lose:** No XDP native mode (GRO_HW issue on e2-micro). Your XDP programs run in generic/SKB mode. You can still learn all the eBPF map logic, BPF verifier rules, and control plane architecture — the C/Rust/Go code is identical. You just cannot benchmark real XDP native performance.

### GCP gVNIC XDP Prerequisites (equivalent to ENA steps)

When using gVNIC on a proper machine (n2-standard-2 etc.):

```bash
# Check interface name (GCP uses ens4, not eth0/eth1)
ip link show
# GCP interface is typically: ens4 (primary), ens5 (secondary)

# Add a second NIC for safe XDP experimentation
# (via GCP Console → VM → Edit → Network interfaces → Add NIC)
# OR via gcloud:
gcloud compute instances network-interfaces add ebpfirewall-dev \
  --zone=us-central1-a \
  --network=default \
  --nic-type=GVNIC

# After reboot, check for ens5 (second NIC)
ip link show

# Halve the queue count before loading XDP
ethtool -l ens5
# Pre-set maximums: RX: 4, TX: 4 (example for n2-standard-2)
sudo ethtool -L ens5 rx 2 tx 2

# NO MTU change needed — gVNIC does not have the ENA jumbo frame restriction
# (gVNIC default MTU is 1460, already XDP-compatible)

# Verify driver version
ethtool -i ens5 | grep driver
# driver: gve
# version: 1.x.x  (must be >= 1.3.4 for GQI-QPL, >= 1.4.6 for DQO-RDA)

# Install the out-of-tree driver if Ubuntu ships an older version:
# git clone https://github.com/GoogleCloudPlatform/compute-virtual-ethernet-linux
# cd compute-virtual-ethernet-linux
# make -C /lib/modules/$(uname -r)/build M=$(pwd)/build modules modules_install
```

### GCP gVNIC vs ENA: Key Differences

| Aspect | AWS ENA | GCP gVNIC |
|---|---|---|
| MTU reduction needed | Yes (9001 → 3498) | No (default is 1460) |
| Queue halving needed | Yes (combined) | Yes (rx + tx separately) |
| Queue syntax | `ethtool -L ethX combined N` | `ethtool -L ensX rx N tx N` |
| Driver min version | ENA >= 2.2.0 | gve >= 1.3.4 (GQI-QPL) |
| XDP_DROP / XDP_PASS | ✅ | ✅ |
| XDP_TX | ⚠️ (TX ring = 1024) | ✅ (no TX ring limitation reported) |
| AF_XDP zero-copy | ✅ (with queue restriction) | ✅ |
| Interface name | eth0, eth1 | ens4, ens5 |

One genuine advantage of gVNIC: **no MTU reduction required.** GCP's gVNIC defaults to 1460 MTU (standard Ethernet minus overhead), which is already XDP-compatible. No `ip link set mtu 3498` step needed.

---

## 6. Side-by-Side Comparison: AWS vs GCP

```
CATEGORY              AWS                         GCP
────────────────────────────────────────────────────────────────────────────────
Free instance         t3.small                    e2-micro
                      (2 vCPU, 2 GB, Nitro)       (2 shared vCPU, 1 GB, KVM)

Free duration         6 months (new plan)         Always (forever)
                      12 months (old plan)         $300 trial = 90 days

XDP native (free)     ✅ ENA, confirmed            ⚠️ virtio-net, problematic
                                                   ❌ gVNIC requires paid VM

RAM for builds        ✅ 2 GB, adequate            ❌ 1 GB, OOM-kill risk
                                                   (need swap workaround)

Second NIC for lab    ✅ Attach ENI easily         ✅ Add NIC via gcloud/console

MTU config needed     Yes (9001 → 3498)           No (already 1460)

Queue halving needed  Yes (combined)              Yes (rx + tx separate)

Interface name        eth0 / eth1                 ens4 / ens5

Ubuntu 24.04 support  ✅ Native AMI               ✅ Native image

Kernel version        6.8 (Ubuntu 24.04)          6.8 (Ubuntu 24.04)

Out-of-tree driver?   Not needed (ENA >= 2.x      May need if gve < 1.3.4
                       in Ubuntu 24.04)             (check: ethtool -i ens4)

Trial credit trap     No — usage tracked by        Yes — $300 runs out after
                       hours/instance type          90 days of even moderate use

Best use case         This XDP firewall lab        General cloud learning,
                                                   longer-lived always-free VM
```

---

## 7. Final Verdict

### Choose AWS if:

- Your account was created after July 15, 2025 → **t3.small is free** and gives you confirmed ENA native XDP with 2 GB RAM
- You want the cleanest, most documented XDP-on-cloud experience (ENA has more community write-ups)
- You plan to work on this for 3–6 months (the full free plan duration)
- You want to follow the existing `setup.md` without major changes

### Choose GCP if:

- You already have $300 in GCP trial credits and want to use them
- You are comfortable spending a few dollars and want gVNIC's cleaner XDP_TX behavior (no TX ring limitation)
- You want an always-free VM to keep notes/reference code on (e2-micro for management plane only, do XDP work on a paid VM you spin up and stop)
- Your team or employer is a Google shop

### The hybrid approach (best of both)

If you're committed to GCP for the long term, consider this split:

```
GCP e2-micro (always free, forever):
  → Run the Go management API
  → Store firewall rules in YAML
  → Serve the metrics endpoint
  → Keep the repo

GCP n2-standard-2 (spin up only when doing XDP work, ~$0.10/hr):
  → Load the XDP C program
  → Run the Rust control plane
  → Do all eBPF experimentation
  → STOP the instance after each session
  → Total cost: 4 hours/day × 30 days = ~$12/month
```

This maximizes the always-free tier while keeping XDP work on a machine that actually supports it natively.

---

## Summary Table: Which Cloud, Which Path

| Your Situation | Recommendation |
|---|---|
| AWS account (post-Jul 2025) | **AWS t3.small** — free, 2 GB, ENA native XDP ✅ |
| AWS account (pre-Jul 2025) | **AWS t3.micro** + 4 GB swap — ENA native XDP, tight on RAM |
| GCP with $300 trial credits | **GCP n2-standard-2** with gVNIC — stop when not using |
| GCP always-free only | **Generic XDP mode only** on e2-micro + larger paid VM on demand |
| No preference | **AWS** for this specific lab, no contest |

Now I have everything I need for a thorough GCP vs AWS comparison. Let me build the document.Yes, GCP can run XDP/eBPF — but there are real complications specific to your free lab setup. Here is the honest picture:

**GCP has two NIC types, and they behave very differently:**

gVNIC is GCP's next-generation network interface that replaces VirtIO-Net as the only supported interface for all new machine types (Generation 3 and onwards). Older machines use the standard `virtio-net` driver. Driver-mode XDP support for gVNIC was introduced in release 1.3.4 for the GQI QPL queue format and 1.4.6 for the DQO RDA queue format, with the same requirement as ENA: RX and TX queues must be set to no more than half their maximum values before attaching an XDP program.

**The free tier problem is serious:**

Only the e2-micro qualifies for the Always Free compute tier — an e2-small, n1-standard-1, or any other machine type charges immediately, even in a free-tier US region. The e2-micro is a shared-core machine with only **1 GB RAM**, uses `virtio-net` (not gVNIC), and attempting native XDP on it frequently fails with hypervisor-blocked offload errors. GCP's $300 trial credit lasts only **90 days** — not 6 months.

**The key differences are captured in the document:**

- **AWS t3.small** (free, post-Jul 2025): 2 GB RAM, Nitro, ENA native XDP confirmed working — best fit for this lab
- **GCP e2-micro** (always free, forever): 1 GB RAM, virtio-net, native XDP problematic — RAM alone kills Rust/LLVM builds
- **GCP n2-standard-2** (gVNIC, $0.10/hr): proper native XDP, no MTU reduction needed (gVNIC defaults to 1460), but you're spending trial credits

One genuine advantage gVNIC has over ENA: **no MTU reduction step required** and no TX ring size limitation for XDP_TX, which could matter later when you add more advanced features. But for a free-plan lab, that advantage doesn't compensate for the free-tier RAM and driver constraints.

# XDP/eBPF Firewall — Native NIC Lab Setup Guide

> **Scope of this document:** Extends your existing AWS/GCP cloud research with the ground truth on
> bare-metal/native NIC setup — hardware selection, OS comparison, toolchain installation, and the
> three-language (C / Rust / Go) firewall architecture with component boundaries.

---

## Table of Contents

1. [XDP Mode Primer — Why Native Matters](#1-xdp-mode-primer)
2. [NIC Hardware — What Supports Native XDP](#2-nic-hardware)
3. [OS Selection — Ubuntu 24.04 vs RHEL 9/10](#3-os-selection)
4. [Lab Topology](#4-lab-topology)
5. [OS & Kernel Configuration](#5-os--kernel-configuration)
6. [Toolchain Installation](#6-toolchain-installation)
7. [Three-Language Architecture — Where Each Language Lives](#7-three-language-architecture)
8. [C: XDP Kernel Data Plane](#8-c-xdp-kernel-data-plane)
9. [Rust (Aya): Control Plane & Map Management](#9-rust-aya-control-plane)
10. [Go (cilium/ebpf): Management API & Rule Engine](#10-go-management-api)
11. [BPF Map Schema — Shared State Across Languages](#11-bpf-map-schema)
12. [Loading & Verifying the Program](#12-loading--verifying)
13. [NIC-Specific Quirks & Tuning](#13-nic-specific-quirks)
14. [Traffic Generator for Lab Testing](#14-traffic-generator)
15. [Debugging Toolkit](#15-debugging-toolkit)
16. [Cloud vs Native — Decision Matrix](#16-cloud-vs-native-decision-matrix)

---

## 1. XDP Mode Primer

XDP has three execution modes. The mode determines **where** your eBPF program runs and, critically,
how early it intercepts a packet.

```
NIC RX Ring
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│  XDP_DRV  (Native / Driver mode)                        │  ← EARLIEST: inside the driver,
│  Runs inside the NIC driver's NAPI poll loop.           │    no sk_buff allocated yet.
│  No sk_buff allocated — packet is a raw page frame.     │    ~10–40 Mpps/core on good NICs.
│  Driver must implement ndo_bpf() + xdp_xmit().          │
└─────────────────────────────────────────────────────────┘
     │  (if driver has no XDP support)
     ▼
┌─────────────────────────────────────────────────────────┐
│  XDP_SKB  (Generic mode)                                │  ← LATE: after sk_buff alloc.
│  Works on ANY NIC. Runs in the generic kernel path.     │    ~1–2 Mpps/core. Essentially
│  sk_buff is allocated, then discarded on XDP_DROP.      │    same cost as iptables. No gain.
└─────────────────────────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│  XDP_HW  (Hardware Offload)                             │  ← ON-CHIP: program runs on NIC
│  Program runs ON the NIC's embedded processor.          │    processor, zero host CPU usage.
│  Currently supported only by Netronome Agilio SmartNIC. │    Extremely limited helper set.
└─────────────────────────────────────────────────────────┘
```

**For a firewall lab: you want `XDP_DRV` (native mode).** Generic mode gives you correctness but not
performance — if you're using XDP just to avoid iptables overhead, generic mode defeats that entirely.

### XDP Return Codes Used in a Firewall

| Code        | Action                                         | Use Case                          |
|-------------|------------------------------------------------|-----------------------------------|
| `XDP_DROP`  | Drop packet at driver level, recycle RX buffer | Block rules, DDoS mitigation      |
| `XDP_PASS`  | Hand packet up to kernel network stack         | Allowed traffic                   |
| `XDP_TX`    | Retransmit packet back out the same interface  | Reflection attacks, loopback test |
| `XDP_ABORTED` | Drop + increment `xdp_exception` counter     | Bug/error path (not production)   |
| `XDP_REDIRECT` | Forward to another interface or CPU queue   | Load balancer, XSK (AF_XDP)       |

---

## 2. NIC Hardware

### Native XDP Driver Support Matrix

| Vendor        | NIC Family            | Driver    | Speed        | XDP Native | XDP ZeroCopy (AF_XDP) | Lab Cost (Used) |
|---------------|-----------------------|-----------|--------------|------------|------------------------|-----------------|
| **Intel**     | X710 / XL710          | `i40e`    | 10G / 40G    | ✅ Yes     | ✅ Yes                 | $30–80 USD      |
| **Intel**     | X520 / 82599ES        | `ixgbe`   | 10G          | ✅ Yes     | ✅ Yes                 | $15–40 USD      |
| **Intel**     | E810 series           | `ice`     | 25G / 100G   | ✅ Yes     | ✅ Yes                 | $150–300 USD    |
| **Mellanox**  | ConnectX-3 / ConnectX-3 Pro | `mlx4` | 10G / 40G | ✅ Yes  | ✅ Yes                 | $20–60 USD      |
| **Mellanox**  | ConnectX-4 / 5 / 6   | `mlx5`    | 25G / 100G   | ✅ Yes     | ✅ Yes (quirky*)       | $50–200 USD     |
| **Broadcom**  | BCM57414 / QL41xxx    | `bnxt_en` | 25G          | ✅ Yes     | ✅ Yes                 | $80–150 USD     |
| **Qlogic**    | FastLinQ 41000        | `qede`    | 25G          | ✅ Yes     | Partial                | $50–120 USD     |
| **Virtio**    | virtio-net (KVM/QEMU) | `virtio_net` | 1G–10G  | ✅ Yes     | ✅ Yes                 | Free (VM)       |
| **Netronome** | Agilio SmartNIC       | `nfp`     | 10G–40G      | ✅ Yes     | ✅ Yes + **HW Offload**| $400–800 USD    |
| **Realtek**   | RTL8111/8168          | `r8169`   | 1G           | ❌ No      | ❌ No                  | $5              |
| **Intel**     | I219-LM / I211        | `e1000e` / `igb` | 1G | ❌ No    | ❌ No                  | $10–20          |

> **Key takeaway:** Consumer/desktop NICs (Realtek, Intel I219) do **not** support native XDP.
> You need a server-class NIC — the Intel X710 or Mellanox ConnectX-4/5 are the sweet spot for a lab.

### Recommended Hardware for This Lab

```
Budget Lab (best ROI for eBPF firewall development):
  ─────────────────────────────────────────────────────
  NIC  : Intel X710-DA2 (dual 10G SFP+)
  Why  : i40e driver ships in every modern kernel.
         Native XDP, AF_XDP zero-copy, SR-IOV, full
         ethtool stats. Excellent community documentation.
         Used cards: $30–80 USD on eBay/AliExpress.
  Conn : SFP+ DAC cable (copper, passive) ~$8 USD.
         No switch needed for back-to-back lab topology.

  Server: Any x86 with PCIe x8 Gen3 slot.
           16 GB RAM minimum for build toolchains.
           8-core CPU (separate cores for NIC queues and
           IRQ affinity). Dell R620/R720 are common lab servers.

Mid-Range Lab (25G, future-proof):
  ─────────────────────────────────────────────────────
  NIC  : Mellanox ConnectX-5 25G (MCX512A-ACAT)
  Why  : mlx5 driver, best-in-class XDP performance.
         Hardware metadata offload (RSS, checksum, timestamps).
         ConnectX-5 is what Azure/OVH use in their hypervisors.
  Note : mlx5 AF_XDP zero-copy uses separate queue IDs.
         If you have N queues, XDP_ZEROCOPY uses queues [N..2N).
         You must halve queue count: ethtool -L <iface> combined N/2.

HW Offload Lab (program runs on-NIC):
  ─────────────────────────────────────────────────────
  NIC  : Netronome Agilio CX 2x10GbE (AGILIO-CX-2X10)
  Why  : Only NIC with true XDP hardware offload (XDP_HW).
         eBPF bytecode executes on the NIC's flow processor.
         Zero host CPU for dropped/redirected packets.
  Note : Subset of BPF helpers available.
         Not needed for a firewall (XDP_DROP + XDP_PASS work in HW).
```

### How to Verify Your NIC Supports Native XDP (After Boot)

```bash
# Method 1: Check for ndo_bpf in driver (most reliable)
ethtool -i <interface>        # note the "driver:" line
# Then check kernel source: grep -r "ndo_bpf" drivers/net/ethernet/<vendor>/

# Method 2: Try to load in native mode and see if it falls back
ip link set <interface> xdp obj xdp_drop.o sec xdp
ip link show <interface>
# Output contains "xdp"      → native mode loaded ✅
# Output contains "xdpgeneric" → fallback, native NOT supported ❌

# Method 3: bpftool (most explicit)
bpftool net show dev <interface>

# Method 4: Enumerate all XDP-capable drivers in your kernel
grep -r 'XDP_SETUP_PROG' /usr/src/linux-headers-$(uname -r)/include/ 2>/dev/null || \
  find /lib/modules/$(uname -r)/kernel/drivers/net -name "*.ko" | xargs modinfo 2>/dev/null \
  | grep -i xdp
```

---

## 3. OS Selection

### Ubuntu 24.04 LTS vs RHEL 9 (Developer Subscription — Free)

| Dimension                    | Ubuntu 24.04 LTS                              | RHEL 9.x                                          |
|------------------------------|-----------------------------------------------|---------------------------------------------------|
| **Kernel version**           | 6.8 (HWE: 6.11+)                             | 5.14 (RHEL9) / 6.12 (RHEL10)                     |
| **BTF embedded in kernel**   | ✅ Yes (`/sys/kernel/btf/vmlinux` present)   | ✅ Yes (since RHEL 8.2+)                          |
| **CO-RE support**            | ✅ Full (libbpf ships as system package)     | ✅ Full                                           |
| **libbpf package**           | `apt install libbpf-dev` (0.8+)              | `dnf install libbpf-devel` (backported)           |
| **bpftool**                  | `apt install linux-tools-$(uname -r)`        | `dnf install bpftool`                             |
| **Clang/LLVM version**       | Clang 18 (default)                           | Clang 17 (via LLVM toolset)                       |
| **Kernel BTF freshness**     | Tracks upstream closely                      | Backports; older struct layouts than upstream     |
| **XDP verifier strictness**  | Upstream verifier (latest)                   | Backport verifier — may accept programs Ubuntu rejects, or vice versa |
| **i40e driver version**      | Ships with kernel 6.8 (i40e 2.x)            | Separate `kmod-i40e` RPM may be needed            |
| **mlx5 driver**              | Ships in-tree                                | Ships in-tree + MLNX_OFED available for extras    |
| **Rust toolchain**           | Manual install via rustup                    | Manual install via rustup                         |
| **Go toolchain**             | Manual install or snap                       | Manual install                                    |
| **Developer friction**       | Low — `apt` resolves most dependencies       | Medium — some headers in `*-devel` separate RPMs  |
| **SELinux / AppArmor**       | AppArmor (less restrictive for BPF by default) | SELinux (can block BPF loading — needs policy)  |
| **Subscription cost**        | Free                                         | Free (Red Hat Developer Program, max 16 systems)  |
| **Kernel HWE**               | Available: 6.11+ on 24.04 via HWE stack     | Upgrade to RHEL 10 for 6.12                       |

### Verdict

**Use Ubuntu 24.04 for this lab.** Reasons:

1. Kernel 6.8 ships with the latest verifier fixes and XDP features. RHEL 9 ships kernel 5.14 —
   a 3-year-old kernel that lacks several modern BPF map types and XDP helpers you'll want.
2. `libbpf-dev`, `bpftool`, `linux-headers-*` are all in the same package manager with no
   version mismatch hunting.
3. AppArmor does not block `CAP_NET_ADMIN` / `CAP_BPF` by default. RHEL's SELinux requires
   policy tuning before `bpf()` syscalls work.
4. Aya (Rust eBPF framework) and cilium/ebpf (Go) both test primarily against upstream kernels —
   Ubuntu's kernel is closest to that.

**Use RHEL 9 if:** you are targeting production deployment on RHEL/CentOS enterprise environments
and need to verify CO-RE portability against an older kernel. Keep it as a secondary test machine.

---

## 4. Lab Topology

```
┌────────────────────────────────────────────────────────────┐
│                    Lab Server (bare metal)                 │
│                                                            │
│  ┌─────────────┐      ┌────────────────────────────────┐   │
│  │  eth0       │      │  Intel X710-DA2 (i40e driver)  │   │
│  │  (1G mgmt)  │      │  ┌──────────┐  ┌──────────┐    │   │
│  │  SSH / API  │      │  │  enp4s0f0│  │  enp4s0f1│    │   │
│  └─────────────┘      │  │  (RX/XDP)│  │  (TX/fwd)│    │   │
│                       │  └────┬─────┘  └────┬─────┘    │   │
│                       └───────┼─────────────┼──────────┘   │
│                               │             │              │
└───────────────────────────────┼─────────────┼──────────────┘
                                │ DAC cable   │ DAC cable
                           ┌────┴─────────────┴─────┐
                           │   Traffic Generator    │
                           │  (second server OR     │
                           │   same server with     │
                           │   network namespaces)  │
                           └────────────────────────┘

Control plane path:
  Go HTTP API server (port 8080)
    └─→ Rust Aya daemon
          └─→ BPF map updates (bpf_map_update_elem)
                └─→ C XDP program reads maps on every packet

Data plane path (per-packet, kernel space only):
  NIC RX ring
    └─→ i40e NAPI poll
          └─→ [XDP C program runs here]
                ├─→ XDP_DROP   (blocked src IP / port)
                ├─→ XDP_PASS   (allowed, sk_buff allocated)
                └─→ XDP_TX     (reflect back — for testing)
```

### Back-to-Back Lab Without a Switch

Connect `enp4s0f0` ↔ `enp4s0f1` with a SFP+ DAC cable. Load XDP only on `enp4s0f0`.
Use `enp4s0f1` as the traffic injection/generation interface.
Keep `eth0` (the onboard 1G NIC) strictly for SSH — never load XDP on it.

---

## 5. OS & Kernel Configuration

### Ubuntu 24.04 — Fresh Install Hardening for eBPF

```bash
# 1. Verify kernel version and BTF presence
uname -r                             # should be 6.8.x or higher
ls /sys/kernel/btf/vmlinux           # must exist for CO-RE

# 2. Enable BPF JIT hardening (recommended — prevents JIT spraying attacks)
echo 1 > /proc/sys/net/core/bpf_jit_enable
echo 1 > /proc/sys/net/core/bpf_jit_harden
# Persist:
cat >> /etc/sysctl.d/99-ebpf.conf << 'EOF'
net.core.bpf_jit_enable = 1
net.core.bpf_jit_harden = 1
kernel.unprivileged_bpf_disabled = 1
EOF
sysctl --system

# 3. Increase locked memory limit (needed for large BPF maps)
cat >> /etc/security/limits.conf << 'EOF'
*    soft    memlock    unlimited
*    hard    memlock    unlimited
EOF
# Also set in systemd service unit: LimitMEMLOCK=infinity

# 4. Enable huge pages (optional, improves XDP ring buffer performance)
echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
# Persist:
echo 'vm.nr_hugepages = 1024' >> /etc/sysctl.d/99-ebpf.conf

# 5. Mount BPF filesystem (usually auto-mounted on 24.04, verify)
mount | grep bpf
# If not mounted:
mount -t bpf bpf /sys/fs/bpf
# Add to /etc/fstab: none /sys/fs/bpf bpf defaults 0 0
```

### RHEL 9 — Additional Steps

```bash
# SELinux: Allow BPF program loading (create a policy module)
# Quick dev workaround (NOT for production):
setenforce 0   # or audit2allow to build proper policy

# Enable BPF syscall (disabled by some RHEL security profiles)
sysctl -w kernel.unprivileged_bpf_disabled=1   # prevents non-root BPF

# Install DKMS and kernel headers (required for out-of-tree NIC drivers)
dnf install -y kernel-devel kernel-headers dkms

# For i40e (Intel X710): driver ships in-tree but may need update
# Check: modinfo i40e | grep version
# If < 2.x: download from https://sourceforge.net/projects/e1000/files/i40e%20stable/
```

---

## 6. Toolchain Installation

### Complete Toolchain (Ubuntu 24.04)

```bash
# ── Core BPF dependencies ────────────────────────────────────────────────
sudo apt update && sudo apt install -y \
  clang-18 llvm-18 llvm-18-dev \
  libbpf-dev \
  linux-headers-$(uname -r) \
  linux-tools-$(uname -r) linux-tools-generic \
  bpftool \
  pkg-config libelf-dev zlib1g-dev \
  iproute2 \
  ethtool \
  pahole \            # generates vmlinux.h via bpftool btf dump
  make gcc

# Verify libbpf version (need >= 0.8 for modern CO-RE)
dpkg -l libbpf-dev | tail -1

# ── xdp-tools (libxdp + xdp-loader) ─────────────────────────────────────
# libxdp enables multi-program dispatch on a single interface
git clone --recurse-submodules https://github.com/xdp-project/xdp-tools.git
cd xdp-tools
./configure
make -j$(nproc)
sudo make install
sudo ldconfig

# ── Generate vmlinux.h (kernel type definitions for CO-RE) ───────────────
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
# This replaces including dozens of kernel headers in your C XDP programs.

# ── Rust toolchain ────────────────────────────────────────────────────────
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source ~/.cargo/env
rustup install stable
rustup target add bpfel-unknown-none  # BPF target (little-endian)

# Aya — Rust eBPF framework
cargo install bpf-linker            # LLVM-based BPF linker for Aya
cargo install cargo-generate        # project templating
# Aya project scaffold:
cargo generate --git https://github.com/aya-rs/aya-template \
  --name ebpfirewall

# ── Go toolchain ─────────────────────────────────────────────────────────
GO_VERSION=1.22.4
wget -q https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz
sudo rm -rf /usr/local/go
sudo tar -C /usr/local -xzf go${GO_VERSION}.linux-amd64.tar.gz
echo 'export PATH=$PATH:/usr/local/go/bin' >> ~/.bashrc
source ~/.bashrc
go version

# cilium/ebpf — Go eBPF library
mkdir -p ~/go/src/ebpfirewall-api && cd ~/go/src/ebpfirewall-api
go mod init ebpfirewall-api
go get github.com/cilium/ebpf@latest
go get github.com/vishvananda/netlink@latest   # for interface management

# ── Verify everything ─────────────────────────────────────────────────────
clang-18 --version | head -1
bpftool version
cargo --version
go version
bpftool feature probe | grep bpf_prog_type_xdp
```

---

## 7. Three-Language Architecture

The firewall is split into three planes with strict language-to-plane mapping:

```
┌──────────────────────────────────────────────────────────────────────────┐
│                        MANAGEMENT PLANE (Go)                             │
│                                                                          │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │  ebpfirewall-api (Go + cilium/ebpf)                                │  │
│  │  • REST/gRPC API: add/delete/list firewall rules                   │  │
│  │  • Rule compilation: IP/port/proto → BPF map key-value             │  │
│  │  • Stats polling: reads per-rule packet/byte counters from maps    │  │
│  │  • Health check: detects if XDP program is still attached          │  │
│  │  • Config persistence: rules in SQLite/YAML, replays on restart    │  │
│  └──────────────────────────────┬─────────────────────────────────────┘  │
│                                 │cilium/ebpf map.Update() / map.Lookup() │
└─────────────────────────────────┼────────────────────────────────────────┘
                                  │ BPF maps (kernel pinned at /sys/fs/bpf/)
┌─────────────────────────────────┼────────────────────────────────────────┐
│                        CONTROL PLANE (Rust)                              │
│                                 │                                        │
│  ┌──────────────────────────────▼─────────────────────────────────────┐  │
│  │  ebpfirewall-ctrl (Rust + Aya)                                     │  │
│  │  • Program lifecycle: compile, load, attach/detach XDP program     │  │
│  │  • Map initialization: create pinned maps on startup               │  │
│  │  • Hot-reload: swap XDP programs without traffic interruption      │  │
│  │  • Event loop: reads ring_buf / perf_event_array from kernel       │  │
│  │  • Alert pipeline: blocked-flow events → stdout / syslog           │  │
│  │  • Topology: manages multi-interface attach (one prog per NIC)     │  │
│  └──────────────────────────────┬─────────────────────────────────────┘  │
│                                 │ Aya Program::load() / Link::attach()   │
└─────────────────────────────────┼────────────────────────────────────────┘
                                  │ XDP program object file (.o)
┌─────────────────────────────────┼────────────────────────────────────────┐
│                        DATA PLANE (C)                                    │
│                                 │                                        │
│  ┌──────────────────────────────▼─────────────────────────────────────┐  │
│  │  xdp_firewall.c (C + libbpf + CO-RE)                               │  │
│  │  • Compiled to BPF bytecode by Clang/LLVM                          │  │
│  │  • Verified and JIT-compiled by the kernel                         │  │
│  │  • Runs per-packet, inside i40e/mlx5 NAPI poll (no sk_buff)        │  │
│  │  • Parses: ETH → IP → TCP/UDP headers in ≤ 512 bytes stack         │  │
│  │  • Lookups: bpf_map_lookup_elem() on blocklist / allowlist maps    │  │
│  │  • Actions: XDP_DROP, XDP_PASS, XDP_TX                             │  │
│  │  • Telemetry: bpf_ringbuf_submit() for blocked-flow events         │  │
│  └────────────────────────────────────────────────────────────────────┘  │
│                                                                          │
│  Runs at: kernel/driver level — BEFORE sk_buff allocation                │
└──────────────────────────────────────────────────────────────────────────┘

Why this split?
  C      → Only language the BPF verifier accepts for kernel-space programs.
            Precise memory model, manual bounds checking, no heap allocation.
  Rust   → Memory safety for the loader/event loop without garbage collection.
            Aya's type-safe BPF map wrappers catch type mismatches at compile time.
            Async event processing (Tokio) for ring buffer draining.
  Go     → Rapid API development, HTTP/gRPC server, database I/O.
            cilium/ebpf provides idiomatic Go access to pinned maps.
            Go's garbage collector is acceptable here — this is not the hot path.
```

---

## 8. C: XDP Kernel Data Plane

### `xdp_firewall.c` — Annotated Production-Grade Example

```c
// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"           // CO-RE: generated from /sys/kernel/btf/vmlinux
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

// ── Map Definitions ──────────────────────────────────────────────────────
// Key: source IPv4 address (network byte order)
// Value: action (0 = PASS, 1 = DROP)
struct {
    __uint(type, BPF_MAP_TYPE_LPM_TRIE);   // Longest Prefix Match for CIDR rules
    __uint(max_entries, 65536);
    __uint(map_flags, BPF_F_NO_PREALLOC);
    __type(key, struct bpf_lpm_trie_key);  // {prefixlen, data[4]}
    __type(value, __u32);                  // action
} ip_blocklist SEC(".maps");

// Per-CPU counters: avoid atomic ops on hot path
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 2);                // [0]=pass_count, [1]=drop_count
    __type(key, __u32);
    __type(value, __u64);
} stats SEC(".maps");

// Ring buffer for blocked-flow events (read by Rust control plane)
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 24);          // 16 MB ring
} events SEC(".maps");

// ── Event struct sent to userspace ───────────────────────────────────────
struct firewall_event {
    __u32 src_ip;
    __u32 dst_ip;
    __u16 src_port;
    __u16 dst_port;
    __u8  proto;
    __u8  action;    // 0 = DROP, 1 = PASS
    __u64 timestamp; // bpf_ktime_get_ns()
};

// ── XDP Program Entry Point ───────────────────────────────────────────────
SEC("xdp")
int xdp_firewall_prog(struct xdp_md *ctx)
{
    void *data_end = (void *)(long)ctx->data_end;
    void *data     = (void *)(long)ctx->data;

    // ── Parse Ethernet header ────────────────────────────────────────────
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;   // malformed: let kernel handle

    // Skip non-IPv4 (ARP, IPv6 handled separately)
    if (bpf_ntohs(eth->h_proto) != ETH_P_IP)
        return XDP_PASS;

    // ── Parse IP header ──────────────────────────────────────────────────
    struct iphdr *ip = (void *)(eth + 1);
    if ((void *)(ip + 1) > data_end)
        return XDP_PASS;

    // ── LPM lookup: is src IP in blocklist? ──────────────────────────────
    struct {
        __u32 prefixlen;
        __u32 addr;
    } lpm_key = {
        .prefixlen = 32,
        .addr = ip->saddr,
    };

    __u32 *action = bpf_map_lookup_elem(&ip_blocklist, &lpm_key);

    if (action && *action == 1) {
        // ── Emit event to ring buffer ────────────────────────────────────
        struct firewall_event *ev;
        ev = bpf_ringbuf_reserve(&events, sizeof(*ev), 0);
        if (ev) {
            ev->src_ip    = ip->saddr;
            ev->dst_ip    = ip->daddr;
            ev->proto     = ip->protocol;
            ev->action    = 0;  // DROP
            ev->timestamp = bpf_ktime_get_ns();

            // Parse L4 ports if TCP/UDP
            if (ip->protocol == IPPROTO_TCP) {
                struct tcphdr *tcp = (void *)(ip + 1);
                if ((void *)(tcp + 1) <= data_end) {
                    ev->src_port = bpf_ntohs(tcp->source);
                    ev->dst_port = bpf_ntohs(tcp->dest);
                }
            } else if (ip->protocol == IPPROTO_UDP) {
                struct udphdr *udp = (void *)(ip + 1);
                if ((void *)(udp + 1) <= data_end) {
                    ev->src_port = bpf_ntohs(udp->source);
                    ev->dst_port = bpf_ntohs(udp->dest);
                }
            }
            bpf_ringbuf_submit(ev, 0);
        }

        // ── Update drop counter ──────────────────────────────────────────
        __u32 key = 1;
        __u64 *cnt = bpf_map_lookup_elem(&stats, &key);
        if (cnt) __sync_fetch_and_add(cnt, 1);

        return XDP_DROP;
    }

    // ── Update pass counter ──────────────────────────────────────────────
    __u32 key = 0;
    __u64 *cnt = bpf_map_lookup_elem(&stats, &key);
    if (cnt) __sync_fetch_and_add(cnt, 1);

    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
```

### Compile the XDP Program

```bash
# Generate vmlinux.h first (once per kernel version)
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

# Compile to BPF bytecode
clang-18 -O2 -g -Wall \
  -target bpf \
  -D__TARGET_ARCH_x86 \
  -I. \
  -c xdp_firewall.c \
  -o xdp_firewall.o

# Inspect the compiled object
llvm-objdump-18 -S xdp_firewall.o
bpftool prog dump xlated file xdp_firewall.o  # after loading
```

---

## 9. Rust (Aya): Control Plane

### Project Structure

```
ebpfirewall-ctrl/
├── Cargo.toml
├── src/
│   ├── main.rs          ← Tokio async runtime, CLI, program lifecycle
│   ├── loader.rs        ← load_xdp_program(), attach(), detach(), hot_reload()
│   ├── maps.rs          ← typed BPF map wrappers (LpmTrie, PerfEventArray, RingBuf)
│   └── events.rs        ← ring buffer drain loop → alert pipeline
└── xtask/
    └── main.rs          ← build script: compile C XDP program via clang
```

### Key Aya Snippets

```rust
// Cargo.toml dependencies:
// aya = { version = "0.13", features = ["async_tokio"] }
// aya-log = "0.2"
// tokio = { version = "1", features = ["full"] }

use aya::{Bpf, programs::{Xdp, XdpFlags}};
use aya::maps::lpm_trie::{LpmTrie, Key};
use aya::maps::RingBuf;
use std::net::Ipv4Addr;

pub struct FirewallLoader {
    bpf: Bpf,
    iface: String,
}

impl FirewallLoader {
    /// Load XDP object file and attach to interface in native mode.
    pub fn load_and_attach(obj_path: &str, iface: &str) -> anyhow::Result<Self> {
        let mut bpf = Bpf::load_file(obj_path)?;

        let program: &mut Xdp = bpf.program_mut("xdp_firewall_prog")
            .unwrap()
            .try_into()?;

        program.load()?;

        // XdpFlags::DRV_MODE = native/driver mode
        // XdpFlags::SKB_MODE = generic fallback
        program.attach(iface, XdpFlags::DRV_MODE)?;

        Ok(Self { bpf, iface: iface.to_string() })
    }

    /// Insert a CIDR block rule into the LPM trie.
    pub fn block_cidr(&mut self, cidr: Ipv4Addr, prefix_len: u32) -> anyhow::Result<()> {
        let mut map: LpmTrie<_, [u8; 4], u32> =
            LpmTrie::try_from(self.bpf.map_mut("ip_blocklist")?)?;

        let key = Key::new(prefix_len, cidr.octets());
        let action: u32 = 1; // DROP
        map.insert(&key, action, 0)?;
        Ok(())
    }

    /// Drain the ring buffer and process blocked-flow events.
    pub async fn drain_events(&mut self) -> anyhow::Result<()> {
        let ring_buf = RingBuf::try_from(self.bpf.map_mut("events")?)?;

        loop {
            // Non-blocking poll; integrate with tokio::io::AsyncFd for production
            while let Some(item) = ring_buf.next() {
                let event = parse_firewall_event(&item);
                log::warn!("BLOCKED: src={} dst={} proto={} ts={}",
                    Ipv4Addr::from(event.src_ip),
                    Ipv4Addr::from(event.dst_ip),
                    event.proto,
                    event.timestamp);
            }
            tokio::time::sleep(std::time::Duration::from_millis(10)).await;
        }
    }
}
```

---

## 10. Go: Management API

### API Server with cilium/ebpf

```go
// main.go — HTTP management API
package main

import (
    "encoding/binary"
    "encoding/json"
    "log"
    "net"
    "net/http"

    "github.com/cilium/ebpf"
    "github.com/cilium/ebpf/rlimit"
)

// Must match the C struct layout exactly (use __u32 for both fields)
type LpmKey struct {
    Prefixlen uint32
    Addr      [4]byte
}

type FirewallAPI struct {
    blocklist *ebpf.Map // opened from pinned path /sys/fs/bpf/ip_blocklist
    stats     *ebpf.Map // opened from pinned path /sys/fs/bpf/stats
}

func NewFirewallAPI() (*FirewallAPI, error) {
    // Remove memlock limit (required for BPF map operations)
    if err := rlimit.RemoveMemlock(); err != nil {
        return nil, err
    }

    blocklist, err := ebpf.LoadPinnedMap("/sys/fs/bpf/ip_blocklist", nil)
    if err != nil {
        return nil, err
    }
    stats, err := ebpf.LoadPinnedMap("/sys/fs/bpf/stats", nil)
    if err != nil {
        return nil, err
    }
    return &FirewallAPI{blocklist: blocklist, stats: stats}, nil
}

// POST /rules/block {"cidr": "192.168.1.0/24"}
func (fw *FirewallAPI) BlockHandler(w http.ResponseWriter, r *http.Request) {
    var req struct{ CIDR string `json:"cidr"` }
    if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
        http.Error(w, err.Error(), 400)
        return
    }

    ip, ipnet, err := net.ParseCIDR(req.CIDR)
    if err != nil {
        http.Error(w, "invalid CIDR", 400)
        return
    }

    prefixLen, _ := ipnet.Mask.Size()
    key := LpmKey{Prefixlen: uint32(prefixLen)}
    copy(key.Addr[:], ip.To4())

    var action uint32 = 1 // DROP
    if err := fw.blocklist.Update(key, action, ebpf.UpdateAny); err != nil {
        http.Error(w, err.Error(), 500)
        return
    }

    w.WriteHeader(http.StatusNoContent)
}

// GET /stats → {"pass": N, "drop": N}
func (fw *FirewallAPI) StatsHandler(w http.ResponseWriter, r *http.Request) {
    var passKey, dropKey uint32 = 0, 1
    var passVal, dropVal []uint64 // per-CPU values

    if err := fw.stats.Lookup(passKey, &passVal); err != nil {
        http.Error(w, err.Error(), 500)
        return
    }
    if err := fw.stats.Lookup(dropKey, &dropVal); err != nil {
        http.Error(w, err.Error(), 500)
        return
    }

    // Sum across CPUs
    var totalPass, totalDrop uint64
    for _, v := range passVal { totalPass += v }
    for _, v := range dropVal { totalDrop += v }

    json.NewEncoder(w).Encode(map[string]uint64{
        "pass": totalPass,
        "drop": totalDrop,
    })
}

func main() {
    fw, err := NewFirewallAPI()
    if err != nil {
        log.Fatal(err)
    }

    http.HandleFunc("/rules/block", fw.BlockHandler)
    http.HandleFunc("/stats", fw.StatsHandler)

    log.Println("Management API listening on :8080")
    log.Fatal(http.ListenAndServe(":8080", nil))
}
```

---

## 11. BPF Map Schema

Maps are the shared memory between all three language components. Pin them to the BPF filesystem
so all processes can access them independently.

```
/sys/fs/bpf/
├── xdp_firewall_prog       ← pinned program link (managed by Rust loader)
├── ip_blocklist            ← LPM_TRIE: CIDR → action (written by Go, read by C)
├── stats                   ← PERCPU_ARRAY: pass/drop counters (written by C, read by Go)
└── events                  ← RINGBUF: blocked-flow events (written by C, drained by Rust)

Map Type Selection Rationale:
  ip_blocklist  → BPF_MAP_TYPE_LPM_TRIE
                  Supports /24, /16, /8 CIDR blocks natively.
                  O(log n) lookup. Max 65536 entries for a lab.
                  Alternative: BPF_MAP_TYPE_HASH for exact /32 lookups (O(1)).

  stats         → BPF_MAP_TYPE_PERCPU_ARRAY
                  Each CPU updates its own slot → no atomic contention.
                  Userspace sums all CPU values on read.
                  Zero false sharing — critical on multi-queue NICs.

  events        → BPF_MAP_TYPE_RINGBUF (kernel 5.8+)
                  Single ring, multiple producers (CPUs), one consumer (Rust).
                  Does NOT copy data — zero-copy reserve/submit model.
                  Replaced BPF_MAP_TYPE_PERF_EVENT_ARRAY for most use cases.
```

---

## 12. Loading & Verifying

```bash
# ── Step 1: Pin maps (done by Rust loader on startup, shown manually here) ─
# The Rust/Aya loader creates and pins maps automatically from the .o file.
# Manually with bpftool (for debugging):
bpftool map create /sys/fs/bpf/ip_blocklist \
  type lpm_trie key 8 value 4 entries 65536 name ip_blocklist flags 1

# ── Step 2: Load and attach XDP program ─────────────────────────────────
# Via Rust loader (recommended):
./ebpfirewall-ctrl --iface enp4s0f0 --obj xdp_firewall.o

# OR manually via ip link (for testing):
ip link set enp4s0f0 xdp obj xdp_firewall.o sec xdp
# ^ Use "xdpdrv" flag to FORCE native mode (fail if not supported):
ip link set enp4s0f0 xdpdrv obj xdp_firewall.o sec xdp

# ── Step 3: Verify mode ──────────────────────────────────────────────────
ip link show enp4s0f0
# Look for: "xdp" (native) vs "xdpgeneric" (fallback)

bpftool net show dev enp4s0f0
# Output: xdp:
#   xdp_firewall_prog  id 42  tag a1b2c3d4e5f6a7b8  jited

# ── Step 4: Insert a block rule ─────────────────────────────────────────
# Via Go API:
curl -X POST http://localhost:8080/rules/block \
  -H 'Content-Type: application/json' \
  -d '{"cidr":"198.51.100.0/24"}'

# OR directly via bpftool (for testing):
# LPM key = prefixlen (4 bytes BE) + addr (4 bytes)
bpftool map update pinned /sys/fs/bpf/ip_blocklist \
  key hex 00 00 00 20 c6 33 64 00 \
  value hex 01 00 00 00

# ── Step 5: Verify stats ─────────────────────────────────────────────────
curl http://localhost:8080/stats
# {"pass": 1234567, "drop": 89}

# ── Step 6: Detach (safe, traffic resumes immediately) ───────────────────
ip link set enp4s0f0 xdp off
```

---

## 13. NIC-Specific Quirks & Tuning

### Intel X710 (i40e)

```bash
# ── MTU: X710 supports jumbo frames up to 9706 bytes. XDP native requires
# packets fit in one buffer. For 1500B MTU frames, default is fine.
# If you see XDP load failures: reduce MTU first.
ip link set enp4s0f0 mtu 1500

# ── Queue count: match XDP queues to CPU count for IRQ affinity
# Check current queues:
ethtool -l enp4s0f0

# Set to number of available CPUs (or half for RX+TX separation):
ethtool -L enp4s0f0 combined 4   # for a 4-core machine

# ── IRQ affinity: pin each queue to a specific CPU
# Prevents cross-CPU interrupts from destroying cache locality.
# i40e ships a script for this:
# /usr/local/lib/i40e/set_irq_affinity.sh 0,1,2,3 enp4s0f0

# ── Disable GRO/LRO: XDP processes individual frames; GRO merges them.
# GRO is automatically disabled in native XDP mode. Verify:
ethtool -k enp4s0f0 | grep receive-offload

# ── RSS hash key: for symmetric hashing (src/dst are interchangeable)
# Useful when XDP redirect is used to load-balance flows.
ethtool -X enp4s0f0 hfunc toeplitz
```

### Mellanox ConnectX-5 (mlx5)

```bash
# ── AF_XDP zero-copy quirk: mlx5 uses separate queue IDs for zero-copy.
# Normal queues: [0..N), zero-copy queues: [N..2N).
# To use zero-copy AF_XDP, halve the queue count first:
ethtool -l enp1s0     # check combined max
ethtool -L enp1s0 combined 16   # if max is 32, set to 16

# ── Channel type: mlx5 uses "combined" channels by default.
# For RX-only XDP workloads, you CAN use rx/tx channels separately.

# ── VLAN stripping: disable if your XDP program needs VLAN headers
ethtool -K enp1s0 rx-vlan-offload off

# ── Firmware update (sometimes required for new XDP features):
# Check: ethtool -i enp1s0 | grep firmware
# Update via mstflint or MLNX_OFED mstconfig tool.
```

---

## 14. Traffic Generator for Lab Testing

Without a second server, use network namespaces to simulate traffic on the same machine.

```bash
# ── Setup: two namespaces connected via veth pair ────────────────────────
ip netns add generator
ip netns add receiver

ip link add veth-gen type veth peer name veth-recv
ip link set veth-gen netns generator
ip link set veth-recv netns receiver

ip netns exec generator ip addr add 10.10.0.1/24 dev veth-gen
ip netns exec receiver  ip addr add 10.10.0.2/24 dev veth-recv
ip netns exec generator ip link set veth-gen up
ip netns exec receiver  ip link set veth-recv up

# Load XDP on veth-recv (note: veth supports native XDP since kernel 4.20)
ip netns exec receiver ip link set veth-recv xdpdrv obj xdp_firewall.o sec xdp

# ── pktgen (kernel packet generator — highest rate) ──────────────────────
modprobe pktgen
# See /proc/net/pktgen/ for per-thread configuration

# ── hping3 (flexible, good for firewall rule testing) ────────────────────
sudo apt install -y hping3
# Flood test (SYN from a blocked IP range):
ip netns exec generator hping3 --flood --syn -p 80 \
  --spoof 198.51.100.50 10.10.0.2

# ── iperf3 (throughput baseline) ─────────────────────────────────────────
ip netns exec receiver  iperf3 -s &
ip netns exec generator iperf3 -c 10.10.0.2 -t 30 -P 4

# ── scapy (precise packet crafting for protocol edge cases) ──────────────
pip install scapy --break-system-packages
ip netns exec generator python3 - << 'EOF'
from scapy.all import *
# Craft a packet from blocked CIDR to test drop:
pkt = Ether()/IP(src="198.51.100.1", dst="10.10.0.2")/TCP(dport=443)
sendp(pkt, iface="veth-gen", count=1000, inter=0.001)
EOF
```

---

## 15. Debugging Toolkit

```bash
# ── 1. Verify JIT compilation ────────────────────────────────────────────
bpftool prog show name xdp_firewall_prog
# Look for "jited" — means BPF bytecode → native x86_64 instructions ✅
# If "not jited": check bpf_jit_enable sysctl

# ── 2. Dump JIT output (native x86_64 assembly) ─────────────────────────
bpftool prog dump jited name xdp_firewall_prog

# ── 3. Trace verifier output (if program is rejected) ────────────────────
# The kernel logs verifier errors to dmesg:
dmesg | tail -50 | grep -i bpf

# To see full verifier log programmatically:
bpftool prog load xdp_firewall.o /sys/fs/bpf/test_prog 2>&1

# ── 4. Monitor XDP stats via ethtool ────────────────────────────────────
watch -n1 'ethtool -S enp4s0f0 | grep -E "xdp|drop|rx_bytes"'

# ── 5. BPF program tracing with bpftrace ─────────────────────────────────
sudo apt install -y bpftrace
# Count XDP actions per second:
bpftrace -e 'tracepoint:xdp:xdp_exception { @[args->act] = count(); }'

# ── 6. Inspect map contents ──────────────────────────────────────────────
bpftool map dump pinned /sys/fs/bpf/ip_blocklist
bpftool map dump pinned /sys/fs/bpf/stats

# ── 7. perf record for cache/PMU stats ──────────────────────────────────
sudo perf stat -e \
  cache-misses,cache-references,LLC-load-misses,LLC-store-misses \
  -a -- sleep 5

# ── 8. XDP exception counter (catches XDP_ABORTED) ──────────────────────
bpftool net show dev enp4s0f0
ethtool -S enp4s0f0 | grep xdp_exception
```

---

## 16. Cloud vs Native — Decision Matrix

| Criterion                              | AWS (ENA on t3.small) | GCP (gVNIC)   | Native NIC (X710) |
|----------------------------------------|-----------------------|---------------|-------------------|
| XDP native mode                        | ✅ Yes (ENA ≥ 2.2)   | ✅ Yes        | ✅ Yes            |
| XDP_TX (hairpin)                       | ⚠️ Limited (TX ring = 1024) | ⚠️ | ✅ Full           |
| AF_XDP zero-copy                       | ✅ Yes               | ✅ Yes        | ✅ Yes            |
| MTU reduction required for XDP        | ✅ Yes (→ 3498)      | ✅ Yes        | ❌ Not required   |
| Queue tuning required                  | ✅ Yes               | Varies        | ✅ Recommended    |
| Physical NIC driver control            | ❌ No (hypervisor hides NIC) | ❌ No | ✅ Full           |
| IRQ affinity control                   | ❌ No                | ❌ No         | ✅ Full           |
| NIC firmware update                    | ❌ No                | ❌ No         | ✅ Full           |
| SR-IOV / VF creation                   | ❌ No                | ❌ No         | ✅ Yes            |
| HW offload (XDP_HW)                    | ❌ No                | ❌ No         | ✅ Netronome only |
| Hardware RSS hash key control          | ❌ No                | ❌ No         | ✅ Full           |
| Observed peak PPS (XDP_DROP)           | ~3–5 Mpps            | ~2–4 Mpps     | ~14–20 Mpps (10G line-rate) |
| Cost (dev lab, monthly)               | ~$15–30 USD          | ~$10–25 USD   | ~$0 (one-time $50–150 HW)  |
| Iteration speed (redeploy)            | Fast                 | Fast          | Instant (no API round-trip) |
| **Recommended for**                   | CI, cloud-native testing | CI        | NIC driver hacking, perf profiling |

### Recommendation Summary

Use **native NIC** when:
- You need to characterize actual XDP_DRV throughput at line rate
- You're debugging NIC driver interactions (IRQ affinity, GRO, RSS)
- You need SR-IOV, AF_XDP zero-copy, or hardware offload experimentation
- You want to develop without cloud egress billing

Use **AWS/GCP** when:
- You want disposable environments for CI/CD testing of correctness (not performance)
- You're testing CO-RE portability across kernel versions (spin up different AMIs)
- You need to replicate a specific cloud customer's deployment environment

---

## Quick Reference: Driver-to-NIC Mapping

```
Driver    NIC Family                      Speed       XDP Mode
──────    ───────────────────────────────────────────────────────
i40e      Intel X710, XL710, XXV710       10/40G      Native ✅
ixgbe     Intel X520, 82599               10G         Native ✅
ice       Intel E810 (Columbiaville)      25/100G     Native ✅
mlx4      Mellanox ConnectX-3/3 Pro       10/40G      Native ✅
mlx5      Mellanox ConnectX-4/5/6, CX-7  25–400G     Native ✅
bnxt_en   Broadcom BCM57xxx               25G         Native ✅
qede      QLogic FastLinQ 41xxx           25G         Native ✅
nfp       Netronome Agilio               10/40G      Native + HW Offload ✅
virtio_net QEMU/KVM virtual              1–10G       Native ✅
tun/veth  Linux virtual device           N/A         Native ✅ (4.20+)
r8169     Realtek RTL8111/8168           1G          Generic only ❌
e1000e    Intel I219/I218 (desktop)      1G          Generic only ❌
igb       Intel I211/I350                1G          Generic only ❌
```

---

*Document version: 2025-Q2 | Kernel baseline: Linux 6.8 | libbpf: 1.x | Aya: 0.13 | cilium/ebpf: 0.16*

# eBPF XDP Firewall — Complete Implementation Guide

> **Stack:** Linux XDP · C (kernel BPF) · Rust/Aya (control plane) · Go (management API)
> **Target:** AWS EC2 Nitro (t3.small+) · Ubuntu 24.04 · Kernel 6.8+

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [AWS Setup & Prerequisites](#2-aws-setup--prerequisites)
3. [Toolchain Installation](#3-toolchain-installation)
4. [Project Structure](#4-project-structure)
5. [C — XDP Kernel Program](#5-c--xdp-kernel-program)
6. [Rust — Aya Control Plane](#6-rust--aya-control-plane)
7. [Go — Management API & CLI](#7-go--management-api--cli)
8. [Build & Deploy](#8-build--deploy)
9. [Testing & Verification](#9-testing--verification)
10. [Troubleshooting](#10-troubleshooting)

---

## 1. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          AWS EC2 (t3.small, Nitro)                       │
│                                                                           │
│   ┌──────────────────────────────────────────────────────────────────┐  │
│   │                      User Space                                    │  │
│   │                                                                    │  │
│   │  ┌──────────────┐    ┌──────────────────────────────────────┐    │  │
│   │  │  Go CLI/API  │───▶│         Rust / Aya Control Plane      │    │  │
│   │  │  (firewall   │    │  - Loads XDP program into kernel       │    │  │
│   │  │   manage)    │    │  - Reads/writes BPF Maps (rules)       │    │  │
│   │  └──────────────┘    │  - Monitors per-IP packet counters     │    │  │
│   │                      └────────────────┬─────────────────────┘    │  │
│   └───────────────────────────────────────┼──────────────────────────┘  │
│                                            │  syscall: bpf()              │
│   ┌───────────────────────────────────────▼──────────────────────────┐  │
│   │                      Kernel Space                                  │  │
│   │                                                                    │  │
│   │   BPF Maps (shared memory)                                        │  │
│   │   ┌─────────────────┐  ┌──────────────┐  ┌──────────────────┐   │  │
│   │   │ blocklist_ipv4  │  │ allow_ports  │  │  stats_map       │   │  │
│   │   │ (LPM Trie)      │  │ (Hash Map)   │  │  (per-IP counts) │   │  │
│   │   └────────┬────────┘  └──────┬───────┘  └────────┬─────────┘   │  │
│   │            │                  │                    │              │  │
│   │   ┌────────▼──────────────────▼────────────────────▼──────────┐  │  │
│   │   │               XDP Program (C / eBPF bytecode)              │  │  │
│   │   │   - Parses Ethernet → IP → TCP/UDP headers                 │  │  │
│   │   │   - Checks IP against LPM blocklist                        │  │  │
│   │   │   - Checks destination port against allowlist              │  │  │
│   │   │   - Returns XDP_DROP or XDP_PASS                           │  │  │
│   │   └───────────────────────────────────────────────────────────┘  │  │
│   │                         ▲                                         │  │
│   │                         │ XDP hook (driver level, before SKB)    │  │
│   └─────────────────────────┼──────────────────────────────────────--┘  │
│                              │                                            │
│   ┌──────────────────────────┴───────────────────────────────────────┐  │
│   │   ENA NIC Driver (eth1) — Native XDP mode                        │  │
│   │   MTU: 3498 · Combined channels: reduced · ENA driver >= 2.2.0   │  │
│   └──────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

### Why Three Languages?

| Layer | Language | Role | Why |
|---|---|---|---|
| **Kernel** | C | XDP packet filter | Only C/restricted subset compiles to eBPF bytecode the verifier accepts |
| **Control Plane** | Rust (Aya) | Load BPF, manage maps | Memory-safe, zero-cost abstractions; Aya has first-class eBPF support |
| **Management Plane** | Go | REST API + CLI | Fast development, great stdlib networking, easy HTTP server |

### Key Concepts

**XDP (eXpress Data Path)** — A hook point in the Linux kernel that runs your eBPF program inside the NIC driver, *before* the kernel allocates an `sk_buff` (socket buffer). This is the earliest interception point — packets you drop here never touch the kernel networking stack.

**BPF Maps** — Kernel-resident key/value stores that both kernel (XDP program) and user space (Rust/Go) can read and write. This is the shared state between all three layers.

**LPM Trie (Longest Prefix Match)** — A kernel BPF map type specifically designed for IP subnet lookups. `192.168.0.0/16` matches before `192.168.0.0/24` loses — the longer (more specific) prefix wins. Essential for efficient CIDR-based firewalling.

**Verifier** — The kernel's static analysis engine. Before any eBPF program runs, the verifier checks every possible execution path to ensure the program cannot crash the kernel, access out-of-bounds memory, or run infinite loops.

---

## 2. AWS Setup & Prerequisites

### Instance Selection

| Instance | Hypervisor | ENA | XDP Native | Notes |
|---|---|---|---|---|
| t2.micro | Xen | No | ❌ Generic only | Avoid |
| t3.micro | Nitro | Yes | ✅ | Too little RAM for builds |
| **t3.small** | **Nitro** | **Yes** | **✅** | **Recommended minimum** |
| c7i-flex.large | Nitro v5 | Yes | ✅ + ntuple | Best performance |

> **Use t3.small** — 2 vCPU, 2 GB RAM. Compiling Rust + Clang/LLVM simultaneously will OOM a t3.micro.

### Step-by-Step AWS Setup

```bash
# Step 1: Launch t3.small with Ubuntu 24.04 LTS
# Console: EC2 → Launch Instance
# AMI: Ubuntu 24.04 LTS (ami-0c7217cdde317cfec in us-east-1)
# Instance type: t3.small
# Storage: 20 GB gp3
# Security Group: allow port 22 from your IP

# Step 2: Add a second ENI (CRITICAL — this is your lab interface)
# If your XDP program crashes eth1, you keep SSH on eth0
# Console: EC2 → Your Instance → Networking → Attach Network Interface
# Create a new ENI in the same subnet and security group
# After attaching, it appears as eth1 inside the instance

# Step 3: Configure eth1 for XDP (run inside the instance)
# Reduce MTU — ENA's XDP requires packets fit in one RX buffer
# Default EC2 MTU is 9001 (jumbo frames) which breaks native XDP
sudo ip link set eth1 mtu 3498

# Reduce combined channels — XDP native needs headroom in the driver
ethtool -l eth1                      # Check current/maximum channels
sudo ethtool -L eth1 combined 2      # Set to half the max combined

# Bring eth1 up
sudo ip link set eth1 up

# Step 4: Verify ENA driver version (must be >= 2.2.0)
modinfo ena | grep version
```

---

## 3. Toolchain Installation

```bash
sudo apt update && sudo apt install -y \
  clang llvm \
  libbpf-dev \
  linux-headers-$(uname -r) \
  bpftool \
  iproute2 \
  pkg-config \
  libelf-dev \
  linux-tools-$(uname -r) \
  linux-tools-generic \
  build-essential \
  curl wget git

# ── Rust ──────────────────────────────────────────────────────────────────
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env

# Install nightly (Aya's BPF target requires it)
rustup toolchain install nightly
rustup component add rust-src --toolchain nightly

# Install bpf-linker (Aya's LLVM-based BPF backend)
cargo install bpf-linker

# Install cargo-generate for Aya project templating
cargo install cargo-generate

# ── Go ────────────────────────────────────────────────────────────────────
wget https://go.dev/dl/go1.22.4.linux-amd64.tar.gz
sudo tar -C /usr/local -xzf go1.22.4.linux-amd64.tar.gz
echo 'export PATH=$PATH:/usr/local/go/bin' >> ~/.bashrc
source ~/.bashrc

go version   # should print go1.22.4

# ── Verify everything ─────────────────────────────────────────────────────
clang --version
llc --version
rustc --version
cargo --version
go version
bpftool version
```

---

## 4. Project Structure

```
ebpfirewall/
├── kernel/                     # C — XDP eBPF kernel program
│   ├── firewall.c              # Main XDP filter logic
│   ├── firewall.h              # Shared structs and map definitions
│   └── Makefile                # clang compilation to BPF object
│
├── control-plane/              # Rust — Aya-based BPF loader & map manager
│   ├── Cargo.toml
│   ├── build.rs                # Aya build script
│   └── src/
│       ├── main.rs             # Entry point: load XDP, attach to interface
│       ├── maps.rs             # BPF map CRUD operations
│       └── loader.rs           # Program loading and lifecycle
│
├── management/                 # Go — REST API + CLI firewall manager
│   ├── go.mod
│   ├── main.go                 # HTTP server entry point
│   ├── api/
│   │   ├── routes.go           # Route definitions
│   │   └── handlers.go         # HTTP handler functions
│   ├── bpfmaps/
│   │   └── client.go           # Talks to Rust control plane via Unix socket
│   └── cli/
│       └── firewall.go         # cobra-based CLI
│
└── README.md
```

---

## 5. C — XDP Kernel Program

The C code runs **inside the kernel**. It is compiled to eBPF bytecode by Clang and verified + JIT-compiled by the kernel before execution. Every packet on `eth1` passes through this code.

### `kernel/firewall.h` — Shared Definitions

```c
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

#ifndef FIREWALL_H
#define FIREWALL_H

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

#endif /* FIREWALL_H */
```

**Why this header matters:** BPF maps are raw binary blobs. Both the kernel C program and user-space Rust/Go code must agree byte-for-byte on struct layout. Any padding mismatch causes silent data corruption where you read garbage from map entries. The `__u32 pad` in `port_rule` is explicit — we document every padding byte.

---

### `kernel/firewall.c` — XDP Filter Logic

```c
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

#include "firewall.h"

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
```

### `kernel/Makefile`

```makefile
# Makefile — compiles C to eBPF object file using Clang's BPF target

# Generate vmlinux.h from the running kernel's BTF data.
# BTF (BPF Type Format) is debug info embedded in the kernel that lets
# BPF programs access kernel structs portably across kernel versions.
vmlinux.h:
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

# Compile the XDP program to BPF bytecode.
#
# Flags explained:
#   -O2              : Optimization required — the BPF verifier is stricter
#                      on unoptimized code (dead stores, redundant checks)
#   -g               : Include BTF debug info in the output object
#   -target bpf      : Emit BPF bytecode instead of x86 machine code
#   -D__TARGET_ARCH_x86 : Tells vmlinux.h which arch we're targeting
#   -I/usr/include/x86_64-linux-gnu : system headers for __u32 etc.
#   -Wall -Wno-unused-value : warn about issues but suppress noisy ones
firewall.o: vmlinux.h firewall.c firewall.h
	clang -O2 -g \
		-target bpf \
		-D__TARGET_ARCH_x86 \
		-I/usr/include/x86_64-linux-gnu \
		-I. \
		-Wall -Wno-unused-value \
		-c firewall.c \
		-o firewall.o

clean:
	rm -f firewall.o vmlinux.h

.PHONY: clean
```

---

## 6. Rust — Aya Control Plane

Aya is a Rust library for eBPF that handles loading programs, managing maps, and attaching hooks — all without depending on `libbpf` (it speaks to the kernel directly via `bpf()` syscalls). This is the layer that bridges kernel and management.

### `control-plane/Cargo.toml`

```toml
[package]
name    = "ebpf-control-plane"
version = "0.1.0"
edition = "2021"

[[bin]]
name = "ebpf-control"
path = "src/main.rs"

[dependencies]
# aya: main eBPF library — loads programs, manages maps, attaches XDP hooks
aya = { version = "0.12", features = ["async_tokio"] }

# aya-log: structured logging from BPF programs (reads bpf_trace_printk ring buffer)
aya-log = "0.2"

# tokio: async runtime — we use it for the Unix socket listener
tokio = { version = "1", features = ["full"] }

# serde/serde_json: serialize map entries to JSON for the Go management layer
serde       = { version = "1", features = ["derive"] }
serde_json  = "1"

# anyhow: ergonomic error handling without boilerplate
anyhow = "1"

# log + env_logger: standard logging facade
log        = "0.4"
env_logger = "0.11"

# network-types: provides Rust types for Ethernet/IP/TCP headers
# matches the C structs we parse in the kernel program
network-types = "0.0.6"
```

### `control-plane/src/maps.rs`

```rust
// maps.rs
//
// BPF map access layer.
// This module wraps the raw Aya map types into ergonomic Rust functions
// that the rest of the control plane uses to read/write firewall rules.
//
// Key design decisions:
//  - All IP addresses are stored in HOST byte order (u32) here in Rust.
//    We convert to network byte order only when writing to the BPF map,
//    matching what the C XDP program reads off the wire.
//  - All map operations return anyhow::Result for clean error propagation.

use anyhow::{Context, Result};
use aya::maps::{HashMap, LpmTrie, PerCpuHashMap, Array};
use aya::maps::lpm_trie::Key;
use aya::Ebpf;
use serde::{Deserialize, Serialize};
use std::net::Ipv4Addr;

// ─────────────────────────────────────────────────────────────────────────
// Mirror of the C structs from firewall.h.
// Must match byte-for-byte. `#[repr(C)]` disables Rust's reordering.
// `Pod` (Plain Old Data) trait from aya means the type can be safely
// read/written as raw bytes in BPF maps.
// ─────────────────────────────────────────────────────────────────────────

/// Mirrors `struct port_rule` in firewall.h
#[repr(C)]
#[derive(Clone, Copy, Debug, Serialize, Deserialize)]
pub struct PortRule {
    pub action:   u8,   // 0 = drop, 1 = allow
    pub protocol: u8,   // 0 = any, 6 = TCP, 17 = UDP
    pub pad:      u16,  // explicit padding — must match C layout
}

/// Mirrors `struct ip_stats` in firewall.h
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize)]
pub struct IpStats {
    pub packets_dropped: u64,
    pub packets_passed:  u64,
    pub bytes_dropped:   u64,
    pub bytes_passed:    u64,
}

// Safety: PortRule contains only integers and explicit padding.
// It has no pointers, no references, no interior mutability.
// Implementing Pod lets Aya read/write it directly as map bytes.
unsafe impl aya::Pod for PortRule {}
unsafe impl aya::Pod for IpStats {}

// ─────────────────────────────────────────────────────────────────────────
// BLOCKLIST OPERATIONS
// ─────────────────────────────────────────────────────────────────────────

/// Block a CIDR range (e.g., "192.168.1.0/24").
///
/// The LpmTrie key format: [prefix_len (4 bytes LE), addr (4 bytes BE)]
/// We use Aya's `Key` wrapper which handles this layout correctly.
///
/// # Arguments
/// * `bpf`    - The loaded eBPF program handle
/// * `ip`     - Base address of the CIDR (e.g., 192.168.1.0)
/// * `prefix` - Prefix length 0–32 (e.g., 24 for /24)
pub fn block_cidr(bpf: &mut Ebpf, ip: Ipv4Addr, prefix: u32) -> Result<()> {
    // Get a mutable reference to the LPM trie map.
    // "blocklist_ipv4" must match the map name in the C SEC(".maps") definition.
    let mut map: LpmTrie<_, [u8; 4], u8> = LpmTrie::try_from(
        bpf.map_mut("blocklist_ipv4")
            .context("map 'blocklist_ipv4' not found — did the BPF program load correctly?")?
    )?;

    // Convert IP to network byte order bytes (big-endian).
    // Ipv4Addr::octets() returns bytes in network order already.
    let addr_bytes = ip.octets();

    // Aya's Key<T> wraps the (prefix_len, data) pair.
    // Internally it lays them out as the kernel's struct bpf_lpm_trie_key expects.
    let key = Key::new(prefix, addr_bytes);

    // Value is just a sentinel byte — we only care if the key EXISTS in the trie.
    map.insert(&key, 1u8, 0)
        .context(format!("failed to insert {}/{} into blocklist", ip, prefix))?;

    log::info!("Blocked CIDR: {}/{}", ip, prefix);
    Ok(())
}

/// Remove a previously blocked CIDR range.
pub fn unblock_cidr(bpf: &mut Ebpf, ip: Ipv4Addr, prefix: u32) -> Result<()> {
    let mut map: LpmTrie<_, [u8; 4], u8> = LpmTrie::try_from(
        bpf.map_mut("blocklist_ipv4")
            .context("map 'blocklist_ipv4' not found")?
    )?;

    let key = Key::new(prefix, ip.octets());
    map.remove(&key)
        .context(format!("failed to remove {}/{} from blocklist", ip, prefix))?;

    log::info!("Unblocked CIDR: {}/{}", ip, prefix);
    Ok(())
}

// ─────────────────────────────────────────────────────────────────────────
// PORT RULE OPERATIONS
// ─────────────────────────────────────────────────────────────────────────

/// Add or update a port rule.
///
/// # Arguments
/// * `port`     - Port number in host byte order (e.g., 22, 80, 443)
/// * `protocol` - 0 = any, 6 = TCP, 17 = UDP
/// * `allow`    - true = allow, false = explicitly deny
///
/// Note: The XDP program stores port keys in NETWORK byte order.
/// We call u16::to_be() here to convert 22 → 0x1600 before inserting.
pub fn set_port_rule(
    bpf: &mut Ebpf,
    port: u16,
    protocol: u8,
    allow: bool,
) -> Result<()> {
    let mut map: HashMap<_, u16, PortRule> = HashMap::try_from(
        bpf.map_mut("allow_ports")
            .context("map 'allow_ports' not found")?
    )?;

    let rule = PortRule {
        action:   if allow { 1 } else { 0 },
        protocol,
        pad: 0,
    };

    // Store in network byte order — matches XDP program's bpf_htons(dest_port)
    map.insert(port.to_be(), rule, 0)
        .context(format!("failed to insert port rule for port {}", port))?;

    log::info!(
        "Port rule: {} {} {}",
        port,
        if protocol == 6 { "TCP" } else if protocol == 17 { "UDP" } else { "ANY" },
        if allow { "ALLOW" } else { "DENY" }
    );
    Ok(())
}

/// Remove a port rule (the port will fall back to default-deny).
pub fn remove_port_rule(bpf: &mut Ebpf, port: u16) -> Result<()> {
    let mut map: HashMap<_, u16, PortRule> = HashMap::try_from(
        bpf.map_mut("allow_ports")
            .context("map 'allow_ports' not found")?
    )?;

    map.remove(&port.to_be())
        .context(format!("failed to remove port rule for port {}", port))?;

    log::info!("Removed port rule for port {}", port);
    Ok(())
}

// ─────────────────────────────────────────────────────────────────────────
// STATISTICS READING
// ─────────────────────────────────────────────────────────────────────────

/// Read per-IP statistics from the PERCPU hash map.
///
/// PERCPU maps store one value per CPU to avoid lock contention.
/// Aya returns a Vec<IpStats> where index = CPU id.
/// We sum across all CPUs to get global totals.
///
/// Returns a Vec of (ip_string, aggregated_stats) sorted by packets_dropped desc.
pub fn read_stats(bpf: &mut Ebpf) -> Result<Vec<(String, IpStats)>> {
    let map: PerCpuHashMap<_, u32, IpStats> = PerCpuHashMap::try_from(
        bpf.map("stats_map")
            .context("map 'stats_map' not found")?
    )?;

    let mut results = Vec::new();

    // Iterate over all keys in the map.
    // For PERCPU maps, iter() returns (key, PerCpuValues<V>).
    // PerCpuValues<V> dereferences to a slice indexed by CPU id.
    for item in map.iter() {
        let (raw_ip, per_cpu_stats) = item?;

        // Sum across all CPUs
        let mut total = IpStats::default();
        for cpu_stats in per_cpu_stats.iter() {
            total.packets_dropped += cpu_stats.packets_dropped;
            total.packets_passed  += cpu_stats.packets_passed;
            total.bytes_dropped   += cpu_stats.bytes_dropped;
            total.bytes_passed    += cpu_stats.bytes_passed;
        }

        // raw_ip is stored in network byte order — convert to display string
        let ip = Ipv4Addr::from(u32::from_be(raw_ip));
        results.push((ip.to_string(), total));
    }

    // Sort by most dropped first — useful for identifying top attackers
    results.sort_by(|a, b| b.1.packets_dropped.cmp(&a.1.packets_dropped));
    Ok(results)
}

// ─────────────────────────────────────────────────────────────────────────
// CONFIGURATION
// ─────────────────────────────────────────────────────────────────────────

/// Set a configuration value in the config_map array.
/// key 0 = global_enable, key 1 = default_policy, key 2 = log_level
pub fn set_config(bpf: &mut Ebpf, key: u32, value: u32) -> Result<()> {
    let mut map: Array<_, u32> = Array::try_from(
        bpf.map_mut("config_map")
            .context("map 'config_map' not found")?
    )?;

    map.set(key, value, 0)
        .context(format!("failed to set config key {} = {}", key, value))?;

    Ok(())
}
```

### `control-plane/src/main.rs`

```rust
// main.rs
//
// Entry point for the Rust control plane.
//
// Responsibilities:
//   1. Load the compiled XDP BPF object (firewall.o) into the kernel
//   2. Attach it to the network interface in NATIVE XDP mode
//   3. Initialize default firewall rules (allow SSH port 22)
//   4. Enable the firewall via config_map
//   5. Listen on a Unix domain socket for commands from the Go management layer
//   6. Gracefully detach XDP on Ctrl-C

use anyhow::{Context, Result};
use aya::{
    programs::{Xdp, XdpFlags},
    Ebpf,
};
use aya_log::EbpfLogger;
use log::{info, warn};
use std::net::Ipv4Addr;
use std::sync::{Arc, Mutex};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::UnixListener;
use tokio::signal;

mod maps;
use maps::{block_cidr, read_stats, remove_port_rule, set_config, set_port_rule, unblock_cidr};

// Control commands sent over the Unix socket from Go management layer.
// Simple line-based protocol: one JSON command per line.
// Using an enum makes exhaustive matching mandatory — no forgotten cases.
#[derive(serde::Deserialize, Debug)]
#[serde(tag = "cmd", rename_all = "snake_case")]
enum Command {
    BlockCidr   { ip: String, prefix: u32 },
    UnblockCidr { ip: String, prefix: u32 },
    AllowPort   { port: u16, protocol: u8 },
    DenyPort    { port: u16 },
    RemovePort  { port: u16 },
    GetStats,
    Enable,
    Disable,
}

#[tokio::main]
async fn main() -> Result<()> {
    // Initialize env_logger. Set RUST_LOG=info or RUST_LOG=debug.
    env_logger::init();

    // ── Command-line args ─────────────────────────────────────────────
    let iface = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "eth1".to_string());
    let bpf_obj = std::env::args()
        .nth(2)
        .unwrap_or_else(|| "../kernel/firewall.o".to_string());

    info!("Loading XDP program from {} onto interface {}", bpf_obj, iface);

    // ── Load the compiled BPF object file ─────────────────────────────
    // Aya reads the ELF file, finds all SEC(".maps") and SEC("xdp")
    // sections, and prepares them for kernel loading.
    let mut bpf = Ebpf::load_file(&bpf_obj)
        .context("failed to load BPF object — did you compile firewall.c?")?;

    // Set up aya-log to forward bpf_printk() output from the kernel
    // program to our env_logger. Useful for debugging.
    if let Err(e) = EbpfLogger::init(&mut bpf) {
        warn!("Failed to init eBPF logger (non-fatal): {}", e);
    }

    // ── Get the XDP program and attach it ─────────────────────────────
    // "firewall_main" must match the C function name exactly.
    let program: &mut Xdp = bpf
        .program_mut("firewall_main")
        .context("XDP program 'firewall_main' not found in BPF object")?
        .try_into()?;

    // Load the program into the kernel. This runs the verifier.
    // If the verifier rejects the program, this returns an error
    // with the verifier log explaining exactly what went wrong.
    program.load().context("kernel verifier rejected the XDP program")?;

    // Attach in NATIVE mode (XdpFlags::default() = SKB_MODE fallback,
    // XdpFlags::DRV_MODE = native only, error if not supported).
    // We try native first, fall back to SKB mode if the driver doesn't support it.
    let attach_result = program.attach(&iface, XdpFlags::DRV_MODE);
    let _link_id = match attach_result {
        Ok(id) => {
            info!("XDP attached in NATIVE mode on {}", iface);
            id
        }
        Err(e) => {
            warn!("Native XDP failed ({}), falling back to SKB mode", e);
            program
                .attach(&iface, XdpFlags::SKB_MODE)
                .context("XDP attachment failed in both native and SKB modes")?
        }
    };

    // ── Initialize default rules ──────────────────────────────────────
    // Allow SSH (port 22, TCP) — CRITICAL: do this before enabling the
    // firewall or you'll lock yourself out.
    set_port_rule(&mut bpf, 22, 6 /* TCP */, true)
        .context("failed to allow SSH port 22")?;

    // Allow established connections on common ports
    set_port_rule(&mut bpf, 80,  6, true)?;   // HTTP
    set_port_rule(&mut bpf, 443, 6, true)?;   // HTTPS
    set_port_rule(&mut bpf, 53,  17, true)?;  // DNS over UDP

    // Allow ICMP (ping) — uses port 0 as sentinel in our map
    set_port_rule(&mut bpf, 0, 1 /* ICMP */, true)?;

    // Enable the firewall (config key 0 = global_enable)
    set_config(&mut bpf, 0, 1)?;

    info!("Firewall active. Default-deny with SSH/HTTP/HTTPS/DNS allowed.");

    // ── Wrap bpf in Arc<Mutex> for sharing across async tasks ─────────
    // The Unix socket listener runs in a separate async task and needs
    // mutable access to bpf to update maps.
    let bpf = Arc::new(Mutex::new(bpf));

    // ── Unix socket listener ──────────────────────────────────────────
    // The Go management layer sends JSON commands over this socket.
    // We use a Unix socket (not TCP) for performance and to avoid
    // the socket being accessible from the network.
    let socket_path = "/var/run/ebpf-firewall.sock";
    let _ = std::fs::remove_file(socket_path); // Clean up leftover socket
    let listener = UnixListener::bind(socket_path)
        .context("failed to bind Unix socket")?;

    let bpf_clone = Arc::clone(&bpf);
    tokio::spawn(async move {
        loop {
            match listener.accept().await {
                Ok((stream, _)) => {
                    let bpf_ref = Arc::clone(&bpf_clone);
                    // Handle each connection in its own task
                    tokio::spawn(handle_connection(stream, bpf_ref));
                }
                Err(e) => {
                    warn!("Unix socket accept error: {}", e);
                }
            }
        }
    });

    info!("Control socket listening at {}", socket_path);
    info!("Send Ctrl-C to detach XDP and exit.");

    // ── Wait for Ctrl-C ───────────────────────────────────────────────
    // When this task exits, `_link_id` is dropped, which automatically
    // detaches the XDP program from the interface. This is Aya's RAII
    // (Resource Acquisition Is Initialization) cleanup model.
    signal::ctrl_c().await?;
    info!("Shutting down — XDP program will be detached.");

    Ok(())
}

/// Handle a single management connection.
/// Reads JSON commands line by line, executes them, sends JSON responses.
async fn handle_connection(
    stream: tokio::net::UnixStream,
    bpf: Arc<Mutex<Ebpf>>,
) {
    let (reader, mut writer) = stream.into_split();
    let mut lines = BufReader::new(reader).lines();

    while let Ok(Some(line)) = lines.next_line().await {
        // Parse the incoming JSON command
        let response = match serde_json::from_str::<Command>(&line) {
            Err(e) => {
                format!("{{\"error\": \"parse error: {}\"}}\n", e)
            }
            Ok(cmd) => {
                // Acquire lock and process command
                let mut bpf_guard = bpf.lock().unwrap();
                execute_command(&mut bpf_guard, cmd)
            }
        };

        if writer.write_all(response.as_bytes()).await.is_err() {
            break;  // Client disconnected
        }
    }
}

/// Execute a parsed command against the BPF maps.
/// Returns a JSON string response.
fn execute_command(bpf: &mut Ebpf, cmd: Command) -> String {
    let result: Result<String> = match cmd {
        Command::BlockCidr { ip, prefix } => {
            let addr: Ipv4Addr = ip.parse()?;
            block_cidr(bpf, addr, prefix)?;
            Ok(format!("{{\"ok\": \"blocked {}/{}\"}}\n", ip, prefix))
        }
        Command::UnblockCidr { ip, prefix } => {
            let addr: Ipv4Addr = ip.parse()?;
            unblock_cidr(bpf, addr, prefix)?;
            Ok(format!("{{\"ok\": \"unblocked {}/{}\"}}\n", ip, prefix))
        }
        Command::AllowPort { port, protocol } => {
            set_port_rule(bpf, port, protocol, true)?;
            Ok(format!("{{\"ok\": \"allowed port {}\"}}\n", port))
        }
        Command::DenyPort { port } => {
            set_port_rule(bpf, port, 0, false)?;
            Ok(format!("{{\"ok\": \"denied port {}\"}}\n", port))
        }
        Command::RemovePort { port } => {
            remove_port_rule(bpf, port)?;
            Ok(format!("{{\"ok\": \"removed rule for port {}\"}}\n", port))
        }
        Command::GetStats => {
            let stats = read_stats(bpf)?;
            Ok(serde_json::to_string(&stats)? + "\n")
        }
        Command::Enable => {
            set_config(bpf, 0, 1)?;
            Ok("{\"ok\": \"firewall enabled\"}\n".to_string())
        }
        Command::Disable => {
            set_config(bpf, 0, 0)?;
            Ok("{\"ok\": \"firewall disabled\"}\n".to_string())
        }
    };

    result.unwrap_or_else(|e| format!("{{\"error\": \"{}\"}}\n", e))
}
```

---

## 7. Go — Management API & CLI

The Go layer provides a human-friendly REST API and a `firewall` CLI command. It talks to the Rust control plane via the Unix socket — Go never touches BPF maps directly, which keeps the BPF state management in one place (Rust).

### `management/bpfmaps/client.go`

```go
// bpfmaps/client.go
//
// Unix socket client that sends JSON commands to the Rust control plane.
//
// We use a simple line-delimited JSON protocol:
//   - Client sends one JSON line per command
//   - Server replies with one JSON line per response
//
// This is not HTTP — using a raw Unix socket avoids HTTP overhead for
// what are ultimately very fast BPF map operations.

package bpfmaps

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net"
	"strings"
	"time"
)

const socketPath = "/var/run/ebpf-firewall.sock"

// IpStats mirrors the Rust IpStats struct.
// json tags must match the serde field names in Rust.
type IpStats struct {
	PacketsDropped uint64 `json:"packets_dropped"`
	PacketsPassed  uint64 `json:"packets_passed"`
	BytesDropped   uint64 `json:"bytes_dropped"`
	BytesPassed    uint64 `json:"bytes_passed"`
}

// StatEntry is one element from the GetStats response.
// The Rust side returns []("ip_string", IpStats) tuples as JSON arrays.
type StatEntry struct {
	IP    string  `json:"ip"`
	Stats IpStats `json:"stats"`
}

// Client holds the Unix socket connection to the Rust control plane.
// We keep a persistent connection rather than reconnecting per command
// to avoid socket setup overhead in the hot path.
type Client struct {
	conn   net.Conn
	reader *bufio.Reader
}

// NewClient dials the Rust control plane's Unix socket.
// Retries up to 3 times with 500ms delay — the control plane may be
// starting up when the management layer first connects.
func NewClient() (*Client, error) {
	var conn net.Conn
	var err error

	for i := 0; i < 3; i++ {
		// net.Dial("unix", path) opens a Unix domain socket connection.
		// This is local IPC — no network stack involved.
		conn, err = net.DialTimeout("unix", socketPath, 2*time.Second)
		if err == nil {
			break
		}
		time.Sleep(500 * time.Millisecond)
	}
	if err != nil {
		return nil, fmt.Errorf("failed to connect to control plane at %s: %w", socketPath, err)
	}

	return &Client{
		conn:   conn,
		reader: bufio.NewReader(conn),
	}, nil
}

// Close cleans up the connection.
func (c *Client) Close() error {
	return c.conn.Close()
}

// send sends a command as a JSON line and reads back one JSON line response.
// The command parameter is any struct with json tags; it is marshaled inline.
func (c *Client) send(cmd map[string]interface{}) (string, error) {
	// Marshal the command to a single JSON line
	data, err := json.Marshal(cmd)
	if err != nil {
		return "", fmt.Errorf("marshal error: %w", err)
	}

	// Write command + newline (the Rust server reads line by line)
	c.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	if _, err := fmt.Fprintf(c.conn, "%s\n", data); err != nil {
		return "", fmt.Errorf("write error: %w", err)
	}

	// Read exactly one line of response
	c.conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	line, err := c.reader.ReadString('\n')
	if err != nil {
		return "", fmt.Errorf("read error: %w", err)
	}

	return strings.TrimSpace(line), nil
}

// BlockCIDR blocks a CIDR range. ip should be "192.168.1.0", prefix = 24.
func (c *Client) BlockCIDR(ip string, prefix uint32) error {
	resp, err := c.send(map[string]interface{}{
		"cmd":    "block_cidr",
		"ip":     ip,
		"prefix": prefix,
	})
	if err != nil {
		return err
	}
	return checkResponse(resp)
}

// UnblockCIDR removes a blocked CIDR range.
func (c *Client) UnblockCIDR(ip string, prefix uint32) error {
	resp, err := c.send(map[string]interface{}{
		"cmd":    "unblock_cidr",
		"ip":     ip,
		"prefix": prefix,
	})
	if err != nil {
		return err
	}
	return checkResponse(resp)
}

// AllowPort adds a port to the allowlist.
// protocol: 0 = any, 6 = TCP, 17 = UDP
func (c *Client) AllowPort(port uint16, protocol uint8) error {
	resp, err := c.send(map[string]interface{}{
		"cmd":      "allow_port",
		"port":     port,
		"protocol": protocol,
	})
	if err != nil {
		return err
	}
	return checkResponse(resp)
}

// DenyPort explicitly denies a port (overrides allowlist entry).
func (c *Client) DenyPort(port uint16) error {
	resp, err := c.send(map[string]interface{}{
		"cmd":  "deny_port",
		"port": port,
	})
	if err != nil {
		return err
	}
	return checkResponse(resp)
}

// GetStats returns per-IP packet statistics from the kernel.
func (c *Client) GetStats() ([]StatEntry, error) {
	// The Rust side serializes []("ip", stats) as a JSON array of 2-element arrays.
	resp, err := c.send(map[string]interface{}{"cmd": "get_stats"})
	if err != nil {
		return nil, err
	}

	// Parse the response: [ ["192.168.1.1", {stats}], ... ]
	var raw [][]json.RawMessage
	if err := json.Unmarshal([]byte(resp), &raw); err != nil {
		return nil, fmt.Errorf("stats parse error: %w\nraw: %s", err, resp)
	}

	var entries []StatEntry
	for _, pair := range raw {
		if len(pair) != 2 {
			continue
		}
		var ip string
		var stats IpStats
		if err := json.Unmarshal(pair[0], &ip); err != nil {
			continue
		}
		if err := json.Unmarshal(pair[1], &stats); err != nil {
			continue
		}
		entries = append(entries, StatEntry{IP: ip, Stats: stats})
	}

	return entries, nil
}

// Enable activates the firewall.
func (c *Client) Enable() error {
	resp, err := c.send(map[string]interface{}{"cmd": "enable"})
	if err != nil {
		return err
	}
	return checkResponse(resp)
}

// Disable deactivates the firewall (pass all traffic).
func (c *Client) Disable() error {
	resp, err := c.send(map[string]interface{}{"cmd": "disable"})
	if err != nil {
		return err
	}
	return checkResponse(resp)
}

// checkResponse inspects a JSON response line for error fields.
func checkResponse(resp string) error {
	var result map[string]interface{}
	if err := json.Unmarshal([]byte(resp), &result); err != nil {
		return fmt.Errorf("invalid response: %s", resp)
	}
	if errMsg, ok := result["error"]; ok {
		return fmt.Errorf("control plane error: %v", errMsg)
	}
	return nil
}
```

### `management/api/handlers.go`

```go
// api/handlers.go
//
// HTTP REST API handlers.
//
// Routes:
//   POST /api/block          { "ip": "1.2.3.0", "prefix": 24 }
//   DELETE /api/block        { "ip": "1.2.3.0", "prefix": 24 }
//   POST /api/port/allow     { "port": 443, "protocol": "tcp" }
//   POST /api/port/deny      { "port": 8080 }
//   GET  /api/stats          → JSON array of IP stats
//   POST /api/enable
//   POST /api/disable
//   GET  /api/health         → 200 OK

package api

import (
	"encoding/json"
	"fmt"
	"net/http"

	"ebpf-management/bpfmaps"
)

// Handler holds dependencies for the HTTP handlers.
// Using a struct keeps handlers testable — we can inject a mock client.
type Handler struct {
	bpf *bpfmaps.Client
}

// NewHandler creates a Handler with a connected BPF client.
func NewHandler(bpf *bpfmaps.Client) *Handler {
	return &Handler{bpf: bpf}
}

// respond is a helper that writes a JSON response with the given status code.
// It always sets Content-Type: application/json and handles marshal errors.
func respond(w http.ResponseWriter, status int, data interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(data); err != nil {
		http.Error(w, `{"error":"response encoding failed"}`, 500)
	}
}

// decodeBody decodes a JSON request body into dst.
// Returns false and writes an error response if decoding fails.
func decodeBody(w http.ResponseWriter, r *http.Request, dst interface{}) bool {
	if err := json.NewDecoder(r.Body).Decode(dst); err != nil {
		respond(w, http.StatusBadRequest, map[string]string{
			"error": fmt.Sprintf("invalid request body: %v", err),
		})
		return false
	}
	return true
}

// BlockHandler handles POST and DELETE /api/block
func (h *Handler) BlockHandler(w http.ResponseWriter, r *http.Request) {
	var req struct {
		IP     string `json:"ip"`
		Prefix uint32 `json:"prefix"`
	}

	if !decodeBody(w, r, &req) {
		return
	}
	if req.IP == "" || req.Prefix > 32 {
		respond(w, http.StatusBadRequest, map[string]string{
			"error": "ip and prefix (0-32) are required",
		})
		return
	}

	var err error
	if r.Method == http.MethodPost {
		// POST = block the CIDR
		err = h.bpf.BlockCIDR(req.IP, req.Prefix)
	} else {
		// DELETE = unblock the CIDR
		err = h.bpf.UnblockCIDR(req.IP, req.Prefix)
	}

	if err != nil {
		respond(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}

	action := "blocked"
	if r.Method == http.MethodDelete {
		action = "unblocked"
	}
	respond(w, http.StatusOK, map[string]string{
		"ok": fmt.Sprintf("%s %s/%d", action, req.IP, req.Prefix),
	})
}

// AllowPortHandler handles POST /api/port/allow
// Expects: { "port": 443, "protocol": "tcp" }
// protocol can be "tcp", "udp", or "any" (default)
func (h *Handler) AllowPortHandler(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Port     uint16 `json:"port"`
		Protocol string `json:"protocol"`
	}

	if !decodeBody(w, r, &req) {
		return
	}
	if req.Port == 0 {
		respond(w, http.StatusBadRequest, map[string]string{"error": "port is required"})
		return
	}

	// Map protocol string to the numeric value expected by the kernel
	var protoNum uint8
	switch req.Protocol {
	case "tcp":
		protoNum = 6
	case "udp":
		protoNum = 17
	case "icmp":
		protoNum = 1
	default:
		protoNum = 0  // "any" — matches all protocols
	}

	if err := h.bpf.AllowPort(req.Port, protoNum); err != nil {
		respond(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}

	respond(w, http.StatusOK, map[string]string{
		"ok": fmt.Sprintf("allowed port %d/%s", req.Port, req.Protocol),
	})
}

// DenyPortHandler handles POST /api/port/deny
func (h *Handler) DenyPortHandler(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Port uint16 `json:"port"`
	}

	if !decodeBody(w, r, &req) {
		return
	}

	if err := h.bpf.DenyPort(req.Port); err != nil {
		respond(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}

	respond(w, http.StatusOK, map[string]string{
		"ok": fmt.Sprintf("denied port %d", req.Port),
	})
}

// StatsHandler handles GET /api/stats
func (h *Handler) StatsHandler(w http.ResponseWriter, r *http.Request) {
	stats, err := h.bpf.GetStats()
	if err != nil {
		respond(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}

	// If no stats yet, return empty array (not null)
	if stats == nil {
		stats = []bpfmaps.StatEntry{}
	}

	respond(w, http.StatusOK, stats)
}

// EnableHandler handles POST /api/enable
func (h *Handler) EnableHandler(w http.ResponseWriter, r *http.Request) {
	if err := h.bpf.Enable(); err != nil {
		respond(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}
	respond(w, http.StatusOK, map[string]string{"ok": "firewall enabled"})
}

// DisableHandler handles POST /api/disable
func (h *Handler) DisableHandler(w http.ResponseWriter, r *http.Request) {
	if err := h.bpf.Disable(); err != nil {
		respond(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}
	respond(w, http.StatusOK, map[string]string{"ok": "firewall disabled — all traffic passes"})
}

// HealthHandler handles GET /api/health
func (h *Handler) HealthHandler(w http.ResponseWriter, r *http.Request) {
	respond(w, http.StatusOK, map[string]string{"status": "ok"})
}
```

### `management/main.go`

```go
// main.go
//
// Management layer entry point.
// Starts the REST API server and registers all routes.
//
// Usage:
//   go run . [--addr :8080]
//
// The server listens on :8080 by default.
// All API calls are forwarded to the Rust control plane via Unix socket.

package main

import (
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"

	"ebpf-management/api"
	"ebpf-management/bpfmaps"
)

func main() {
	addr := flag.String("addr", ":8080", "HTTP listen address")
	flag.Parse()

	// If the first argument is a subcommand, run the CLI instead of the server.
	// This lets the same binary serve both `firewall block 1.2.3.4/24`
	// and `firewall serve` (the HTTP API mode).
	if len(os.Args) > 1 && os.Args[1] != "serve" {
		runCLI()
		return
	}

	// Connect to the Rust control plane
	bpf, err := bpfmaps.NewClient()
	if err != nil {
		log.Fatalf("Cannot connect to control plane: %v\n"+
			"Is ebpf-control running? Check: ls -la /var/run/ebpf-firewall.sock", err)
	}
	defer bpf.Close()

	// Build HTTP handler with all routes
	handler := api.NewHandler(bpf)
	mux := http.NewServeMux()

	// Middleware: log all requests
	logged := func(h http.HandlerFunc) http.HandlerFunc {
		return func(w http.ResponseWriter, r *http.Request) {
			log.Printf("%s %s", r.Method, r.URL.Path)
			h(w, r)
		}
	}

	// Register routes
	mux.HandleFunc("/api/block",      logged(handler.BlockHandler))
	mux.HandleFunc("/api/port/allow", logged(handler.AllowPortHandler))
	mux.HandleFunc("/api/port/deny",  logged(handler.DenyPortHandler))
	mux.HandleFunc("/api/stats",      logged(handler.StatsHandler))
	mux.HandleFunc("/api/enable",     logged(handler.EnableHandler))
	mux.HandleFunc("/api/disable",    logged(handler.DisableHandler))
	mux.HandleFunc("/api/health",     logged(handler.HealthHandler))

	fmt.Printf("eBPF Firewall Management API listening on %s\n", *addr)
	fmt.Println("Endpoints:")
	fmt.Println("  POST   /api/block          {\"ip\":\"x.x.x.x\",\"prefix\":N}")
	fmt.Println("  DELETE /api/block          {\"ip\":\"x.x.x.x\",\"prefix\":N}")
	fmt.Println("  POST   /api/port/allow     {\"port\":N,\"protocol\":\"tcp|udp|any\"}")
	fmt.Println("  POST   /api/port/deny      {\"port\":N}")
	fmt.Println("  GET    /api/stats")
	fmt.Println("  POST   /api/enable|disable")

	log.Fatal(http.ListenAndServe(*addr, mux))
}
```

### `management/cli/firewall.go`

```go
// cli/firewall.go
//
// CLI frontend — wraps the bpfmaps client with a human-friendly interface.
//
// Commands:
//   firewall block   <ip/prefix>         Block a CIDR
//   firewall unblock <ip/prefix>         Unblock a CIDR
//   firewall allow   <port> [proto]      Allow a port
//   firewall deny    <port>              Deny a port
//   firewall stats                       Show per-IP statistics
//   firewall enable / disable            Toggle firewall

package cli

import (
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"text/tabwriter"

	"ebpf-management/bpfmaps"
)

// RunCLI is called by main() when a subcommand is given.
func RunCLI(args []string) {
	if len(args) == 0 {
		printUsage()
		os.Exit(1)
	}

	// Connect to control plane
	bpf, err := bpfmaps.NewClient()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: cannot connect to control plane: %v\n", err)
		fmt.Fprintf(os.Stderr, "Is 'ebpf-control eth1' running?\n")
		os.Exit(1)
	}
	defer bpf.Close()

	cmd := args[0]
	rest := args[1:]

	switch cmd {
	case "block":
		cmdBlock(bpf, rest, false)
	case "unblock":
		cmdBlock(bpf, rest, true)
	case "allow":
		cmdPort(bpf, rest, true)
	case "deny":
		cmdPort(bpf, rest, false)
	case "stats":
		cmdStats(bpf)
	case "enable":
		if err := bpf.Enable(); err != nil {
			die(err)
		}
		fmt.Println("✓ Firewall enabled")
	case "disable":
		if err := bpf.Disable(); err != nil {
			die(err)
		}
		fmt.Println("⚠  Firewall disabled — all traffic is passing")
	default:
		fmt.Fprintf(os.Stderr, "Unknown command: %s\n", cmd)
		printUsage()
		os.Exit(1)
	}
}

// cmdBlock handles the block/unblock command.
// Accepts CIDR notation: "1.2.3.0/24" or plain IP "1.2.3.4" (treated as /32)
func cmdBlock(bpf *bpfmaps.Client, args []string, unblock bool) {
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "Usage: firewall block <ip[/prefix]>")
		os.Exit(1)
	}

	// Parse CIDR or plain IP
	cidr := args[0]
	if !strings.Contains(cidr, "/") {
		cidr = cidr + "/32"  // treat bare IP as /32 host route
	}

	_, network, err := net.ParseCIDR(cidr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Invalid CIDR: %s\n", cidr)
		os.Exit(1)
	}

	// Get prefix length from the parsed network
	ones, _ := network.Mask.Size()
	ip := network.IP.String()

	action := bpf.BlockCIDR
	word := "Blocked"
	if unblock {
		action = bpf.UnblockCIDR
		word = "Unblocked"
	}

	if err := action(ip, uint32(ones)); err != nil {
		die(err)
	}

	fmt.Printf("✓ %s %s/%d\n", word, ip, ones)
}

// cmdPort handles the allow/deny port command.
func cmdPort(bpf *bpfmaps.Client, args []string, allow bool) {
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "Usage: firewall allow <port> [tcp|udp|any]")
		os.Exit(1)
	}

	portNum, err := strconv.ParseUint(args[0], 10, 16)
	if err != nil || portNum == 0 || portNum > 65535 {
		fmt.Fprintf(os.Stderr, "Invalid port: %s (must be 1-65535)\n", args[0])
		os.Exit(1)
	}

	if !allow {
		if err := bpf.DenyPort(uint16(portNum)); err != nil {
			die(err)
		}
		fmt.Printf("✓ Denied port %d\n", portNum)
		return
	}

	// Parse optional protocol argument
	proto := "any"
	if len(args) > 1 {
		proto = strings.ToLower(args[1])
	}

	var protoNum uint8
	switch proto {
	case "tcp":
		protoNum = 6
	case "udp":
		protoNum = 17
	case "any", "":
		protoNum = 0
	default:
		fmt.Fprintf(os.Stderr, "Unknown protocol: %s (use tcp, udp, or any)\n", proto)
		os.Exit(1)
	}

	if err := bpf.AllowPort(uint16(portNum), protoNum); err != nil {
		die(err)
	}
	fmt.Printf("✓ Allowed port %d/%s\n", portNum, proto)
}

// cmdStats prints the per-IP statistics table.
func cmdStats(bpf *bpfmaps.Client) {
	stats, err := bpf.GetStats()
	if err != nil {
		die(err)
	}

	if len(stats) == 0 {
		fmt.Println("No traffic statistics yet.")
		return
	}

	// tabwriter aligns columns automatically
	w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
	fmt.Fprintln(w, "SOURCE IP\tDROPPED PKTS\tPASSED PKTS\tDROPPED BYTES\tPASSED BYTES")
	fmt.Fprintln(w, "─────────────\t────────────\t───────────\t─────────────\t────────────")

	for _, e := range stats {
		fmt.Fprintf(w, "%s\t%d\t%d\t%s\t%s\n",
			e.IP,
			e.Stats.PacketsDropped,
			e.Stats.PacketsPassed,
			humanBytes(e.Stats.BytesDropped),
			humanBytes(e.Stats.BytesPassed),
		)
	}
	w.Flush()
}

// humanBytes formats a byte count as a human-readable string.
func humanBytes(b uint64) string {
	switch {
	case b >= 1<<30:
		return fmt.Sprintf("%.1fGB", float64(b)/(1<<30))
	case b >= 1<<20:
		return fmt.Sprintf("%.1fMB", float64(b)/(1<<20))
	case b >= 1<<10:
		return fmt.Sprintf("%.1fKB", float64(b)/(1<<10))
	default:
		return fmt.Sprintf("%dB", b)
	}
}

func die(err error) {
	fmt.Fprintf(os.Stderr, "Error: %v\n", err)
	os.Exit(1)
}

func printUsage() {
	fmt.Println(`eBPF Firewall CLI

Usage:
  firewall block   <ip[/prefix]>        Block IP or CIDR (e.g. 1.2.3.0/24)
  firewall unblock <ip[/prefix]>        Remove a block
  firewall allow   <port> [tcp|udp|any] Allow inbound port
  firewall deny    <port>               Explicitly deny a port
  firewall stats                        Show per-IP packet statistics
  firewall enable                       Activate firewall
  firewall disable                      Deactivate firewall (pass-all)
  firewall serve   [--addr :8080]       Start REST API server

Examples:
  firewall block 185.224.128.0/24       # Block a known malicious subnet
  firewall allow 8443 tcp               # Allow HTTPS on alternate port
  firewall stats                        # See who's being dropped
  firewall disable                      # Emergency: pass everything
`)
}
```

---

## 8. Build & Deploy

```bash
# ── Step 1: Compile the XDP kernel program ─────────────────────────
cd kernel/
make vmlinux.h       # Generate BTF header from running kernel
make firewall.o      # Compile C to BPF bytecode

# Verify the output is a valid BPF ELF object
file firewall.o
# Output: firewall.o: ELF 64-bit LSB relocatable, eBPF, ...

# Inspect the compiled program (optional — great for debugging)
bpftool prog show
llvm-objdump -d firewall.o   # Disassemble BPF bytecode

# ── Step 2: Build the Rust control plane ───────────────────────────
cd ../control-plane/
cargo build --release

# The binary is at: target/release/ebpf-control

# ── Step 3: Build the Go management layer ──────────────────────────
cd ../management/
go mod tidy
go build -o firewall .

# ── Step 4: Run everything ─────────────────────────────────────────

# Terminal 1: Start the control plane (needs root for BPF syscalls)
sudo RUST_LOG=info ./target/release/ebpf-control eth1 kernel/firewall.o

# Expected output:
# INFO Loading XDP program from kernel/firewall.o onto interface eth1
# INFO XDP attached in NATIVE mode on eth1
# INFO Firewall active. Default-deny with SSH/HTTP/HTTPS/DNS allowed.
# INFO Control socket listening at /var/run/ebpf-firewall.sock

# Terminal 2: Verify XDP is attached (look for "xdp" NOT "xdpgeneric")
ip link show eth1
# eth1: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 3498 xdp ...

# Terminal 3: Use the CLI
sudo ./firewall block 185.224.128.0/24
sudo ./firewall allow 9090 tcp
sudo ./firewall stats

# Or start the REST API server
sudo ./firewall serve --addr :8080

# Test the REST API
curl -s -X POST http://localhost:8080/api/block \
  -H "Content-Type: application/json" \
  -d '{"ip":"10.0.0.5","prefix":32}' | jq

curl -s http://localhost:8080/api/stats | jq

curl -s -X POST http://localhost:8080/api/port/allow \
  -H "Content-Type: application/json" \
  -d '{"port":9090,"protocol":"tcp"}' | jq
```

---

## 9. Testing & Verification

```bash
# ── Verify native XDP mode (not generic) ──────────────────────────
ip link show eth1 | grep xdp
# Good:  "... xdp ..."           (native — inside the driver)
# Bad:   "... xdpgeneric ..."    (generic — after SKB allocation, no speedup)

# ── Test IP blocking ──────────────────────────────────────────────
# From another machine (or using network namespaces locally):
# Before blocking:
ping -c 3 <eth1-ip>          # Should succeed

# Block the test IP:
sudo ./firewall block <test-machine-ip>/32

# After blocking:
ping -c 3 <eth1-ip>          # Should time out (packets silently dropped)

# ── Verify with bpftool ──────────────────────────────────────────
# List loaded BPF programs
bpftool prog list
# Look for: type xdp  name firewall_main

# Inspect BPF maps
bpftool map list
# You should see: blocklist_ipv4 (lpm_trie), allow_ports (hash), stats_map (percpu_hash)

# Dump the blocklist
bpftool map dump name blocklist_ipv4

# Dump port rules
bpftool map dump name allow_ports

# ── Performance sanity check ─────────────────────────────────────
# Install hping3 for packet generation
sudo apt install hping3

# Flood SYN packets to a blocked IP and measure drop rate
sudo hping3 -S -p 80 --faster <eth1-ip>

# In another terminal, watch stats:
watch -n 1 'sudo ./firewall stats'

# ── Test the REST API end-to-end ─────────────────────────────────
# Health check
curl http://localhost:8080/api/health
# → {"status":"ok"}

# Block a subnet
curl -X POST http://localhost:8080/api/block \
  -H "Content-Type: application/json" \
  -d '{"ip":"192.168.100.0","prefix":24}'
# → {"ok":"blocked 192.168.100.0/24"}

# Allow a custom port
curl -X POST http://localhost:8080/api/port/allow \
  -H "Content-Type: application/json" \
  -d '{"port":8443,"protocol":"tcp"}'
# → {"ok":"allowed port 8443/tcp"}

# Get stats
curl http://localhost:8080/api/stats
# → [{"ip":"192.168.100.5","stats":{"packets_dropped":142,...}}]
```

---

## 10. Troubleshooting

### XDP loaded in generic mode instead of native

```bash
# Symptom: ip link show eth1 says "xdpgeneric" instead of "xdp"
# Cause 1: MTU too large
sudo ip link set eth1 mtu 3498

# Cause 2: Too many combined channels
sudo ethtool -L eth1 combined 2

# Cause 3: ENA driver too old
modinfo ena | grep version
# Need >= 2.2.0; update kernel: sudo apt upgrade linux-image-$(uname -r)

# Verify native after fixes:
ip link show eth1 | grep -o 'xdp[^ ]*'
# Should print "xdp" (not "xdpgeneric")
```

### Verifier rejection errors

```bash
# Symptom: program.load() returns an error mentioning "verifier"
# Fix: run bpftool to see the full verifier log
bpftool prog load firewall.o /sys/fs/bpf/fw type xdp 2>&1 | head -100

# Common causes:
# "invalid access to map value"    → forgot BOUNDS_CHECK somewhere
# "R1 type=inv expected=ctx"       → wrong SEC() annotation
# "back-edge from insn X to Y"     → loop detected (BPF doesn't allow unbounded loops)
# "program too large"              → simplify logic; kernel has ~1M instruction limit
```

### SSH lockout prevention

```bash
# ALWAYS check this before enabling the firewall:
sudo ./firewall stats   # confirm port 22 is in the allowlist

# Emergency recovery: if locked out, use AWS Session Manager
# (EC2 Console → Connect → Session Manager — doesn't need port 22)

# Or detach XDP directly:
sudo ip link set eth1 xdpgeneric off
sudo ip link set eth1 xdp off
```

### Build failures

```bash
# "vmlinux.h not found"
# The BTF dump requires kernel BTF support
ls /sys/kernel/btf/vmlinux     # must exist
bpftool btf dump file /sys/kernel/btf/vmlinux format c > kernel/vmlinux.h

# Rust "bpf-linker not found"
cargo install bpf-linker
# If it fails: ensure llvm is installed
sudo apt install llvm-14 clang-14

# Go "module not found"
cd management && go mod tidy
```

---

## Quick Reference

```bash
# ── Load firewall ─────────────────────────────────────────────────
sudo ./ebpf-control eth1 ./firewall.o

# ── Block/unblock ─────────────────────────────────────────────────
sudo ./firewall block 1.2.3.0/24
sudo ./firewall unblock 1.2.3.0/24
sudo ./firewall block 185.220.101.0/24   # Known Tor exit node range

# ── Port rules ───────────────────────────────────────────────────
sudo ./firewall allow 22 tcp             # SSH
sudo ./firewall allow 80 tcp             # HTTP
sudo ./firewall allow 443 tcp            # HTTPS
sudo ./firewall deny 23                  # Telnet (explicitly)

# ── Monitor ───────────────────────────────────────────────────────
sudo ./firewall stats
watch -n 2 'sudo ./firewall stats'

# ── Emergency off switch ──────────────────────────────────────────
sudo ./firewall disable                  # Disables via config_map (no reload)
sudo ip link set eth1 xdp off            # Nuclear option: removes XDP entirely

# ── Inspect kernel state ──────────────────────────────────────────
bpftool prog list
bpftool map list
bpftool map dump name blocklist_ipv4
bpftool map dump name allow_ports
```

---

*Built with: Linux 6.8+ · Clang 17 · Rust 1.78 (nightly) · Aya 0.12 · Go 1.22 · AWS ENA driver 2.x*

Good. I have full context from your setup.md. The environment is AWS t3.small + Ubuntu 24.04 + ENA with XDP native configured. Setup is done. Now we build the project.

Here is the complete codebase. I'll go file by file in dependency order — common types first, kernel program second, user-space loader third, build tooling, then Go CLI.

---

## Project structure

```
ebpfirewall/
├── rust-toolchain.toml
├── Cargo.toml                   ← workspace (excludes eBPF crate)
├── Makefile
├── ebpfirewall-common/          ← shared types (no_std, compiles for both targets)
│   ├── Cargo.toml
│   └── src/lib.rs
├── ebpfirewall-ebpf/            ← XDP kernel program (bpfel-unknown-none)
│   ├── Cargo.toml
│   ├── .cargo/config.toml
│   └── src/main.rs
├── ebpfirewall/                 ← Aya loader + CLI (x86_64-unknown-linux-gnu)
│   ├── Cargo.toml
│   └── src/main.rs
├── xtask/                       ← cargo xtask build helper
│   ├── Cargo.toml
│   └── src/main.rs
├── scripts/
│   ├── verify_setup.sh          ← pre-flight checks before any build
│   └── integration_test.sh
└── ebpfctl/                     ← Go management CLI (accesses pinned maps)
    ├── go.mod
    └── main.go
```

---

## `rust-toolchain.toml`

```toml
[toolchain]
channel = "nightly"
components = ["rust-src", "rustfmt", "clippy"]
targets   = ["bpfel-unknown-none", "x86_64-unknown-linux-gnu"]
```

Nightly is required for `-Z build-std=core` used when cross-compiling for `bpfel-unknown-none`.

---

## `Cargo.toml` (workspace root)

```toml
[workspace]
members  = ["ebpfirewall", "ebpfirewall-common", "xtask"]
exclude  = ["ebpfirewall-ebpf"]   # different target — built via xtask
resolver = "2"

[profile.dev]
opt-level = 3     # eBPF verifier handles simpler IR; optimize even in dev

[profile.release]
opt-level = 3
debug     = 2     # keep symbols for bpftool / perf
strip     = false
lto       = true
```

`ebpfirewall-ebpf` is excluded from the workspace because it targets `bpfel-unknown-none`, not the host. Mixing targets in one workspace workspace causes `cargo build` to try both, which breaks.

---

## `ebpfirewall-common/Cargo.toml`

```toml
[package]
name    = "ebpfirewall-common"
version = "0.1.0"
edition = "2021"

[features]
default = ["user"]
user    = []          # enables std-dependent derives (Hash, Debug with fmt)
```

---

## `ebpfirewall-common/src/lib.rs`

```rust
//! Types shared between the eBPF kernel program and the Aya user-space loader.
//!
//! This crate must compile for BOTH targets:
//!   - bpfel-unknown-none  (no_std)  → used by ebpfirewall-ebpf
//!   - x86_64-unknown-linux-gnu      → used by ebpfirewall (default feature "user")
//!
//! Rule: never import anything from std here unless gated on `#[cfg(feature = "user")]`.

#![cfg_attr(not(feature = "user"), no_std)]

// ─── Firewall rule key (5-tuple) ──────────────────────────────────────────
//
// All fields are in HOST byte order. The XDP program converts from network
// byte order (wire) to host byte order before doing map lookups.
// User-space does the same when inserting.
//
// #[repr(C)] is critical: the BPF verifier sees this as a raw byte key.
// Padding bytes must be zeroed — the verifier will reject uninitialized key bytes.

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[cfg_attr(feature = "user", derive(Hash))]
pub struct FirewallKey {
    pub src_ip:   u32,
    pub dst_ip:   u32,
    pub src_port: u16,
    pub dst_port: u16,
    pub protocol: u8,
    pub _pad:     [u8; 3],   // MUST be zero — verifier checks all key bytes
}

// ─── Per-CPU packet statistics ───────────────────────────────────────────

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct PacketStats {
    pub packets: u64,
    pub bytes:   u64,   // reserved for future byte accounting
}

// ─── Index constants for PACKET_STATS PerCpuArray ────────────────────────

pub const STATS_IDX_PASS: u32 = 0;
pub const STATS_IDX_DROP: u32 = 1;

// ─── Firewall actions ─────────────────────────────────────────────────────

pub const ACTION_PASS: u32 = 0;
pub const ACTION_DROP: u32 = 1;
pub const ACTION_LOG:  u32 = 2;   // future: log + pass

// ─── Protocol bitmask for PORT_BLOCKLIST ──────────────────────────────────

pub const PROTO_TCP:  u8 = 1 << 0;
pub const PROTO_UDP:  u8 = 1 << 1;
pub const PROTO_BOTH: u8 = PROTO_TCP | PROTO_UDP;
```

---

## `ebpfirewall-ebpf/.cargo/config.toml`

```toml
[build]
target = "bpfel-unknown-none"

[unstable]
build-std = ["core"]
```

---

## `ebpfirewall-ebpf/Cargo.toml`

```toml
[package]
name    = "ebpfirewall-ebpf"
version = "0.1.0"
edition = "2021"

[dependencies]
aya-ebpf             = { version = "0.1" }
aya-log-ebpf         = { version = "0.1" }
network-types        = { version = "0.0.7" }
ebpfirewall-common   = { path = "../ebpfirewall-common", default-features = false }

[profile.dev]
opt-level       = 3
debug           = false
lto             = true
codegen-units   = 1
panic           = "abort"    # no unwinding runtime in BPF

[profile.release]
opt-level       = 3
debug           = false
lto             = true
codegen-units   = 1
panic           = "abort"
```

---

## `ebpfirewall-ebpf/src/main.rs` — XDP kernel program

```rust
#![no_std]
#![no_main]
#![allow(nonstandard_style)]   // BPF map names are UPPER_SNAKE per convention

use aya_ebpf::{
    bindings::xdp_action,
    macros::{map, xdp},
    maps::{HashMap, PerCpuArray},
    programs::XdpContext,
};
use core::mem;
use network_types::{
    eth::{EthHdr, EtherType},
    ip::{IpProto, Ipv4Hdr},
    tcp::TcpHdr,
    udp::UdpHdr,
};
use ebpfirewall_common::{
    ACTION_DROP,
    FirewallKey, PacketStats,
    PROTO_TCP, PROTO_UDP,
    STATS_IDX_DROP, STATS_IDX_PASS,
};

// ─── BPF Maps ─────────────────────────────────────────────────────────────
//
// Map names become ELF section names in the compiled .o object.
// The Aya user-space loader accesses them by these exact name strings.
//
// Key/value byte order convention (applies to ALL maps):
//   Keys are stored in HOST byte order (little-endian on x86).
//   The XDP program converts from network byte order (wire format) before lookup.
//   The user-space loader must also insert in host byte order.
//   See: byte_order_contract() comment in process_ipv4().

/// Source IPv4 address blocklist.
/// Key:   src_ip in host byte order.
/// Value: reason code (unused, reserved for future rate-limit tiers).
#[map]
static BLOCKLIST_IPV4: HashMap<u32, u32> =
    HashMap::with_max_entries(65536, 0);

/// Destination port blocklist.
/// Key:   dst_port in host byte order (u16 = 0..65535).
/// Value: protocol bitmask — PROTO_TCP | PROTO_UDP.
#[map]
static PORT_BLOCKLIST: HashMap<u16, u8> =
    HashMap::with_max_entries(1024, 0);

/// 5-tuple firewall rules.
/// Key:   FirewallKey (all fields host byte order, _pad MUST be zero).
/// Value: ACTION_PASS | ACTION_DROP | ACTION_LOG.
#[map]
static FIREWALL_RULES: HashMap<FirewallKey, u32> =
    HashMap::with_max_entries(65536, 0);

/// Per-CPU packet counters.
/// Index 0 (STATS_IDX_PASS): packets that passed.
/// Index 1 (STATS_IDX_DROP): packets that were dropped.
/// PerCpuArray avoids cache-line contention on multi-core — each CPU
/// writes to its own slot; user space aggregates across CPUs.
#[map]
static PACKET_STATS: PerCpuArray<PacketStats> =
    PerCpuArray::with_max_entries(2, 0);

// ─── XDP entry point ──────────────────────────────────────────────────────

#[xdp]
pub fn xdp_firewall(ctx: XdpContext) -> u32 {
    // Wrap in Result so we can use `?` for bounds-check propagation.
    // XDP_ABORTED signals a programming error — visible in bpftool stats.
    match try_firewall(&ctx) {
        Ok(action) => action,
        Err(_)     => xdp_action::XDP_ABORTED,
    }
}

#[inline(always)]
fn try_firewall(ctx: &XdpContext) -> Result<u32, ()> {
    let eth: *const EthHdr = ptr_at(ctx, 0)?;

    match unsafe { (*eth).ether_type } {
        EtherType::Ipv4 => process_ipv4(ctx, EthHdr::LEN),
        // Pass everything else: ARP, IPv6 (handle IPv6 separately later),
        // 802.1Q, etc.
        _ => Ok(xdp_action::XDP_PASS),
    }
}

// ─── IPv4 processing ──────────────────────────────────────────────────────

fn process_ipv4(ctx: &XdpContext, eth_len: usize) -> Result<u32, ()> {
    let ip: *const Ipv4Hdr = ptr_at(ctx, eth_len)?;

    // ── Byte order contract ────────────────────────────────────────────
    // Wire format (NBO, big-endian). On bpfel (little-endian eBPF VM):
    //   reading bytes [0x01, 0x02, 0x03, 0x04] as u32 = 0x04030201
    //   u32::from_be(0x04030201) = 0x01020304 = host-order repr of 1.2.3.4
    //
    // User-space: u32::from(Ipv4Addr::new(1,2,3,4)) = 0x01020304 (same)
    // Both sides use host byte order in the map. ✓

    let src_ip = u32::from_be(unsafe { (*ip).src_addr });
    let dst_ip = u32::from_be(unsafe { (*ip).dst_addr });
    let proto  = unsafe { (*ip).proto };

    // IHL is the IP header length in 32-bit words; multiply by 4 for bytes.
    // We need this to skip over IP options to reach the transport header.
    let ihl      = unsafe { (((*ip).version_ihl & 0x0F) * 4) as usize };
    let l4_start = eth_len + ihl;

    // ── Stage 1: IP blocklist (checked before L4 parsing) ─────────────
    // This is the hot path for blocked hosts — avoid unnecessary parsing.
    if BLOCKLIST_IPV4.get(&src_ip).is_some() {
        bump_stats(STATS_IDX_DROP);
        return Ok(xdp_action::XDP_DROP);
    }

    // ── Parse L4 headers ──────────────────────────────────────────────
    let (src_port, dst_port) = parse_ports(ctx, l4_start, proto)?;

    // ── Stage 2: Port blocklist ────────────────────────────────────────
    // Same byte-order contract: ports are stored in host order.
    if dst_port != 0 {
        if let Some(&mask) = PORT_BLOCKLIST.get(&dst_port) {
            let proto_bit = proto_to_bit(proto);
            if proto_bit != 0 && (mask & proto_bit != 0) {
                bump_stats(STATS_IDX_DROP);
                return Ok(xdp_action::XDP_DROP);
            }
        }
    }

    // ── Stage 3: 5-tuple rule table ────────────────────────────────────
    // Most specific match. All FirewallKey fields in host byte order.
    // _pad MUST be zeroed — BPF verifier checks every byte of the key.
    let key = FirewallKey {
        src_ip,
        dst_ip,
        src_port,
        dst_port,
        protocol: proto as u8,
        _pad: [0u8; 3],
    };

    if let Some(&action) = FIREWALL_RULES.get(&key) {
        if action == ACTION_DROP {
            bump_stats(STATS_IDX_DROP);
            return Ok(xdp_action::XDP_DROP);
        }
        bump_stats(STATS_IDX_PASS);
        return Ok(xdp_action::XDP_PASS);
    }

    // ── Default policy: PASS ───────────────────────────────────────────
    // Change to XDP_DROP here for a default-deny posture.
    // In that mode, only explicitly allowed rules let traffic through.
    bump_stats(STATS_IDX_PASS);
    Ok(xdp_action::XDP_PASS)
}

// ─── Helpers ──────────────────────────────────────────────────────────────

/// Parse source and destination ports from TCP or UDP header.
/// Returns (0, 0) for protocols with no port concept (ICMP, etc.).
#[inline(always)]
fn parse_ports(ctx: &XdpContext, offset: usize, proto: IpProto)
    -> Result<(u16, u16), ()>
{
    match proto {
        IpProto::Tcp => {
            let hdr: *const TcpHdr = ptr_at(ctx, offset)?;
            Ok(unsafe {
                (u16::from_be((*hdr).source), u16::from_be((*hdr).dest))
            })
        }
        IpProto::Udp => {
            let hdr: *const UdpHdr = ptr_at(ctx, offset)?;
            Ok(unsafe {
                (u16::from_be((*hdr).source), u16::from_be((*hdr).dest))
            })
        }
        _ => Ok((0u16, 0u16)),
    }
}

/// Map IpProto to the bitmask used in PORT_BLOCKLIST.
#[inline(always)]
fn proto_to_bit(proto: IpProto) -> u8 {
    match proto {
        IpProto::Tcp => PROTO_TCP,
        IpProto::Udp => PROTO_UDP,
        _             => 0,
    }
}

/// Bounds-checked pointer into XDP packet data.
///
/// This is the ONLY safe way to access packet bytes in an XDP program.
/// The BPF verifier statically traces every memory access; any path where
/// `start + offset + size_of::<T>()` could exceed `data_end` will be rejected.
/// The explicit comparison here satisfies the verifier.
///
/// Must be #[inline(always)] — the verifier tracks bounds PER CALL SITE.
/// If this were not inlined, the verifier would need to re-verify bounds
/// at every call, which older kernels reject.
#[inline(always)]
fn ptr_at<T>(ctx: &XdpContext, offset: usize) -> Result<*const T, ()> {
    let start = ctx.data();
    let end   = ctx.data_end();
    let size  = mem::size_of::<T>();

    if start + offset + size > end {
        return Err(());   // packet too short — bounds check failed
    }
    Ok((start + offset) as *const T)
}

/// Increment per-CPU packet counter. wrapping_add prevents overflow traps.
#[inline(always)]
fn bump_stats(idx: u32) {
    if let Some(s) = PACKET_STATS.get_ptr_mut(idx) {
        unsafe {
            (*s).packets = (*s).packets.wrapping_add(1);
        }
    }
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    // BPF programs should never panic. This satisfies the linker.
    // The verifier rejects programs with reachable panic paths anyway.
    unsafe { core::hint::unreachable_unchecked() }
}
```

---

## `ebpfirewall/Cargo.toml`

```toml
[package]
name        = "ebpfirewall"
version     = "0.1.0"
edition     = "2021"
description = "XDP firewall — Aya loader, rule manager, pinned-map CLI"

[[bin]]
name = "ebpfirewall"
path = "src/main.rs"

[dependencies]
aya                  = { version = "0.13", features = ["async_tokio"] }
aya-log              = { version = "0.2" }
ebpfirewall-common   = { path = "../ebpfirewall-common" }
anyhow               = "1"
clap                 = { version = "4", features = ["derive"] }
env_logger           = "0.11"
log                  = "0.4"
tokio                = { version = "1", features = ["full"] }
```

---

## `ebpfirewall/src/main.rs` — Aya loader + CLI

```rust
//! ebpfirewall — user-space control plane
//!
//! Two modes of operation:
//!
//!   DAEMON mode (`start`):
//!     Loads the XDP program, attaches it to the interface, pins BPF maps
//!     to /sys/fs/bpf/ebpfirewall/, then loops until Ctrl+C.
//!     After pinning, any sub-command (block-ip, stats, …) can run
//!     concurrently without re-loading the eBPF object.
//!
//!   COMMAND mode (block-ip, stats, …):
//!     Opens the pinned maps directly (does NOT re-load the BPF object),
//!     performs the operation, and exits.
//!     This is the correct pattern for tools like ip(8), tc(8), bpftool.

use anyhow::{bail, Context, Result};
use aya::{
    maps::{HashMap, MapData, PerCpuArray},
    programs::{Xdp, XdpFlags},
    Bpf,
};
use aya_log::BpfLogger;
use clap::{Parser, Subcommand};
use ebpfirewall_common::{
    FirewallKey, PacketStats, ACTION_DROP, ACTION_PASS,
    PROTO_BOTH, PROTO_TCP, PROTO_UDP, STATS_IDX_DROP, STATS_IDX_PASS,
};
use log::{info, warn};
use std::net::Ipv4Addr;
use std::path::Path;
use std::time::Duration;
use tokio::{signal, time};

// Pinned map paths. The Go CLI (ebpfctl) uses these same paths.
const PIN_DIR:      &str = "/sys/fs/bpf/ebpfirewall";
const PIN_BLOCKLIST: &str = "/sys/fs/bpf/ebpfirewall/BLOCKLIST_IPV4";
const PIN_PORTS:    &str = "/sys/fs/bpf/ebpfirewall/PORT_BLOCKLIST";
const PIN_RULES:    &str = "/sys/fs/bpf/ebpfirewall/FIREWALL_RULES";
const PIN_STATS:    &str = "/sys/fs/bpf/ebpfirewall/PACKET_STATS";

// ─── CLI ──────────────────────────────────────────────────────────────────

#[derive(Parser)]
#[command(name = "ebpfirewall", version, about = "XDP firewall control plane")]
struct Cli {
    #[arg(short, long, default_value = "eth1")]
    iface: String,

    /// Use generic XDP (SKB mode) — no ENA driver requirement.
    /// Native mode is the default and must be used for ENA performance.
    #[arg(long, default_value_t = false)]
    generic: bool,

    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Load XDP program, attach to interface, pin maps, run as daemon.
    Start,

    /// Block all packets from a source IPv4 address.
    BlockIp { ip: Ipv4Addr },

    /// Remove an IP from the blocklist.
    UnblockIp { ip: Ipv4Addr },

    /// Block a destination port. --proto tcp|udp|both
    BlockPort {
        port: u16,
        #[arg(long, default_value = "both")]
        proto: String,
    },

    /// Remove a port from the blocklist.
    UnblockPort { port: u16 },

    /// Add a 5-tuple rule. --action drop|pass
    AddRule {
        src_ip:   Ipv4Addr,
        dst_ip:   Ipv4Addr,
        src_port: u16,
        dst_port: u16,
        proto:    String,
        #[arg(long, default_value = "drop")]
        action: String,
    },

    /// Print aggregated per-CPU packet counters.
    Stats,

    /// Print all blocked source IPs.
    ListBlockedIps,
}

// ─── Entry point ──────────────────────────────────────────────────────────

#[tokio::main]
async fn main() -> Result<()> {
    env_logger::init();
    let cli = Cli::parse();

    match &cli.cmd {
        Cmd::Start => cmd_start(&cli.iface, cli.generic).await,

        // All other commands access pinned maps — no BPF object load needed.
        Cmd::BlockIp     { ip }             => cmd_block_ip(*ip),
        Cmd::UnblockIp   { ip }             => cmd_unblock_ip(*ip),
        Cmd::BlockPort   { port, proto }    => cmd_block_port(*port, proto),
        Cmd::UnblockPort { port }           => cmd_unblock_port(*port),
        Cmd::AddRule { src_ip, dst_ip, src_port, dst_port, proto, action } =>
            cmd_add_rule(*src_ip, *dst_ip, *src_port, *dst_port, proto, action),
        Cmd::Stats        => cmd_stats(),
        Cmd::ListBlockedIps => cmd_list_ips(),
    }
}

// ─── Daemon: load + attach + pin ──────────────────────────────────────────

async fn cmd_start(iface: &str, generic: bool) -> Result<()> {
    // Embed the compiled eBPF object at build time.
    // Run `cargo xtask build-ebpf` before `cargo build`.
    let bpf_bytes: &[u8] = {
        #[cfg(debug_assertions)]
        { include_bytes!("../../target/bpfel-unknown-none/debug/ebpfirewall-ebpf") }
        #[cfg(not(debug_assertions))]
        { include_bytes!("../../target/bpfel-unknown-none/release/ebpfirewall-ebpf") }
    };

    let mut bpf = Bpf::load(bpf_bytes)
        .context("BPF object load failed — did you run `cargo xtask build-ebpf`?")?;

    if let Err(e) = BpfLogger::init(&mut bpf) {
        warn!("aya-log ring-buffer unavailable: {} — kernel log disabled", e);
    }

    // ── Attach XDP ──────────────────────────────────────────────────────
    let flags = if generic { XdpFlags::SKB_MODE } else { XdpFlags::default() };

    let prog: &mut Xdp = bpf
        .program_mut("xdp_firewall")
        .context("program 'xdp_firewall' not in object — check ebpf crate")?
        .try_into()?;

    prog.load().context("BPF verifier rejected the program")?;
    prog.attach(iface, flags).with_context(|| {
        format!(
            "attach to {} failed. \
             For native mode: MTU must be ≤3498 and channels reduced. \
             Check: ip link show {0} | grep xdp",
            iface
        )
    })?;

    info!(
        "XDP firewall ATTACHED to {} mode={}",
        iface,
        if generic { "generic(SKB)" } else { "native(DRV)" }
    );

    // ── Pin maps to bpffs ───────────────────────────────────────────────
    // Pinning allows the Go CLI and future `ebpfirewall <cmd>` invocations
    // to access live maps without re-loading the BPF object.
    std::fs::create_dir_all(PIN_DIR)
        .context("failed to create BPF pin directory — is bpffs mounted at /sys/fs/bpf?")?;

    for (name, path) in &[
        ("BLOCKLIST_IPV4", PIN_BLOCKLIST),
        ("PORT_BLOCKLIST",  PIN_PORTS),
        ("FIREWALL_RULES",  PIN_RULES),
        ("PACKET_STATS",    PIN_STATS),
    ] {
        // Skip if already pinned (e.g. after a daemon restart)
        if Path::new(path).exists() {
            continue;
        }
        bpf.map(name)
            .with_context(|| format!("map {} not found", name))?
            .pin(path)
            .with_context(|| format!("pin {} to {} failed", name, path))?;
        info!("Pinned {} → {}", name, path);
    }

    info!("Maps pinned. Go CLI: cd ebpfctl && go run . -stats");
    info!("Ctrl+C to detach and stop");

    // ── Event loop with periodic stats ─────────────────────────────────
    let mut tick = time::interval(Duration::from_secs(30));
    loop {
        tokio::select! {
            _ = tick.tick()       => { let _ = cmd_stats(); }
            _ = signal::ctrl_c() => {
                info!("Detaching XDP program and removing pins");
                // Aya drops the Bpf object here, which auto-detaches.
                // Clean up pin files so next run re-pins.
                for (_, path) in &[
                    ("", PIN_BLOCKLIST),
                    ("", PIN_PORTS),
                    ("", PIN_RULES),
                    ("", PIN_STATS),
                ] {
                    let _ = std::fs::remove_file(path);
                }
                break;
            }
        }
    }
    Ok(())
}

// ─── Pinned-map commands ──────────────────────────────────────────────────

fn cmd_block_ip(ip: Ipv4Addr) -> Result<()> {
    let data = MapData::from_pin(PIN_BLOCKLIST)
        .context("failed to open pinned BLOCKLIST_IPV4 — is the daemon running?")?;
    let mut map: HashMap<_, u32, u32> = HashMap::try_from(data)?;

    // u32::from(ip) gives the host-byte-order representation of the IP.
    // The XDP program uses the same convention after u32::from_be(src_addr).
    map.insert(u32::from(ip), 0u32, 0)?;
    println!("Blocked: {}", ip);
    Ok(())
}

fn cmd_unblock_ip(ip: Ipv4Addr) -> Result<()> {
    let data = MapData::from_pin(PIN_BLOCKLIST)
        .context("BLOCKLIST_IPV4 not pinned — is the daemon running?")?;
    let mut map: HashMap<_, u32, u32> = HashMap::try_from(data)?;
    map.remove(&u32::from(ip))?;
    println!("Unblocked: {}", ip);
    Ok(())
}

fn cmd_block_port(port: u16, proto: &str) -> Result<()> {
    let mask: u8 = match proto {
        "tcp"  => PROTO_TCP,
        "udp"  => PROTO_UDP,
        "both" => PROTO_BOTH,
        other  => bail!("unknown protocol '{}' — use tcp|udp|both", other),
    };
    let data = MapData::from_pin(PIN_PORTS)
        .context("PORT_BLOCKLIST not pinned")?;
    let mut map: HashMap<_, u16, u8> = HashMap::try_from(data)?;
    map.insert(port, mask, 0)?;
    println!("Blocked port {} ({})", port, proto);
    Ok(())
}

fn cmd_unblock_port(port: u16) -> Result<()> {
    let data = MapData::from_pin(PIN_PORTS)?;
    let mut map: HashMap<_, u16, u8> = HashMap::try_from(data)?;
    map.remove(&port)?;
    println!("Unblocked port {}", port);
    Ok(())
}

fn cmd_add_rule(
    src_ip: Ipv4Addr, dst_ip: Ipv4Addr,
    src_port: u16, dst_port: u16,
    proto: &str, action: &str,
) -> Result<()> {
    let proto_num: u8 = match proto {
        "tcp"  => 6,
        "udp"  => 17,
        "icmp" => 1,
        other  => bail!("unknown proto '{}'", other),
    };
    let action_val: u32 = match action {
        "drop" => ACTION_DROP,
        "pass" => ACTION_PASS,
        other  => bail!("unknown action '{}' — use drop|pass", other),
    };
    let key = FirewallKey {
        src_ip:   u32::from(src_ip),
        dst_ip:   u32::from(dst_ip),
        src_port,
        dst_port,
        protocol: proto_num,
        _pad:     [0u8; 3],
    };
    let data = MapData::from_pin(PIN_RULES)?;
    let mut map: HashMap<_, FirewallKey, u32> = HashMap::try_from(data)?;
    map.insert(key, action_val, 0)?;
    println!("Rule added: {}:{} → {}:{} proto={} action={}", src_ip, src_port, dst_ip, dst_port, proto, action);
    Ok(())
}

fn cmd_stats() -> Result<()> {
    let data = MapData::from_pin(PIN_STATS)
        .context("PACKET_STATS not pinned — is the daemon running?")?;
    let map: PerCpuArray<_, PacketStats> = PerCpuArray::try_from(data)?;

    let pass_vals = map.get(&STATS_IDX_PASS, 0)?;
    let drop_vals = map.get(&STATS_IDX_DROP, 0)?;

    let (total_pass, total_drop) = pass_vals
        .iter()
        .zip(drop_vals.iter())
        .fold((0u64, 0u64), |(p, d), (pv, dv)| (p + pv.packets, d + dv.packets));

    println!("PASSED   {:>12}", total_pass);
    println!("DROPPED  {:>12}", total_drop);
    Ok(())
}

fn cmd_list_ips() -> Result<()> {
    let data = MapData::from_pin(PIN_BLOCKLIST)?;
    let map: HashMap<_, u32, u32> = HashMap::try_from(data)?;
    let mut count = 0usize;
    for entry in map.iter() {
        let (key, _) = entry?;
        println!("{}", Ipv4Addr::from(key));
        count += 1;
    }
    if count == 0 { println!("(no blocked IPs)"); }
    Ok(())
}
```

---

## `xtask/Cargo.toml`

```toml
[package]
name    = "xtask"
version = "0.1.0"
edition = "2021"

[dependencies]
anyhow = "1"
```

---

## `xtask/src/main.rs`

```rust
//! cargo xtask — build helper for the eBPF crate.
//!
//! The eBPF crate targets `bpfel-unknown-none` and requires
//! `-Z build-std=core` (nightly only). Normal `cargo build` cannot
//! do this for the workspace automatically, so we use xtask.
//!
//! Usage:
//!   cargo xtask build-ebpf            # debug
//!   cargo xtask build-ebpf-release    # release

use anyhow::{bail, Context, Result};
use std::{env, process::Command};

fn main() -> Result<()> {
    let task = env::args().nth(1).unwrap_or_default();
    match task.as_str() {
        "build-ebpf"         => build_ebpf(false),
        "build-ebpf-release" => build_ebpf(true),
        other => {
            eprintln!("Unknown task: {other}");
            eprintln!("Available: build-ebpf, build-ebpf-release");
            bail!("no task selected");
        }
    }
}

fn build_ebpf(release: bool) -> Result<()> {
    let mut cmd = Command::new("cargo");
    cmd.args([
        "+nightly",
        "build",
        "--package", "ebpfirewall-ebpf",
        "-Z", "build-std=core",
        "--target", "bpfel-unknown-none",
    ]);

    if release {
        cmd.arg("--release");
    }

    println!(
        "Building eBPF object [{}]...",
        if release { "release" } else { "debug" }
    );

    let status = cmd
        .status()
        .context("failed to spawn cargo — is nightly installed? run: rustup toolchain install nightly")?;

    if !status.success() {
        bail!("eBPF build failed (status {})", status);
    }

    println!("eBPF build OK");
    Ok(())
}
```

---

## `Makefile`

```makefile
IFACE   ?= eth1
DEBUG   := ./target/debug/ebpfirewall
RELEASE := ./target/release/ebpfirewall
PIN_DIR := /sys/fs/bpf/ebpfirewall

.PHONY: all build build-ebpf build-release run stop fmt lint \
        test verify clean block-ip stats

all: build

# ── Build ──────────────────────────────────────────────────────────────────

build-ebpf:
	cargo xtask build-ebpf

build: build-ebpf
	cargo build --workspace --exclude ebpfirewall-ebpf

build-release:
	cargo xtask build-ebpf-release
	cargo build --release --workspace --exclude ebpfirewall-ebpf

# ── Run ────────────────────────────────────────────────────────────────────

run: build
	sudo RUST_LOG=info $(DEBUG) --iface $(IFACE) start

# Detach without waiting for daemon (useful if daemon is hung)
stop:
	sudo ip link set $(IFACE) xdp off 2>/dev/null || true
	sudo rm -rf $(PIN_DIR) || true

stats: build
	sudo $(DEBUG) stats

# ── Go CLI ─────────────────────────────────────────────────────────────────

go-build:
	cd ebpfctl && go build -o ../ebpfctl-bin .

go-stats:
	sudo ./ebpfctl-bin -stats

# ── Quality ────────────────────────────────────────────────────────────────

fmt:
	cargo fmt --all
	cargo fmt --manifest-path ebpfirewall-ebpf/Cargo.toml

lint:
	cargo clippy --workspace --exclude ebpfirewall-ebpf -- -D warnings
	cd ebpfctl && go vet ./...

# ── Test ───────────────────────────────────────────────────────────────────

verify:
	bash scripts/verify_setup.sh

test: build verify
	sudo IFACE=$(IFACE) bash scripts/integration_test.sh

# ── Clean ──────────────────────────────────────────────────────────────────

clean:
	cargo clean
	sudo rm -rf $(PIN_DIR) || true
	rm -f ebpfctl-bin
```

---

## `scripts/verify_setup.sh`

```bash
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
```

---

## `scripts/integration_test.sh`

```bash
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
```

---

## `ebpfctl/go.mod`

```go
module github.com/sushink70/ebpfirewall/ebpfctl

go 1.22

require github.com/cilium/ebpf v0.15.0
```

---

## `ebpfctl/main.go` — Go management CLI

```go
// ebpfctl — Go management CLI for ebpfirewall.
//
// Accesses pinned BPF maps directly using cilium/ebpf.
// The Aya daemon (ebpfirewall start) must be running and have pinned
// maps to /sys/fs/bpf/ebpfirewall/ before using this tool.
//
// Usage:
//   sudo go run . -stats
//   sudo go run . -block-ip 10.0.0.5
//   sudo go run . -block-port 22 -proto tcp
//   sudo go run . -list-ips

package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"log"
	"net"
	"os"

	"github.com/cilium/ebpf"
)

// Pin paths — must match constants in ebpfirewall/src/main.rs
const (
	pinDir      = "/sys/fs/bpf/ebpfirewall"
	pinBlocklist = pinDir + "/BLOCKLIST_IPV4"
	pinPorts    = pinDir + "/PORT_BLOCKLIST"
	pinRules    = pinDir + "/FIREWALL_RULES"
	pinStats    = pinDir + "/PACKET_STATS"
)

// Protocol masks — must match PROTO_* constants in ebpfirewall-common
const (
	protoTCP  = uint8(1)
	protoUDP  = uint8(2)
	protoBoth = protoTCP | protoUDP
)

// Action constants — must match ACTION_* in ebpfirewall-common
const (
	actionPass = uint32(0)
	actionDrop = uint32(1)
)

// perCPUStats matches the PacketStats struct in ebpfirewall-common.
// cilium/ebpf uses encoding/binary to deserialize PerCPU values.
type perCPUStats struct {
	Packets uint64
	Bytes   uint64
}

// firewallKey matches FirewallKey in ebpfirewall-common (repr(C)).
// All fields in host byte order.
type firewallKey struct {
	SrcIP    uint32
	DstIP    uint32
	SrcPort  uint16
	DstPort  uint16
	Protocol uint8
	Pad      [3]uint8
}

func main() {
	var (
		flagBlockIP    = flag.String("block-ip", "", "Block source IP")
		flagUnblockIP  = flag.String("unblock-ip", "", "Unblock source IP")
		flagBlockPort  = flag.Uint("block-port", 0, "Block destination port (use with -proto)")
		flagUnblockPort = flag.Uint("unblock-port", 0, "Unblock destination port")
		flagProto      = flag.String("proto", "both", "Protocol: tcp|udp|both")
		flagStats      = flag.Bool("stats", false, "Show packet statistics")
		flagListIPs    = flag.Bool("list-ips", false, "List blocked IPs")
	)
	flag.Parse()

	if os.Geteuid() != 0 {
		log.Fatal("ebpfctl requires root (BPF map access)")
	}

	switch {
	case *flagBlockIP != "":
		blockIP(*flagBlockIP)
	case *flagUnblockIP != "":
		unblockIP(*flagUnblockIP)
	case *flagBlockPort != 0:
		blockPort(uint16(*flagBlockPort), *flagProto)
	case *flagUnblockPort != 0:
		unblockPort(uint16(*flagUnblockPort))
	case *flagStats:
		showStats()
	case *flagListIPs:
		listBlockedIPs()
	default:
		flag.Usage()
		os.Exit(1)
	}
}

// ─── IP blocklist ─────────────────────────────────────────────────────────

func blockIP(ipStr string) {
	ip := parseIPv4(ipStr)
	m  := openMap(pinBlocklist)
	defer m.Close()

	// Key is the host-byte-order u32.
	// binary.BigEndian.Uint32([1,2,3,4]) = 0x01020304 = u32::from(Ipv4Addr) in Rust.
	key := binary.BigEndian.Uint32(ip)
	if err := m.Put(key, uint32(0)); err != nil {
		log.Fatalf("map insert: %v", err)
	}
	fmt.Printf("Blocked: %s\n", ipStr)
}

func unblockIP(ipStr string) {
	ip := parseIPv4(ipStr)
	m  := openMap(pinBlocklist)
	defer m.Close()

	key := binary.BigEndian.Uint32(ip)
	if err := m.Delete(key); err != nil {
		log.Fatalf("map delete: %v", err)
	}
	fmt.Printf("Unblocked: %s\n", ipStr)
}

func listBlockedIPs() {
	m := openMap(pinBlocklist)
	defer m.Close()

	var key uint32
	var val uint32
	iter  := m.Iterate()
	count := 0
	for iter.Next(&key, &val) {
		// key is host-byte-order u32; convert back to dotted-decimal
		b := make([]byte, 4)
		binary.BigEndian.PutUint32(b, key)
		fmt.Println(net.IP(b).String())
		count++
	}
	if err := iter.Err(); err != nil {
		log.Fatalf("map iterate: %v", err)
	}
	if count == 0 {
		fmt.Println("(no blocked IPs)")
	}
}

// ─── Port blocklist ───────────────────────────────────────────────────────

func blockPort(port uint16, proto string) {
	mask := parseMask(proto)
	m    := openMap(pinPorts)
	defer m.Close()

	if err := m.Put(port, mask); err != nil {
		log.Fatalf("map insert: %v", err)
	}
	fmt.Printf("Blocked port %d (%s)\n", port, proto)
}

func unblockPort(port uint16) {
	m := openMap(pinPorts)
	defer m.Close()

	if err := m.Delete(port); err != nil {
		log.Fatalf("map delete: %v", err)
	}
	fmt.Printf("Unblocked port %d\n", port)
}

// ─── Statistics ───────────────────────────────────────────────────────────

func showStats() {
	m := openMap(pinStats)
	defer m.Close()

	var passVals []perCPUStats
	var dropVals []perCPUStats

	if err := m.Lookup(uint32(0), &passVals); err != nil {
		log.Fatalf("lookup pass: %v", err)
	}
	if err := m.Lookup(uint32(1), &dropVals); err != nil {
		log.Fatalf("lookup drop: %v", err)
	}

	var totalPass, totalDrop uint64
	for _, v := range passVals {
		totalPass += v.Packets
	}
	for _, v := range dropVals {
		totalDrop += v.Packets
	}

	fmt.Printf("%-12s %12d\n", "PASSED:", totalPass)
	fmt.Printf("%-12s %12d\n", "DROPPED:", totalDrop)
}

// ─── Helpers ─────────────────────────────────────────────────────────────

func openMap(pin string) *ebpf.Map {
	m, err := ebpf.LoadPinnedMap(pin, &ebpf.LoadPinOptions{})
	if err != nil {
		log.Fatalf(
			"Cannot open pinned map %s: %v\n"+
				"Hint: is `ebpfirewall start` running?",
			pin, err,
		)
	}
	return m
}

func parseIPv4(s string) net.IP {
	ip := net.ParseIP(s).To4()
	if ip == nil {
		log.Fatalf("invalid IPv4 address: %s", s)
	}
	return ip
}

func parseMask(proto string) uint8 {
	switch proto {
	case "tcp":
		return protoTCP
	case "udp":
		return protoUDP
	case "both":
		return protoBoth
	default:
		log.Fatalf("unknown proto '%s' — use tcp|udp|both", proto)
		return 0
	}
}
```

---

## Build and first run sequence

```bash
# 1. Pre-flight (run once on a fresh instance)
bash scripts/verify_setup.sh

# 2. Build — eBPF object first, then user-space
make build

# 3. Start the daemon (needs root for XDP attach + BPF pin)
make run IFACE=eth1        # or: sudo RUST_LOG=info ./target/debug/ebpfirewall start

# 4. In another terminal — Rust CLI
sudo ./target/debug/ebpfirewall block-ip 10.0.0.100
sudo ./target/debug/ebpfirewall stats
sudo ./target/debug/ebpfirewall list-blocked-ips

# 5. Go CLI (same machine, separate process — reads pinned maps)
cd ebpfctl && go mod tidy
sudo go run . -stats
sudo go run . -block-ip 10.0.0.200
sudo go run . -list-ips

# 6. Verify XDP mode is native (not generic)
ip link show eth1 | grep xdp
# Should show: xdp (not xdpgeneric)

# 7. Integration tests
sudo make test IFACE=eth1
```

---

## Key design decisions to understand

**Byte order contract.** The XDP program calls `u32::from_be(src_addr)` to convert from wire (network) byte order to host byte order before every map lookup. Both the Rust CLI and Go CLI insert keys in host byte order. This is the canonical pattern from the Aya codebase and avoids the common bug where user-space inserts NBO and kernel reads NBO as-is on a LE BPF VM — they look the same value but compare differently because of how `u32` is stored in memory on x86.

**Map pinning.** The daemon pins maps on start. CLI subcommands (`block-ip`, `stats`, etc.) access pinned maps directly without re-loading the BPF object. This is how production tools like Cilium and Falco work. You can add/remove rules while the daemon is running without restarting it.

**PerCpuArray for stats.** Lock-free counter updates. Each CPU core writes to its own slot, eliminating false sharing on the cache line. User space aggregates by summing across all CPUs. Never use a regular array for high-frequency counters in XDP.

**`_pad: [0u8; 3]` in FirewallKey.** The BPF verifier rejects map lookups with uninitialized bytes in the key. `FirewallKey` has protocol (u8) followed by 3 bytes of padding for alignment. These MUST be zeroed before every lookup. The `Default` derive initializes them to zero for you if you use `..Default::default()` or explicit `[0u8; 3]`.

**`#[inline(always)]` on `ptr_at`.** The verifier does not track bounds across function call boundaries in older kernels. Always inline your bounds-check helper — otherwise the verifier cannot prove each access is safe and will reject the program.