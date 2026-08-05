# VKage

VKage is a tool that can be used to create virtio devices that are backed by
Linux device drivers running in userspace.

# Installation

```
meson setup build
ninja -C build
```

Make sure the appropriate kernel modules are installed:

```
sudo modprobe -a virtio_vdpa vduse vfio-pci
```

# Examples

## NVMe

This example assumes that there is an NVMe device at PCI address
`0000:01:00.0`. You can use `./scripts/vfio-bind.sh 0000:01:00.0` to relinquish
control from the kernel and expose it as a VFIO device. Then run the following
command to expose a virtio-blk device that is backed by the VFIO device,
controlled by the Linux NVMe driver running in a userspace instance of UML.

```
sudo ./build/vkage-uml --name nvme0 --uml ../linux-uml/build-nvme/linux --uml-args 'mem=256M vfio_uml.device=0000:01:00.0 umvirtio_blk.disk=259:0 umvirtio_blk.readonly=1'
```

The drive should become available as `/dev/vda` (or similar).

## Network

This example assumes there is a two-port Intel I350 (`igb`) at PCI addresses
`0000:01:00.0` and `0000:01:00.1` with a cable connecting them. Both ports
share an IOMMU group, so both must be bound and passed through together:

```
sudo ./scripts/vfio-bind.sh 0000:01:00.0
sudo ./scripts/vfio-bind.sh 0000:01:00.1
```

Then expose port 0 as a virtio-net device, backed by the Linux igb driver
running inside UML:

```
sudo ./build/vkage-uml --name igb0 --uml ../linux-uml/build-igb/linux --uml-args 'mem=256M vfio_uml.device=0000:01:00.0 vfio_uml.device=0000:01:00.1 umvirtio_net.dev=eth0'
```

A virtio-net interface appears on the host with the port's real MAC
address. Configure it as usual (`ip addr add ... dev eth0`, `ip link set
eth0 up`). Link state follows the physical port.
