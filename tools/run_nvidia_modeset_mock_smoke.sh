#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build-current}"
nvidia_module="${KOBOX_NVIDIA_KO:-}"
modeset_module="${KOBOX_NVIDIA_MODESET_KO:-}"

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

if [ -z "$nvidia_module" ] || [ ! -f "$nvidia_module" ]; then
    echo "skip nvidia modeset mock smoke: nvidia.ko was not found under .artifacts/nvidia-*" >&2
    exit 0
fi

if [ -z "$modeset_module" ] || [ ! -f "$modeset_module" ]; then
    echo "skip nvidia modeset mock smoke: nvidia-modeset.ko was not found under .artifacts/nvidia-*" >&2
    exit 0
fi

log="$repo_root/.artifacts/nvidia-modeset-mock-smoke.log"
mkdir -p "$repo_root/.artifacts"

KOBOX_TRACE_PCI="${KOBOX_TRACE_PCI:-1}" \
KOBOX_MOCK_PCI_ID="${KOBOX_MOCK_PCI_ID:-10de:25b6:03:00:00}" \
KOBOX_MOCK_NVIDIA_PROBE_COUNT="${KOBOX_MOCK_NVIDIA_PROBE_COUNT:-1}" \
    "$build_dir/kobox-run" --dep="$nvidia_module" run "$modeset_module" >"$log" 2>&1

grep -q "dependency .*nvidia\\.ko init_module returned 0" "$log"
grep -q "nvidia-modeset: Loading NVIDIA Kernel Mode Setting Driver" "$log"
grep -q "init_module returned 0" "$log"
grep -q "cleanup_module returned" "$log"
if grep -q "probe routine was not called" "$log"; then
    echo "nvidia modeset mock smoke: unexpected probe-count warning" >&2
    exit 1
fi
echo "nvidia modeset mock smoke: $(tail -n 1 "$log")"
