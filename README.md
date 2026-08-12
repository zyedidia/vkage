# VKage

VKage is a tool that can be used to create virtio devices that are backed by
Linux device drivers running in userspace.

* linux-uml holds the source code for the UML guest. There are significant
  modifications here.
* linux-host holds the source code for the host kernel. Most modifications here
  are contained to kernel modules so they can be reloaded without rebooting the
  host kernel (after installing a custom kernel in the host).

Run `./download.sh --shallow` to download the two Linux kernels.

## Example: NVMe driver

Make sure the appropriate kernel modules are installed:

```
sudo modprobe -a vhost_iotlb vdpa vduse virtio_vdpa vfio-pci
```

Build the UML:

```
cd linux-uml/linux
make ARCH=um O=../build-nvme vkage_nvme_defconfig
make ARCH=um O=../build-nvme -j$(nproc)
```

Bind the NVMe to vfio-pci. Use `lspci -D` to discover the PCI address of the
device.

```
sudo scripts/vfio-bind.sh 0000:01:00.0
```

Start the UML (as root since it needs /dev/vfio/$GROUP and
/dev/vduse/control). It creates /dev/vduse/nvme0.

```
sudo ./linux-uml/build-nvme/linux mem=256M vfio_uml.device=0000:01:00.0 umvduse.name=nvme0 umvduse_blk.disk=259:0 con=fd:0,fd:1
```

From another terminal, attach it to the vDPA bus.

```
sudo vdpa dev add name nvme0 mgmtdev vduse
```

The new virtual block device should be visible from `lsblk`.

To shutdown:

```
sudo vdpa dev del nvme0
sudo pkill -f build-nvme/linux # kill the UML instance
sudo ./scripts/vduse-destroy.py nvme0
sudo ./scripts/vfio-unbind.sh 0000:01:00.0
```

The destroy step is optional if you rerun with the same name: the UML
clears its own leftover device at boot.

## Example: WiFi driver

This example assumes the following device:

```
00:14.3 Network controller: Intel Corporation Cannon Lake PCH CNVi WiFi (rev 10)
        DeviceName: Onboard - Ethernet
        Subsystem: Intel Corporation Wireless-AC 9560
        Kernel driver in use: iwlwifi
        Kernel modules: iwlwifi
```

For WiFi you need a custom VDUSE module and the virtio-wlan module. This means
you need to build them from linux-host and install them with `insmod`. You will
also need to install the full custom kernel from linux-host, since the modules
will not be compatible otherwise.

Once you have the custom kernel installed on your host, make sure the
appropriate kernel modules are installed:

```
sudo modprobe -a vhost_iotlb vdpa vfio-pci
sudo insmod linux-host/linux/drivers/vdpa/vdpa_user/vduse.ko
sudo modprobe virtio_vdpa
sudo insmod linux-host/linux/drivers/net/wireless/virtual/virtio_wlan.ko
```

Build the UML:

```
cd linux-uml/linux
make ARCH=um O=../build-9560 vkage_9560_defconfig
make ARCH=um O=../build-9560 -j$(nproc)
```

Bind the WiFi to vfio-pci. Use `lspci -D` to discover the PCI address of the
device.

```
sudo scripts/vfio-bind.sh 0000:00:14.3
```

Start the UML:

```
sudo ./linux-uml/build-9560/linux mem=256M vfio_uml.device=0000:00:14.3 con0=fd:0,fd:1 con=null
```

Add the vDPA device:

```
sudo vdpa dev add name wlan mgmtdev vduse
```

The wlan interface should appear in `ip link show` and if you have a network
manager it may automatically start scanning for WiFi networks.
