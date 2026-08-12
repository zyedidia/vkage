This is the Linux host tree for VKage. This will be used if we make
modifications to the host kernel that runs VKage drivers. At the moment this is
unused (the host kernel is still unpatched), but in the future we will use this
kernel to allow more virtio devices and features to be used from VDUSE.

Currently this tree contains a patch that enables CTRL_VQ for VDUSE network
devices.

To install the kernel

```
make
sudo make modules_install
sudo make install
```
