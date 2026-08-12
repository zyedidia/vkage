#!/usr/bin/env python3
# Destroy a VDUSE device left behind by a driver domain that was killed
# rather than shut down. There is no command-line tool for this ioctl.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import ctypes, errno, fcntl, os, sys

VDUSE_DESTROY_DEV = 0x41008103  # _IOW(VDUSE_BASE=0x81, 0x03, char[256])

if len(sys.argv) != 2:
    sys.exit(f"usage: {os.path.basename(sys.argv[0])} NAME")

name = sys.argv[1]
try:
    fd = os.open("/dev/vduse/control", os.O_RDWR)
    fcntl.ioctl(fd, VDUSE_DESTROY_DEV, ctypes.create_string_buffer(name.encode(), 256))
except OSError as e:
    hint = {errno.EBUSY: " (run 'vdpa dev del' first, and check the UML has exited)",
            errno.EINVAL: " (no such device -- already gone?)"}.get(e.errno, "")
    sys.exit(f"{name}: {e.strerror}{hint}")
