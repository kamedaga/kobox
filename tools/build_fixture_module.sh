#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixture_dir="$repo_root/tests/fixtures/linux_module"
kdir="${KDIR:-/lib/modules/$(uname -r)/build}"

if [ ! -e "$kdir/Makefile" ]; then
    echo "kernel build tree not found: $kdir" >&2
    echo "set KDIR=/path/to/kernel/build or install matching kernel headers" >&2
    exit 1
fi

make -C "$kdir" M="$fixture_dir" modules
