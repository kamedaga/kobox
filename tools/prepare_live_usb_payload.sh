#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${BUILD_DIR:-$repo_root/.artifacts/build-current}"
payload_root="${PAYLOAD_ROOT:-$repo_root/.artifacts/live-usb-payload}"
payload="$payload_root/kobox-live"
tmp_payload="$payload.tmp"

require_file() {
    if [ ! -e "$1" ]; then
        echo "missing: $1" >&2
        exit 1
    fi
}

copy_binary() {
    src="$1"
    dst="$payload/bin/$(basename "$src")"
    require_file "$src"
    cp "$src" "$dst"
    chmod +x "$dst"
    if command -v ldd >/dev/null 2>&1; then
        ldd "$src" 2>/dev/null | awk '
            $2 == "=>" && $3 ~ /^\// { print $3 }
            $1 ~ /^\// { print $1 }
        ' | sort -u | while IFS= read -r dep; do
            [ -n "$dep" ] || continue
            cp "$dep" "$payload/lib/$(basename "$dep")" 2>/dev/null || true
        done
    fi
}

case "$payload_root" in
    "$repo_root"/.artifacts/*) ;;
    *)
        echo "PAYLOAD_ROOT must stay under $repo_root/.artifacts" >&2
        exit 1
        ;;
esac

rm -rf "$tmp_payload"
mkdir -p "$tmp_payload/bin" "$tmp_payload/lib" "$tmp_payload/modules/nvidia-535" "$tmp_payload/scripts" "$payload_root"
payload="$tmp_payload"

copy_binary "$build_dir/kobox-run"
copy_binary "$build_dir/kobox-ls-devices"
copy_binary "$build_dir/kobox-inspect"
copy_binary "$build_dir/kobox-vfio-nvme-smoke"

nvidia_bits="$repo_root/.artifacts/nvidia-535/root/lib/modules/6.8.0-117-generic/kernel/nvidia-535/bits"
if [ -d "$nvidia_bits" ]; then
    cp "$nvidia_bits"/nvidia*.ko "$payload/modules/nvidia-535/" 2>/dev/null || true
else
    echo "warning: NVIDIA module directory not found: $nvidia_bits" >&2
fi

cp "$repo_root/tools/live_usb/kobox-live-env.sh" "$payload/scripts/"
cp "$repo_root/tools/live_usb/kobox-live-nvidia-vfio.sh" "$payload/scripts/"
chmod +x "$payload/scripts/"*.sh

printf '%s\n' "kobox live payload" >"$payload/manifest.txt"
printf 'created_from=%s\n' "$repo_root" >>"$payload/manifest.txt"
printf 'build_dir=%s\n' "$build_dir" >>"$payload/manifest.txt"
printf 'created_at_utc=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" >>"$payload/manifest.txt"

rm -rf "$payload_root/kobox-live"
mv "$tmp_payload" "$payload_root/kobox-live"
echo "$payload_root/kobox-live"
