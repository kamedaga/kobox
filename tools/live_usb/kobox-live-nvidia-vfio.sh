#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$script_dir/kobox-live-env.sh"

state_dir="${KOBOX_STATE_DIR:-/tmp/kobox-live}"
target_bdf="${KOBOX_GPU_BDF:-}"

die() {
    echo "error: $*" >&2
    exit 1
}

require_root() {
    if [ "$(id -u)" != "0" ]; then
        die "root is required for VFIO bind/run"
    fi
}

sysfs_device() {
    printf '/sys/bus/pci/devices/%s' "$1"
}

read_trimmed() {
    tr -d '\n' <"$1"
}

driver_name() {
    dev_path=$(sysfs_device "$1")
    if [ -L "$dev_path/driver" ]; then
        basename "$(readlink "$dev_path/driver")"
    else
        printf 'none'
    fi
}

find_nvidia_gpu() {
    if [ -n "$target_bdf" ]; then
        [ -d "$(sysfs_device "$target_bdf")" ] || die "PCI device not found: $target_bdf"
        printf '%s\n' "$target_bdf"
        return
    fi

    for dev_path in /sys/bus/pci/devices/*; do
        [ -f "$dev_path/vendor" ] || continue
        vendor=$(read_trimmed "$dev_path/vendor")
        class=$(read_trimmed "$dev_path/class")
        case "$vendor:$class" in
            0x10de:0x03*) basename "$dev_path"; return ;;
        esac
    done

    die "NVIDIA display/3D PCI device was not found; set KOBOX_GPU_BDF=0000:xx:yy.z"
}

group_for_bdf() {
    dev_path=$(sysfs_device "$1")
    [ -L "$dev_path/iommu_group" ] || die "IOMMU group is missing for $1"
    basename "$(readlink "$dev_path/iommu_group")"
}

show_status() {
    bdf=$(find_nvidia_gpu)
    dev_path=$(sysfs_device "$bdf")
    group=$(group_for_bdf "$bdf" 2>/dev/null || true)

    echo "kobox-live: gpu=$bdf"
    echo "  vendor=$(read_trimmed "$dev_path/vendor") device=$(read_trimmed "$dev_path/device") class=$(read_trimmed "$dev_path/class")"
    echo "  driver=$(driver_name "$bdf")"
    if [ -n "$group" ]; then
        echo "  iommu_group=$group"
        for peer in /sys/kernel/iommu_groups/"$group"/devices/*; do
            [ -e "$peer" ] || continue
            echo "    group_device=$(basename "$peer") driver=$(driver_name "$(basename "$peer")")"
        done
    else
        echo "  iommu_group=missing"
    fi
}

ensure_vfio() {
    modprobe vfio || true
    modprobe vfio_iommu_type1 || true
    modprobe vfio-pci || modprobe vfio_pci || true
    [ -e /dev/vfio/vfio ] || die "/dev/vfio/vfio is missing; VFIO is not available"
}

ensure_group_node() {
    group="$1"
    [ -e "/dev/vfio/$group" ] && return
    [ -r "/sys/class/vfio/$group/dev" ] || return
    devno=$(cat "/sys/class/vfio/$group/dev")
    major=${devno%:*}
    minor=${devno#*:}
    mknod "/dev/vfio/$group" c "$major" "$minor" 2>/dev/null || true
}

bind_one_vfio() {
    bdf="$1"
    dev_path=$(sysfs_device "$bdf")
    vendor=$(read_trimmed "$dev_path/vendor")
    device=$(read_trimmed "$dev_path/device")
    old_driver=$(driver_name "$bdf")
    printf '%s\n' "$old_driver" >"$state_dir/$bdf.driver"

    if [ "$old_driver" != "none" ] && [ "$old_driver" != "vfio-pci" ]; then
        echo "$bdf" >"$dev_path/driver/unbind"
    fi

    printf '%s %s\n' "${vendor#0x}" "${device#0x}" >/sys/bus/pci/drivers/vfio-pci/new_id 2>/dev/null || true
    echo "$bdf" >/sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null || true
}

bind_vfio() {
    die "runtime GPU bind is intentionally disabled. Reserve the GPU at boot with vfio-pci.ids=vendor:device and use status/run only."
}

restore_driver() {
    require_root
    if [ ! -d "$state_dir" ]; then
        die "state directory not found: $state_dir"
    fi

    for state in "$state_dir"/*.driver; do
        [ -e "$state" ] || continue
        bdf=$(basename "$state" .driver)
        [ -d "$(sysfs_device "$bdf")" ] || continue
        old_driver=$(cat "$state")
        if [ "$(driver_name "$bdf")" = "vfio-pci" ]; then
            echo "$bdf" >/sys/bus/pci/drivers/vfio-pci/unbind 2>/dev/null || true
        fi
        if [ "$old_driver" != "none" ] && [ -d "/sys/bus/pci/drivers/$old_driver" ]; then
            echo "$bdf" >"/sys/bus/pci/drivers/$old_driver/bind" 2>/dev/null || true
        fi
        echo "kobox-live: restored $bdf to $(driver_name "$bdf")"
    done
}

find_nvidia_module_dir() {
    module_dir="${KOBOX_NVIDIA_MODULE_DIR:-}"
    if [ -z "$module_dir" ]; then
        for candidate in \
            "$KOBOX_LIVE_ROOT/modules/nvidia-535" \
            "${KOBOX_REPO_ROOT:-}/.artifacts/nvidia-535/root/lib/modules/6.8.0-117-generic/kernel/nvidia-535/bits" \
            "${KOBOX_REPO_ROOT:-}/.artifacts/nvidia-535/root/lib/modules/6.8.0-117-generic/kernel/nvidia-535"
        do
            if [ -f "$candidate/nvidia.ko" ]; then
                module_dir="$candidate"
                break
            fi
        done
    fi
    if [ -z "$module_dir" ] || [ ! -f "$module_dir/nvidia.ko" ]; then
        return 1
    fi
    printf '%s\n' "$module_dir"
}

check_modules() {
    module_dir=$(find_nvidia_module_dir) ||
        die "NVIDIA module directory was not found; run tools/live_usb/bootstrap_ubuntu_clone.sh first"
    echo "kobox-live: nvidia module directory=$module_dir"
    echo "kobox-live: nvidia module=$module_dir/nvidia.ko"
}

run_kobox() {
    require_root
    bdf=$(find_nvidia_gpu)
    module_dir=$(find_nvidia_module_dir) ||
        die "NVIDIA module directory was not found; run tools/live_usb/bootstrap_ubuntu_clone.sh first"
    module="$module_dir/nvidia.ko"
    [ "$(driver_name "$bdf")" = "vfio-pci" ] || die "$bdf is not bound to vfio-pci; run: $0 bind"

    kobox-ls-devices vfio "$bdf"
    KOBOX_TRACE_PCI="${KOBOX_TRACE_PCI:-1}" \
    KOBOX_CRASH_STACK="${KOBOX_CRASH_STACK:-1}" \
        kobox-run --backend=vfio --pci="$bdf" run "$module"
}

case "${1:-status}" in
    status)
        show_status
        ;;
    bind)
        bind_vfio
        ;;
    check-modules)
        check_modules
        ;;
    run)
        run_kobox
        ;;
    restore)
        restore_driver
        show_status
        ;;
    *)
        echo "usage: $0 [status|check-modules|run|restore]" >&2
        exit 1
        ;;
esac
