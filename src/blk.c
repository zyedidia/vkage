#include "vkage/blk.h"

#include "bus.h"
#include "internal.h"

#include <endian.h>
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/virtio_blk.h>
#include <linux/virtio_ids.h>

#include "libvduse.h"

#define DEFAULT_BLOCK_SIZE     512u
#define DEFAULT_QUEUE_SIZE     256u
#define DEFAULT_SIZE_MAX       (128u * 1024u)
#define DEFAULT_VDPA_BIN       "vdpa"
#define DEFAULT_MGMTDEV        "vduse"
#define DEFAULT_BUS_TIMEOUT_MS 10000u

#define NUM_QUEUES 1

struct vk_blk_req {
    VduseVirtqElement elem;

    struct vk_blk *blk;
    VduseVirtq *vq;
    uint32_t used_len;
};

struct vk_blk {
    VduseDev *dev;
    struct vk_loop *loop;
    struct vk_blk_backend be;

    char *name;
    char *vdpa_bin;
    char *mgmtdev;
    unsigned bus_timeout_ms;

    uint64_t capacity;
    uint32_t block_size;
    uint32_t size_max;
    uint32_t queue_size;
    bool readonly;

    uint8_t serial[VK_BLK_ID_BYTES];
    bool has_serial;

    // -1 when no queue is enabled. Tracked because vduse_dev_destroy() frees
    // the vq array without disabling queues, so nothing else would close it.
    int kick_fd;

    // -1 when no `vdpa dev add` is in flight.
    int bus_pidfd;
    bool bus_added;
};

static uint64_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint64_t iov_total(const struct iovec *iov, int n) {
    uint64_t total = 0;

    for (int i = 0; i < n; i++)
        total += iov[i].iov_len;

    return total;
}

// Reject requests that fall outside the advertised capacity. The division
// also guards the sector * VK_BLK_SECTOR multiply below from wrapping, which
// would otherwise turn an out-of-range request into one aimed at block zero.
static bool in_range(const struct vk_blk *blk, uint64_t sector, uint64_t len) {
    uint64_t offset;

    if (sector > blk->capacity / VK_BLK_SECTOR)
        return false;

    offset = sector * VK_BLK_SECTOR;
    return len <= blk->capacity - offset;
}

void vk_blk_complete(struct vk_blk_req *req, uint8_t status) {
    VduseVirtqElement *e = &req->elem;
    uint32_t len = req->used_len;

    if (e->in_num > 0 && e->in_sg[e->in_num - 1].iov_len >= 1) {
        *(uint8_t *)e->in_sg[e->in_num - 1].iov_base = status;

        // The used ring reports bytes actually written to device-writable
        // buffers. Claiming a full transfer after a failure would hand the
        // driver uninitialised bounce buffer.
        if (status != VIRTIO_BLK_S_OK)
            len = 1;
    } else {
        len = 0;
    }

    vduse_queue_push(req->vq, e, len);
    vduse_queue_notify(req->vq);
    free(req);
}

static void handle_req(struct vk_blk *blk, struct vk_blk_req *req) {
    VduseVirtqElement *e = &req->elem;
    struct virtio_blk_outhdr hdr;
    const struct iovec *data;
    uint64_t sector, len;
    uint32_t type;
    int ndata;

    // Every request needs a header out-descriptor and a status in-descriptor.
    if (e->out_num < 1 || e->in_num < 1 ||
            e->in_sg[e->in_num - 1].iov_len < 1) {
        req->used_len = 0;
        vk_blk_complete(req, VIRTIO_BLK_S_IOERR);
        return;
    }

    // Linux always gives the header a descriptor of its own. Handling a
    // header packed together with data would mean splitting the first
    // out-descriptor, which we do not do.
    if (e->out_sg[0].iov_len != sizeof(hdr)) {
        req->used_len = 1;
        vk_blk_complete(req, VIRTIO_BLK_S_UNSUPP);
        return;
    }

    memcpy(&hdr, e->out_sg[0].iov_base, sizeof(hdr));
    type = le32toh(hdr.type);
    sector = le64toh(hdr.sector);

    switch (type) {
    case VIRTIO_BLK_T_IN:
        data = e->in_sg;
        ndata = (int)e->in_num - 1; // last in_sg is the status byte
        len = iov_total(data, ndata);
        if (!in_range(blk, sector, len)) {
            req->used_len = 1;
            vk_blk_complete(req, VIRTIO_BLK_S_IOERR);
            return;
        }
        req->used_len = (uint32_t)len + 1;
        if (!blk->be.readv(blk->be.priv, req, data, ndata,
                           sector * VK_BLK_SECTOR))
            vk_blk_complete(req, VIRTIO_BLK_S_IOERR);
        return;

    case VIRTIO_BLK_T_OUT:
        if (blk->readonly) {
            req->used_len = 1;
            vk_blk_complete(req, VIRTIO_BLK_S_IOERR);
            return;
        }
        data = e->out_sg + 1; // out_sg[0] is the header
        ndata = (int)e->out_num - 1;
        len = iov_total(data, ndata);
        if (!in_range(blk, sector, len)) {
            req->used_len = 1;
            vk_blk_complete(req, VIRTIO_BLK_S_IOERR);
            return;
        }
        req->used_len = 1;
        if (!blk->be.writev(blk->be.priv, req, data, ndata,
                            sector * VK_BLK_SECTOR))
            vk_blk_complete(req, VIRTIO_BLK_S_IOERR);
        return;

    case VIRTIO_BLK_T_FLUSH:
        req->used_len = 1;
        if (!blk->be.flush(blk->be.priv, req))
            vk_blk_complete(req, VIRTIO_BLK_S_IOERR);
        return;

    case VIRTIO_BLK_T_GET_ID: {
        size_t n;

        if (!blk->has_serial || e->in_num < 2) {
            req->used_len = 1;
            vk_blk_complete(req, VIRTIO_BLK_S_UNSUPP);
            return;
        }
        n = e->in_sg[0].iov_len < VK_BLK_ID_BYTES ? e->in_sg[0].iov_len
                                                  : VK_BLK_ID_BYTES;
        memcpy(e->in_sg[0].iov_base, blk->serial, n);
        req->used_len = (uint32_t)n + 1;
        vk_blk_complete(req, VIRTIO_BLK_S_OK);
        return;
    }

    case VIRTIO_BLK_T_DISCARD: {
        struct virtio_blk_discard_write_zeroes d;

        // max_discard_seg is 1, so exactly one range.
        if (!blk->be.discard || e->out_num < 2 ||
            e->out_sg[1].iov_len < sizeof(d)) {
            req->used_len = 1;
            vk_blk_complete(req, VIRTIO_BLK_S_UNSUPP);
            return;
        }
        memcpy(&d, e->out_sg[1].iov_base, sizeof(d));
        sector = le64toh(d.sector);
        len = (uint64_t)le32toh(d.num_sectors) * VK_BLK_SECTOR;
        if (!in_range(blk, sector, len)) {
            req->used_len = 1;
            vk_blk_complete(req, VIRTIO_BLK_S_IOERR);
            return;
        }
        req->used_len = 1;
        if (!blk->be.discard(blk->be.priv, req, sector * VK_BLK_SECTOR, len))
            vk_blk_complete(req, VIRTIO_BLK_S_IOERR);
        return;
    }

    default:
        req->used_len = 1;
        vk_blk_complete(req, VIRTIO_BLK_S_UNSUPP);
        return;
    }
}

static void drain_queue(struct vk_blk *blk, VduseVirtq *vq) {
    struct vk_blk_req *req;

    while ((req = vduse_queue_pop(vq, sizeof(*req))) != NULL) {
        req->blk = blk;
        req->vq = vq;
        req->used_len = 0;
        handle_req(blk, req);
    }
}

static void on_kick(int fd, void *user) {
    struct vk_blk *blk = user;
    uint64_t counter;

    // The kick fd is an EFD_NONBLOCK eventfd and one read clears the counter.
    if (read(fd, &counter, sizeof(counter)) < 0 && errno != EAGAIN)
        return;

    drain_queue(blk, vduse_dev_get_queue(blk->dev, 0));
}

static void on_ctrl(int fd, void *user) {
    struct vk_blk *blk = user;

    (void)fd;
    if (vduse_dev_handler(blk->dev) < 0) {
        vk_set_error(EIO, "vduse control channel failed");
        vk_loop_fail(blk->loop);
    }
}

static void on_bus_added(int fd, void *user) {
    struct vk_blk *blk = user;

    vk_loop_del(blk->loop, fd);
    blk->bus_pidfd = -1;

    if (vk_bus_reap(fd))
        blk->bus_added = true;
    else
        vk_loop_fail(blk->loop);

    close(fd);
}

static void enable_queue(VduseDev *dev, VduseVirtq *vq) {
    struct vk_blk *blk = vduse_dev_get_priv(dev);

    if (!blk->loop)
        return;

    if (!vk_loop_add(blk->loop, vduse_queue_get_fd(vq), on_kick, blk))
        return;

    blk->kick_fd = vduse_queue_get_fd(vq);

    // Descriptors may already be pending; when adopting a live device the
    // driver will not kick again for work it submitted before we attached.
    drain_queue(blk, vq);
}

static void disable_queue(VduseDev *dev, VduseVirtq *vq) {
    struct vk_blk *blk = vduse_dev_get_priv(dev);

    // libvduse closes the kick fd immediately after this returns, so drop
    // our copy without closing it.
    if (blk->loop)
        vk_loop_del(blk->loop, vduse_queue_get_fd(vq));

    blk->kick_fd = -1;
}

static const VduseOps blk_ops = {
    .enable_queue = enable_queue,
    .disable_queue = disable_queue,
};

static void build_config(const struct vk_blk *blk,
                         struct virtio_blk_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->capacity = htole64(blk->capacity / VK_BLK_SECTOR);
    cfg->seg_max = htole32(blk->queue_size - 2); // header + status
    cfg->size_max = htole32(blk->size_max);
    cfg->blk_size = htole32(blk->block_size);
    cfg->num_queues = htole16(NUM_QUEUES);

    if (blk->be.discard) {
        cfg->max_discard_sectors = htole32(UINT32_MAX);
        cfg->max_discard_seg = htole32(1);
        cfg->discard_sector_alignment =
            htole32(blk->block_size / VK_BLK_SECTOR);
    }
}

static bool validate(const struct vk_blk *blk) {
    if (blk->block_size != 512 && blk->block_size != 4096) {
        vk_set_error(EINVAL, "block_size %u must be 512 or 4096",
                     blk->block_size);
        return false;
    }
    if (blk->queue_size < 4 || blk->queue_size > 1024 ||
        (blk->queue_size & (blk->queue_size - 1)) != 0) {
        vk_set_error(EINVAL, "queue_size %u must be a power of two in [4,1024]",
                     blk->queue_size);
        return false;
    }
    if (blk->size_max < blk->block_size) {
        vk_set_error(EINVAL, "size_max %u must be at least block_size %u",
                     blk->size_max, blk->block_size);
        return false;
    }
    if (blk->capacity == 0) {
        vk_set_error(EINVAL, "capacity is zero");
        return false;
    }
    return true;
}

bool vk_blk_new(struct vk_blk_opts opts, struct vk_blk_backend backend,
                struct vk_blk **out) {
    struct virtio_blk_config cfg;
    struct vk_blk *blk;
    uint64_t features;

    if (!opts.name || !out || !backend.readv || !backend.writev ||
        !backend.flush) {
        vk_set_error(EINVAL, "name, out and readv/writev/flush are required");
        return false;
    }

    blk = calloc(1, sizeof(*blk));
    if (!blk) {
        vk_set_error(ENOMEM, "out of memory");
        return false;
    }

    blk->be = backend;
    blk->kick_fd = -1;
    blk->bus_pidfd = -1;
    blk->readonly = opts.readonly;
    blk->block_size = opts.block_size ? opts.block_size : DEFAULT_BLOCK_SIZE;
    blk->size_max = opts.size_max ? opts.size_max : DEFAULT_SIZE_MAX;
    blk->queue_size = opts.queue_size ? opts.queue_size : DEFAULT_QUEUE_SIZE;
    blk->bus_timeout_ms =
        opts.bus_timeout_ms ? opts.bus_timeout_ms : DEFAULT_BUS_TIMEOUT_MS;
    blk->capacity = opts.capacity - (opts.capacity % blk->block_size);

    if (!validate(blk))
        goto err;

    blk->name = strdup(opts.name);
    blk->vdpa_bin = strdup(opts.vdpa_bin ? opts.vdpa_bin : DEFAULT_VDPA_BIN);
    blk->mgmtdev = strdup(opts.mgmtdev ? opts.mgmtdev : DEFAULT_MGMTDEV);
    if (!blk->name || !blk->vdpa_bin || !blk->mgmtdev) {
        vk_set_error(ENOMEM, "out of memory");
        goto err;
    }

    if (opts.serial) {
        memcpy(blk->serial, opts.serial,
               strnlen(opts.serial, VK_BLK_ID_BYTES));
        blk->has_serial = true;
    }

    features = vduse_get_virtio_features() |
               (1ULL << VIRTIO_BLK_F_SEG_MAX) |
               (1ULL << VIRTIO_BLK_F_SIZE_MAX) |
               (1ULL << VIRTIO_BLK_F_BLK_SIZE) |
               (1ULL << VIRTIO_BLK_F_FLUSH);
    if (blk->readonly)
        features |= 1ULL << VIRTIO_BLK_F_RO;
    if (blk->be.discard)
        features |= 1ULL << VIRTIO_BLK_F_DISCARD;

    build_config(blk, &cfg);

    blk->dev = vduse_dev_create(blk->name, VIRTIO_ID_BLOCK, 0, features,
                                NUM_QUEUES, sizeof(cfg), (char *)&cfg,
                                &blk_ops, blk);
    if (!blk->dev) {
        vk_set_error(errno ? errno : EIO, "failed to create vduse device %s",
                     blk->name);
        goto err;
    }

    // Must precede setup_queue.
    if (opts.reconnect_log &&
        vduse_set_reconnect_log_file(blk->dev, opts.reconnect_log) < 0) {
        vk_set_error(EIO, "failed to open reconnect log %s",
                     opts.reconnect_log);
        goto err;
    }

    // The virtqueue is set up in vk_blk_attach(), not here: setup_queue can
    // invoke enable_queue immediately when adopting a device the driver
    // already has live, and that needs blk->loop to be set.

    *out = blk;
    return true;

err:
    if (blk->dev)
        vduse_dev_destroy(blk->dev);
    free(blk->name);
    free(blk->vdpa_bin);
    free(blk->mgmtdev);
    free(blk);
    return false;
}

bool vk_blk_attach(struct vk_blk *blk, struct vk_loop *loop) {
    blk->loop = loop;

    if (!vk_loop_add(loop, vduse_dev_get_fd(blk->dev), on_ctrl, blk))
        return false;

    if (blk->be.attach && !blk->be.attach(blk->be.priv, loop))
        return false;

    // Must follow the assignment of blk->loop above, and must precede the
    // bus attach below, since queue configuration freezes once the device
    // joins the vDPA bus.
    if (vduse_dev_setup_queue(blk->dev, 0, blk->queue_size) < 0) {
        vk_set_error(EIO, "failed to set up virtqueue");
        return false;
    }

    // The child blocks until the probe is answered, which cannot happen
    // until the caller runs the loop.
    blk->bus_pidfd = vk_bus_spawn_add(blk->vdpa_bin, blk->name, blk->mgmtdev);
    if (blk->bus_pidfd < 0)
        return false;

    if (!vk_loop_add(loop, blk->bus_pidfd, on_bus_added, blk)) {
        close(blk->bus_pidfd);
        blk->bus_pidfd = -1;
        return false;
    }

    return true;
}

// Service the control fd until the child behind pidfd exits. vk_blk_free()
// runs after the caller's loop has stopped, but `vdpa dev del` drives a
// reset whose SET_STATUS still needs an answer.
static void pump_until_exit(struct vk_blk *blk, int pidfd) {
    uint64_t deadline = now_ms() + blk->bus_timeout_ms;
    struct pollfd fds[2] = {
        {.fd = vduse_dev_get_fd(blk->dev), .events = POLLIN},
        {.fd = pidfd, .events = POLLIN},
    };

    for (;;) {
        uint64_t now = now_ms();
        int rc;

        if (now >= deadline) {
            vk_set_error(ETIMEDOUT, "timed out waiting for %s", blk->vdpa_bin);
            return;
        }

        rc = poll(fds, 2, (int)(deadline - now));
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            vk_set_error(errno, "poll: %s", strerror(errno));
            return;
        }
        if (rc == 0)
            continue;

        if (fds[1].revents & POLLIN)
            return;
        if (fds[0].revents & POLLIN)
            vduse_dev_handler(blk->dev);
    }
}

void vk_blk_free(struct vk_blk *blk) {
    if (!blk)
        return;

    // An add may still be in flight if the loop stopped early.
    if (blk->bus_pidfd >= 0) {
        if (blk->loop)
            vk_loop_del(blk->loop, blk->bus_pidfd);
        pump_until_exit(blk, blk->bus_pidfd);
        if (vk_bus_reap(blk->bus_pidfd))
            blk->bus_added = true;
        close(blk->bus_pidfd);
        blk->bus_pidfd = -1;
    }

    if (blk->bus_added) {
        int fd = vk_bus_spawn_del(blk->vdpa_bin, blk->name);

        if (fd >= 0) {
            pump_until_exit(blk, fd);
            vk_bus_reap(fd);
            close(fd);
        }
        blk->bus_added = false;
    }

    // Only set if the queue was never disabled, i.e. no reset reached us.
    // vduse_dev_destroy() will not clean this up.
    if (blk->kick_fd >= 0) {
        if (blk->loop)
            vk_loop_del(blk->loop, blk->kick_fd);
        close(blk->kick_fd);
        blk->kick_fd = -1;
    }

    if (blk->loop && blk->dev)
        vk_loop_del(blk->loop, vduse_dev_get_fd(blk->dev));

    if (blk->dev)
        vduse_dev_destroy(blk->dev);

    if (blk->be.destroy)
        blk->be.destroy(blk->be.priv);

    free(blk->name);
    free(blk->vdpa_bin);
    free(blk->mgmtdev);
    free(blk);
}

const char *vk_blk_name(const struct vk_blk *blk) {
    return blk->name;
}

uint64_t vk_blk_capacity(const struct vk_blk *blk) {
    return blk->capacity;
}

bool vk_blk_set_capacity(struct vk_blk *blk, uint64_t capacity) {
    uint64_t sectors;

    blk->capacity = capacity - (capacity % blk->block_size);
    sectors = htole64(blk->capacity / VK_BLK_SECTOR);

    if (vduse_dev_update_config(blk->dev, sizeof(sectors),
                                offsetof(struct virtio_blk_config, capacity),
                                (char *)&sectors) < 0) {
        vk_set_error(EIO, "failed to update config space");
        return false;
    }

    return true;
}
