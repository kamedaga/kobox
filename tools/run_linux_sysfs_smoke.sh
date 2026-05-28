#!/usr/bin/env sh
set -eu

if [ ! -d /sys/bus/pci/devices ]; then
    echo "skip linux_sysfs smoke: /sys/bus/pci/devices is not available"
    exit 0
fi

"$1"
