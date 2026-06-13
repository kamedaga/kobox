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

run_case() {
    name="$1"
    features="$2"
    expected="$3"
    work_dir=".artifacts/ext4-feature-matrix-$name"
    image="$work_dir/probe.img"
    log="$work_dir/probe.log"
    rm -rf "$work_dir"
    mkdir -p "$work_dir"

    echo "kobox-ext4-feature-matrix: fixture=$name expected=$expected features=$features"
    set +e
    KOBOX_TRACE_FS="${KOBOX_TRACE_FS:-0}" "$runner" $args \
        --work-dir="$work_dir" \
        --image="$image" \
        --mkfs-features="$features" \
        "$ext4_ko" >"$log" 2>&1
    status=$?
    set -e

    case "$expected" in
        pass)
            if [ "$status" -ne 0 ]; then
                cat "$log" >&2
                exit 1
            fi
            grep -q "module-vfs: readdir_result=0" "$log"
            grep -q "module-vfs: lookup name=hello.txt result=" "$log"
            grep -q "module-vfs: read_iter label=hello-initial offset=0 result=22" "$log"
            grep -q "module-vfs: write_iter label=hello-overwrite offset=0 result=22" "$log"
            grep -q "module-vfs: read_iter label=hello-post-write offset=0 result=22" "$log"
            grep -q "module-vfs: lookup name=multi.txt result=" "$log"
            grep -q "module-vfs: read_iter label=multi-full offset=0 result=3072" "$log"
            grep -q "module-vfs: write_iter label=multi-boundary-write offset=900 result=512" "$log"
            grep -q "module-vfs: read_iter label=multi-boundary-post-write offset=900 result=512" "$log"
            ;;
        *)
            echo "unknown expected outcome: $expected" >&2
            exit 2
            ;;
    esac
}

run_case minimal "^has_journal,^extent,^64bit,^metadata_csum,^extra_isize,^dir_index" pass
run_case extents "^has_journal,extent,^64bit,^metadata_csum,^extra_isize,^dir_index" pass
run_case dir_index "^has_journal,^extent,^64bit,^metadata_csum,^extra_isize,dir_index" pass
run_case extra_isize "^has_journal,^extent,^64bit,^metadata_csum,extra_isize,^dir_index" pass
run_case 64bit "^has_journal,extent,64bit,^metadata_csum,^extra_isize,^dir_index" pass
run_case metadata_csum "^has_journal,^extent,^64bit,metadata_csum,^extra_isize,^dir_index" pass
run_case journal "has_journal,^extent,^64bit,^metadata_csum,^extra_isize,^dir_index" pass
