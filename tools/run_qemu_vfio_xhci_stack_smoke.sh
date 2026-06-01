#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build-current}"
work_dir="$repo_root/.artifacts/qemu-vfio-xhci-stack"
debs_dir="$repo_root/.artifacts/debs"
root_dir="$work_dir/initramfs-root"
serial_log="$work_dir/serial.log"
qemu_stderr="$work_dir/qemu.stderr"
kernel_pkg="${KERNEL_IMAGE_PACKAGE:-linux-image-6.8.0-117-generic}"
kernel_version="${KERNEL_VERSION:-6.8.0-117-generic}"
modules_pkg="${KERNEL_MODULES_PACKAGE:-linux-modules-6.8.0-117-generic}"
usb_stack_root="${KOBOX_USB_STACK_MODULES_ROOT:-/lib/modules/$(uname -r)}"
qemu_usb_device="${KOBOX_QEMU_USB_DEVICE:-}"
expect_usb_device="${KOBOX_EXPECT_USB_DEVICE:-0}"
enable_usb_hid="${KOBOX_ENABLE_USB_HID:-0}"
expect_usb_hid="${KOBOX_EXPECT_USB_HID:-0}"
enable_usb_storage="${KOBOX_ENABLE_USB_STORAGE:-0}"
expect_usb_storage="${KOBOX_EXPECT_USB_STORAGE:-0}"
usb_storage_image="${KOBOX_USB_STORAGE_IMAGE:-$work_dir/usb-storage.img}"
usb_storage_count="${KOBOX_USB_STORAGE_COUNT:-1}"
run_drain_ms="${KOBOX_RUN_DRAIN_MS:-0}"
run_trace_internal="${KOBOX_RUN_TRACE_INTERNAL:-}"
run_trace_xhci="${KOBOX_RUN_TRACE_XHCI:-}"
run_trace_irq="${KOBOX_RUN_TRACE_IRQ:-}"
run_trace_usb="${KOBOX_RUN_TRACE_USB:-}"
run_trace_work="${KOBOX_RUN_TRACE_WORK:-}"
run_trace_dma="${KOBOX_RUN_TRACE_DMA:-}"
run_trace_device="${KOBOX_RUN_TRACE_DEVICE:-}"
run_trace_modules="${KOBOX_RUN_TRACE_MODULES:-}"
run_trace_shim_calls="${KOBOX_RUN_TRACE_SHIM_CALLS:-}"
run_trace_shim_calls_top="${KOBOX_RUN_TRACE_SHIM_CALLS_TOP:-}"
run_crash_stack="${KOBOX_RUN_CRASH_STACK:-}"
run_input_summary="${KOBOX_RUN_INPUT_SUMMARY:-$expect_usb_hid}"
run_usb_storage_summary="${KOBOX_RUN_USB_STORAGE_SUMMARY:-$expect_usb_storage}"
run_usb_storage_io_smoke="${KOBOX_RUN_USB_STORAGE_IO_SMOKE:-$expect_usb_storage}"
run_enable_sysfs_dirent="${KOBOX_RUN_ENABLE_SYSFS_DIRENT:-}"
run_enable_usb_event_inject="${KOBOX_RUN_ENABLE_USB_EVENT_INJECT:-${KOBOX_ENABLE_USB_EVENT_INJECT:-}}"

mkdir -p "$debs_dir" "$work_dir"

case "$usb_storage_count" in
    ''|*[!0-9]*)
        echo "invalid KOBOX_USB_STORAGE_COUNT: $usb_storage_count" >&2
        exit 1
        ;;
esac
if [ "$usb_storage_count" -lt 1 ]; then
    usb_storage_count=1
fi

usb_storage_image_for_index() {
    index="$1"
    if [ "$index" -eq 0 ]; then
        printf '%s\n' "$usb_storage_image"
        return
    fi
    case "$usb_storage_image" in
        *.*)
            printf '%s-%s.%s\n' "${usb_storage_image%.*}" "$index" "${usb_storage_image##*.}"
            ;;
        *)
            printf '%s-%s\n' "$usb_storage_image" "$index"
            ;;
    esac
}

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

for rel in \
    kernel/drivers/usb/core/usbcore.ko \
    kernel/drivers/usb/host/xhci-hcd.ko \
    kernel/drivers/usb/host/xhci-pci.ko
do
    if [ ! -f "$usb_stack_root/$rel" ]; then
        echo "skip qemu vfio xhci stack smoke: missing $usb_stack_root/$rel" >&2
        exit 0
    fi
done

if [ "$enable_usb_hid" = "1" ] || [ "$expect_usb_hid" = "1" ]; then
    for rel in \
        kernel/drivers/hid/hid.ko \
        kernel/drivers/hid/hid-generic.ko \
        kernel/drivers/hid/usbhid/usbhid.ko
    do
        if [ ! -f "$usb_stack_root/$rel" ]; then
            echo "skip qemu vfio xhci stack smoke: missing $usb_stack_root/$rel" >&2
            exit 0
        fi
    done
fi
if [ "$enable_usb_storage" = "1" ] || [ "$expect_usb_storage" = "1" ]; then
    rel=kernel/drivers/usb/storage/usb-storage.ko
    if [ ! -f "$usb_stack_root/$rel" ]; then
        echo "skip qemu vfio xhci stack smoke: missing $usb_stack_root/$rel" >&2
        exit 0
    fi
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
ln -s busybox "$root_dir/bin/timeout"
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

copy_guest_module() {
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
    echo "missing guest module: $rel" >&2
    exit 1
}

copy_guest_module kernel/drivers/vfio/vfio.ko
copy_guest_module kernel/drivers/vfio/vfio_iommu_type1.ko
copy_guest_module kernel/drivers/vfio/pci/vfio-pci-core.ko
copy_guest_module kernel/drivers/vfio/pci/vfio-pci.ko
copy_guest_module kernel/drivers/iommu/iommufd/iommufd.ko
copy_guest_module kernel/virt/lib/irqbypass.ko

cp "$usb_stack_root/kernel/drivers/usb/core/usbcore.ko" "$root_dir/usr/lib/kobox/usbcore.ko"
cp "$usb_stack_root/kernel/drivers/usb/host/xhci-hcd.ko" "$root_dir/usr/lib/kobox/xhci-hcd.ko"
cp "$usb_stack_root/kernel/drivers/usb/host/xhci-pci.ko" "$root_dir/usr/lib/kobox/xhci-pci.ko"
if [ "$enable_usb_hid" = "1" ] || [ "$expect_usb_hid" = "1" ]; then
    cp "$usb_stack_root/kernel/drivers/hid/hid.ko" "$root_dir/usr/lib/kobox/hid.ko"
    cp "$usb_stack_root/kernel/drivers/hid/hid-generic.ko" "$root_dir/usr/lib/kobox/hid-generic.ko"
    cp "$usb_stack_root/kernel/drivers/hid/usbhid/usbhid.ko" "$root_dir/usr/lib/kobox/usbhid.ko"
fi
if [ "$enable_usb_storage" = "1" ] || [ "$expect_usb_storage" = "1" ]; then
    cp "$usb_stack_root/kernel/drivers/usb/storage/usb-storage.ko" "$root_dir/usr/lib/kobox/usb-storage.ko"
    storage_index=0
    while [ "$storage_index" -lt "$usb_storage_count" ]; do
        image=$(usb_storage_image_for_index "$storage_index")
        if [ ! -f "$image" ]; then
            mkdir -p "$(dirname "$image")"
            dd if=/dev/zero of="$image" bs=1M count=16 >/dev/null 2>&1
        fi
        storage_index=$((storage_index + 1))
    done
fi

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

echo "kobox-qemu-vfio-xhci-stack: booted"

insmod /lib/modules/@KERNEL_VERSION@/kernel/virt/lib/irqbypass.ko || true
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/iommu/iommufd/iommufd.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/vfio.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/vfio_iommu_type1.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/pci/vfio-pci-core.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/pci/vfio-pci.ko

xhci_bdf=""
for dev in /sys/bus/pci/devices/*; do
    [ -f "$dev/class" ] || continue
    if [ "$(cat "$dev/class")" = "0x0c0330" ]; then
        xhci_bdf=${dev##*/}
        break
    fi
done

if [ -z "$xhci_bdf" ]; then
    echo "kobox-qemu-vfio-xhci-stack: xhci device not found"
    poweroff -f
fi

vendor=$(cat "/sys/bus/pci/devices/$xhci_bdf/vendor")
device=$(cat "/sys/bus/pci/devices/$xhci_bdf/device")
vendor=${vendor#0x}
device=${device#0x}
echo "kobox-qemu-vfio-xhci-stack: xhci=$xhci_bdf id=$vendor:$device"
echo "$vendor $device" >/sys/bus/pci/drivers/vfio-pci/new_id || true
if [ -e "/sys/bus/pci/devices/$xhci_bdf/driver/unbind" ]; then
    echo "$xhci_bdf" >"/sys/bus/pci/devices/$xhci_bdf/driver/unbind" || true
fi
echo "$xhci_bdf" >/sys/bus/pci/drivers/vfio-pci/bind || true

group=$(basename "$(readlink "/sys/bus/pci/devices/$xhci_bdf/iommu_group")")
if [ ! -e "/dev/vfio/$group" ]; then
    devno=$(cat "/sys/class/vfio/$group/dev")
    major=${devno%:*}
    minor=${devno#*:}
    mknod "/dev/vfio/$group" c "$major" "$minor"
fi

echo "kobox-qemu-vfio-xhci-stack: group=$group"
/usr/bin/kobox-ls-devices vfio "$xhci_bdf"

set +e
KOBOX_TRACE_INTERNAL="@RUN_TRACE_INTERNAL@" KOBOX_TRACE_XHCI="@RUN_TRACE_XHCI@" \
KOBOX_TRACE_IRQ="@RUN_TRACE_IRQ@" KOBOX_TRACE_USB="@RUN_TRACE_USB@" \
KOBOX_TRACE_WORK="@RUN_TRACE_WORK@" KOBOX_TRACE_DMA="@RUN_TRACE_DMA@" \
KOBOX_TRACE_DEVICE="@RUN_TRACE_DEVICE@" \
KOBOX_TRACE_MODULES="@RUN_TRACE_MODULES@" \
KOBOX_TRACE_SHIM_CALLS="@RUN_TRACE_SHIM_CALLS@" \
KOBOX_TRACE_SHIM_CALLS_TOP="@RUN_TRACE_SHIM_CALLS_TOP@" \
KOBOX_CRASH_STACK="@RUN_CRASH_STACK@" \
KOBOX_INPUT_SUMMARY="@RUN_INPUT_SUMMARY@" \
KOBOX_USB_STORAGE_SUMMARY="@RUN_USB_STORAGE_SUMMARY@" \
KOBOX_USB_STORAGE_IO_SMOKE="@RUN_USB_STORAGE_IO_SMOKE@" \
KOBOX_ENABLE_SYSFS_DIRENT="@RUN_ENABLE_SYSFS_DIRENT@" \
KOBOX_ENABLE_USB_EVENT_INJECT="@RUN_ENABLE_USB_EVENT_INJECT@" \
    timeout 45 /usr/bin/kobox-run \
        --backend=vfio \
        --pci="$xhci_bdf" \
        --drain-ms="@RUN_DRAIN_MS@" \
        --dep=/usr/lib/kobox/usbcore.ko \
        @RUN_USB_HID_DEPS@ \
        @RUN_USB_STORAGE_DEPS@ \
        --dep=/usr/lib/kobox/xhci-hcd.ko \
        run /usr/lib/kobox/xhci-pci.ko
status=$?
set -e

echo "kobox-qemu-vfio-xhci-stack: status=$status"
poweroff -f
INIT
chmod +x "$root_dir/init"
sed -i "s/@KERNEL_VERSION@/$kernel_version/g" "$root_dir/init"
sed -i "s/@RUN_DRAIN_MS@/$run_drain_ms/g" "$root_dir/init"
trace_internal_escaped=$(printf '%s' "$run_trace_internal" | sed 's/[\/&]/\\&/g')
trace_xhci_escaped=$(printf '%s' "$run_trace_xhci" | sed 's/[\/&]/\\&/g')
trace_irq_escaped=$(printf '%s' "$run_trace_irq" | sed 's/[\/&]/\\&/g')
trace_usb_escaped=$(printf '%s' "$run_trace_usb" | sed 's/[\/&]/\\&/g')
trace_work_escaped=$(printf '%s' "$run_trace_work" | sed 's/[\/&]/\\&/g')
trace_dma_escaped=$(printf '%s' "$run_trace_dma" | sed 's/[\/&]/\\&/g')
trace_device_escaped=$(printf '%s' "$run_trace_device" | sed 's/[\/&]/\\&/g')
trace_modules_escaped=$(printf '%s' "$run_trace_modules" | sed 's/[\/&]/\\&/g')
trace_shim_calls_escaped=$(printf '%s' "$run_trace_shim_calls" | sed 's/[\/&]/\\&/g')
trace_shim_calls_top_escaped=$(printf '%s' "$run_trace_shim_calls_top" | sed 's/[\/&]/\\&/g')
crash_stack_escaped=$(printf '%s' "$run_crash_stack" | sed 's/[\/&]/\\&/g')
input_summary_escaped=$(printf '%s' "$run_input_summary" | sed 's/[\/&]/\\&/g')
usb_storage_summary_escaped=$(printf '%s' "$run_usb_storage_summary" | sed 's/[\/&]/\\&/g')
usb_storage_io_smoke_escaped=$(printf '%s' "$run_usb_storage_io_smoke" | sed 's/[\/&]/\\&/g')
enable_sysfs_dirent_escaped=$(printf '%s' "$run_enable_sysfs_dirent" | sed 's/[\/&]/\\&/g')
enable_usb_event_inject_escaped=$(printf '%s' "$run_enable_usb_event_inject" | sed 's/[\/&]/\\&/g')
usb_hid_deps=""
if [ "$enable_usb_hid" = "1" ] || [ "$expect_usb_hid" = "1" ]; then
    usb_hid_deps="--dep=/usr/lib/kobox/hid.ko --dep=/usr/lib/kobox/hid-generic.ko --dep=/usr/lib/kobox/usbhid.ko"
fi
usb_hid_deps_escaped=$(printf '%s' "$usb_hid_deps" | sed 's/[\/&]/\\&/g')
usb_storage_deps=""
if [ "$enable_usb_storage" = "1" ] || [ "$expect_usb_storage" = "1" ]; then
    usb_storage_deps="--dep=/usr/lib/kobox/usb-storage.ko"
fi
usb_storage_deps_escaped=$(printf '%s' "$usb_storage_deps" | sed 's/[\/&]/\\&/g')
sed -i "s/@RUN_TRACE_INTERNAL@/$trace_internal_escaped/g" "$root_dir/init"
sed -i "s/@RUN_TRACE_XHCI@/$trace_xhci_escaped/g" "$root_dir/init"
sed -i "s/@RUN_TRACE_IRQ@/$trace_irq_escaped/g" "$root_dir/init"
sed -i "s/@RUN_TRACE_USB@/$trace_usb_escaped/g" "$root_dir/init"
sed -i "s/@RUN_TRACE_WORK@/$trace_work_escaped/g" "$root_dir/init"
sed -i "s/@RUN_TRACE_DMA@/$trace_dma_escaped/g" "$root_dir/init"
sed -i "s/@RUN_TRACE_DEVICE@/$trace_device_escaped/g" "$root_dir/init"
sed -i "s/@RUN_TRACE_MODULES@/$trace_modules_escaped/g" "$root_dir/init"
sed -i "s/@RUN_TRACE_SHIM_CALLS@/$trace_shim_calls_escaped/g" "$root_dir/init"
sed -i "s/@RUN_TRACE_SHIM_CALLS_TOP@/$trace_shim_calls_top_escaped/g" "$root_dir/init"
sed -i "s/@RUN_CRASH_STACK@/$crash_stack_escaped/g" "$root_dir/init"
sed -i "s/@RUN_INPUT_SUMMARY@/$input_summary_escaped/g" "$root_dir/init"
sed -i "s/@RUN_USB_STORAGE_SUMMARY@/$usb_storage_summary_escaped/g" "$root_dir/init"
sed -i "s/@RUN_USB_STORAGE_IO_SMOKE@/$usb_storage_io_smoke_escaped/g" "$root_dir/init"
sed -i "s/@RUN_ENABLE_SYSFS_DIRENT@/$enable_sysfs_dirent_escaped/g" "$root_dir/init"
sed -i "s/@RUN_ENABLE_USB_EVENT_INJECT@/$enable_usb_event_inject_escaped/g" "$root_dir/init"
sed -i "s/@RUN_USB_HID_DEPS@/$usb_hid_deps_escaped/g" "$root_dir/init"
sed -i "s/@RUN_USB_STORAGE_DEPS@/$usb_storage_deps_escaped/g" "$root_dir/init"

initramfs="$work_dir/initramfs.cpio"
(cd "$root_dir" && find . -print | "$cpio_bin" -o -H newc --quiet >"$initramfs")

qemu_usb_storage_args=""
if [ "$enable_usb_storage" = "1" ] || [ "$expect_usb_storage" = "1" ]; then
    storage_index=0
    while [ "$storage_index" -lt "$usb_storage_count" ]; do
        image=$(usb_storage_image_for_index "$storage_index")
        drive_id="usbstick$storage_index"
        qemu_usb_storage_args="$qemu_usb_storage_args -drive file=$image,if=none,format=raw,id=$drive_id -device usb-storage,drive=$drive_id,bus=xhci0.0"
        storage_index=$((storage_index + 1))
    done
fi

rm -f "$serial_log" "$qemu_stderr"
qemu-system-x86_64 \
    -enable-kvm \
    -machine q35,accel=kvm,kernel-irqchip=split \
    -cpu host \
    -m 512M \
    -kernel "$kernel" \
    -initrd "$initramfs" \
    -append "console=ttyS0 panic=-1 quiet intel_iommu=on iommu=pt module_blacklist=xhci_pci,xhci_hcd" \
    -device intel-iommu,intremap=on \
    -device qemu-xhci,id=xhci0 \
    ${qemu_usb_device:+-device "$qemu_usb_device"} \
    ${qemu_usb_storage_args} \
    -no-reboot \
    -display none \
    -serial "file:$serial_log" \
    2>"$qemu_stderr"

cat "$serial_log"
if [ -s "$qemu_stderr" ]; then
    cat "$qemu_stderr" >&2
fi
grep -q "dependency /usr/lib/kobox/usbcore.ko init_module returned 0" "$serial_log"
grep -q "dependency /usr/lib/kobox/xhci-hcd.ko init_module returned 0" "$serial_log"
grep -q "^init_module returned 0" "$serial_log"
grep -q "kobox-qemu-vfio-xhci-stack: status=0" "$serial_log"
if [ "$expect_usb_device" = "1" ]; then
    if ! grep -q "New USB device found" "$serial_log"; then
        grep -q "USB disconnect, device number 2" "$serial_log"
        ! grep -q "device descriptor read/.*error" "$serial_log"
        ! grep -q "unable to enumerate USB device" "$serial_log"
    fi
fi
if [ "$expect_usb_hid" = "1" ]; then
    grep -Eq "hid-generic|USB HID|input:" "$serial_log"
    grep -q "kobox-input: device" "$serial_log"
fi
if [ "$expect_usb_storage" = "1" ]; then
    grep -q "dependency /usr/lib/kobox/usb-storage.ko init_module returned 0" "$serial_log"
    grep -q "kobox-usb-storage-io:" "$serial_log"
    grep -q "kobox-usb-storage-scsi:" "$serial_log"
    grep -q "kobox-usb-storage-bot:" "$serial_log"
    grep -q "kobox-usb-storage:" "$serial_log"
    storage_io_count=$(grep -c "^kobox-usb-storage-io:" "$serial_log" || true)
    storage_scsi_count=$(grep -c "^kobox-usb-storage-scsi:" "$serial_log" || true)
    storage_bot_count=$(grep -c "^kobox-usb-storage-bot:" "$serial_log" || true)
    storage_summary_count=$(grep -c "^kobox-usb-storage:" "$serial_log" || true)
    if [ "$storage_io_count" -lt "$usb_storage_count" ] ||
        [ "$storage_scsi_count" -lt "$usb_storage_count" ] ||
        [ "$storage_bot_count" -lt "$usb_storage_count" ] ||
        [ "$storage_summary_count" -lt "$usb_storage_count" ]
    then
        echo "expected at least $usb_storage_count USB storage smoke lines, got io=$storage_io_count scsi=$storage_scsi_count bot=$storage_bot_count summary=$storage_summary_count" >&2
        exit 1
    fi
    ! grep -q "device descriptor read/.*error" "$serial_log"
    ! grep -q "unable to enumerate USB device" "$serial_log"
fi
