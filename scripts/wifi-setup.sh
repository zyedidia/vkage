#!/bin/sh

set -ex

sudo modprobe vfio-pci
echo vfio-pci | sudo tee /sys/bus/pci/devices/0000:00:14.3/driver_override
echo 0000:00:14.3 | sudo tee /sys/bus/pci/devices/0000:00:14.3/driver/unbind
echo 0000:00:14.3 | sudo tee /sys/bus/pci/drivers_probe

KDIR=~/programming/vkage/linux-host/linux
sudo modprobe vhost_iotlb
sudo modprobe vdpa
sudo insmod $KDIR/drivers/vdpa/vdpa_user/vduse.ko
sudo modprobe virtio_vdpa
sudo insmod $KDIR/drivers/net/wireless/virtual/virtio_wlan.ko
