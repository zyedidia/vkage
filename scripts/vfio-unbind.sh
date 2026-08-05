#!/usr/bin/env bash
# Return a PCI device from vfio-pci to its native host driver.
#
# Reverse of vfio-bind.sh.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

PROG=${0##*/}

usage() {
    cat >&2 <<EOF
usage: $PROG <pci-address>

Clear the vfio-pci override on a PCI device and let the kernel rebind it
to whichever driver matches, e.g. handing an NVMe back to the host so it
reappears as /dev/nvmeXn1.

Stop whatever is using the device first; a process holding /dev/vfio/<group>
open will keep it claimed.
EOF
    exit 2
}

[ $# -eq 1 ] || usage
ADDR=$1

if ! [[ $ADDR =~ ^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$ ]]; then
    echo "$PROG: '$ADDR' is not a PCI address like 0000:01:00.0" >&2
    exit 1
fi

DEV=/sys/bus/pci/devices/$ADDR

[ -d "$DEV" ] || { echo "$PROG: no such PCI device: $ADDR" >&2; exit 1; }
[ "$(id -u)" -eq 0 ] || { echo "$PROG: must run as root" >&2; exit 1; }

driver_of() {
    local d
    d=$(readlink -f "/sys/bus/pci/devices/$1/driver" 2>/dev/null) || return 0
    [ -n "$d" ] && basename "$d"
}

CURRENT=$(driver_of "$ADDR")

if [ -n "${CURRENT:-}" ] && [ "$CURRENT" != "vfio-pci" ]; then
    echo "$PROG: $ADDR is bound to $CURRENT, not vfio-pci; nothing to do"
    exit 0
fi

# Warn if the group node is still open, since the unbind will fail in a
# way that is easy to misread.
if [ -e "$DEV/iommu_group" ]; then
    GROUP=$(basename "$(readlink -f "$DEV/iommu_group")")
    holders=$(
        for fd in /proc/[0-9]*/fd/*; do
            tgt=$(readlink "$fd" 2>/dev/null) || continue
            [ "$tgt" = "/dev/vfio/$GROUP" ] || continue
            pid=${fd#/proc/}; pid=${pid%%/*}
            printf '%s ' "$pid"
        done 2>/dev/null | tr ' ' '\n' | sort -u | tr '\n' ' '
    )
    if [ -n "${holders// /}" ]; then
        echo "$PROG: /dev/vfio/$GROUP is still open by pid(s): $holders" >&2
        echo "$PROG: stop them first" >&2
        exit 1
    fi
fi

# Clearing the override has to come first, or drivers_probe just picks
# vfio-pci straight back up.
echo > "$DEV/driver_override"

if [ -n "${CURRENT:-}" ]; then
    echo "$PROG: unbinding from $CURRENT"
    echo "$ADDR" > "$DEV/driver/unbind"
fi

echo "$ADDR" > /sys/bus/pci/drivers_probe

NEW=$(driver_of "$ADDR")

if [ -z "${NEW:-}" ]; then
    echo "$PROG: $ADDR is now unbound; no native driver matched it" >&2
    exit 1
fi

echo "$PROG: $ADDR bound to $NEW"

for b in /sys/block/*; do
    [ -e "$b/device" ] || continue
    case "$(readlink -f "$b/device")" in
        */"$ADDR"/*|*/"$ADDR") echo "  block device: /dev/$(basename "$b")" ;;
    esac
done
