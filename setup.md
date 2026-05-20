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