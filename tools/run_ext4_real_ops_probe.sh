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

run_fixture() {
    name="$1"
    features="$2"
    work_dir=".artifacts/ext4-real-ops-$name"
    image="$work_dir/probe.img"
    echo "kobox-ext4-real-ops: fixture=$name features=$features"
    KOBOX_TRACE_FS="${KOBOX_TRACE_FS:-0}" "$runner" $args \
        --work-dir="$work_dir" \
        --image="$image" \
        --mkfs-features="$features" \
        "$ext4_ko"
}

fixtures="${KOBOX_EXT4_FIXTURES:-minimal extents dir_index extra_isize 64bit metadata_csum journal all_features}"
for fixture in $fixtures; do
    case "$fixture" in
        minimal)
            run_fixture "$fixture" "^has_journal,^extent,^64bit,^metadata_csum,^extra_isize,^dir_index"
            ;;
        dir_index)
            run_fixture "$fixture" "^has_journal,^extent,^64bit,^metadata_csum,^extra_isize,dir_index"
            ;;
        extents)
            run_fixture "$fixture" "^has_journal,extent,^64bit,^metadata_csum,^extra_isize,^dir_index"
            ;;
        extents_metadata_csum)
            run_fixture "$fixture" "^has_journal,extent,^64bit,metadata_csum,^extra_isize,^dir_index"
            ;;
        extents_extra_isize)
            run_fixture "$fixture" "^has_journal,extent,^64bit,^metadata_csum,extra_isize,^dir_index"
            ;;
        extents_dir_index)
            run_fixture "$fixture" "^has_journal,extent,^64bit,^metadata_csum,^extra_isize,dir_index"
            ;;
        extents_metadata_csum_extra_isize)
            run_fixture "$fixture" "^has_journal,extent,^64bit,metadata_csum,extra_isize,^dir_index"
            ;;
        extents_dir_index_extra_isize)
            run_fixture "$fixture" "^has_journal,extent,^64bit,^metadata_csum,extra_isize,dir_index"
            ;;
        extra_isize)
            run_fixture "$fixture" "^has_journal,^extent,^64bit,^metadata_csum,extra_isize,^dir_index"
            ;;
        metadata_csum)
            run_fixture "$fixture" "^has_journal,^extent,^64bit,metadata_csum,^extra_isize,^dir_index"
            ;;
        metadata_csum_extra_isize)
            run_fixture "$fixture" "^has_journal,^extent,^64bit,metadata_csum,extra_isize,^dir_index"
            ;;
        64bit)
            run_fixture "$fixture" "^has_journal,extent,64bit,^metadata_csum,^extra_isize,^dir_index"
            ;;
        64bit_metadata_csum)
            run_fixture "$fixture" "^has_journal,extent,64bit,metadata_csum,^extra_isize,^dir_index"
            ;;
        journal)
            run_fixture "$fixture" "has_journal,^extent,^64bit,^metadata_csum,^extra_isize,^dir_index"
            ;;
        journal_extents)
            run_fixture "$fixture" "has_journal,extent,^64bit,^metadata_csum,^extra_isize,^dir_index"
            ;;
        all_features)
            run_fixture "$fixture" "has_journal,extent,64bit,metadata_csum,extra_isize,dir_index"
            ;;
        dir_index_extra_isize)
            run_fixture "$fixture" "^has_journal,^extent,^64bit,^metadata_csum,extra_isize,dir_index"
            ;;
        *)
            echo "unknown KOBOX_EXT4_FIXTURES entry: $fixture" >&2
            exit 2
            ;;
    esac
done
