#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -z "${KOBOX_LIVE_ROOT:-}" ]; then
    if [ -f "$script_dir/../../CMakeLists.txt" ]; then
        export KOBOX_REPO_ROOT=$(CDPATH= cd -- "$script_dir/../.." && pwd)
        export KOBOX_LIVE_ROOT="$KOBOX_REPO_ROOT/.artifacts/live-usb-payload/kobox-live"
    else
        export KOBOX_LIVE_ROOT=$(CDPATH= cd -- "$script_dir/.." && pwd)
    fi
fi

if [ -z "${KOBOX_REPO_ROOT:-}" ] && [ -f "$KOBOX_LIVE_ROOT/../../CMakeLists.txt" ]; then
    export KOBOX_REPO_ROOT=$(CDPATH= cd -- "$KOBOX_LIVE_ROOT/../.." && pwd)
fi

if [ -n "${KOBOX_REPO_ROOT:-}" ]; then
    export PATH="$KOBOX_REPO_ROOT/.artifacts/build-current:$PATH"
fi
export PATH="$KOBOX_LIVE_ROOT/bin:$PATH"
if [ -d "$KOBOX_LIVE_ROOT/lib" ]; then
    export LD_LIBRARY_PATH="$KOBOX_LIVE_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

if [ "$(basename -- "$0")" = "kobox-live-env.sh" ] && [ "$#" -gt 0 ]; then
    exec "$@"
fi
