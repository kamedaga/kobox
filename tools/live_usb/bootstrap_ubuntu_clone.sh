#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build-current}"
debs_dir="$repo_root/.artifacts/debs"
nvidia_root="$repo_root/.artifacts/nvidia-535/root"

sudo_cmd=""
if [ "$(id -u)" != "0" ]; then
    sudo_cmd="sudo"
fi

run_root() {
    if [ -n "$sudo_cmd" ]; then
        $sudo_cmd "$@"
    else
        "$@"
    fi
}

have_cmd() {
    command -v "$1" >/dev/null 2>&1
}

echo "kobox bootstrap: installing host packages"
run_root apt update
run_root apt install -y git clang cmake make build-essential pkg-config zstd

echo "kobox bootstrap: configuring/building"
cmake -S "$repo_root" -B "$build_dir" -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$build_dir"
ctest --test-dir "$build_dir" --output-on-failure

mkdir -p "$debs_dir" "$nvidia_root"

download_pkg() {
    pkg="$1"
    if ls "$debs_dir"/"$pkg"_*.deb >/dev/null 2>&1; then
        return
    fi
    echo "kobox bootstrap: apt download $pkg"
    (cd "$debs_dir" && apt download "$pkg")
}

nvidia_kernel="${KOBOX_NVIDIA_KERNEL:-6.8.0-117-generic}"
nvidia_branch="${KOBOX_NVIDIA_BRANCH:-535}"

download_pkg "linux-objects-nvidia-$nvidia_branch-$nvidia_kernel"
download_pkg "linux-modules-nvidia-$nvidia_branch-$nvidia_kernel"
download_pkg "linux-signatures-nvidia-$nvidia_kernel"
download_pkg "nvidia-kernel-common-$nvidia_branch"

echo "kobox bootstrap: extracting NVIDIA modules"
for deb in "$debs_dir"/*.deb; do
    case "$(basename "$deb")" in
        linux-objects-nvidia-"$nvidia_branch"-"$nvidia_kernel"_*.deb|\
        linux-modules-nvidia-"$nvidia_branch"-"$nvidia_kernel"_*.deb|\
        linux-signatures-nvidia-"$nvidia_kernel"_*.deb|\
        nvidia-kernel-common-"$nvidia_branch"_*.deb)
            dpkg-deb -x "$deb" "$nvidia_root"
            ;;
    esac
done

module=""
for candidate in \
    "$nvidia_root/lib/modules/$nvidia_kernel/kernel/nvidia-$nvidia_branch/bits/nvidia.ko" \
    "$nvidia_root/lib/modules/$nvidia_kernel/kernel/nvidia-$nvidia_branch/nvidia.ko"
do
    if [ -f "$candidate" ]; then
        module="$candidate"
        break
    fi
done

if [ -z "$module" ]; then
    echo "kobox bootstrap: nvidia.ko was not extracted" >&2
    echo "Check whether Ubuntu apt has linux-objects-nvidia-$nvidia_branch-$nvidia_kernel." >&2
    exit 1
fi

if have_cmd "$build_dir/kobox-inspect"; then
    "$build_dir/kobox-inspect" "$module" >/dev/null
fi

echo "kobox bootstrap: ready"
echo "  module: $module"
echo ""
echo "Next on the Live Ubuntu host:"
echo "  sudo $repo_root/tools/live_usb/kobox-live-nvidia-vfio.sh status"
echo "  sudo $repo_root/tools/live_usb/kobox-live-nvidia-vfio.sh bind"
echo "  sudo $repo_root/tools/live_usb/kobox-live-nvidia-vfio.sh run"
