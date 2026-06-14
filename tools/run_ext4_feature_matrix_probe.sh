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
        pass|pass-depth1|pass-mutation|pass-depth1-mutation)
            if [ "$status" -ne 0 ]; then
                cat "$log" >&2
                exit 1
            fi
            grep -q "module-vfs: dir_inode_operations=.*linked=1/1/1/1 file_inode_operations=.*linked=1" "$log"
            grep -q "module-vfs: readdir_result=0" "$log"
            grep -q "module-vfs: lookup name=hello.txt result=" "$log"
            grep -q "module-vfs: read_iter label=hello-initial offset=0 result=22" "$log"
            grep -q "module-vfs: write_iter label=hello-overwrite offset=0 result=22" "$log"
            grep -q "module-vfs: read_iter label=hello-post-write offset=0 result=22" "$log"
            grep -q "module-vfs: fsync label=hello-post-write start=0 end=21 datasync=0 result=0" "$log"
            grep -q "module-vfs: read_iter label=hello-eof offset=22 result=0" "$log"
            grep -q "module-vfs: image-verify label=hello-post-write path=/hello.txt offset=0 bytes=22" "$log"
            grep -q "module-vfs: lookup name=multi.txt result=" "$log"
            grep -q "module-vfs: read_iter label=multi-full offset=0 result=3072" "$log"
            grep -q "module-vfs: write_iter label=multi-boundary-write offset=900 result=512" "$log"
            grep -q "module-vfs: read_iter label=multi-boundary-post-write offset=900 result=512" "$log"
            grep -q "module-vfs: fsync label=multi-boundary-post-write start=900 end=1411 datasync=0 result=0" "$log"
            grep -q "module-vfs: image-verify label=multi-boundary-post-write path=/multi.txt offset=900 bytes=512" "$log"
            grep -q "module-vfs: lookup name=extend.bin result=" "$log"
            grep -q "module-vfs: inode_size label=extend-before size=2048" "$log"
            grep -q "module-vfs: read_iter label=extend-eof-before offset=2048 result=0" "$log"
            grep -q "module-vfs: write_iter label=extend-eof-write offset=2048 result=512" "$log"
            grep -q "module-vfs: inode_size label=extend-after size=2560" "$log"
            grep -q "module-vfs: read_iter label=extend-eof-post-write offset=2048 result=512" "$log"
            grep -q "module-vfs: fsync label=extend-eof-post-write start=2048 end=2559 datasync=0 result=0" "$log"
            case "$expected" in
                *depth1*)
                grep -q "module-vfs: lookup name=large.bin result=" "$log"
                grep -q "module-vfs: extent_depth name=large.bin depth=1" "$log"
                grep -q "module-vfs: read_iter label=large-depth1-initial offset=204800 result=1" "$log"
                grep -q "module-vfs: write_iter label=large-depth1-write offset=204800 result=512" "$log"
                grep -q "module-vfs: read_iter label=large-depth1-post-write offset=204800 result=512" "$log"
                grep -q "module-vfs: fsync label=large-depth1-post-write start=204800 end=205311 datasync=0 result=0" "$log"
                grep -q "module-vfs: image-verify label=large-depth1-post-write path=/large.bin offset=204800 bytes=512" "$log"
                    ;;
            esac
            case "$expected" in
                *mutation*)
                    grep -q "module-vfs: create name=created.txt result=0" "$log"
                    grep -q "module-vfs: lookup name=created.txt result=" "$log"
                    grep -q "module-vfs: unlink name=created.txt result=0" "$log"
                    grep -q "module-vfs: create name=rename-src.txt result=0" "$log"
                    grep -q "module-vfs: rename old=rename-src.txt new=renamed.txt result=0" "$log"
                    grep -q "module-vfs: lookup name=renamed.txt result=" "$log"
                    ;;
            esac
            ;;
        *)
            echo "unknown expected outcome: $expected" >&2
            exit 2
            ;;
    esac
}

run_case minimal "^has_journal,^extent,^64bit,^metadata_csum,^extra_isize,^dir_index" pass-mutation
run_case extents "^has_journal,extent,^64bit,^metadata_csum,^extra_isize,^dir_index" pass-depth1-mutation
run_case extents_metadata_csum "^has_journal,extent,^64bit,metadata_csum,^extra_isize,^dir_index" pass-depth1-mutation
run_case extents_extra_isize "^has_journal,extent,^64bit,^metadata_csum,extra_isize,^dir_index" pass-depth1-mutation
run_case extents_dir_index "^has_journal,extent,^64bit,^metadata_csum,^extra_isize,dir_index" pass-depth1-mutation
run_case extents_metadata_csum_extra_isize "^has_journal,extent,^64bit,metadata_csum,extra_isize,^dir_index" pass-depth1-mutation
run_case extents_dir_index_extra_isize "^has_journal,extent,^64bit,^metadata_csum,extra_isize,dir_index" pass-depth1-mutation
run_case dir_index "^has_journal,^extent,^64bit,^metadata_csum,^extra_isize,dir_index" pass-mutation
run_case extra_isize "^has_journal,^extent,^64bit,^metadata_csum,extra_isize,^dir_index" pass-mutation
run_case 64bit "^has_journal,extent,64bit,^metadata_csum,^extra_isize,^dir_index" pass-depth1-mutation
run_case 64bit_metadata_csum "^has_journal,extent,64bit,metadata_csum,^extra_isize,^dir_index" pass-depth1-mutation
run_case metadata_csum "^has_journal,^extent,^64bit,metadata_csum,^extra_isize,^dir_index" pass-mutation
run_case journal "has_journal,^extent,^64bit,^metadata_csum,^extra_isize,^dir_index" pass
run_case journal_extents "has_journal,extent,^64bit,^metadata_csum,^extra_isize,^dir_index" pass-depth1
run_case all_features "has_journal,extent,64bit,metadata_csum,extra_isize,dir_index" pass-depth1
