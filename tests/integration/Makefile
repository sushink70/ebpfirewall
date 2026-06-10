# SPDX-License-Identifier: GPL-2.0
#
# kernel/Makefile — compile the XDP BPF program to a BPF ELF object.
#
# Prerequisites (Ubuntu 24.04):
#   sudo apt install clang llvm libbpf-dev linux-headers-$(uname -r)
#
# Usage:
#   make              # build xdp_firewall.o
#   make clean        # remove build artefacts
#   make verify       # dry-load with bpftool to catch verifier errors

CLANG   ?= clang
BPFTOOL ?= bpftool

# Detect host architecture and map to BPF target arch name.
# The BPF target is always "bpf" regardless of host arch, but we need
# __TARGET_ARCH_<arch> defined so bpf_tracing.h works correctly.
HOST_ARCH := $(shell uname -m)
ifeq ($(HOST_ARCH), x86_64)
    BPF_ARCH := x86
else ifeq ($(HOST_ARCH), aarch64)
    BPF_ARCH := arm64
else
    BPF_ARCH := $(HOST_ARCH)
endif

# Kernel headers: prefer the running kernel's headers.
KHEADERS ?= /usr/include

BPF_CFLAGS := \
    -O2 -g \
    -Wall -Wextra \
    -Wno-unused-value \
    -Wno-pointer-sign \
    -Wno-compare-distinct-pointer-types \
    -target bpf \
    -D__TARGET_ARCH_$(BPF_ARCH) \
    -I$(KHEADERS) \
    -I$(KHEADERS)/$(shell dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo x86_64-linux-gnu) \
    -I.

TARGET := xdp_firewall.o
SRCS   := xdp_firewall.c
HDRS   := xdp_firewall.h maps.h

.PHONY: all clean verify

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CLANG) $(BPF_CFLAGS) -c -o $@ $<
	@echo "[BPF] built $@"

clean:
	rm -f $(TARGET)

# Dry-load the program against the BPF verifier without attaching it.
# Requires root (or CAP_BPF).
verify: $(TARGET)
	@echo "[verify] running BPF verifier via bpftool..."
	$(BPFTOOL) prog load $(TARGET) /sys/fs/bpf/_ebpfirewall_verify type xdp
	$(BPFTOOL) map show pinned /sys/fs/bpf/_ebpfirewall_verify 2>/dev/null || true
	$(BPFTOOL) prog show pinned /sys/fs/bpf/_ebpfirewall_verify
	rm -f /sys/fs/bpf/_ebpfirewall_verify
	@echo "[verify] OK — verifier accepted the program"
