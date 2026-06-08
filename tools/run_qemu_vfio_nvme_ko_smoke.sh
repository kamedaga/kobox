#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build-current}"
work_dir="$repo_root/.artifacts/qemu-vfio-nvme-ko"
debs_dir="$repo_root/.artifacts/debs"
root_dir="$work_dir/initramfs-root"
serial_log="$work_dir/serial.log"
qemu_stderr="$work_dir/qemu.stderr"
qemu_trace_events="$work_dir/qemu-trace-events"
kernel_pkg="${KERNEL_IMAGE_PACKAGE:-linux-image-6.8.0-117-generic}"
kernel_version="${KERNEL_VERSION:-6.8.0-117-generic}"
modules_pkg="${KERNEL_MODULES_PACKAGE:-linux-modules-6.8.0-117-generic}"

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

if [ ! -x "$build_dir/kobox-run" ] || [ ! -x "$build_dir/kobox-ls-devices" ]; then
    echo "missing kobox tools in $build_dir" >&2
    exit 1
fi
if [ ! -f "$kernel" ]; then
    echo "missing $kernel" >&2
    exit 1
fi

rm -rf "$root_dir"
mkdir -p "$root_dir"/bin "$root_dir"/dev "$root_dir"/etc "$root_dir"/lib/modules "$root_dir"/lib64 "$root_dir"/proc "$root_dir"/sys "$root_dir"/tmp "$root_dir"/usr/bin "$root_dir"/usr/lib/kobox

cp "$work_dir/busybox-root/usr/bin/busybox" "$root_dir/bin/busybox"
ln -s busybox "$root_dir/bin/sh"
ln -s busybox "$root_dir/bin/mount"
ln -s busybox "$root_dir/bin/insmod"
ln -s busybox "$root_dir/bin/poweroff"
cp "$build_dir/kobox-run" "$root_dir/usr/bin/kobox-run"
cp "$build_dir/kobox-ls-devices" "$root_dir/usr/bin/kobox-ls-devices"

for tool in "$build_dir/kobox-run" "$build_dir/kobox-ls-devices"; do
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
copy_module kernel/drivers/nvme/common/nvme-auth.ko
copy_module kernel/drivers/nvme/host/nvme-core.ko
copy_module kernel/drivers/nvme/host/nvme.ko
cp "$root_dir/lib/modules/$kernel_version/kernel/drivers/nvme/common/nvme-auth.ko" "$root_dir/usr/lib/kobox/nvme-auth.ko"
cp "$root_dir/lib/modules/$kernel_version/kernel/drivers/nvme/host/nvme-core.ko" "$root_dir/usr/lib/kobox/nvme-core.ko"
cp "$root_dir/lib/modules/$kernel_version/kernel/drivers/nvme/host/nvme.ko" "$root_dir/usr/lib/kobox/nvme.ko"

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

echo "kobox-qemu-vfio-nvme-ko: booted"

insmod /lib/modules/@KERNEL_VERSION@/kernel/virt/lib/irqbypass.ko || true
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/iommu/iommufd/iommufd.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/vfio.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/vfio_iommu_type1.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/pci/vfio-pci-core.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/pci/vfio-pci.ko

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
    echo "kobox-qemu-vfio-nvme-ko: nvme device not found"
    poweroff -f
fi

vendor=$(cat "/sys/bus/pci/devices/$nvme_bdf/vendor")
device=$(cat "/sys/bus/pci/devices/$nvme_bdf/device")
vendor=${vendor#0x}
device=${device#0x}
echo "kobox-qemu-vfio-nvme-ko: nvme=$nvme_bdf id=$vendor:$device"
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

echo "kobox-qemu-vfio-nvme-ko: group=$group"
/usr/bin/kobox-ls-devices vfio "$nvme_bdf"

set +e
/usr/bin/kobox-run \
    --backend=vfio \
    --pci="$nvme_bdf" \
    --dep=/usr/lib/kobox/nvme-auth.ko \
    --dep=/usr/lib/kobox/nvme-core.ko \
    run /usr/lib/kobox/nvme.ko
status=$?
set -e

echo "kobox-qemu-vfio-nvme-ko: status=$status"
poweroff -f
INIT
chmod +x "$root_dir/init"
sed -i "s/@KERNEL_VERSION@/$kernel_version/g" "$root_dir/init"
kobox_run_env="KOBOX_NVME_IO_SMOKE=1"
if [ "${KOBOX_TRACE_NVME:-}" = "1" ]; then
    kobox_run_env="$kobox_run_env KOBOX_TRACE_NVME=1"
fi
if [ "${KOBOX_TRACE_PCI:-}" = "1" ]; then
    kobox_run_env="$kobox_run_env KOBOX_TRACE_PCI=1"
fi
if [ "${KOBOX_TRACE_IRQ:-}" = "1" ]; then
    kobox_run_env="$kobox_run_env KOBOX_TRACE_IRQ=1"
fi
if [ -n "${KOBOX_TRACE_MODULES:-}" ]; then
    kobox_run_env="$kobox_run_env KOBOX_TRACE_MODULES=$KOBOX_TRACE_MODULES"
fi
if [ -n "$kobox_run_env" ]; then
    kobox_run_env=${kobox_run_env# }
    sed -i "s#^/usr/bin/kobox-run #$kobox_run_env /usr/bin/kobox-run #" "$root_dir/init"
fi

initramfs="$work_dir/initramfs.cpio"
(cd "$root_dir" && find . -print | "$cpio_bin" -o -H newc --quiet >"$initramfs")

disk="$work_dir/nvme.img"
if [ ! -f "$disk" ]; then
    qemu-img create -f raw "$disk" 16M >/dev/null
fi

rm -f "$serial_log" "$qemu_stderr"
qemu_trace_args=""
if [ "${KOBOX_QEMU_TRACE_NVME:-}" = "1" ]; then
    cat >"$qemu_trace_events" <<'TRACE'
pci_nvme_admin_cmd
pci_nvme_io_cmd
pci_nvme_dbbuf_config
pci_nvme_setfeat
pci_nvme_getfeat
pci_nvme_process_aers
pci_nvme_aer
pci_nvme_aer_post_cqe
pci_nvme_enqueue_req_completion
pci_nvme_map_prp
pci_nvme_dma_read
pci_nvme_mmio_cfg
pci_nvme_mmio_stopped
pci_nvme_pci_reset
TRACE
    qemu_trace_args="-trace events=$qemu_trace_events"
fi
qemu-system-x86_64 \
    $qemu_trace_args \
    -enable-kvm \
    -machine q35,accel=kvm,kernel-irqchip=split \
    -cpu host \
    -m 512M \
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

cat "$serial_log"
if [ -s "$qemu_stderr" ]; then
    cat "$qemu_stderr" >&2
fi
grep -q "kobox-qemu-vfio-nvme-ko: status=0" "$serial_log"
grep -q "dependency /usr/lib/kobox/nvme-core.ko init_module returned 0" "$serial_log"
grep -q "1/0/0 default/read/poll queues" "$serial_log"
grep -q "init_module returned 0" "$serial_log"
grep -q "kobox nvme io smoke: read-after-reset lba=17 blocks=1 bytes=512 compare=ok" "$serial_log"
grep -q "kobox nvme io smoke: read-after-reset lba=109 blocks=2 bytes=1024 compare=ok" "$serial_log"
grep -q "kobox nvme io smoke: read-after-reset lba=257 blocks=4 bytes=2048 compare=ok" "$serial_log"
grep -q "kobox nvme io smoke: cases=3 reset=ok msix-waits=6 block-requests=ok" "$serial_log"
grep -q "cleanup_module returned" "$serial_log"
! grep -q "missing or invalid SUBNQN field" "$serial_log"
! grep -q "IO queues not created" "$serial_log"
! grep -q "blk_execute_rq timeout" "$serial_log"
! grep -q "vtd_iova_to_slpte" "$qemu_stderr"
! grep -q "vtd_iommu_translate" "$qemu_stderr"
! grep -q "detected translation failure" "$qemu_stderr"
