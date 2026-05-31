#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build-current}"
nvidia_module="${KOBOX_NVIDIA_KO:-}"
modeset_module="${KOBOX_NVIDIA_MODESET_KO:-}"
uvm_module="${KOBOX_NVIDIA_UVM_KO:-}"
drm_module="${KOBOX_NVIDIA_DRM_KO:-}"

if [ -z "$nvidia_module" ]; then
    for candidate in \
        "$repo_root"/.artifacts/nvidia-*/root/lib/modules/*/kernel/nvidia-*/bits/nvidia.ko \
        "$repo_root"/.artifacts/nvidia-*/root/lib/modules/*/kernel/nvidia-*/nvidia.ko
    do
        if [ -f "$candidate" ]; then
            nvidia_module="$candidate"
            break
        fi
    done
fi

if [ -z "$modeset_module" ]; then
    for candidate in \
        "$repo_root"/.artifacts/nvidia-*/root/lib/modules/*/kernel/nvidia-*/bits/nvidia-modeset.ko \
        "$repo_root"/.artifacts/nvidia-*/root/lib/modules/*/kernel/nvidia-*/nvidia-modeset.ko
    do
        if [ -f "$candidate" ]; then
            modeset_module="$candidate"
            break
        fi
    done
fi

if [ -z "$uvm_module" ]; then
    for candidate in \
        "$repo_root"/.artifacts/nvidia-*/root/lib/modules/*/kernel/nvidia-*/bits/nvidia-uvm.ko \
        "$repo_root"/.artifacts/nvidia-*/root/lib/modules/*/kernel/nvidia-*/nvidia-uvm.ko
    do
        if [ -f "$candidate" ]; then
            uvm_module="$candidate"
            break
        fi
    done
fi

if [ -z "$drm_module" ]; then
    for candidate in \
        "$repo_root"/.artifacts/nvidia-*/root/lib/modules/*/kernel/nvidia-*/bits/nvidia-drm.ko \
        "$repo_root"/.artifacts/nvidia-*/root/lib/modules/*/kernel/nvidia-*/nvidia-drm.ko
    do
        if [ -f "$candidate" ]; then
            drm_module="$candidate"
            break
        fi
    done
fi

if [ -z "$nvidia_module" ] || [ ! -f "$nvidia_module" ]; then
    echo "skip nvidia stack mock smoke: nvidia.ko was not found under .artifacts/nvidia-*" >&2
    exit 0
fi

if [ -z "$modeset_module" ] || [ ! -f "$modeset_module" ]; then
    echo "skip nvidia stack mock smoke: nvidia-modeset.ko was not found under .artifacts/nvidia-*" >&2
    exit 0
fi

if [ -z "$uvm_module" ] || [ ! -f "$uvm_module" ]; then
    echo "skip nvidia stack mock smoke: nvidia-uvm.ko was not found under .artifacts/nvidia-*" >&2
    exit 0
fi

if [ -z "$drm_module" ] || [ ! -f "$drm_module" ]; then
    echo "skip nvidia stack mock smoke: nvidia-drm.ko was not found under .artifacts/nvidia-*" >&2
    exit 0
fi

log="$repo_root/.artifacts/nvidia-stack-mock-smoke.log"
mkdir -p "$repo_root/.artifacts"

KOBOX_TRACE_PCI="${KOBOX_TRACE_PCI:-1}" \
KOBOX_TRACE_MODULES="${KOBOX_TRACE_MODULES:-1}" \
KOBOX_TRACE_SHIM_CALLS="${KOBOX_TRACE_SHIM_CALLS:-1}" \
KOBOX_TRACE_SHIM_CALLS_TOP="${KOBOX_TRACE_SHIM_CALLS_TOP:-128}" \
KOBOX_TRACE_INTERNAL="${KOBOX_TRACE_INTERNAL:+$KOBOX_TRACE_INTERNAL,}uvm_va_range_create_mmap" \
KOBOX_TRACE_KERNEL_OBJECTS="${KOBOX_TRACE_KERNEL_OBJECTS:-1}" \
KOBOX_FOPS_SMOKE="${KOBOX_FOPS_SMOKE:-1}" \
KOBOX_MOCK_PCI_ID="${KOBOX_MOCK_PCI_ID:-10de:25b6:03:00:00}" \
KOBOX_MOCK_NVIDIA_PROBE_COUNT="${KOBOX_MOCK_NVIDIA_PROBE_COUNT:-1}" \
    "$build_dir/kobox-run" \
        --dep="$nvidia_module" \
        --dep="$modeset_module" \
        --dep="$uvm_module" \
        run "$drm_module" >"$log" 2>&1

grep -q "dependency .*nvidia\\.ko init_module returned 0" "$log"
grep -q "dependency .*nvidia-modeset\\.ko init_module returned 0" "$log"
grep -q "dependency .*nvidia-uvm\\.ko init_module returned 0" "$log"
grep -q "nvidia-modeset: Loading NVIDIA Kernel Mode Setting Driver" "$log"
grep -q "nvidia-uvm: Loaded the UVM driver" "$log"
grep -q "loaded .*nvidia-drm\\.ko" "$log"
grep -q "init_module returned 0" "$log"
grep -q "cleanup_module returned" "$log"
grep -q "dependency .*nvidia-uvm\\.ko cleanup_module returned" "$log"
grep -q "dependency .*nvidia-modeset\\.ko cleanup_module returned" "$log"
grep -q "dependency .*nvidia\\.ko cleanup_module returned" "$log"
grep -q "kobox-shim-calls: module=.*nvidia\\.ko total=" "$log"
grep -q "kobox-shim-calls: module=.*nvidia-modeset\\.ko total=" "$log"
grep -q "kobox-shim-calls: module=.*nvidia-uvm\\.ko total=" "$log"
grep -q "kobox-shim-calls: module=.*nvidia-drm\\.ko total=" "$log"
grep -q "kobox-shim-calls: module=.*nvidia-drm\\.ko symbol=drm_dev_register count=1" "$log"
grep -q "kobox-kobj: register_chrdev .*name=nvidia" "$log"
grep -q "kobox-kobj: proc_create .*path=/proc/driver/nvidia" "$log"
grep -q "kobox-kobj-summary: chrdev .*name=nvidia" "$log"
grep -q "kobox-kobj-summary: proc .*path=/proc/driver/nvidia" "$log"
grep -q "kobox-fops-summary: kind=chrdev name=nvidia-frontend" "$log"
grep -q "kobox-fops-summary: kind=cdev name=nvidia-uvm" "$log"
grep -q "kobox-fops-summary: kind=proc path=/proc/driver/nvidia/version" "$log"
grep -q "kobox-fops-smoke: target=proc:/proc/driver/nvidia/version op=open" "$log"
grep -q "kobox-fops-smoke: target=proc:/proc/driver/nvidia/version op=read" "$log"
grep -q "kobox-fops-smoke: target=chrdev:nvidia-frontend op=open" "$log"
grep -q "kobox-fops-smoke: target=chrdev:nvidia-frontend op=ioctl name=NV_ESC_CARD_INFO" "$log"
grep -q "kobox-fops-smoke: target=chrdev:nvidia-frontend op=ioctl name=NV_ESC_CHECK_VERSION_STR" "$log"
grep -q "kobox-fops-smoke: target=chrdev:nvidia-frontend op=ioctl name=NV_ESC_SYS_PARAMS" "$log"
grep -q "kobox-fops-smoke: target=cdev:nvidia-uvm:0 op=open" "$log"
grep -q "kobox-fops-smoke: target=cdev:nvidia-uvm:0 op=ioctl name=UVM_INITIALIZE" "$log"
grep -q "kobox-fops-smoke: target=cdev:nvidia-uvm:0 op=ioctl name=UVM_INITIALIZE rmStatus=0x0" "$log"
grep -q "kobox-fops-smoke: target=cdev:nvidia-uvm:0 op=ioctl name=UVM_PAGEABLE_MEM_ACCESS" "$log"
grep -q "kobox-fops-smoke: target=cdev:nvidia-uvm:0 op=ioctl name=UVM_PAGEABLE_MEM_ACCESS rmStatus=0x0" "$log"
grep -q "kobox-fops-smoke: target=cdev:nvidia-uvm:0 op=ioctl name=UVM_PAGEABLE_MEM_ACCESS_ON_GPU" "$log"
grep -q "kobox-fops-smoke: target=cdev:nvidia-uvm:0 op=mmap .* result=0" "$log"
grep -q "kobox-trace: uvm_va_range_create_mmap rdi=" "$log"
grep -q "kobox-trace: uvm_va_range_create_mmap returned 0x0 (0)" "$log"
grep -q "kobox-fops-smoke: target=cdev:nvidia-uvm:0 op=ioctl name=UVM_DEINITIALIZE" "$log"
grep -q "kobox-fd: filp_open path=/sys/bus/pci/devices/.*/config" "$log"
if grep -q "probe routine was not called" "$log"; then
    echo "nvidia stack mock smoke: unexpected probe-count warning" >&2
    exit 1
fi
echo "nvidia stack mock smoke: $(tail -n 1 "$log")"
