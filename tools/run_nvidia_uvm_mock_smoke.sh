#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build-current}"
nvidia_module="${KOBOX_NVIDIA_KO:-}"
uvm_module="${KOBOX_NVIDIA_UVM_KO:-}"

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

if [ -z "$nvidia_module" ] || [ ! -f "$nvidia_module" ]; then
    echo "skip nvidia uvm mock smoke: nvidia.ko was not found under .artifacts/nvidia-*" >&2
    exit 0
fi

if [ -z "$uvm_module" ] || [ ! -f "$uvm_module" ]; then
    echo "skip nvidia uvm mock smoke: nvidia-uvm.ko was not found under .artifacts/nvidia-*" >&2
    exit 0
fi

log="$repo_root/.artifacts/nvidia-uvm-mock-smoke.log"
mkdir -p "$repo_root/.artifacts"

KOBOX_TRACE_PCI="${KOBOX_TRACE_PCI:-1}" \
KOBOX_MOCK_PCI_ID="${KOBOX_MOCK_PCI_ID:-10de:25b6:03:00:00}" \
KOBOX_MOCK_NVIDIA_PROBE_COUNT="${KOBOX_MOCK_NVIDIA_PROBE_COUNT:-1}" \
    "$build_dir/kobox-run" --dep="$nvidia_module" run "$uvm_module" >"$log" 2>&1

grep -q "dependency .*nvidia\\.ko init_module returned 0" "$log"
grep -q "nvidia-uvm: Loaded the UVM driver" "$log"
grep -q "init_module returned 0" "$log"
grep -q "cleanup_module returned" "$log"
if grep -q "probe routine was not called" "$log"; then
    echo "nvidia uvm mock smoke: unexpected probe-count warning" >&2
    exit 1
fi
echo "nvidia uvm mock smoke: $(tail -n 1 "$log")"
