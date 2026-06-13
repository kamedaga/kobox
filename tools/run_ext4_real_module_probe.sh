#!/usr/bin/env sh
set -eu

runner="${1:-.artifacts/build/kobox-run}"
ext4_ko="${KOBOX_EXT4_KO:-}"
deps="${KOBOX_EXT4_DEPS:-}"

if [ -z "$ext4_ko" ]; then
    for candidate in \
        .artifacts/ext4-ko/ext4.ko \
        /lib/modules/"$(uname -r)"/kernel/fs/ext4/ext4.ko \
        /lib/modules/"$(uname -r)"/kernel/fs/ext4/ext4.ko.gz \
        /usr/lib/modules/"$(uname -r)"/kernel/fs/ext4/ext4.ko \
        /usr/lib/modules/"$(uname -r)"/kernel/fs/ext4/ext4.ko.gz
    do
        if [ -f "$candidate" ]; then
            ext4_ko="$candidate"
            break
        fi
    done
fi

if [ -z "$ext4_ko" ] || [ ! -f "$ext4_ko" ]; then
    echo "skip: ext4.ko not found; set KOBOX_EXT4_KO=/path/to/ext4.ko"
    exit 0
fi

work_dir=".artifacts/ext4-probe"
mkdir -p "$work_dir"

case "$ext4_ko" in
    *.gz)
        gzip -dc "$ext4_ko" > "$work_dir/ext4.ko"
        ext4_ko="$work_dir/ext4.ko"
        ;;
esac

args=""
for dep in $deps; do
    case "$dep" in
        *.gz)
            dep_out="$work_dir/$(basename "$dep" .gz)"
            gzip -dc "$dep" > "$dep_out"
            dep="$dep_out"
            ;;
    esac
    args="$args --dep=$dep"
done

echo "runner: $runner"
echo "ext4: $ext4_ko"
if [ -n "$deps" ]; then
    echo "deps: $deps"
fi

# shellcheck disable=SC2086
KOBOX_TRACE_MODULES="${KOBOX_TRACE_MODULES:-1}" "$runner" $args run "$ext4_ko"
