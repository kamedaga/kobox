#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build}"
runner="${KOBOX_KVM_REAL_OPS_RUNNER:-$build_dir/kobox-kvm-real-ops}"
work_dir="$repo_root/.artifacts/kvm-linux-rootfs"
disk="$work_dir/rootfs.ext4.img"
init_src="$work_dir/kobox-init.c"
init_bin="$work_dir/kobox-init"
log="$work_dir/serial.log"
decoded_log="$work_dir/serial.decoded.log"
debugfs_cmds="$work_dir/rootfs.debugfs"

mkdir -p "$work_dir"

if [ ! -x "$runner" ]; then
    cmake -S "$repo_root" -B "$build_dir" -DCMAKE_C_COMPILER="${CC:-clang}"
    cmake --build "$build_dir" -j2 --target kobox-kvm-real-ops
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
    echo "skip: kvm.ko not found; set KOBOX_KVM_COMMON_KO=/path/to/kvm.ko" >&2
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
    echo "skip: KVM arch module not found; set KOBOX_KVM_ARCH_KO=/path/to/kvm-amd.ko" >&2
    exit 77
fi

bzimage="${KOBOX_KVM_BZIMAGE:-}"
if [ -z "$bzimage" ] && [ -f "$repo_root/.artifacts/qemu-vfio-virtio-blk-ext4/kernel-root/boot/vmlinuz-6.8.0-117-generic" ]; then
    bzimage="$repo_root/.artifacts/qemu-vfio-virtio-blk-ext4/kernel-root/boot/vmlinuz-6.8.0-117-generic"
fi
if [ -z "$bzimage" ]; then
    kernel_release="$(uname -r 2>/dev/null || true)"
    if [ -n "$kernel_release" ] && [ -f "/boot/vmlinuz-$kernel_release" ]; then
        bzimage="/boot/vmlinuz-$kernel_release"
    elif [ -f /boot/vmlinuz ]; then
        bzimage="/boot/vmlinuz"
    fi
fi

if [ -z "$bzimage" ] || [ ! -f "$bzimage" ]; then
    echo "skip: bzImage not found; set KOBOX_KVM_BZIMAGE=/path/to/vmlinuz" >&2
    exit 77
fi

if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
    echo "skip: /dev/kvm is not accessible" >&2
    exit 77
fi

if ! command -v mkfs.ext4 >/dev/null 2>&1 || ! command -v debugfs >/dev/null 2>&1; then
    echo "skip: mkfs.ext4/debugfs not found" >&2
    exit 77
fi

if ! command -v gcc >/dev/null 2>&1; then
    echo "skip: gcc not found" >&2
    exit 77
fi

cat >"$init_src" <<'INIT_C'
static long kobox_syscall3(long n, long a, long b, long c)
{
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(n), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return ret;
}

void _start(void)
{
    static const char message[] = "KOBOX_INIT_REACHED\n";
    (void)kobox_syscall3(1, 1, (long)message, (long)(sizeof(message) - 1));
    (void)kobox_syscall3(1, 2, (long)message, (long)(sizeof(message) - 1));
    (void)kobox_syscall3(60, 42, 0, 0);
    for (;;) { }
}
INIT_C

gcc -static -nostdlib -Os -s -o "$init_bin" "$init_src"

rm -f "$disk" "$log"
truncate -s 32M "$disk"
mkfs.ext4 -q -F -O '^has_journal,^extent,^64bit,^metadata_csum,^extra_isize,^dir_index' "$disk"

debugfs -w -R "mkdir /sbin" "$disk" >/dev/null 2>&1 || true
debugfs -w -R "mkdir /dev" "$disk" >/dev/null 2>&1 || true
debugfs -w -R "write $init_bin /sbin/init" "$disk" >/dev/null 2>&1
debugfs -w -R "sif /sbin/init mode 0100755" "$disk" >/dev/null 2>&1
cat >"$debugfs_cmds" <<'DEBUGFS_CMDS'
cd /dev
mknod console c 5 1
sif console mode 020600
mknod null c 1 3
sif null mode 020666
DEBUGFS_CMDS
debugfs -w -f "$debugfs_cmds" "$disk" >/dev/null 2>&1 || true

cmdline="${KOBOX_KVM_CMDLINE:-console=ttyS0 earlycon root=/dev/vda rw rootwait panic=-1 virtio_mmio.device=4K@0x10001000:5}"

set +e
KOBOX_KVM_RUN_BACKEND=linux-kvm \
KOBOX_KVM_LINUX_ENTRY_BOOT_LOOP=1 \
KOBOX_KVM_LINUX_ENTRY_BOOT_STEPS="${KOBOX_KVM_LINUX_ENTRY_BOOT_STEPS:-420000}" \
KOBOX_KVM_LINUX_ENTRY_SERIAL_UNTIL="${KOBOX_KVM_LINUX_ENTRY_SERIAL_UNTIL:-exitcode=0x00002a00}" \
"$runner" \
    "--dep=$common_ko" \
    "--bzimage=$bzimage" \
    "--cmdline=$cmdline" \
    "--block-image=$disk" \
    "$target_ko" >"$log" 2>&1
status=$?
set -e

cat "$log"

if [ "$status" -ne 0 ]; then
    exit "$status"
fi

sed -n 's/.*port=0x3f8.*data0=0x\([0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' "$log" |
while IFS= read -r hex; do
    dec=$((0x$hex))
    oct=$(printf '%03o' "$dec")
    printf "\\$oct"
done >"$decoded_log"

grep -q "virtio_blk virtio0: \\[vda\\]" "$decoded_log"
grep -q "VFS: Mounted root (ext4 filesystem) on device 253:0" "$decoded_log"
grep -q "Run /sbin/init as init process" "$decoded_log"
grep -q "exitcode=0x00002a00" "$log"
