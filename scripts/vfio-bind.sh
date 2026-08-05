#!/usr/bin/env bash
# Detach a PCI device from its host driver and bind it to vfio-pci.
#
# Reverse with vfio-unbind.sh.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

PROG=${0##*/}

usage() {
    cat >&2 <<EOF
usage: $PROG <pci-address>

Bind a PCI device to vfio-pci so a userspace driver or VM can claim it.
The address is a full domain:bus:slot.function, e.g. 0000:01:00.0; see
'lspci -D' for the list.

Refuses to act if any block device behind the target is mounted or in use
as swap, so the disk you are booted from cannot be taken away by accident.
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

driver_of() {
    local d
    d=$(readlink -f "/sys/bus/pci/devices/$1/driver" 2>/dev/null) || return 0
    [ -n "$d" ] && basename "$d"
}

# Block devices sitting behind this PCI address, if any.
block_devices() {
    local b
    for b in /sys/block/*; do
        [ -e "$b/device" ] || continue
        case "$(readlink -f "$b/device")" in
            */"$ADDR"/*|*/"$ADDR") basename "$b" ;;
        esac
    done
}

# ---- refuse if anything behind the device is in use -------------------

in_use=""

for name in $(block_devices); do
    while read -r dev mnt; do
        [ -n "${mnt:-}" ] && in_use+="  /dev/$dev is mounted at $mnt"$'\n'
    done < <(lsblk -nro NAME,MOUNTPOINT "/dev/$name" 2>/dev/null || true)

    while read -r swapdev _; do
        case "$swapdev" in
            /dev/"$name"|/dev/"$name"[p0-9]*)
                in_use+="  $swapdev is active swap"$'\n' ;;
        esac
    done < <(swapon --show=NAME --noheadings 2>/dev/null || true)
done

if [ -n "$in_use" ]; then
    echo "$PROG: refusing to bind $ADDR, it is in use:" >&2
    printf '%s' "$in_use" >&2
    echo "$PROG: unmount / swapoff those first" >&2
    exit 1
fi

# Checked after the in-use test so that pointing this at a busy disk says
# so, rather than complaining about privileges first.
[ "$(id -u)" -eq 0 ] || { echo "$PROG: must run as root" >&2; exit 1; }

# ---- report the IOMMU group ------------------------------------------

if [ ! -e "$DEV/iommu_group" ]; then
    echo "$PROG: $ADDR has no IOMMU group; is an iommu enabled on the" \
         "kernel command line?" >&2
    exit 1
fi

GROUP=$(basename "$(readlink -f "$DEV/iommu_group")")
echo "$PROG: $ADDR is in IOMMU group $GROUP"

# Everything in a group is passed through together. Bridges are exempt
# from VFIO's viability check, anything else has to be bound too.
for peer in /sys/kernel/iommu_groups/"$GROUP"/devices/*; do
    paddr=$(basename "$peer")
    [ "$paddr" = "$ADDR" ] && continue

    pclass=$(cat "/sys/bus/pci/devices/$paddr/class" 2>/dev/null || echo "")
    pdrv=$(driver_of "$paddr")

    case "$pclass" in
        0x06*)
            echo "  group peer $paddr (bridge, driver=${pdrv:-none}) - ok"
            ;;
        *)
            echo "  group peer $paddr (driver=${pdrv:-none}) is NOT a bridge;" \
                 "VFIO will likely need it bound too" >&2
            ;;
    esac
done

# ---- bind -------------------------------------------------------------

CURRENT=$(driver_of "$ADDR")

if [ "${CURRENT:-}" = "vfio-pci" ]; then
    echo "$PROG: $ADDR is already bound to vfio-pci"
    exit 0
fi

modprobe vfio-pci

# driver_override first, so drivers_probe cannot hand it back to the
# native driver in the window after unbind.
echo vfio-pci > "$DEV/driver_override"

if [ -n "${CURRENT:-}" ]; then
    echo "$PROG: unbinding from $CURRENT"
    echo "$ADDR" > "$DEV/driver/unbind"
fi

echo "$ADDR" > /sys/bus/pci/drivers_probe

NEW=$(driver_of "$ADDR")
if [ "${NEW:-}" != "vfio-pci" ]; then
    echo "$PROG: bind failed, driver is now '${NEW:-none}'" >&2
    exit 1
fi

echo "$PROG: $ADDR bound to vfio-pci"
ls -l "/dev/vfio/$GROUP"
