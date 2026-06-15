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
guest_proof_dump="$work_dir/guest-proof.txt"

mkdir -p "$work_dir"

if [ ! -x "$runner" ]; then
    cmake -S "$repo_root" -B "$build_dir" -DCMAKE_C_COMPILER="${CC:-clang}"
    cmake --build "$build_dir" -j2 --target kobox-kvm-real-ops
fi

common_ko="${KOBOX_KVM_COMMON_KO:-${KOBOX_KVM_KO:-}}"
target_ko="${KOBOX_KVM_ARCH_KO:-}"
block_backend="${KOBOX_KVM_BLOCK_BACKEND:-image}"

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

static long kobox_syscall4(long n, long a, long b, long c, long d)
{
    long ret;
    register long r10 __asm__("r10") = d;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
        : "rcx", "r11", "memory");
    return ret;
}

static unsigned long kobox_strlen(const char *text)
{
    unsigned long len = 0;
    while (text[len] != '\0') {
        len++;
    }
    return len;
}

static long kobox_write_all(long fd, const char *data, unsigned long len)
{
    unsigned long done = 0;
    while (done < len) {
        long n = kobox_syscall3(1, fd, (long)(data + done), (long)(len - done));
        if (n <= 0) {
            return n;
        }
        done += (unsigned long)n;
    }
    return (long)done;
}

static void kobox_console(const char *message)
{
    long fd = kobox_syscall4(257, -100, (long)"/dev/console", 1, 0);
    if (fd >= 0) {
        (void)kobox_write_all(fd, message, kobox_strlen(message));
        (void)kobox_syscall3(3, fd, 0, 0);
    }
}

static int kobox_bytes_equal(const char *a, const char *b, unsigned long len)
{
    for (unsigned long i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

void _start(void)
{
    static const char path[] = "/kobox-init-proof.txt";
    static const char payload[] = "KOBOX_GUEST_BLOCK_WRITE_READ_OK\n";
    char readback[sizeof(payload)];

    kobox_console("KOBOX_INIT_REACHED\n");

    long fd = kobox_syscall4(257, -100, (long)path, 2 | 0100 | 01000, 0644);
    if (fd < 0) {
        kobox_console("KOBOX_GUEST_OPEN_FAILED\n");
        (void)kobox_syscall3(60, 50, 0, 0);
    }

    if (kobox_write_all(fd, payload, sizeof(payload) - 1) != (long)(sizeof(payload) - 1)) {
        kobox_console("KOBOX_GUEST_WRITE_FAILED\n");
        (void)kobox_syscall3(60, 51, 0, 0);
    }
    kobox_console("KOBOX_GUEST_WRITE_OK\n");

    if (kobox_syscall3(74, fd, 0, 0) < 0) {
        kobox_console("KOBOX_GUEST_FSYNC_FAILED\n");
        (void)kobox_syscall3(60, 52, 0, 0);
    }
    (void)kobox_syscall3(3, fd, 0, 0);

    fd = kobox_syscall4(257, -100, (long)path, 0, 0);
    if (fd < 0) {
        kobox_console("KOBOX_GUEST_REOPEN_FAILED\n");
        (void)kobox_syscall3(60, 53, 0, 0);
    }

    long n = kobox_syscall3(0, fd, (long)readback, (long)(sizeof(payload) - 1));
    (void)kobox_syscall3(3, fd, 0, 0);
    if (n != (long)(sizeof(payload) - 1)) {
        kobox_console("KOBOX_GUEST_READ_FAILED\n");
        (void)kobox_syscall3(60, 54, 0, 0);
    }
    if (!kobox_bytes_equal(readback, payload, sizeof(payload) - 1)) {
        kobox_console("KOBOX_GUEST_VERIFY_FAILED\n");
        (void)kobox_syscall3(60, 55, 0, 0);
    }

    kobox_console("KOBOX_GUEST_READ_OK\n");
    kobox_console("KOBOX_GUEST_FS_OK\n");
    (void)kobox_syscall3(60, 43, 0, 0);
    for (;;) { }
}
INIT_C

gcc -static -nostdlib -fno-builtin -Os -s -o "$init_bin" "$init_src"

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

extra_dep_args=
block_arg="--block-image=$disk"
if [ "$block_backend" = "ahci" ] || [ "$block_backend" = "kobox-ata-ahci" ]; then
    kernel_version="${KERNEL_VERSION:-6.8.0-117-generic}"
    modules_root="${KOBOX_6_8_MODULES_ROOT:-$repo_root/.artifacts/qemu-vfio-nvme-ext4/modules-root/lib/modules/$kernel_version}"
    zstd_bin="${ZSTD_BIN:-$repo_root/.artifacts/qemu-vfio-nvme-ext4/zstd-root/usr/bin/zstd}"
    libahci_src="$modules_root/kernel/drivers/ata/libahci.ko.zst"
    ahci_src="$modules_root/kernel/drivers/ata/ahci.ko.zst"
    if [ ! -x "$zstd_bin" ]; then
        echo "skip: zstd not found for AHCI backend at $zstd_bin" >&2
        exit 77
    fi
    if [ ! -f "$libahci_src" ] || [ ! -f "$ahci_src" ]; then
        echo "skip: AHCI modules not found under $modules_root" >&2
        exit 77
    fi
    "$zstd_bin" -q -d -f "$libahci_src" -o "$work_dir/libahci.ko"
    "$zstd_bin" -q -d -f "$ahci_src" -o "$work_dir/ahci.ko"
    extra_dep_args="--dep=$work_dir/libahci.ko --dep=$work_dir/ahci.ko"
    block_arg="--kobox-ata-image=$disk"
fi

set +e
KOBOX_KVM_RUN_BACKEND=linux-kvm \
KOBOX_KVM_LINUX_ENTRY_BOOT_LOOP=1 \
KOBOX_KVM_LINUX_ENTRY_BOOT_STEPS="${KOBOX_KVM_LINUX_ENTRY_BOOT_STEPS:-420000}" \
KOBOX_KVM_LINUX_ENTRY_SERIAL_UNTIL="${KOBOX_KVM_LINUX_ENTRY_SERIAL_UNTIL:-exitcode=0x00002b00}" \
KOBOX_KVM_TRACE_SERIAL_BYTES="${KOBOX_KVM_TRACE_SERIAL_BYTES:-1}" \
KOBOX_MOCK_PCI_ID="${KOBOX_MOCK_PCI_ID:-8086:2922:01:06:01}" \
KOBOX_TRACE_ATA="${KOBOX_TRACE_ATA:-0}" \
KOBOX_TRACE_IRQ="${KOBOX_TRACE_IRQ:-0}" \
"$runner" \
    "--dep=$common_ko" \
    $extra_dep_args \
    "--bzimage=$bzimage" \
    "--cmdline=$cmdline" \
    "$block_arg" \
    "$target_ko" >"$log" 2>&1
status=$?
set -e

if [ "$status" -ne 0 ] || [ "${KOBOX_KVM_PRINT_FULL_LOG:-0}" != "0" ]; then
    cat "$log"
fi
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
grep -q "VFS: Mounted root (ext4 filesystem) on devic.*253:0" "$decoded_log"
grep -q "Run /sbin/init as init process" "$decoded_log"
grep -q "kvm-virtio-blk: request .* type=1 " "$log"
if [ "$block_backend" = "ahci" ] || [ "$block_backend" = "kobox-ata-ahci" ]; then
    grep -q "kvm-block-route: backend=kobox-ata-ahci" "$log"
    grep -q "kvm-block-route-summary: backend=kobox-ata-ahci .* ahci_block_reads=.* ahci_block_writes=.* completions=.* irq_dispatches=.* errors=0 result=0" "$log"
fi
grep -q "exitcode=0x00002b00" "$log"

rm -f "$guest_proof_dump"
debugfs -R "dump /kobox-init-proof.txt $guest_proof_dump" "$disk" >/dev/null 2>&1
grep -q "KOBOX_GUEST_BLOCK_WRITE_READ_OK" "$guest_proof_dump"

grep -E "kvm-block-route: backend=|kvm-linux-boot-loop-serial-match|kvm-block-route: writeback|kvm-block-route-summary" "$log"
grep -E "virtio_blk virtio0|VFS: Mounted root|Run /sbin/init" "$decoded_log"
echo "kobox-kvm-linux-rootfs-smoke: backend=$block_backend result=0"
