#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vduse/error.h"
#include "vduse/loop.h"

// Re-export a virtio device implemented inside a UML instance.
//
// This sits parallel to vd_blk rather than beneath it. vd_blk models a
// virtio-blk device and asks a backend for storage; the bridge models
// nothing and relays descriptors. Device identity -- id, features, config
// space, queue geometry -- arrives from UML at handshake time and is fed
// straight into VDUSE, so adding a new device type means writing a UML
// shim and touching nothing here.
//
// The tradeoff is that the bridge cannot do anything a host-side backend
// could: there is no file backend on this path, and a request too large
// for UML's slot size can only be failed bluntly, since the bridge does
// not know which byte is the status byte.
struct vd_bridge;

struct vd_bridge_opts {
    // VDUSE device name; becomes /dev/vduse/$name. Required.
    const char *name;

    // Unix socket the bridge listens on for the UML instance.
    // NULL picks an abstract socket derived from name.
    const char *sock_path;

    // UML kernel binary to spawn. NULL means the caller starts UML
    // itself, in which case it must pass umvirtio.sock=<sock_path>.
    const char *uml_bin;

    // Extra UML arguments, split on spaces. umvirtio.sock= is appended
    // automatically and must not appear here.
    const char *uml_args;

    // How long to wait for UML to connect and send HELLO. Default 30000.
    unsigned handshake_timeout_ms;

    const char *vdpa_bin;    // default "vdpa"
    const char *mgmtdev;     // default "vduse"
    unsigned bus_timeout_ms; // default 10000
};

// Waits for the UML instance to hand over its identity, then creates the
// VDUSE device. Blocks for up to handshake_timeout_ms.
bool vd_bridge_new(struct vd_bridge_opts opts, struct vd_bridge **out);

// Registers descriptors with the loop and attaches to the vDPA bus.
bool vd_bridge_attach(struct vd_bridge *br, struct vd_loop *loop);

void vd_bridge_free(struct vd_bridge *br);

const char *vd_bridge_name(const struct vd_bridge *br);

// virtio device id UML declared, e.g. 2 for block.
uint32_t vd_bridge_device_id(const struct vd_bridge *br);
