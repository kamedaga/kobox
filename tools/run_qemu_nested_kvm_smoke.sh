#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build}"
work_dir="$repo_root/.artifacts/qemu-nested-kvm"
debs_dir="$repo_root/.artifacts/debs"
root_dir="$work_dir/initramfs-root"
serial_log="$work_dir/serial.log"
qemu_stderr="$work_dir/qemu.stderr"
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
    echo "skip qemu nested kvm smoke: qemu-system-x86_64 not found" >&2
    exit 77
fi
if [ ! -e /dev/kvm ]; then
    echo "skip qemu nested kvm smoke: /dev/kvm not available" >&2
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

rm -rf "$root_dir"
mkdir -p "$root_dir"/bin "$root_dir"/dev "$root_dir"/lib/modules/kobox-guest "$root_dir"/lib64 "$root_dir"/proc "$root_dir"/sys "$root_dir"/tmp "$root_dir"/usr/bin "$root_dir"/usr/lib/kobox

cp "$work_dir/busybox-root/usr/bin/busybox" "$root_dir/bin/busybox"
ln -s busybox "$root_dir/bin/sh"
ln -s busybox "$root_dir/bin/mount"
ln -s busybox "$root_dir/bin/insmod"
ln -s busybox "$root_dir/bin/poweroff"
ln -s busybox "$root_dir/bin/dmesg"
cp "$build_dir/kobox-kvm-real-ops" "$root_dir/usr/bin/kobox-kvm-real-ops"

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

copy_module() {
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
    echo "missing module: $name in $kvm_module_dir" >&2
    exit 1
}

copy_guest_module() {
    rel="$1"
    name="$(basename "$rel")"
    src="$work_dir/modules-root/lib/modules/$kernel_version/$rel"
    dst="$root_dir/lib/modules/kobox-guest/$name"
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

copy_guest_module kernel/drivers/crypto/ccp/ccp.ko
copy_guest_module kernel/virt/lib/irqbypass.ko
copy_guest_module kernel/arch/x86/kvm/kvm.ko
copy_guest_module kernel/arch/x86/kvm/kvm-amd.ko
copy_guest_module kernel/arch/x86/kvm/kvm-intel.ko

copy_module kvm.ko
copy_module kvm-amd.ko
copy_module kvm-intel.ko

cat >"$root_dir/init" <<'INIT'
#!/bin/sh
set -eu

export PATH=/bin:/usr/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev || true
[ -e /dev/console ] || mknod /dev/console c 5 1
[ -e /dev/null ] || mknod /dev/null c 1 3

echo "kobox-qemu-nested-kvm: booted"
echo "kobox-qemu-nested-kvm: flags=$(grep -m1 '^flags' /proc/cpuinfo | grep -Eo 'vmx|svm' | sort -u | tr '\n' ' ')"

guest_arch=/lib/modules/kobox-guest/kvm-amd.ko
flags=$(grep -m1 '^flags' /proc/cpuinfo || true)
if printf '%s\n' "$flags" | grep -qw vmx; then
    guest_arch=/lib/modules/kobox-guest/kvm-intel.ko
elif printf '%s\n' "$flags" | grep -qw svm; then
    guest_arch=/lib/modules/kobox-guest/kvm-amd.ko
fi
echo "kobox-qemu-nested-kvm: guest-arch=$guest_arch"
insmod /lib/modules/kobox-guest/ccp.ko 2>/tmp/ccp.err || true
insmod /lib/modules/kobox-guest/irqbypass.ko 2>/tmp/irqbypass.err || true
insmod /lib/modules/kobox-guest/kvm.ko 2>/tmp/kvm-common.err || true
insmod "$guest_arch" 2>/tmp/kvm-arch.err || true

if [ -e /dev/kvm ]; then
    ls -l /dev/kvm
else
    echo "kobox-qemu-nested-kvm: /dev/kvm not present in guest"
    echo "kobox-qemu-nested-kvm: guest insmod stderr"
    cat /tmp/ccp.err /tmp/irqbypass.err /tmp/kvm-common.err /tmp/kvm-arch.err 2>/dev/null || true
    dmesg | tail -n 30 || true
fi

arch=/usr/lib/kobox/kvm-amd.ko
if printf '%s\n' "$flags" | grep -qw vmx; then
    arch=/usr/lib/kobox/kvm-intel.ko
elif printf '%s\n' "$flags" | grep -qw svm; then
    arch=/usr/lib/kobox/kvm-amd.ko
fi
echo "kobox-qemu-nested-kvm: arch=$arch"

set +e
KOBOX_TRACE_KVM="${KOBOX_TRACE_KVM:-1}" /usr/bin/kobox-kvm-real-ops --dep=/usr/lib/kobox/kvm.ko "$arch"
status=$?
set -e

echo "kobox-qemu-nested-kvm: status=$status"
poweroff -f
INIT
chmod +x "$root_dir/init"

initramfs="$work_dir/initramfs.cpio"
(cd "$root_dir" && find . -print | "$cpio_bin" -o -H newc --quiet >"$initramfs")

rm -f "$serial_log" "$qemu_stderr"
qemu-system-x86_64 \
    -enable-kvm \
    -machine q35,accel=kvm \
    -cpu host \
    -m 768M \
    -kernel "$kernel" \
    -initrd "$initramfs" \
    -append "console=ttyS0 panic=-1 quiet" \
    -no-reboot \
    -display none \
    -serial "file:$serial_log" \
    2>"$qemu_stderr"

cat "$serial_log"
if ! grep -q "kobox-qemu-nested-kvm: status=0" "$serial_log"; then
    if grep -q "kobox-qemu-nested-kvm: status=77" "$serial_log"; then
        exit 77
    fi
    echo "qemu stderr:" >&2
    cat "$qemu_stderr" >&2
    exit 1
fi
