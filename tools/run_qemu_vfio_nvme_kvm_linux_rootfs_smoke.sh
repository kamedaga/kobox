#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build}"
work_dir="$repo_root/.artifacts/qemu-vfio-nvme-kvm-linux-rootfs"
debs_dir="$repo_root/.artifacts/debs"
root_dir="$work_dir/initramfs-root"
serial_log="$work_dir/serial.log"
decoded_log="$work_dir/serial.decoded.log"
qemu_stderr="$work_dir/qemu.stderr"
qemu_timeout="${KOBOX_QEMU_TIMEOUT:-360}"
kernel_pkg="${KERNEL_IMAGE_PACKAGE:-linux-image-6.8.0-117-generic}"
kernel_version="${KERNEL_VERSION:-6.8.0-117-generic}"
modules_pkg="${KERNEL_MODULES_PACKAGE:-linux-modules-6.8.0-117-generic}"
kvm_module_dir="${KOBOX_KVM_MODULE_DIR:-/lib/modules/$(uname -r)/kernel/arch/x86/kvm}"

mkdir -p "$debs_dir" "$work_dir"

fetch_deb() {
    pkg="$1"
    if ! ls "$debs_dir"/"$pkg"_*.deb >/dev/null 2>&1; then
        (cd "$debs_dir" && apt download "$pkg")
    fi
}

extract_deb_once() {
    pkg="$1"
    stamp="$2"
    dest="$3"
    if [ ! -e "$stamp" ]; then
        rm -rf "$dest"
        mkdir -p "$dest"
        dpkg-deb -x "$debs_dir"/"$pkg"_*.deb "$dest"
        : >"$stamp"
    fi
}

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "skip qemu vfio nvme kvm linux rootfs smoke: qemu-system-x86_64 not found" >&2
    exit 77
fi
if ! command -v qemu-img >/dev/null 2>&1; then
    echo "skip qemu vfio nvme kvm linux rootfs smoke: qemu-img not found" >&2
    exit 77
fi
if [ ! -e /dev/kvm ]; then
    echo "skip qemu vfio nvme kvm linux rootfs smoke: /dev/kvm not available" >&2
    exit 77
fi
if ! command -v mkfs.ext4 >/dev/null 2>&1 || ! command -v debugfs >/dev/null 2>&1; then
    echo "skip qemu vfio nvme kvm linux rootfs smoke: mkfs.ext4/debugfs not found" >&2
    exit 77
fi
if ! command -v gcc >/dev/null 2>&1; then
    echo "skip qemu vfio nvme kvm linux rootfs smoke: gcc not found" >&2
    exit 77
fi

if [ ! -x "$build_dir/kobox-kvm-real-ops" ]; then
    cmake -S "$repo_root" -B "$build_dir" -DCMAKE_C_COMPILER="${CC:-clang}"
    cmake --build "$build_dir" -j2 --target kobox-kvm-real-ops
fi

fetch_deb "$kernel_pkg"
fetch_deb "$modules_pkg"
fetch_deb busybox-static
fetch_deb cpio
fetch_deb zstd

extract_deb_once "$kernel_pkg" "$work_dir/.kernel-extracted" "$work_dir/kernel-root"
extract_deb_once "$modules_pkg" "$work_dir/.modules-extracted" "$work_dir/modules-root"
extract_deb_once busybox-static "$work_dir/.busybox-extracted" "$work_dir/busybox-root"
extract_deb_once cpio "$work_dir/.cpio-extracted" "$work_dir/cpio-root"
extract_deb_once zstd "$work_dir/.zstd-extracted" "$work_dir/zstd-root"

kernel="$work_dir/kernel-root/boot/vmlinuz-$kernel_version"
cpio_bin="$work_dir/cpio-root/usr/bin/cpio"
zstd_bin="$work_dir/zstd-root/usr/bin/zstd"
if [ ! -f "$kernel" ]; then
    echo "missing $kernel" >&2
    exit 1
fi

disk="$work_dir/nvme-rootfs.ext4.img"
init_src="$work_dir/kobox-init.c"
init_bin="$work_dir/kobox-init"
debugfs_cmds="$work_dir/rootfs.debugfs"
guest_proof_dump="$work_dir/guest-proof.txt"

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

rm -f "$disk"
qemu-img create -f raw "$disk" 64M >/dev/null
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

rm -rf "$root_dir"
mkdir -p "$root_dir"/bin "$root_dir"/dev "$root_dir"/lib/modules/kobox-guest "$root_dir"/lib/modules/vfio-host "$root_dir"/lib64 "$root_dir"/proc "$root_dir"/sys "$root_dir"/tmp "$root_dir"/usr/bin "$root_dir"/usr/lib/kobox

cp "$work_dir/busybox-root/usr/bin/busybox" "$root_dir/bin/busybox"
ln -s busybox "$root_dir/bin/sh"
ln -s busybox "$root_dir/bin/mount"
ln -s busybox "$root_dir/bin/insmod"
ln -s busybox "$root_dir/bin/poweroff"
ln -s busybox "$root_dir/bin/dmesg"
cp "$build_dir/kobox-kvm-real-ops" "$root_dir/usr/bin/kobox-kvm-real-ops"
cp "$kernel" "$root_dir/usr/lib/kobox/vmlinuz"

for tool in "$build_dir/kobox-kvm-real-ops"; do
    ldd "$tool" | awk '
        $1 ~ /^\// { print $1 }
        $3 ~ /^\// { print $3 }
    '
done | sort -u | while IFS= read -r lib; do
    if [ -n "$lib" ]; then
        dest="$root_dir$lib"
        mkdir -p "$(dirname "$dest")"
        cp "$lib" "$dest"
    fi
done

copy_kobox_module() {
    name="$1"
    src="$kvm_module_dir/$name"
    dst="$root_dir/usr/lib/kobox/$name"
    if [ -f "$src" ]; then
        cp "$src" "$dst"
        return
    fi
    if [ -f "$src.zst" ]; then
        "$zstd_bin" -q -d -f "$src.zst" -o "$dst"
        return
    fi
    echo "missing kobox module: $name in $kvm_module_dir" >&2
    exit 77
}

copy_guest_module() {
    rel="$1"
    dst_dir="$2"
    name="$(basename "$rel")"
    src="$work_dir/modules-root/lib/modules/$kernel_version/$rel"
    dst="$root_dir/$dst_dir/$name"
    mkdir -p "$(dirname "$dst")"
    if [ -f "$src" ]; then
        cp "$src" "$dst"
        return
    fi
    if [ -f "$src.zst" ]; then
        "$zstd_bin" -q -d -f "$src.zst" -o "$dst"
        return
    fi
    echo "missing guest module: $rel" >&2
    exit 1
}

copy_guest_module kernel/drivers/crypto/ccp/ccp.ko lib/modules/kobox-guest
copy_guest_module kernel/virt/lib/irqbypass.ko lib/modules/kobox-guest
copy_guest_module kernel/arch/x86/kvm/kvm.ko lib/modules/kobox-guest
copy_guest_module kernel/arch/x86/kvm/kvm-amd.ko lib/modules/kobox-guest
copy_guest_module kernel/arch/x86/kvm/kvm-intel.ko lib/modules/kobox-guest

copy_guest_module kernel/drivers/vfio/vfio.ko lib/modules/vfio-host
copy_guest_module kernel/drivers/vfio/vfio_iommu_type1.ko lib/modules/vfio-host
copy_guest_module kernel/drivers/vfio/pci/vfio-pci-core.ko lib/modules/vfio-host
copy_guest_module kernel/drivers/vfio/pci/vfio-pci.ko lib/modules/vfio-host
copy_guest_module kernel/drivers/iommu/iommufd/iommufd.ko lib/modules/vfio-host

copy_kobox_module kvm.ko
copy_kobox_module kvm-amd.ko
copy_kobox_module kvm-intel.ko

cat >"$root_dir/init" <<'INIT'
#!/bin/sh
set -eu

export PATH=/bin:/usr/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev || true
mkdir -p /dev/vfio
[ -e /dev/console ] || mknod /dev/console c 5 1
[ -e /dev/null ] || mknod /dev/null c 1 3
[ -e /dev/vfio/vfio ] || mknod /dev/vfio/vfio c 10 196

echo "kobox-qemu-vfio-nvme-kvm-linux-rootfs: booted"
echo "kobox-qemu-vfio-nvme-kvm-linux-rootfs: flags=$(grep -m1 '^flags' /proc/cpuinfo | grep -Eo 'vmx|svm' | sort -u | tr '\n' ' ')"

guest_arch=/lib/modules/kobox-guest/kvm-amd.ko
kobox_arch=/usr/lib/kobox/kvm-amd.ko
flags=$(grep -m1 '^flags' /proc/cpuinfo || true)
if printf '%s\n' "$flags" | grep -qw vmx; then
    guest_arch=/lib/modules/kobox-guest/kvm-intel.ko
    kobox_arch=/usr/lib/kobox/kvm-intel.ko
elif printf '%s\n' "$flags" | grep -qw svm; then
    guest_arch=/lib/modules/kobox-guest/kvm-amd.ko
    kobox_arch=/usr/lib/kobox/kvm-amd.ko
else
    echo "kobox-qemu-vfio-nvme-kvm-linux-rootfs: no nested vmx/svm"
    echo "kobox-qemu-vfio-nvme-kvm-linux-rootfs: status=77"
    poweroff -f
fi

insmod /lib/modules/kobox-guest/ccp.ko 2>/tmp/ccp.err || true
insmod /lib/modules/kobox-guest/irqbypass.ko 2>/tmp/irqbypass.err || true
insmod /lib/modules/kobox-guest/kvm.ko 2>/tmp/kvm-common.err || true
insmod "$guest_arch" 2>/tmp/kvm-arch.err || true
if [ ! -e /dev/kvm ]; then
    echo "kobox-qemu-vfio-nvme-kvm-linux-rootfs: /dev/kvm not present"
    cat /tmp/ccp.err /tmp/irqbypass.err /tmp/kvm-common.err /tmp/kvm-arch.err 2>/dev/null || true
    dmesg | tail -n 30 || true
    echo "kobox-qemu-vfio-nvme-kvm-linux-rootfs: status=77"
    poweroff -f
fi

insmod /lib/modules/vfio-host/irqbypass.ko 2>/tmp/vfio-irqbypass.err || true
insmod /lib/modules/vfio-host/iommufd.ko
insmod /lib/modules/vfio-host/vfio.ko
insmod /lib/modules/vfio-host/vfio_iommu_type1.ko
insmod /lib/modules/vfio-host/vfio-pci-core.ko
insmod /lib/modules/vfio-host/vfio-pci.ko

nvme_bdf=""
for dev in /sys/bus/pci/devices/*; do
    [ -f "$dev/class" ] || continue
    class=$(cat "$dev/class")
    if [ "$class" = "0x010802" ]; then
        nvme_bdf=${dev##*/}
        break
    fi
done

if [ -z "$nvme_bdf" ]; then
    echo "kobox-qemu-vfio-nvme-kvm-linux-rootfs: nvme device not found"
    echo "kobox-qemu-vfio-nvme-kvm-linux-rootfs: status=77"
    poweroff -f
fi

vendor=$(cat "/sys/bus/pci/devices/$nvme_bdf/vendor")
device=$(cat "/sys/bus/pci/devices/$nvme_bdf/device")
vendor=${vendor#0x}
device=${device#0x}
echo "kobox-qemu-vfio-nvme-kvm-linux-rootfs: nvme=$nvme_bdf id=$vendor:$device"
echo "$vendor $device" >/sys/bus/pci/drivers/vfio-pci/new_id || true
if [ -e "/sys/bus/pci/devices/$nvme_bdf/driver/unbind" ]; then
    echo "$nvme_bdf" >"/sys/bus/pci/devices/$nvme_bdf/driver/unbind" || true
fi
echo "$nvme_bdf" >/sys/bus/pci/drivers/vfio-pci/bind || true

group=$(basename "$(readlink "/sys/bus/pci/devices/$nvme_bdf/iommu_group")")
if [ ! -e "/dev/vfio/$group" ]; then
    devno=$(cat "/sys/class/vfio/$group/dev")
    major=${devno%:*}
    minor=${devno#*:}
    mknod "/dev/vfio/$group" c "$major" "$minor"
fi
echo "kobox-qemu-vfio-nvme-kvm-linux-rootfs: group=$group"

cmdline="${KOBOX_KVM_CMDLINE:-console=ttyS0 earlycon root=/dev/vda rw rootwait panic=-1 lpj=${KOBOX_KVM_LINUX_LPJ:-3700034} tsc=reliable clocksource=tsc virtio_mmio.device=4K@0x10001000:5}"
set +e
KOBOX_TRACE_KVM="${KOBOX_TRACE_KVM:-1}" \
KOBOX_KVM_TRACE_SERIAL_BYTES=0 \
KOBOX_KVM_TRACE_SERIAL_LINES=1 \
KOBOX_KVM_RUN_BACKEND=linux-kvm \
KOBOX_KVM_LINUX_ENTRY_BOOT_LOOP=1 \
KOBOX_KVM_LINUX_ENTRY_BOOT_STEPS="${KOBOX_KVM_LINUX_ENTRY_BOOT_STEPS:-420000}" \
KOBOX_KVM_LINUX_ENTRY_SERIAL_UNTIL="${KOBOX_KVM_LINUX_ENTRY_SERIAL_UNTIL:-exitcode=0x00002b00}" \
/usr/bin/kobox-kvm-real-ops \
    --dep=/usr/lib/kobox/kvm.ko \
    --bzimage=/usr/lib/kobox/vmlinuz \
    "--cmdline=$cmdline" \
    "--vfio-nvme=$nvme_bdf" \
    "$kobox_arch"
status=$?
set -e
echo "kobox-qemu-vfio-nvme-kvm-linux-rootfs: status=$status"
poweroff -f
INIT
chmod +x "$root_dir/init"

initramfs="$work_dir/initramfs.cpio"
(cd "$root_dir" && find . -print | "$cpio_bin" -o -H newc --quiet >"$initramfs")

rm -f "$serial_log" "$decoded_log" "$qemu_stderr"
qemu_cmd="qemu-system-x86_64"
if command -v timeout >/dev/null 2>&1; then
    qemu_cmd="timeout $qemu_timeout qemu-system-x86_64"
fi
set +e
$qemu_cmd \
    -enable-kvm \
    -machine q35,accel=kvm,kernel-irqchip=split \
    -cpu host \
    -m 1024M \
    -kernel "$kernel" \
    -initrd "$initramfs" \
    -append "console=ttyS0 panic=-1 quiet intel_iommu=on iommu=pt" \
    -device intel-iommu,intremap=on \
    -drive id=nvme0,if=none,format=raw,file="$disk" \
    -device nvme,serial=koboxnvme0,drive=nvme0 \
    -no-reboot \
    -display none \
    -serial "file:$serial_log" \
    2>"$qemu_stderr"
qemu_status=$?
set -e

cat "$serial_log"
if [ -s "$qemu_stderr" ]; then
    cat "$qemu_stderr" >&2
fi
if [ "$qemu_status" -ne 0 ]; then
    echo "qemu vfio nvme kvm linux rootfs smoke: qemu_status=$qemu_status" >&2
    exit "$qemu_status"
fi

if grep -q "kobox-qemu-vfio-nvme-kvm-linux-rootfs: status=77" "$serial_log"; then
    exit 77
fi

sed -n 's/.*port=0x3f8.*data0=0x\([0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' "$serial_log" |
while IFS= read -r hex; do
    dec=$((0x$hex))
    oct=$(printf '%03o' "$dec")
    printf "\\$oct"
done >"$decoded_log"

grep -q "kobox-qemu-vfio-nvme-kvm-linux-rootfs: status=0" "$serial_log"
grep -q "kvm-block-route: backend=vfio-nvme" "$serial_log"
grep -q "nvme vfio: capacity_sectors=" "$serial_log"
grep -q "kvm-virtio-blk: request .* type=1 " "$serial_log"
grep -q "virtio_blk virtio0: \\[vda\\]" "$serial_log"
grep -q "VFS: Mounted root (ext4 filesystem) on device 253:0" "$serial_log"
grep -q "Run /sbin/init as init process" "$serial_log"
grep -q "exitcode=0x00002b00" "$serial_log"

rm -f "$guest_proof_dump"
debugfs -R "dump /kobox-init-proof.txt $guest_proof_dump" "$disk" >/dev/null 2>&1
grep -q "KOBOX_GUEST_BLOCK_WRITE_READ_OK" "$guest_proof_dump"
