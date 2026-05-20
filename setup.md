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
│  Driver must implement ndo_bpf() + xdp_xmit().         │
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
┌─────────────────────────────────────────────────────────────┐
│                    Lab Server (bare metal)                   │
│                                                             │
│  ┌─────────────┐      ┌────────────────────────────────┐   │
│  │  eth0       │      │  Intel X710-DA2 (i40e driver)   │   │
│  │  (1G mgmt)  │      │  ┌──────────┐  ┌──────────┐   │   │
│  │  SSH / API  │      │  │  enp4s0f0│  │  enp4s0f1│   │   │
│  └─────────────┘      │  │  (RX/XDP)│  │  (TX/fwd)│   │   │
│                        │  └────┬─────┘  └────┬─────┘   │   │
│                        └───────┼─────────────┼──────────┘   │
│                                │             │              │
└────────────────────────────────┼─────────────┼──────────────┘
                                 │ DAC cable   │ DAC cable
                            ┌────┴─────────────┴────┐
                            │   Traffic Generator    │
                            │  (second server OR     │
                            │   same server with     │
                            │   network namespaces)  │
                            └───────────────────────-┘

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
│                                 │ cilium/ebpf map.Update() / map.Lookup() │
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
│  │  • Runs per-packet, inside i40e/mlx5 NAPI poll (no sk_buff)       │  │
│  │  • Parses: ETH → IP → TCP/UDP headers in ≤ 512 bytes stack        │  │
│  │  • Lookups: bpf_map_lookup_elem() on blocklist / allowlist maps    │  │
│  │  • Actions: XDP_DROP, XDP_PASS, XDP_TX                             │  │
│  │  • Telemetry: bpf_ringbuf_submit() for blocked-flow events         │  │
│  └────────────────────────────────────────────────────────────────────┘  │
│                                                                          │
│  Runs at: kernel/driver level — BEFORE sk_buff allocation               │
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