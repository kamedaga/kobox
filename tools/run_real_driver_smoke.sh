#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build-current}"
package_name="${KERNEL_MODULES_PACKAGE:-linux-modules-6.8.0-117-generic}"
package_version="${KERNEL_MODULES_VERSION:-6.8.0-117.117}"
deb_name="${package_name}_${package_version}_amd64.deb"
module_relpath="lib/modules/6.8.0-117-generic/kernel/drivers/pci/controller/pci-hyperv-intf.ko.zst"

mkdir -p "$repo_root/.artifacts/debs"
mkdir -p "$repo_root/.artifacts/linux-modules-real"
mkdir -p "$repo_root/.artifacts/real-modules"
mkdir -p "$repo_root/.artifacts/tools"

if [ ! -f "$repo_root/.artifacts/debs/$deb_name" ]; then
    (cd "$repo_root/.artifacts/debs" && apt download "$package_name")
fi

if [ ! -x "$repo_root/.artifacts/tools/zstd-root/usr/bin/zstd" ]; then
    (cd "$repo_root/.artifacts/debs" && apt download zstd)
    dpkg-deb -x "$repo_root"/.artifacts/debs/zstd_*_amd64.deb "$repo_root/.artifacts/tools/zstd-root"
fi

dpkg-deb -x "$repo_root/.artifacts/debs/$deb_name" "$repo_root/.artifacts/linux-modules-real"

zstd="$repo_root/.artifacts/tools/zstd-root/usr/bin/zstd"
compressed="$repo_root/.artifacts/linux-modules-real/$module_relpath"
module="$repo_root/.artifacts/real-modules/pci-hyperv-intf.ko"

"$zstd" -q -d -f "$compressed" -o "$module"
"$build_dir/kobox-inspect" "$module" >/dev/null
"$build_dir/kobox-run" run "$module"

module_relpath="lib/modules/6.8.0-117-generic/kernel/drivers/net/dummy.ko.zst"
compressed="$repo_root/.artifacts/linux-modules-real/$module_relpath"
module="$repo_root/.artifacts/real-modules/dummy.ko"

"$zstd" -q -d -f "$compressed" -o "$module"
"$build_dir/kobox-inspect" "$module" >/dev/null
"$build_dir/kobox-run" run "$module"

module_relpath="lib/modules/6.8.0-117-generic/kernel/drivers/virtio/virtio_input.ko.zst"
compressed="$repo_root/.artifacts/linux-modules-real/$module_relpath"
module="$repo_root/.artifacts/real-modules/virtio_input.ko"

"$zstd" -q -d -f "$compressed" -o "$module"
"$build_dir/kobox-inspect" "$module" >/dev/null
"$build_dir/kobox-run" run "$module"

module_relpath="lib/modules/6.8.0-117-generic/kernel/drivers/uio/uio_pdrv_genirq.ko.zst"
compressed="$repo_root/.artifacts/linux-modules-real/$module_relpath"
module="$repo_root/.artifacts/real-modules/uio_pdrv_genirq.ko"

"$zstd" -q -d -f "$compressed" -o "$module"
"$build_dir/kobox-inspect" "$module" >/dev/null
"$build_dir/kobox-run" run "$module"
