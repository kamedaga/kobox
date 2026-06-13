#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build}"
work_dir="$repo_root/.artifacts/qemu-vfio-virtio-blk-ext4"
debs_dir="$repo_root/.artifacts/debs"
root_dir="$work_dir/initramfs-root"
serial_log="$work_dir/serial.log"
qemu_stderr="$work_dir/qemu.stderr"
kernel_pkg="${KERNEL_IMAGE_PACKAGE:-linux-image-6.8.0-117-generic}"
kernel_version="${KERNEL_VERSION:-6.8.0-117-generic}"
modules_pkg="${KERNEL_MODULES_PACKAGE:-linux-modules-6.8.0-117-generic}"
ext4_ko="${KOBOX_EXT4_KO:-$repo_root/.artifacts/ext4-ko/ext4.ko}"
crc16_ko="${KOBOX_CRC16_KO:-$repo_root/.artifacts/ext4-ko/crc16.ko}"
mbcache_ko="${KOBOX_MBCACHE_KO:-$repo_root/.artifacts/ext4-ko/mbcache.ko}"
jbd2_ko="${KOBOX_JBD2_KO:-$repo_root/.artifacts/ext4-ko/jbd2.ko}"

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

if [ ! -x "$build_dir/kobox-ext4-real-ops" ]; then
    echo "missing kobox-ext4-real-ops in $build_dir" >&2
    exit 1
fi
for module in "$ext4_ko" "$crc16_ko" "$mbcache_ko" "$jbd2_ko"; do
    if [ ! -f "$module" ]; then
        echo "missing ext4 dependency: $module" >&2
        exit 77
    fi
done
if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "skip qemu vfio virtio-blk ext4 smoke: qemu-system-x86_64 not found" >&2
    exit 77
fi
if ! command -v qemu-img >/dev/null 2>&1; then
    echo "skip qemu vfio virtio-blk ext4 smoke: qemu-img not found" >&2
    exit 77
fi
if ! command -v mkfs.ext4 >/dev/null 2>&1 || ! command -v debugfs >/dev/null 2>&1; then
    echo "skip qemu vfio virtio-blk ext4 smoke: mkfs.ext4/debugfs not found" >&2
    exit 77
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

disk="$work_dir/virtio-ext4.img"
seed="$work_dir/seed.txt"
multi="$work_dir/seed-multi.bin"
printf '%s' "kobox-ext4-module-read" >"$seed"
awk 'BEGIN { for (i = 0; i < 3072; i++) printf "%c", 65 + ((i * 7) % 26) }' >"$multi"
rm -f "$disk"
qemu-img create -f raw "$disk" 32M >/dev/null
mkfs.ext4 -q -F -O '^has_journal,^extent,^64bit,^metadata_csum,^extra_isize,^dir_index' "$disk"
debugfs -w -R "write $seed /hello.txt" "$disk" >/dev/null 2>&1
debugfs -w -R "write $multi /multi.txt" "$disk" >/dev/null 2>&1

rm -rf "$root_dir"
mkdir -p "$root_dir"/bin "$root_dir"/dev "$root_dir"/lib/modules "$root_dir"/lib64 "$root_dir"/proc "$root_dir"/sys "$root_dir"/tmp "$root_dir"/usr/bin "$root_dir"/usr/lib/kobox

cp "$work_dir/busybox-root/usr/bin/busybox" "$root_dir/bin/busybox"
ln -s busybox "$root_dir/bin/sh"
ln -s busybox "$root_dir/bin/mount"
ln -s busybox "$root_dir/bin/insmod"
ln -s busybox "$root_dir/bin/poweroff"
cp "$build_dir/kobox-ext4-real-ops" "$root_dir/usr/bin/kobox-ext4-real-ops"
cp "$ext4_ko" "$root_dir/usr/lib/kobox/ext4.ko"
cp "$crc16_ko" "$root_dir/usr/lib/kobox/crc16.ko"
cp "$mbcache_ko" "$root_dir/usr/lib/kobox/mbcache.ko"
cp "$jbd2_ko" "$root_dir/usr/lib/kobox/jbd2.ko"

ldd "$build_dir/kobox-ext4-real-ops" | awk '
    $1 ~ /^\// { print $1 }
    $3 ~ /^\// { print $3 }
' | sort -u | while IFS= read -r lib; do
    if [ -n "$lib" ]; then
        dest="$root_dir$lib"
        mkdir -p "$(dirname "$dest")"
        cp "$lib" "$dest"
    fi
done

copy_module() {
    rel="$1"
    src="$work_dir/modules-root/lib/modules/$kernel_version/$rel"
    dst="$root_dir/lib/modules/$kernel_version/$rel"
    mkdir -p "$(dirname "$dst")"
    if [ -f "$src" ]; then
        cp "$src" "$dst"
        return
    fi
    if [ -f "$src.zst" ]; then
        "$zstd_bin" -q -d -f "$src.zst" -o "$dst"
        return
    fi
    echo "missing module: $rel" >&2
    exit 1
}

copy_module kernel/drivers/vfio/vfio.ko
copy_module kernel/drivers/vfio/vfio_iommu_type1.ko
copy_module kernel/drivers/vfio/pci/vfio-pci-core.ko
copy_module kernel/drivers/vfio/pci/vfio-pci.ko
copy_module kernel/drivers/iommu/iommufd/iommufd.ko
copy_module kernel/virt/lib/irqbypass.ko

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

echo "kobox-qemu-vfio-virtio-blk-ext4: booted"

insmod /lib/modules/@KERNEL_VERSION@/kernel/virt/lib/irqbypass.ko || true
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/iommu/iommufd/iommufd.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/vfio.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/vfio_iommu_type1.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/pci/vfio-pci-core.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/pci/vfio-pci.ko

virtio_bdf=""
for dev in /sys/bus/pci/devices/*; do
    [ -f "$dev/vendor" ] || continue
    vendor=$(cat "$dev/vendor")
    device=$(cat "$dev/device")
    case "$vendor:$device" in
        0x1af4:0x1001|0x1af4:0x1042)
            virtio_bdf=${dev##*/}
            break
            ;;
    esac
done

if [ -z "$virtio_bdf" ]; then
    echo "kobox-qemu-vfio-virtio-blk-ext4: virtio-blk device not found"
    poweroff -f
fi

vendor=$(cat "/sys/bus/pci/devices/$virtio_bdf/vendor")
device=$(cat "/sys/bus/pci/devices/$virtio_bdf/device")
vendor=${vendor#0x}
device=${device#0x}
echo "kobox-qemu-vfio-virtio-blk-ext4: virtio_blk=$virtio_bdf id=$vendor:$device"
echo "$vendor $device" >/sys/bus/pci/drivers/vfio-pci/new_id || true
if [ -e "/sys/bus/pci/devices/$virtio_bdf/driver/unbind" ]; then
    echo "$virtio_bdf" >"/sys/bus/pci/devices/$virtio_bdf/driver/unbind" || true
fi
echo "$virtio_bdf" >/sys/bus/pci/drivers/vfio-pci/bind || true

group=$(basename "$(readlink "/sys/bus/pci/devices/$virtio_bdf/iommu_group")")
if [ ! -e "/dev/vfio/$group" ]; then
    devno=$(cat "/sys/class/vfio/$group/dev")
    major=${devno%:*}
    minor=${devno#*:}
    mknod "/dev/vfio/$group" c "$major" "$minor"
fi

echo "kobox-qemu-vfio-virtio-blk-ext4: group=$group"
set +e
KOBOX_TRACE_FS="${KOBOX_TRACE_FS:-0}" /usr/bin/kobox-ext4-real-ops \
    --dep=/usr/lib/kobox/crc16.ko \
    --dep=/usr/lib/kobox/mbcache.ko \
    --dep=/usr/lib/kobox/jbd2.ko \
    --vfio-virtio-blk="$virtio_bdf" \
    --skip-prepare \
    --skip-interface-ops \
    /usr/lib/kobox/ext4.ko
status=$?
set -e
echo "kobox-qemu-vfio-virtio-blk-ext4: status=$status"
poweroff -f
INIT
chmod +x "$root_dir/init"
sed -i "s/@KERNEL_VERSION@/$kernel_version/g" "$root_dir/init"

initramfs="$work_dir/initramfs.cpio"
(cd "$root_dir" && find . -print | "$cpio_bin" -o -H newc --quiet >"$initramfs")

rm -f "$serial_log" "$qemu_stderr"
qemu-system-x86_64 \
    -enable-kvm \
    -machine q35,accel=kvm,kernel-irqchip=split \
    -cpu host \
    -m 512M \
    -kernel "$kernel" \
    -initrd "$initramfs" \
    -append "console=ttyS0 panic=-1 quiet intel_iommu=on iommu=pt" \
    -device intel-iommu,intremap=on \
    -drive id=blk0,if=none,format=raw,file="$disk" \
    -device virtio-blk-pci,disable-legacy=on,iommu_platform=on,drive=blk0 \
    -no-reboot \
    -display none \
    -serial "file:$serial_log" \
    2>"$qemu_stderr"

cat "$serial_log"
if [ -s "$qemu_stderr" ]; then
    cat "$qemu_stderr" >&2
fi

grep -q "kobox-qemu-vfio-virtio-blk-ext4: status=0" "$serial_log"
grep -q "virtio-blk vfio: capacity_sectors=" "$serial_log"
grep -q "module-vfs: fill_super_result=0" "$serial_log"
grep -q "module-vfs: readdir_result=0" "$serial_log"
grep -q "module-vfs: lookup name=hello.txt result=" "$serial_log"
grep -q "module-vfs: lookup name=multi.txt result=" "$serial_log"
grep -q "module-vfs: read_iter label=hello-initial offset=0 result=22" "$serial_log"
grep -q "module-vfs: write_iter label=hello-overwrite offset=0 result=22" "$serial_log"
grep -q "module-vfs: read_iter label=hello-post-write offset=0 result=22" "$serial_log"
grep -q "module-vfs: read_iter label=multi-full offset=0 result=3072" "$serial_log"
grep -q "module-vfs: write_iter label=multi-boundary-write offset=900 result=512" "$serial_log"
grep -q "module-vfs: read_iter label=multi-boundary-post-write offset=900 result=512" "$serial_log"
