// build.rs — compile the BPF C program as part of `cargo build`.
//
// This runs the kernel/ Makefile (which calls clang) so the generated
// xdp_firewall.o is always in sync with the source headers.
//
// If clang / llvm / libbpf-dev are not installed, the build fails with a
// clear message rather than a cryptic link error at runtime.

use std::path::PathBuf;
use std::process::Command;

fn main() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let kernel_dir = manifest
        .parent()
        .expect("ctrl/ must be inside the project root")
        .join("kernel");

    // Tell cargo to re-run this script when any kernel source changes.
    for name in &["xdp_firewall.c", "xdp_firewall.h", "maps.h", "Makefile"] {
        println!(
            "cargo:rerun-if-changed={}",
            kernel_dir.join(name).display()
        );
    }

    // Invoke the kernel Makefile.
    let status = Command::new("make")
        .arg("-C")
        .arg(&kernel_dir)
        .status()
        .unwrap_or_else(|e| {
            panic!(
                "Failed to run `make` in {}.\n\
                 Install clang, llvm, libbpf-dev, and linux-headers-$(uname -r).\n\
                 Error: {}",
                kernel_dir.display(),
                e
            )
        });

    if !status.success() {
        panic!(
            "Kernel BPF build failed (exit {:?}).\n\
             Run `make -C {}` manually to see the full error.",
            status.code(),
            kernel_dir.display()
        );
    }

    // Export the object path so main.rs can embed it as a compile-time
    // string instead of a runtime config value.
    println!(
        "cargo:rustc-env=BPF_OBJECT_PATH={}",
        kernel_dir.join("xdp_firewall.o").display()
    );
}
