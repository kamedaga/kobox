#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build-current}"
modules_root="${KOBOX_6_8_MODULES_ROOT:-$repo_root/.artifacts/linux-modules-6.8.0-117/lib/modules/6.8.0-117-generic}"
zstd_bin="${ZSTD_BIN:-$repo_root/.artifacts/tools/zstd-root/usr/bin/zstd}"
work_dir="$repo_root/.artifacts/usb-missing"

if [ ! -x "$build_dir/kobox-run" ]; then
    echo "skip xhci pci mock smoke: missing kobox-run in $build_dir" >&2
    exit 0
fi

xhci_src="$modules_root/kernel/drivers/usb/host/xhci-pci.ko.zst"
if [ ! -f "$xhci_src" ]; then
    echo "skip xhci pci mock smoke: xhci modules were not found under $modules_root" >&2
    exit 0
fi
if [ ! -x "$zstd_bin" ]; then
    echo "skip xhci pci mock smoke: zstd was not found at $zstd_bin" >&2
    exit 0
fi

mkdir -p "$work_dir"
"$zstd_bin" -q -d -f "$xhci_src" -o "$work_dir/xhci-pci.ko"

log="$work_dir/xhci-pci-mock-smoke.log"
KOBOX_TRACE_PCI="${KOBOX_TRACE_PCI:-1}" \
KOBOX_TRACE_USB="${KOBOX_TRACE_USB:-1}" \
KOBOX_TRACE_MODULES="${KOBOX_TRACE_MODULES:-1}" \
KOBOX_TRACE_SHIM_CALLS="${KOBOX_TRACE_SHIM_CALLS:-1}" \
KOBOX_TRACE_SHIM_CALLS_TOP="${KOBOX_TRACE_SHIM_CALLS_TOP:-96}" \
KOBOX_MOCK_PCI_ID="${KOBOX_MOCK_PCI_ID:-1912:0014:0c:03:30}" \
    "$build_dir/kobox-run" \
        run "$work_dir/xhci-pci.ko" >"$log" 2>&1

grep -q "kobox pci: pci_register_driver" "$log"
grep -q "kobox usb: usb_hcd_pci_probe" "$log"
grep -q "init_module returned 0" "$log"
grep -q "cleanup_module returned" "$log"
grep -q "kobox usb: usb_hcd_pci_remove" "$log"
echo "xhci pci mock smoke: $(tail -n 1 "$log")"
