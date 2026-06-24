#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build-current}"
runner="${KOBOX_RUNNER:-$build_dir/kobox-run}"
kernel_version="${KERNEL_VERSION:-6.8.0-117-generic}"
modules_root="${KOBOX_6_8_MODULES_ROOT:-$repo_root/.artifacts/kobox-linux-modules/modules-root/lib/modules/$kernel_version}"
zstd_bin="${ZSTD_BIN:-$repo_root/.artifacts/kobox-linux-modules/zstd-root/usr/bin/zstd}"
work_dir="$repo_root/.artifacts/ahci-ko"

if [ ! -x "$runner" ]; then
    echo "skip ahci ko smoke: missing kobox-run at $runner" >&2
    exit 77
fi
if [ ! -x "$zstd_bin" ]; then
    echo "skip ahci ko smoke: missing zstd at $zstd_bin" >&2
    exit 77
fi

libahci_src="$modules_root/kernel/drivers/ata/libahci.ko.zst"
ahci_src="$modules_root/kernel/drivers/ata/ahci.ko.zst"
if [ ! -f "$libahci_src" ] || [ ! -f "$ahci_src" ]; then
    echo "skip ahci ko smoke: AHCI modules not found under $modules_root" >&2
    exit 77
fi

mkdir -p "$work_dir"
"$zstd_bin" -q -d -f "$libahci_src" -o "$work_dir/libahci.ko"
"$zstd_bin" -q -d -f "$ahci_src" -o "$work_dir/ahci.ko"

log="$work_dir/ahci-ko-smoke.log"
KOBOX_TRACE_PCI="${KOBOX_TRACE_PCI:-1}" \
    "$runner" --dep="$work_dir/libahci.ko" run "$work_dir/ahci.ko" >"$log" 2>&1

grep -q "kobox pci: pci_register_driver no match driver=ahci" "$log"
grep -q "dependency .*libahci.ko has no init_module" "$log"
grep -q "init_module returned 0" "$log"
grep -q "cleanup_module returned" "$log"
echo "ahci ko smoke: $(tail -n 1 "$log")"
