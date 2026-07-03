#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build-current}"
work_dir="$repo_root/.artifacts/qemu-vfio-virtio-net-ko"
debs_dir="$repo_root/.artifacts/debs"
root_dir="$work_dir/initramfs-root"
serial_log="$work_dir/serial.log"
qemu_stderr="$work_dir/qemu.stderr"
pcap_log="$work_dir/net.pcap"
kernel_pkg="${KERNEL_IMAGE_PACKAGE:-linux-image-6.8.0-117-generic}"
kernel_version="${KERNEL_VERSION:-6.8.0-117-generic}"
modules_pkg="${KERNEL_MODULES_PACKAGE:-linux-modules-6.8.0-117-generic}"
modules_extra_pkg="${KERNEL_MODULES_EXTRA_PACKAGE:-linux-modules-extra-6.8.0-117-generic}"
virtio_rpm_url="${KOBOX_VIRTIO_RPM_URL:-https://vault.centos.org/7.9.2009/os/x86_64/Packages/kernel-3.10.0-1160.el7.x86_64.rpm}"
virtio_rpm_name="${virtio_rpm_url##*/}"
virtio_rpm_kernel_version="${KOBOX_VIRTIO_RPM_KERNEL_VERSION:-3.10.0-1160.el7.x86_64}"
: "${KOBOX_NET_AUTO_OPEN:=1}"
: "${KOBOX_NET_TX_SMOKE:=1}"
: "${KOBOX_NET_DRIVER:=virtio}"
: "${KOBOX_VIRTIO_NO_INDIRECT:=1}"
: "${KOBOX_VIRTIO_NO_EVENT_IDX:=1}"

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
fetch_deb "$modules_extra_pkg"
fetch_deb busybox-static
fetch_deb cpio
fetch_deb zstd

if [ ! -f "$debs_dir/$virtio_rpm_name" ]; then
    curl -L -o "$debs_dir/$virtio_rpm_name" "$virtio_rpm_url"
fi

extract_deb_once "$kernel_pkg" "$work_dir/.kernel-extracted" "$work_dir/kernel-root"
extract_deb_once "$modules_pkg" "$work_dir/.modules-extracted" "$work_dir/modules-root"
extract_deb_once "$modules_extra_pkg" "$work_dir/.modules-extra-extracted" "$work_dir/modules-extra-root"
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
mkdir -p \
    "$root_dir"/bin \
    "$root_dir"/dev \
    "$root_dir"/etc \
    "$root_dir"/lib/modules \
    "$root_dir"/lib64 \
    "$root_dir"/proc \
    "$root_dir"/sys \
    "$root_dir"/tmp \
    "$root_dir"/usr/bin \
    "$root_dir"/usr/lib/kobox

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
    extra_src="$work_dir/modules-extra-root/lib/modules/$kernel_version/$rel"
    dst="$root_dir/lib/modules/$kernel_version/$rel"
    mkdir -p "$(dirname "$dst")"
    if [ -f "$src" ]; then
        cp "$src" "$dst"
        return
    fi
    if [ -f "$extra_src" ]; then
        cp "$extra_src" "$dst"
        return
    fi
    if [ -f "$src.zst" ]; then
        "$zstd_bin" -q -d -f "$src.zst" -o "$dst"
        return
    fi
    if [ -f "$extra_src.zst" ]; then
        "$zstd_bin" -q -d -f "$extra_src.zst" -o "$dst"
        return
    fi
    echo "missing module: $rel" >&2
    exit 1
}

copy_module_optional() {
    rel="$1"
    src="$work_dir/modules-root/lib/modules/$kernel_version/$rel"
    extra_src="$work_dir/modules-extra-root/lib/modules/$kernel_version/$rel"
    dst="$root_dir/lib/modules/$kernel_version/$rel"
    mkdir -p "$(dirname "$dst")"
    if [ -f "$src" ]; then
        cp "$src" "$dst"
        return 0
    fi
    if [ -f "$extra_src" ]; then
        cp "$extra_src" "$dst"
        return 0
    fi
    if [ -f "$src.zst" ]; then
        "$zstd_bin" -q -d -f "$src.zst" -o "$dst"
        return 0
    fi
    if [ -f "$extra_src.zst" ]; then
        "$zstd_bin" -q -d -f "$extra_src.zst" -o "$dst"
        return 0
    fi
    return 1
}

extract_virtio_rpm_once() {
    stamp="$work_dir/.virtio-rpm-extracted"
    dest="$work_dir/virtio-rpm-root"
    if [ -e "$stamp" ]; then
        return
    fi
    rm -rf "$dest"
    mkdir -p "$dest"
    payload_offset=$(python3 - "$debs_dir/$virtio_rpm_name" <<'PY'
import pathlib
import struct
import sys

data = pathlib.Path(sys.argv[1]).read_bytes()

def read_header(offset):
    if data[offset:offset + 3] != b"\x8e\xad\xe8":
        raise SystemExit(f"bad rpm header at {offset}")
    index_count, store_size = struct.unpack(">II", data[offset + 8:offset + 16])
    return offset + 16 + index_count * 16 + store_size

offset = 96
offset = read_header(offset)
while offset % 8:
    offset += 1
offset = read_header(offset)
for magic in (b"\xfd7zXZ\x00", b"\x1f\x8b", b"BZh", b"\x28\xb5\x2f\xfd"):
    found = data.find(magic, offset, offset + 4096)
    if found != -1:
        offset = found
        break
print(offset)
PY
)
    tail -c +$((payload_offset + 1)) "$debs_dir/$virtio_rpm_name" |
        xz -dc |
        (cd "$dest" && "$cpio_bin" -id --quiet \
            "./lib/modules/$virtio_rpm_kernel_version/kernel/drivers/virtio/virtio.ko.xz" \
            "./lib/modules/$virtio_rpm_kernel_version/kernel/drivers/virtio/virtio_ring.ko.xz" \
            "./lib/modules/$virtio_rpm_kernel_version/kernel/drivers/virtio/virtio_pci.ko.xz" \
            "./lib/modules/$virtio_rpm_kernel_version/kernel/net/core/failover.ko.xz" \
            "./lib/modules/$virtio_rpm_kernel_version/kernel/drivers/net/net_failover.ko.xz" \
            "./lib/modules/$virtio_rpm_kernel_version/kernel/drivers/net/virtio_net.ko.xz")
    : >"$stamp"
}

copy_rhel_virtio_module() {
    rel="$1"
    out="$2"
    src="$work_dir/virtio-rpm-root/lib/modules/$virtio_rpm_kernel_version/$rel"
    if [ ! -f "$src" ]; then
        echo "missing virtio rpm module: $rel" >&2
        exit 1
    fi
    xz -dc "$src" >"$root_dir/usr/lib/kobox/$out"
}

copy_linux_module_to_kobox() {
    rel="$1"
    out="$2"
    src="$work_dir/modules-root/lib/modules/$kernel_version/$rel"
    extra_src="$work_dir/modules-extra-root/lib/modules/$kernel_version/$rel"
    if [ -f "$src" ]; then
        cp "$src" "$root_dir/usr/lib/kobox/$out"
        return
    fi
    if [ -f "$extra_src" ]; then
        cp "$extra_src" "$root_dir/usr/lib/kobox/$out"
        return
    fi
    if [ -f "$src.zst" ]; then
        "$zstd_bin" -q -d -f "$src.zst" -o "$root_dir/usr/lib/kobox/$out"
        return
    fi
    if [ -f "$extra_src.zst" ]; then
        "$zstd_bin" -q -d -f "$extra_src.zst" -o "$root_dir/usr/lib/kobox/$out"
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

extract_virtio_rpm_once
copy_rhel_virtio_module "kernel/drivers/virtio/virtio.ko.xz" "virtio.ko"
copy_rhel_virtio_module "kernel/drivers/virtio/virtio_ring.ko.xz" "virtio_ring.ko"
copy_rhel_virtio_module "kernel/drivers/virtio/virtio_pci.ko.xz" "virtio_pci.ko"
copy_rhel_virtio_module "kernel/net/core/failover.ko.xz" "failover.ko"
copy_rhel_virtio_module "kernel/drivers/net/net_failover.ko.xz" "net_failover.ko"
copy_rhel_virtio_module "kernel/drivers/net/virtio_net.ko.xz" "virtio_net.ko"
copy_linux_module_to_kobox "kernel/drivers/net/ethernet/intel/e1000e/e1000e.ko" "e1000e.ko"

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

echo "kobox-qemu-vfio-net-ko: booted driver=@KOBOX_NET_DRIVER@"

insmod /lib/modules/@KERNEL_VERSION@/kernel/virt/lib/irqbypass.ko || true
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/iommu/iommufd/iommufd.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/vfio.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/vfio_iommu_type1.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/pci/vfio-pci-core.ko
insmod /lib/modules/@KERNEL_VERSION@/kernel/drivers/vfio/pci/vfio-pci.ko

net_bdf=""
for dev in /sys/bus/pci/devices/*; do
    [ -f "$dev/vendor" ] || continue
    [ -f "$dev/class" ] || continue
    vendor=$(cat "$dev/vendor")
    class=$(cat "$dev/class")
    case "@KOBOX_NET_DRIVER@" in
        virtio)
            if [ "$vendor" = "0x1af4" ] && [ "$class" = "0x020000" ]; then
                net_bdf=${dev##*/}
                break
            fi
            ;;
        e1000e)
            if [ "$vendor" = "0x8086" ] && [ "$class" = "0x020000" ]; then
                net_bdf=${dev##*/}
                break
            fi
            ;;
    esac
done

if [ -z "$net_bdf" ]; then
    echo "kobox-qemu-vfio-net-ko: net device not found driver=@KOBOX_NET_DRIVER@"
    poweroff -f
fi

vendor=$(cat "/sys/bus/pci/devices/$net_bdf/vendor")
device=$(cat "/sys/bus/pci/devices/$net_bdf/device")
vendor=${vendor#0x}
device=${device#0x}
echo "kobox-qemu-vfio-net-ko: net=$net_bdf id=$vendor:$device driver=@KOBOX_NET_DRIVER@"
echo "$vendor $device" >/sys/bus/pci/drivers/vfio-pci/new_id || true
if [ -e "/sys/bus/pci/devices/$net_bdf/driver/unbind" ]; then
    echo "$net_bdf" >"/sys/bus/pci/devices/$net_bdf/driver/unbind" || true
fi
echo "$net_bdf" >/sys/bus/pci/drivers/vfio-pci/bind || true

group=$(basename "$(readlink "/sys/bus/pci/devices/$net_bdf/iommu_group")")
if [ ! -e "/dev/vfio/$group" ]; then
    devno=$(cat "/sys/class/vfio/$group/dev")
    major=${devno%:*}
    minor=${devno#*:}
    mknod "/dev/vfio/$group" c "$major" "$minor"
fi

echo "kobox-qemu-vfio-net-ko: group=$group"
/usr/bin/kobox-ls-devices vfio "$net_bdf"

set +e
case "@KOBOX_NET_DRIVER@" in
    virtio)
        /usr/bin/kobox-run \
            --device=vfio \
            --pci="$net_bdf" \
            --dep=/usr/lib/kobox/virtio.ko \
            --dep=/usr/lib/kobox/virtio_ring.ko \
            --dep=/usr/lib/kobox/virtio_pci.ko \
            --dep=/usr/lib/kobox/failover.ko \
            --dep=/usr/lib/kobox/net_failover.ko \
            run /usr/lib/kobox/virtio_net.ko
        ;;
    e1000e)
        /usr/bin/kobox-run \
            --device=vfio \
            --pci="$net_bdf" \
            run /usr/lib/kobox/e1000e.ko
        ;;
esac
status=$?
set -e

echo "kobox-qemu-vfio-net-ko: status=$status"
poweroff -f
INIT
chmod +x "$root_dir/init"
sed -i "s/@KERNEL_VERSION@/$kernel_version/g" "$root_dir/init"
sed -i "s/@KOBOX_NET_DRIVER@/$KOBOX_NET_DRIVER/g" "$root_dir/init"

case "$KOBOX_NET_DRIVER" in
    virtio)
        qemu_net_device="-device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56,iommu_platform=on,disable-legacy=on"
        ;;
    e1000e)
        qemu_net_device="-device e1000e,netdev=net0,mac=52:54:00:12:34:56"
        ;;
    *)
        echo "unsupported KOBOX_NET_DRIVER=$KOBOX_NET_DRIVER" >&2
        exit 1
        ;;
esac

kobox_run_env=""
if [ -n "${KOBOX_TRACE_MODULES:-}" ]; then
    kobox_run_env="$kobox_run_env KOBOX_TRACE_MODULES=$KOBOX_TRACE_MODULES"
fi
if [ -n "${KOBOX_TRACE_PCI:-}" ]; then
    kobox_run_env="$kobox_run_env KOBOX_TRACE_PCI=$KOBOX_TRACE_PCI"
fi
if [ -n "${KOBOX_TRACE_NET:-}" ]; then
    kobox_run_env="$kobox_run_env KOBOX_TRACE_NET=$KOBOX_TRACE_NET"
fi
if [ -n "${KOBOX_TRACE_NET_MEMCPY:-}" ]; then
    kobox_run_env="$kobox_run_env KOBOX_TRACE_NET_MEMCPY=$KOBOX_TRACE_NET_MEMCPY"
fi
if [ -n "${KOBOX_TRACE_DEVICE:-}" ]; then
    kobox_run_env="$kobox_run_env KOBOX_TRACE_DEVICE=$KOBOX_TRACE_DEVICE"
fi
if [ -n "${KOBOX_TRACE_DMA:-}" ]; then
    kobox_run_env="$kobox_run_env KOBOX_TRACE_DMA=$KOBOX_TRACE_DMA"
fi
if [ -n "${KOBOX_TRACE_MMIO:-}" ]; then
    kobox_run_env="$kobox_run_env KOBOX_TRACE_MMIO=$KOBOX_TRACE_MMIO"
fi
if [ -n "${KOBOX_TRACE_VIRTIO:-}" ]; then
    kobox_run_env="$kobox_run_env KOBOX_TRACE_VIRTIO=$KOBOX_TRACE_VIRTIO"
fi
if [ -n "${KOBOX_NET_AUTO_OPEN:-}" ]; then
    kobox_run_env="$kobox_run_env KOBOX_NET_AUTO_OPEN=$KOBOX_NET_AUTO_OPEN"
fi
if [ -n "${KOBOX_NET_TX_SMOKE:-}" ]; then
    kobox_run_env="$kobox_run_env KOBOX_NET_TX_SMOKE=$KOBOX_NET_TX_SMOKE"
fi
if [ -n "${KOBOX_NET_DRIVER:-}" ]; then
    kobox_run_env="$kobox_run_env KOBOX_NET_DRIVER=$KOBOX_NET_DRIVER"
fi
if [ -n "${KOBOX_VIRTIO_NO_INDIRECT:-}" ]; then
    kobox_run_env="$kobox_run_env KOBOX_VIRTIO_NO_INDIRECT=$KOBOX_VIRTIO_NO_INDIRECT"
fi
if [ -n "${KOBOX_VIRTIO_NO_EVENT_IDX:-}" ]; then
    kobox_run_env="$kobox_run_env KOBOX_VIRTIO_NO_EVENT_IDX=$KOBOX_VIRTIO_NO_EVENT_IDX"
fi
if [ -n "$kobox_run_env" ]; then
    kobox_run_env=${kobox_run_env# }
    sed -i "s#\\(^[[:space:]]*\\)/usr/bin/kobox-run #\\1$kobox_run_env /usr/bin/kobox-run #" "$root_dir/init"
fi

initramfs="$work_dir/initramfs.cpio"
(cd "$root_dir" && find . -print | "$cpio_bin" -o -H newc --quiet >"$initramfs")

rm -f "$serial_log" "$qemu_stderr" "$pcap_log"
qemu-system-x86_64 \
    -enable-kvm \
    -machine q35,accel=kvm,kernel-irqchip=split \
    -cpu host \
    -m 512M \
    -kernel "$kernel" \
    -initrd "$initramfs" \
    -append "console=ttyS0 panic=-1 quiet intel_iommu=on iommu=pt module_blacklist=virtio_net,virtio_pci,virtio_pci_modern_dev,virtio_pci_legacy_dev,e1000e" \
    -device intel-iommu,intremap=on \
    -netdev user,id=net0 \
    -object "filter-dump,id=netdump,netdev=net0,file=$pcap_log" \
    $qemu_net_device \
    -no-reboot \
    -display none \
    -serial "file:$serial_log" \
    2>"$qemu_stderr"

cat "$serial_log"
if [ -s "$qemu_stderr" ]; then
    cat "$qemu_stderr" >&2
fi
grep -q "kobox-qemu-vfio-net-ko: status=0" "$serial_log"
grep -q "init_module returned 0" "$serial_log"
case "$KOBOX_NET_DRIVER" in
    virtio)
        ! grep -q "leaving for legacy driver" "$serial_log"
        ;;
esac
if [ -n "${KOBOX_NET_EXPECT_OPEN:-}" ]; then
    grep -q "kobox net: dev_open" "$serial_log"
fi
case "${KOBOX_NET_TX_SMOKE:-}" in
    ""|1|arp|both)
        grep -q "kobox net: xmit_smoke .* result=0" "$serial_log"
        if [ "$KOBOX_NET_DRIVER" = "e1000e" ]; then
            grep -q "kobox net: napi_gro_receive" "$serial_log"
            python3 - "$pcap_log" <<'PY'
import pathlib
import sys

pcap = pathlib.Path(sys.argv[1]).read_bytes()
request = (
    b"\xff\xff\xff\xff\xff\xff"
    b"\x52\x54\x00\x12\x34\x56"
    b"\x08\x06"
    b"\x00\x01\x08\x00\x06\x04\x00\x01"
)
reply = (
    b"\x52\x54\x00\x12\x34\x56"
    b"\x52\x55\x0a\x00\x02\x02"
    b"\x08\x06"
    b"\x00\x01\x08\x00\x06\x04\x00\x02"
)
if request not in pcap:
    raise SystemExit("kobox e1000e tx ARP frame not found in pcap")
if reply not in pcap:
    raise SystemExit("kobox e1000e ARP reply frame not found in pcap")
PY
        else
            grep -q "kobox net: napi_gro_receive" "$serial_log"
            python3 - "$pcap_log" <<'PY'
import pathlib
import sys

pcap = pathlib.Path(sys.argv[1]).read_bytes()
request = (
    b"\xff\xff\xff\xff\xff\xff"
    b"\x52\x54\x00\x12\x34\x56"
    b"\x08\x06"
    b"\x00\x01\x08\x00\x06\x04\x00\x01"
)
reply = (
    b"\x52\x54\x00\x12\x34\x56"
    b"\x52\x55\x0a\x00\x02\x02"
    b"\x08\x06"
    b"\x00\x01\x08\x00\x06\x04\x00\x02"
)
if request not in pcap:
    raise SystemExit("kobox virtio-net tx ARP frame not found in pcap")
if reply not in pcap:
    raise SystemExit("kobox virtio-net ARP reply frame not found in pcap")
PY
        fi
        ;;
esac
case "${KOBOX_NET_TX_SMOKE:-}" in
    udp|internet|both)
        grep -q "kobox net: internet_smoke" "$serial_log"
        python3 - "$pcap_log" <<'PY'
import pathlib
import sys

pcap = pathlib.Path(sys.argv[1]).read_bytes()
request = (
    b"\x52\x55\x0a\x00\x02\x02"
    b"\x52\x54\x00\x12\x34\x56"
    b"\x08\x00"
    b"\x45\x00"
    b"\x00\x39"
)
dns_query = b"\x4b\x42\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00\x07example\x03com\x00\x00\x01\x00\x01"
reply_ip = b"\x0a\x00\x02\x03\x0a\x00\x02\x0f"
reply_ports = b"\x00\x35\xc0\x00"
if request not in pcap or dns_query not in pcap:
    raise SystemExit("kobox net DNS query frame not found in pcap")
if reply_ip not in pcap or reply_ports not in pcap or b"\x4b\x42\x81" not in pcap:
    raise SystemExit("kobox net DNS reply frame not found in pcap")
PY
        ;;
esac
