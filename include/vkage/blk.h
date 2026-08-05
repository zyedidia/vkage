#pragma once

#include "vkage/error.h"
#include "vkage/loop.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/uio.h>

#define VK_BLK_SECTOR 512u

// GET_ID payload width.
#define VK_BLK_ID_BYTES 20u

struct vk_blk;
struct vk_blk_req;

struct vk_blk_backend {
    void *priv;

    bool (*readv)(void *priv, struct vk_blk_req *req,
            const struct iovec *iov, int n, uint64_t offset);
    bool (*writev)(void *priv, struct vk_blk_req *req,
            const struct iovec *iov, int n, uint64_t offset);
    bool (*flush)(void *priv, struct vk_blk_req *req);

    // NULL means VIRTIO_BLK_F_DISCARD is not offered.
    bool (*discard)(void *priv, struct vk_blk_req *req,
            uint64_t offset, uint64_t length);

    // Both optional.
    bool (*attach)(void *priv, struct vk_loop *loop);
    void (*destroy)(void *priv);
};

// status is a VIRTIO_BLK_S_* value. req is invalid on return.
void vk_blk_complete(struct vk_blk_req *req, uint8_t status);

struct vk_blk_opts {
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

bool vk_blk_new(struct vk_blk_opts opts, struct vk_blk_backend backend, struct vk_blk **out);

bool vk_blk_attach(struct vk_blk *blk, struct vk_loop *loop);

void vk_blk_free(struct vk_blk *blk);

const char *vk_blk_name(const struct vk_blk *blk);
uint64_t vk_blk_capacity(const struct vk_blk *blk);

bool vk_blk_set_capacity(struct vk_blk *blk, uint64_t capacity);

struct vk_backend_file_opts {
    const char *path;
    bool readonly;
    bool direct; // O_DIRECT
};

bool vk_backend_file_open(struct vk_backend_file_opts opts,
        struct vk_blk_backend *out_backend, uint64_t *out_size);
