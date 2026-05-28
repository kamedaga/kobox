#!/usr/bin/env sh
set -eu

runner="$1"
shift

ran=0
for module in "$@"; do
    if [ ! -f "$module" ]; then
        echo "skip missing fixture: $module"
        continue
    fi
    ran=1
    "$runner" run "$module"
done

if [ "$ran" -eq 0 ]; then
    echo "no fixture modules built; run tools/build_fixture_module.sh to enable this test"
fi
