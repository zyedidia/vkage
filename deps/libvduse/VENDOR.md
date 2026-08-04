# Vendored libvduse

Source: QEMU `v10.0.0`, `subprojects/libvduse/`
Upstream: <https://gitlab.com/qemu-project/qemu/-/tree/v10.0.0/subprojects/libvduse>

**License: GPL-2.0-or-later.** This is why the rest of this project is also
GPL-2.0-or-later.

## Local changes

`libvduse.h` is unmodified. `libvduse.c` has three sets of local changes.

### 1. Include paths

| Upstream                                  | Here                            |
| ----------------------------------------- | ------------------------------- |
| `#include "include/atomic.h"`              | `#include "compat/atomic.h"`    |
| `#include "linux-headers/linux/virtio_ring.h"`   | `#include <linux/virtio_ring.h>`   |
| `#include "linux-headers/linux/virtio_config.h"` | `#include <linux/virtio_config.h>` |
| `#include "linux-headers/linux/vduse.h"`         | `#include <linux/vduse.h>`         |

`compat/atomic.h` is a local file, not from QEMU. libvduse.c uses exactly four
things from QEMU's atomics header — `barrier()`, `smp_mb()`, `smp_wmb()`,
`smp_rmb()` — and the shim defines those in terms of `__atomic_thread_fence`.

Everything else libvduse needs (`unlikely`, `ALIGN_UP`, `ALIGN_DOWN`) is
defined inside libvduse.c itself.

### 2. `O_CLOEXEC` on every `open()`

| Upstream                                  | Here                       |
| ----------------------------------------- | -------------------------- |
| `open(filename, O_RDWR \| O_CREAT, 0600)`  | `... \| O_CLOEXEC, 0600`   |
| `open(dev_path, O_RDWR)`                   | `... \| O_CLOEXEC`         |
| `open("/dev/vduse/control", O_RDWR)`       | `... \| O_CLOEXEC`         |

We spawn `vdpa dev add`/`dev del` as child processes. Without `O_CLOEXEC` the
device fd leaks into the child, which keeps the kernel's reference alive and
makes `VDUSE_DESTROY_DEV` fail with `EBUSY`. The control fd is never exposed
by the public API, so this cannot be fixed with `fcntl()` from outside.

### 3. `vq->log` NULL guards

Upstream assigns `vq->log` in exactly one place — `vduse_set_reconnect_log_file()`
— but dereferences it unconditionally in four functions on the mandatory path:

- `vduse_queue_check_inflights()`, reached from `vduse_queue_enable()` at
  `DRIVER_OK`
- `vduse_queue_inflight_get()`, called by `vduse_queue_pop()`
- `vduse_queue_inflight_pre_put()` and `vduse_queue_inflight_post_put()`,
  called by `vduse_queue_push()`

So a caller that does not set a reconnect log gets a NULL dereference the
moment its first virtqueue is enabled. QEMU never hits this because its
vduse-blk export always sets one.

Each site now returns early when `vq->log` is NULL. In `check_inflights` the
guard wraps only the log-dependent blocks rather than returning early, so
`vq->shadow_avail_idx`/`vq->last_avail_idx` are still initialised from
`vq->used_idx` and `vduse_inject_irq()` still runs.

### 4. Pointer arithmetic in `vduse_queue_read_indirect_desc()`

Upstream advances the destination cursor with `desc += read_len`, where `desc`
is a `struct vring_desc *` and `read_len` is a **byte** count — so the pointer
moves 16x too far:

```c
        memcpy(desc, ori_desc, read_len);
        len -= read_len;
        addr += read_len;
        desc += read_len;          /* upstream */
```

The loop only iterates more than once when `iova_to_va()` returns a short
length, i.e. when the indirect descriptor table straddles an IOVA region
boundary. That is precisely the condition under which `vduse_queue_map_desc()`
calls this function at all, so on every real invocation the second `memcpy()`
writes past the caller's `desc_buf[VIRTQUEUE_MAX_SIZE]` stack array. Reachable
because `vduse_get_virtio_features()` advertises `VIRTIO_RING_F_INDIRECT_DESC`.

Now:

```c
        desc = (struct vring_desc *)((char *)desc + read_len);
```

Same bug pattern exists in QEMU's `libvhost-user.c`; worth reporting upstream.

## Re-vendoring

```sh
V=v10.0.0   # or newer
B=https://gitlab.com/qemu-project/qemu/-/raw/$V/subprojects/libvduse
curl -sSLO $B/libvduse.c -O $B/libvduse.h
sed -i \
  -e 's|#include "include/atomic.h"|#include "compat/atomic.h"|' \
  -e 's|#include "linux-headers/linux/virtio_ring.h"|#include <linux/virtio_ring.h>|' \
  -e 's|#include "linux-headers/linux/virtio_config.h"|#include <linux/virtio_config.h>|' \
  -e 's|#include "linux-headers/linux/vduse.h"|#include <linux/vduse.h>|' \
  libvduse.c
```

Then re-check that no new QEMU-relative includes appeared:

```sh
grep -nE '#include "(include|linux-headers)/' libvduse.c   # should print nothing
```
