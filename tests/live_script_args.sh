#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

sh -n "$repo_root/tools/live_usb/kobox-live-env.sh"
sh -n "$repo_root/tools/live_usb/kobox-live-nvidia-vfio.sh"

output=$(
    sh "$repo_root/tools/live_usb/kobox-live-nvidia-vfio.sh" __kobox_invalid_command__ 2>&1 || true
)

case "$output" in
    *"usage: "*"kobox-live-nvidia-vfio.sh [status|check-modules|run|restore]"*) ;;
    *)
        printf '%s\n' "$output" >&2
        exit 1
        ;;
esac

output=$(
    sh "$repo_root/tools/live_usb/kobox-live-nvidia-vfio.sh" bind 2>&1 || true
)

case "$output" in
    *"runtime GPU bind is intentionally disabled"*) ;;
    *)
        printf '%s\n' "$output" >&2
        exit 1
        ;;
esac

output=$(
    sh -c '. "$1"; printf "%s\n" "$1"' sh "$repo_root/tools/live_usb/kobox-live-env.sh" 2>&1
)

case "$output" in
    *"kobox-live-env.sh"*) ;;
    *)
        printf '%s\n' "$output" >&2
        exit 1
        ;;
esac

missing_root=$(mktemp -d)
trap 'rm -rf "$missing_root" "${present_root:-}"' EXIT

output=$(
    KOBOX_LIVE_ROOT="$missing_root/live" KOBOX_REPO_ROOT="$missing_root/repo" \
        sh "$repo_root/tools/live_usb/kobox-live-nvidia-vfio.sh" check-modules 2>&1 || true
)

case "$output" in
    *"NVIDIA module directory was not found"*) ;;
    *)
        printf '%s\n' "$output" >&2
        exit 1
        ;;
esac

present_root=$(mktemp -d)
mkdir -p "$present_root/modules/nvidia-535"
: >"$present_root/modules/nvidia-535/nvidia.ko"

output=$(
    KOBOX_LIVE_ROOT="$present_root" \
        sh "$repo_root/tools/live_usb/kobox-live-nvidia-vfio.sh" check-modules
)

case "$output" in
    *"nvidia module=$present_root/modules/nvidia-535/nvidia.ko"*) ;;
    *)
        printf '%s\n' "$output" >&2
        exit 1
        ;;
esac
