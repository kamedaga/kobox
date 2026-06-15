#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build-current}"
runner="${KOBOX_RUNNER:-$build_dir/kobox-run}"
kernel_version="${KERNEL_VERSION:-6.8.0-117-generic}"
modules_root="${KOBOX_6_8_MODULES_ROOT:-$repo_root/.artifacts/qemu-vfio-nvme-ext4/modules-root/lib/modules/$kernel_version}"
zstd_bin="${ZSTD_BIN:-$repo_root/.artifacts/qemu-vfio-nvme-ext4/zstd-root/usr/bin/zstd}"
work_dir="$repo_root/.artifacts/ahci-ko"

if [ ! -x "$runner" ]; then
    echo "skip ahci mock probe smoke: missing kobox-run at $runner" >&2
    exit 77
fi
if [ ! -x "$zstd_bin" ]; then
    echo "skip ahci mock probe smoke: missing zstd at $zstd_bin" >&2
    exit 77
fi

libahci_src="$modules_root/kernel/drivers/ata/libahci.ko.zst"
ahci_src="$modules_root/kernel/drivers/ata/ahci.ko.zst"
if [ ! -f "$libahci_src" ] || [ ! -f "$ahci_src" ]; then
    echo "skip ahci mock probe smoke: AHCI modules not found under $modules_root" >&2
    exit 77
fi

mkdir -p "$work_dir"
"$zstd_bin" -q -d -f "$libahci_src" -o "$work_dir/libahci.ko"
"$zstd_bin" -q -d -f "$ahci_src" -o "$work_dir/ahci.ko"

log="$work_dir/ahci-mock-probe-smoke.log"
KOBOX_TRACE_PCI="${KOBOX_TRACE_PCI:-1}" \
KOBOX_TRACE_ATA="${KOBOX_TRACE_ATA:-1}" \
KOBOX_TRACE_IRQ="${KOBOX_TRACE_IRQ:-1}" \
KOBOX_ATA_IO_SMOKE="${KOBOX_ATA_IO_SMOKE:-1}" \
KOBOX_MOCK_PCI_ID="${KOBOX_MOCK_PCI_ID:-8086:2922:01:06:01}" \
    "$runner" --dep="$work_dir/libahci.ko" run "$work_dir/ahci.ko" >"$log" 2>&1

grep -q "kobox pci: pci_register_driver driver=ahci vendor=0x8086 device=0x2922 class=0x10601" "$log"
grep -q "kobox pci: pci_iomap bar=5" "$log"
grep -q "AHCI 0001.0300 1 slots 1 ports 3 Gbps 0x1 impl SATA mode" "$log"
grep -q "kobox ata: host_activate .* n_ports=1 active_ports=1 irq=0" "$log"
grep -q "kobox ata: host_start" "$log"
grep -q "kobox ata: host_register" "$log"
grep -q "kobox irq: request irq=0" "$log"
test "$(grep -c "kobox ahci: pxci write .* result=0" "$log")" -eq 7
test "$(grep -c "kobox ahci: irq dispatch .* result=0" "$log")" -eq 7
test "$(grep -c "kobox irq: trigger .* irq=0" "$log")" -eq 7
grep -q "kobox-ata-scsi-block: .* registered=1 result=0" "$log"
grep -q "kobox-ahci-block: .* block_reads=1 block_writes=1 ahci_block_reads=1 ahci_block_writes=1 result=0" "$log"
grep -q "kobox-ata-identify: .* model=KOBOX SATA DISK" "$log"
grep -q "kobox-ata-scsi-queuecmd: .* queued=6 done=6 synthetic=3 linux_view=3 .* bytes=512 linux_bytes=512 result=0" "$log"
grep -q "kobox-ata-io: .* result=0" "$log"
grep -q "kobox-ahci-engine: .* identify=1 reads=3 writes=3 completions=7 errors=0 prdts=11 bytes=5632 irq_dispatches=7 irq_errors=0 pxci=0x00000000 .* result=0" "$log"
grep -q "kobox pci: pci_set_master" "$log"
grep -q "kobox pci: pci_unregister_driver probed=1" "$log"
grep -q "kobox irq: free-all irq=0" "$log"
grep -q "init_module returned 0" "$log"
grep -q "cleanup_module returned" "$log"
echo "ahci mock probe smoke: $(grep "AHCI 0001.0300" "$log" | tail -n 1)"
