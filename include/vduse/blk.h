#pragma once

#include "vduse/error.h"
#include "vduse/loop.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/uio.h>

#define VD_BLK_SECTOR 512u

// GET_ID payload width.
#define VD_BLK_ID_BYTES 20u

struct vd_blk;
struct vd_blk_req;

struct vd_blk_backend {
    void *priv;

    bool (*readv)(void *priv, struct vd_blk_req *req,
            const struct iovec *iov, int n, uint64_t offset);
    bool (*writev)(void *priv, struct vd_blk_req *req,
            const struct iovec *iov, int n, uint64_t offset);
    bool (*flush)(void *priv, struct vd_blk_req *req);

    // NULL means VIRTIO_BLK_F_DISCARD is not offered.
    bool (*discard)(void *priv, struct vd_blk_req *req,
            uint64_t offset, uint64_t length);

    // Both optional.
    bool (*attach)(void *priv, struct vd_loop *loop);
    void (*destroy)(void *priv);
};

// status is a VIRTIO_BLK_S_* value. req is invalid on return.
void vd_blk_complete(struct vd_blk_req *req, uint8_t status);

struct vd_blk_opts {
    const char *name;
    uint64_t capacity;
    uint32_t block_size;
    uint32_t size_max; // max bytes per descriptor; default 128 KiB
    uint16_t queue_size;
    bool readonly;
    const char *serial;
    const char* reconnect_log;

    const char *vdpa_bin;
    const char *mgmtdev;
    unsigned bus_timeout_ms;
};

bool vd_blk_new(struct vd_blk_opts opts, struct vd_blk_backend backend, struct vd_blk **out);

bool vd_blk_attach(struct vd_blk *blk, struct vd_loop *loop);

void vd_blk_free(struct vd_blk *blk);

const char *vd_blk_name(const struct vd_blk *blk);
uint64_t vd_blk_capacity(const struct vd_blk *blk);

bool vd_blk_set_capacity(struct vd_blk *blk, uint64_t capacity);

struct vd_backend_file_opts {
    const char *path;
    bool readonly;
    bool direct; // O_DIRECT
};

bool vd_backend_file_open(struct vd_backend_file_opts opts,
        struct vd_blk_backend *out_backend, uint64_t *out_size);
