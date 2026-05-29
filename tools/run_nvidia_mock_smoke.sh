#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build-current}"
module="${KOBOX_NVIDIA_KO:-}"

if [ -z "$module" ]; then
    for candidate in \
        "$repo_root"/.artifacts/nvidia-*/root/lib/modules/*/kernel/nvidia-*/bits/nvidia.ko \
        "$repo_root"/.artifacts/nvidia-*/root/lib/modules/*/kernel/nvidia-*/nvidia.ko
    do
        if [ -f "$candidate" ]; then
            module="$candidate"
            break
        fi
    done
fi

if [ -z "$module" ] || [ ! -f "$module" ]; then
    echo "skip nvidia mock smoke: nvidia.ko was not found under .artifacts/nvidia-*" >&2
    exit 0
fi

log="$repo_root/.artifacts/nvidia-mock-smoke.log"
mkdir -p "$repo_root/.artifacts"

KOBOX_TRACE_PCI="${KOBOX_TRACE_PCI:-1}" \
KOBOX_MOCK_PCI_ID="${KOBOX_MOCK_PCI_ID:-10de:25a2:03:00:00}" \
    "$build_dir/kobox-run" run "$module" >"$log" 2>&1

grep -q "init_module returned" "$log"
grep -q "cleanup_module skipped after failed init_module" "$log"
echo "nvidia mock smoke: $(tail -n 1 "$log")"
