#!/usr/bin/env sh
set -eu

runner="${KOBOX_RUNNER:-${1:-.artifacts/build/kobox-run}}"
ext4_ko="${KOBOX_EXT4_KO:-}"
deps="${KOBOX_EXT4_DEPS:-}"
work_dir=".artifacts/ext4-probe"

if [ ! -x "$runner" ]; then
    echo "skip: kobox-run not found; set KOBOX_RUNNER=/path/to/kobox-run"
    exit 77
fi

if [ -z "$ext4_ko" ]; then
    for candidate in \
        .artifacts/ext4-ko/ext4.ko \
        ../.artifacts/kobox-modules/ext4.ko \
        ../.artifacts/userland-fixtures/kobox-modules/ext4.ko \
        /lib/modules/"$(uname -r)"/kernel/fs/ext4/ext4.ko \
        /lib/modules/"$(uname -r)"/kernel/fs/ext4/ext4.ko.gz \
        /lib/modules/"$(uname -r)"/kernel/fs/ext4/ext4.ko.zst \
        /usr/lib/modules/"$(uname -r)"/kernel/fs/ext4/ext4.ko \
        /usr/lib/modules/"$(uname -r)"/kernel/fs/ext4/ext4.ko.gz \
        /usr/lib/modules/"$(uname -r)"/kernel/fs/ext4/ext4.ko.zst
    do
        if [ -f "$candidate" ]; then
            ext4_ko="$candidate"
            break
        fi
    done
fi

if [ -z "$ext4_ko" ] || [ ! -f "$ext4_ko" ]; then
    echo "skip: ext4.ko not found; set KOBOX_EXT4_KO=/path/to/ext4.ko"
    exit 77
fi

mkdir -p "$work_dir"

case "$ext4_ko" in
    *.gz)
        gzip -dc "$ext4_ko" > "$work_dir/ext4.ko"
        ext4_ko="$work_dir/ext4.ko"
        ;;
    *.zst)
        if ! command -v zstd >/dev/null 2>&1; then
            echo "skip: zstd not found for $ext4_ko"
            exit 77
        fi
        zstd -q -d -f "$ext4_ko" -o "$work_dir/ext4.ko"
        ext4_ko="$work_dir/ext4.ko"
        ;;
esac

find_module() {
    rel="$1"
    base=$(basename "$rel")
    for candidate in "../.artifacts/kobox-modules/$base" "../.artifacts/userland-fixtures/kobox-modules/$base"; do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    for root in /lib/modules/"$(uname -r)" /usr/lib/modules/"$(uname -r)"; do
        for suffix in "" ".gz" ".zst"; do
            candidate="$root/$rel$suffix"
            if [ -f "$candidate" ]; then
                printf '%s\n' "$candidate"
                return 0
            fi
        done
    done
    return 1
}

if [ -z "$deps" ]; then
    for rel in \
        kernel/lib/crc16.ko \
        kernel/fs/mbcache.ko \
        kernel/fs/jbd2/jbd2.ko
    do
        dep_candidate=$(find_module "$rel" || true)
        if [ -n "$dep_candidate" ]; then
            deps="${deps:+$deps }$dep_candidate"
        fi
    done
fi

if ! command -v mkfs.ext4 >/dev/null 2>&1; then
    echo "skip: mkfs.ext4 not found"
    exit 77
fi
if ! command -v debugfs >/dev/null 2>&1; then
    echo "skip: debugfs not found"
    exit 77
fi

image="$work_dir/ext4-smoke.img"
seed_dir="$work_dir/seed"
rm -rf "$seed_dir"
mkdir -p "$seed_dir"
printf 'kobox host ext4 smoke original data\n' > "$seed_dir/kobox-smoke.txt"
awk 'BEGIN { for (i = 0; i < 600; i++) printf "0123456789abcdef" }' > "$seed_dir/kobox-large.txt"
awk 'BEGIN { for (i = 0; i < 65536; i++) printf "%08x%08x\n", i, 65535 - i }' > "$seed_dir/kobox-ldlike.txt"
printf 'kobox zero truncate payload\n' > "$seed_dir/kobox-zero.txt"
rm -f "$image"
truncate -s 64M "$image"
mkfs.ext4 -q -F -b 4096 -d "$seed_dir" "$image"

resolve_inode() {
    debugfs -R "stat $1" "$image" 2>/dev/null |
    awk '/^Inode:/ { print $2; exit }'
}

inode=$(resolve_inode /kobox-smoke.txt)
large_inode=$(resolve_inode /kobox-large.txt)
ldlike_inode=$(resolve_inode /kobox-ldlike.txt)
zero_inode=$(resolve_inode /kobox-zero.txt)
if [ -z "$inode" ] || [ -z "$large_inode" ] || [ -z "$ldlike_inode" ] || [ -z "$zero_inode" ]; then
    echo "failed: could not resolve smoke inodes"
    exit 1
fi
initial_large_blocks=$(
    debugfs -R 'stat /kobox-large.txt' "$image" 2>/dev/null |
    awk '/Blockcount:/ { for (i = 1; i <= NF; i++) if ($i == "Blockcount:") { print $(i + 1); exit } }'
)
initial_zero_blocks=$(
    debugfs -R 'stat /kobox-zero.txt' "$image" 2>/dev/null |
    awk '/Blockcount:/ { for (i = 1; i <= NF; i++) if ($i == "Blockcount:") { print $(i + 1); exit } }'
)

args=""
for dep in $deps; do
    case "$dep" in
        *.gz)
            dep_out="$work_dir/$(basename "$dep" .gz)"
            gzip -dc "$dep" > "$dep_out"
            dep="$dep_out"
            ;;
        *.zst)
            if ! command -v zstd >/dev/null 2>&1; then
                echo "skip: zstd not found for $dep"
                exit 77
            fi
            dep_out="$work_dir/$(basename "$dep" .zst)"
            zstd -q -d -f "$dep" -o "$dep_out"
            dep="$dep_out"
            ;;
    esac
    args="$args --dep=$dep"
done

echo "runner: $runner"
echo "ext4: $ext4_ko"
echo "image: $image"
echo "inode: $inode"
echo "large inode: $large_inode"
echo "ldlike inode: $ldlike_inode"
echo "zero inode: $zero_inode"
echo "large blocks: $initial_large_blocks"
echo "zero blocks: $initial_zero_blocks"
if [ -n "$deps" ]; then
    echo "deps: $deps"
fi

# shellcheck disable=SC2086
KOBOX_TRACE_MODULES="${KOBOX_TRACE_MODULES:-0}" \
KOBOX_EXT4_IMAGE_SMOKE="$image" \
KOBOX_EXT4_IMAGE_SMOKE_INODE="$inode" \
KOBOX_EXT4_IMAGE_SMOKE_LARGE_INODE="$large_inode" \
KOBOX_EXT4_IMAGE_SMOKE_LDLIKE_INODE="$ldlike_inode" \
KOBOX_EXT4_IMAGE_SMOKE_ZERO_INODE="$zero_inode" \
"$runner" $args run "$ext4_ko"

if [ "${KOBOX_EXT4_IMAGE_SMOKE_READ_ONLY:-0}" != "0" ] && [ -n "${KOBOX_EXT4_IMAGE_SMOKE_READ_ONLY:-}" ]; then
    echo "kobox-ext4-smoke: read-only persistence checks skipped"
    exit 0
fi

persisted=$(
    debugfs -R 'cat /kobox-smoke.txt' "$image" 2>/dev/null || true
)
if [ "$persisted" != "kobox-ho" ]; then
    echo "failed: persisted content mismatch"
    printf '%s\n' "$persisted"
    exit 1
fi
smoke_size=$(
    debugfs -R 'stat /kobox-smoke.txt' "$image" 2>/dev/null |
    awk '/Size:/ { for (i = 1; i <= NF; i++) if ($i == "Size:") { print $(i + 1); exit } }'
)
if [ "$smoke_size" != "8" ]; then
    echo "failed: truncate size mismatch size=$smoke_size"
    exit 1
fi
echo "kobox-ext4-smoke: truncate persistence ok size=$smoke_size"

smoke_mode=$(
    debugfs -R 'stat /kobox-smoke.txt' "$image" 2>/dev/null |
    awk '/Mode:/ { for (i = 1; i <= NF; i++) if ($i == "Mode:") { print $(i + 1); exit } }'
)
if [ "$smoke_mode" != "0600" ]; then
    echo "failed: chmod persistence mismatch mode=$smoke_mode"
    debugfs -R 'stat /kobox-smoke.txt' "$image" 2>/dev/null || true
    exit 1
fi
if ! debugfs -R 'stat /kobox-smoke.txt' "$image" 2>/dev/null | grep -q 'mtime: 0x00017ca5:00000000'; then
    echo "failed: utimens persistence mismatch"
    debugfs -R 'stat /kobox-smoke.txt' "$image" 2>/dev/null || true
    exit 1
fi
echo "kobox-ext4-smoke: metadata persistence ok mode=$smoke_mode mtime=1970-01-02T03:04:05Z"

large_size=$(
    debugfs -R 'stat /kobox-large.txt' "$image" 2>/dev/null |
    awk '/Size:/ { for (i = 1; i <= NF; i++) if ($i == "Size:") { print $(i + 1); exit } }'
)
if [ "$large_size" != "4096" ]; then
    echo "failed: large truncate size mismatch size=$large_size"
    exit 1
fi
large_blocks=$(
    debugfs -R 'stat /kobox-large.txt' "$image" 2>/dev/null |
    awk '/Blockcount:/ { for (i = 1; i <= NF; i++) if ($i == "Blockcount:") { print $(i + 1); exit } }'
)
if [ -z "$initial_large_blocks" ] || [ -z "$large_blocks" ] || [ "$large_blocks" -ge "$initial_large_blocks" ]; then
    echo "failed: large truncate blockcount did not shrink before=$initial_large_blocks after=$large_blocks"
    exit 1
fi
echo "kobox-ext4-smoke: large truncate persistence ok size=$large_size blocks=$large_blocks"

zero_size=$(
    debugfs -R 'stat /kobox-zero.txt' "$image" 2>/dev/null |
    awk '/Size:/ { for (i = 1; i <= NF; i++) if ($i == "Size:") { print $(i + 1); exit } }'
)
if [ "$zero_size" != "0" ]; then
    echo "failed: zero truncate size mismatch size=$zero_size"
    exit 1
fi
zero_content=$(
    debugfs -R 'cat /kobox-zero.txt' "$image" 2>/dev/null || true
)
if [ -n "$zero_content" ]; then
    echo "failed: zero truncate content mismatch"
    printf '%s\n' "$zero_content"
    exit 1
fi
zero_blocks=$(
    debugfs -R 'stat /kobox-zero.txt' "$image" 2>/dev/null |
    awk '/Blockcount:/ { for (i = 1; i <= NF; i++) if ($i == "Blockcount:") { print $(i + 1); exit } }'
)
if [ -z "$zero_blocks" ] || [ "$zero_blocks" != "0" ]; then
    echo "failed: zero truncate blockcount mismatch before=$initial_zero_blocks after=$zero_blocks"
    exit 1
fi
echo "kobox-ext4-smoke: zero truncate persistence ok size=$zero_size blocks=$zero_blocks"

if debugfs -R 'stat /kobox-created.txt' "$image" 2>/dev/null | grep -q '^Inode:'; then
    echo "failed: created path still exists after ext4 unlink"
    exit 1
fi
if debugfs -R 'stat /kobox-renamed.txt' "$image" 2>/dev/null | grep -q '^Inode:'; then
    echo "failed: renamed path still exists after ext4 unlink"
    exit 1
fi
if ! debugfs -R 'stat /kobox-smoke.txt' "$image" 2>/dev/null | grep -q '^Inode:'; then
    echo "failed: original smoke path missing after ext4 dir ops"
    exit 1
fi
if debugfs -R 'stat /kobox-created-dir' "$image" 2>/dev/null | grep -q '^Inode:'; then
    echo "failed: mkdir path still exists after ext4 rmdir"
    exit 1
fi
echo "kobox-ext4-smoke: directory consistency ok"

if ! command -v e2fsck >/dev/null 2>&1; then
    echo "skip: e2fsck not found"
    exit 77
fi
e2fsck -fn "$image"
echo "kobox-ext4-smoke: fsck ok"
