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
firewall.o: vmlinux.h xdp_firewall.c xdp_firewall.h
	clang -O2 -g \
		-target bpf \
		-D__TARGET_ARCH_x86 \
		-I/usr/include/x86_64-linux-gnu \
		-I. \
		-Wall -Wno-unused-value \
		-c firewall.c \
		-o firewall.o

clean:
	rm -f xdp_firewall.o vmlinux.h

.PHONY: clean