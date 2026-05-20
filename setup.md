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
│  ↓ packets arrive at driver level                          │
├─────────────────────────────────────────────────────────────┤
│  C / XDP Program (kernel space)                            │
│  - Header parsing, map lookup, XDP_DROP / XDP_PASS         │
│  - BPF maps: LPM trie, hash map, per-CPU counters          │
├─────────────────────────────────────────────────────────────┤
│  Rust / Aya Control Plane (userspace)                      │
│  - Loads BPF object, attaches XDP hook                     │
│  - Reads/writes BPF maps (the "bridge" between planes)     │
│  - Unix socket server for management commands              │
├─────────────────────────────────────────────────────────────┤
│  Go / Management API + CLI (userspace)                     │
│  - REST/gRPC API for operators                             │
│  - CLI tool, rule persistence, Prometheus metrics          │
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
│  (Aya / libbpf-rs)         │     BPF object loader
│  - map: BLOCKLIST (LPM)    │     XDP attacher
│  - map: RULES (hash)       │     Rule→map translator
│  - map: COUNTERS (percpu)  │     Stats aggregator
└────────────┬────────────────┘
             │ BPF syscall (bpf_map_update_elem)
             ▼
┌─────────────────────────────┐
│  C: xdp_firewall.c          │   ← Kernel datapath
│  Compiled → xdp_firewall.o  │     Runs in ENA driver hook
│                             │     Zero SKB allocation
│  parse eth → ip → tcp/udp  │     Line-rate DROP/PASS
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