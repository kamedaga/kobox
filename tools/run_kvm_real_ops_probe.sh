#!/usr/bin/env sh
set -eu

runner="${KOBOX_KVM_REAL_OPS_RUNNER:-.artifacts/build/kobox-kvm-real-ops}"

if [ ! -x "$runner" ]; then
    cmake -S . -B .artifacts/build -DCMAKE_C_COMPILER="${CC:-clang}"
    cmake --build .artifacts/build -j2 --target kobox-kvm-real-ops
fi

common_ko="${KOBOX_KVM_COMMON_KO:-${KOBOX_KVM_KO:-}}"
target_ko="${KOBOX_KVM_ARCH_KO:-}"

if [ -z "$common_ko" ]; then
    kernel_release="$(uname -r 2>/dev/null || true)"
    if [ -n "$kernel_release" ]; then
        common_ko="/lib/modules/$kernel_release/kernel/arch/x86/kvm/kvm.ko"
    fi
fi

if [ -z "$common_ko" ] || [ ! -f "$common_ko" ]; then
    echo "skip: kvm.ko not found; set KOBOX_KVM_COMMON_KO=/path/to/kvm.ko"
    exit 77
fi

if [ -z "$target_ko" ]; then
    kernel_release="$(uname -r 2>/dev/null || true)"
    module_dir="/lib/modules/$kernel_release/kernel/arch/x86/kvm"
    flags="$(grep -m1 '^flags' /proc/cpuinfo 2>/dev/null || true)"
    if printf '%s\n' "$flags" | grep -qw svm && [ -f "$module_dir/kvm-amd.ko" ]; then
        target_ko="$module_dir/kvm-amd.ko"
    elif printf '%s\n' "$flags" | grep -qw vmx && [ -f "$module_dir/kvm-intel.ko" ]; then
        target_ko="$module_dir/kvm-intel.ko"
    elif [ -f "$module_dir/kvm-amd.ko" ]; then
        target_ko="$module_dir/kvm-amd.ko"
    elif [ -f "$module_dir/kvm-intel.ko" ]; then
        target_ko="$module_dir/kvm-intel.ko"
    fi
fi

if [ -z "$target_ko" ] || [ ! -f "$target_ko" ]; then
    echo "skip: KVM arch module not found; set KOBOX_KVM_ARCH_KO=/path/to/kvm-amd.ko"
    exit 77
fi

bzimage="${KOBOX_KVM_BZIMAGE:-}"
if [ -z "$bzimage" ]; then
    kernel_release="$(uname -r 2>/dev/null || true)"
    if [ -n "$kernel_release" ] && [ -f "/boot/vmlinuz-$kernel_release" ]; then
        bzimage="/boot/vmlinuz-$kernel_release"
    elif [ -f /boot/vmlinuz ]; then
        bzimage="/boot/vmlinuz"
    fi
fi

run_probe() {
    backend_prefix="$1"
    shift
    set -- "--dep=$common_ko" "$@"
    if [ -n "${KOBOX_KVM_INITRD:-}" ]; then
        set -- "$@" "--initrd=$KOBOX_KVM_INITRD"
    fi
    if [ -n "${KOBOX_KVM_CMDLINE:-}" ]; then
        set -- "$@" "--cmdline=$KOBOX_KVM_CMDLINE"
    fi
    if [ -n "${KOBOX_KVM_BLOCK_IMAGE:-}" ]; then
        set -- "$@" "--block-image=$KOBOX_KVM_BLOCK_IMAGE"
    fi
    set -- "$@" "$target_ko"
    if [ "$backend_prefix" = "linux-kvm" ]; then
        KOBOX_KVM_RUN_BACKEND=linux-kvm "$runner" "$@"
    else
        "$runner" "$@"
    fi
}

if [ -n "$bzimage" ] && [ -f "$bzimage" ]; then
    echo "kvm-bzimage: using $bzimage"
    run_probe kobox "--bzimage=$bzimage"
else
    echo "skip: bzImage not found; set KOBOX_KVM_BZIMAGE=/path/to/vmlinuz for Linux boot placement proof"
    run_probe kobox
fi

if [ "${KOBOX_KVM_REAL_OPS_SKIP_HOST_PROVIDER:-0}" != "1" ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        if [ -n "$bzimage" ] && [ -f "$bzimage" ]; then
            run_probe linux-kvm "--bzimage=$bzimage"
        else
            run_probe linux-kvm
        fi
    else
        echo "skip: /dev/kvm is not accessible; linux-kvm provider smoke not run"
    fi
fi
