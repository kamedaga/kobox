#!/usr/bin/env sh
set -eu

runner="${KOBOX_EXT4_REAL_OPS_RUNNER:-.artifacts/build/kobox-ext4-real-ops}"

if [ ! -x "$runner" ]; then
    cmake -S . -B .artifacts/build -DCMAKE_C_COMPILER="${CC:-clang}"
    cmake --build .artifacts/build -j2 --target kobox-ext4-real-ops
fi

ext4_ko="${KOBOX_EXT4_KO:-.artifacts/ext4-ko/ext4.ko}"
crc16_ko="${KOBOX_CRC16_KO:-.artifacts/ext4-ko/crc16.ko}"
mbcache_ko="${KOBOX_MBCACHE_KO:-.artifacts/ext4-ko/mbcache.ko}"
jbd2_ko="${KOBOX_JBD2_KO:-.artifacts/ext4-ko/jbd2.ko}"

if [ ! -f "$ext4_ko" ]; then
    echo "skip: ext4.ko not found; set KOBOX_EXT4_KO=/path/to/ext4.ko"
    exit 77
fi

args=""
for dep in "$crc16_ko" "$mbcache_ko" "$jbd2_ko"; do
    if [ -f "$dep" ]; then
        args="$args --dep=$dep"
    fi
done

KOBOX_TRACE_FS="${KOBOX_TRACE_FS:-0}" "$runner" $args "$ext4_ko"
