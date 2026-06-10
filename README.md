# eBPFirewall
## Firewall rules that used to configure with cilium and calico with the use of Aya-rs.

A production-oriented eBPF-based firewall/NAT/rate-limiting project with XDP fast-path packet processing and Go-based control plane utilities. This README is written from the public repository setup instructions and is intended to replace or complement a minimal setup guide.

### Repository

github.com/sushink70/ebpfirewall

### What this project does

The project attaches an XDP (eXpress Data Path) program to a network interface and performs packet filtering, rate limiting, and connection/NAT-related handling in kernel space using eBPF maps. A Go-based userspace component is used to load programs, manage maps/rules, and run demos/tests. The setup instructions also configure Linux forwarding and nftables masquerading for routed/NAT scenarios.

XDP fast path

Drop/allow decisions before the traditional Linux network stack for lower latency and higher packet throughput than iptables/nftables-only paths.

eBPF maps

Kernel-resident state for rules, counters, rate limiting, and connection tracking/NAT-related metadata.

Go control plane

Loads the eBPF object, attaches it to an interface, and manages maps/rules from userspace.

Routing/NAT integration

Linux IP forwarding plus nftables masquerade are configured by the setup guide for routed lab topologies.

Prerequisites

* Linux with eBPF/XDP support (modern kernel recommended; many XDP features are best on 5.10+).

* Root privileges to load/attach eBPF programs and configure networking.

* Go toolchain (per repository setup/build steps).

* Clang/LLVM and kernel headers/BTF support if rebuilding the eBPF object locally.

* nftables installed for NAT/masquerade configuration.

### Quick start

Clone the repository, follow the setup guide, then build and run the control plane. Replace interface names with those that exist on your machine.

Important

The exact binary name, flags, and build targets should be taken from the repository’s source tree and setup guide. The setup guide is the authoritative source for interface names, namespaces, and NAT configuration used by the project.

### What the setup guide configures

Based on the public setup document, the lab topology uses Linux networking primitives (veth pairs/network namespaces), enables IPv4 forwarding, and adds an nftables masquerade rule for outbound NAT.

Use the repository’s exact commands and interface names rather than copying the illustrative snippet blindly.

### Verifying the XDP program is attached

If the program is attached successfully, `ip -details link show` will report an XDP program on the interface.

### Testing in a namespace lab

The setup guide appears to use network namespaces and veth pairs for reproducible testing. A typical flow is:

1. Create two namespaces and a veth pair between them.

2. Assign IP addresses and bring links up.

3. Enable forwarding on the host if traffic is routed through it.

4. Attach the XDP program to the host-facing interface.

5. Use `ping`, `curl`, or `iperf3` between namespaces to validate allow/drop and rate-limit behavior.

Prefer the exact namespace names, addresses, and interface names from `setup.md`.

### Common operations

| Task                 | Command (typical)                               |
| -------------------- | ----------------------------------------------- |
| Attach XDP program   | ip link set dev eth0 xdp obj firewall.o sec xdp |
| Detach XDP program   | ip link set dev eth0 xdp off                    |
| Inspect maps         | bpftool map dump id <MAP_ID>                    |
| Check nftables rules | sudo nft list ruleset                           |
| Check forwarding     | sysctl net.ipv4.ip_forward                      |

These commands are generic Linux/XDP operations

The repository may wrap some of them in Go tooling or scripts. Use the repo-provided entrypoints when available.

### Troubleshooting

| Symptom                              | Likely cause                                                 | What to check                                                 |
| ------------------------------------ | ------------------------------------------------------------ | ------------------------------------------------------------- |
| XDP attach fails                     | Driver/kernel/BTF mismatch or insufficient privileges        | dmesg | tail -100, bpftool feature probe, run as root         |
| No traffic between namespaces        | Links down, IPs/routes missing, forwarding disabled          | ip -n <ns> addr, ip -n <ns> route, sysctl net.ipv4.ip_forward |
| NAT not working                      | nftables chain/table missing or wrong egress interface       | sudo nft list ruleset                                         |
| Packets unexpectedly dropped         | Rule map contents or rate limiter state                      | Dump eBPF maps with bpftool map dump                          |
| Program loads but counters stay zero | Attached to the wrong interface or traffic bypasses XDP path | ip -details link show dev <if>                                |

Security & operational notes

* XDP programs run in kernel context; mistakes can disrupt networking on the attached interface.

* Test in namespaces or a VM before attaching to a production NIC.

* Keep a recovery shell/console open so you can detach XDP if you lock yourself out.

* Version-control your nftables rules and sysctl changes; the setup guide modifies forwarding/NAT state.

### Repository structure (expected)

The exact layout should be confirmed from the repository tree, but an eBPF firewall project commonly contains:

### Suggested README additions from the maintainer

1. Document the exact binary name and flags used to attach the program.

2. Add a one-command demo target (for example `make demo`) that creates namespaces, attaches XDP, and runs a connectivity test.

3. Include a kernel feature matrix (native XDP vs generic XDP, required helpers, BTF expectations).

4. Document the eBPF map schema (rule keys/values, counter maps, rate-limit maps).

5. Add a safe rollback section with the exact detach and nftables cleanup commands.

Source used: the repository’s public setup guide and repository page. The statements about IP forwarding and nftables masquerade are derived from that setup document.
