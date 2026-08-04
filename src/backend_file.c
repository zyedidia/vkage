#define _GNU_SOURCE

#include "vduse/blk.h"

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <linux/fs.h>
#include <linux/virtio_blk.h>

struct file_backend {
    int fd;
};

static uint64_t iov_total(const struct iovec *iov, int n) {
    uint64_t total = 0;

    for (int i = 0; i < n; i++)
        total += iov[i].iov_len;

    return total;
}

static bool file_readv(void *priv, struct vd_blk_req *req,
                       const struct iovec *iov, int n, uint64_t offset) {
    struct file_backend *fb = priv;
    ssize_t want = (ssize_t)iov_total(iov, n);
    ssize_t got = preadv(fb->fd, iov, n, (off_t)offset);

    // A short read means the request ran past the capacity we advertised.
    vd_blk_complete(req, got == want ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR);
    return true;
}

static bool file_writev(void *priv, struct vd_blk_req *req,
                        const struct iovec *iov, int n, uint64_t offset) {
    struct file_backend *fb = priv;
    ssize_t want = (ssize_t)iov_total(iov, n);
    ssize_t put = pwritev(fb->fd, iov, n, (off_t)offset);

    vd_blk_complete(req, put == want ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR);
    return true;
}

static bool file_flush(void *priv, struct vd_blk_req *req) {
    struct file_backend *fb = priv;

    vd_blk_complete(req, fdatasync(fb->fd) == 0 ? VIRTIO_BLK_S_OK
                                                : VIRTIO_BLK_S_IOERR);
    return true;
}

static bool file_discard(void *priv, struct vd_blk_req *req, uint64_t offset,
                         uint64_t length) {
    struct file_backend *fb = priv;
    int rc = fallocate(fb->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                       (off_t)offset, (off_t)length);

    vd_blk_complete(req, rc == 0 ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR);
    return true;
}

static void file_destroy(void *priv) {
    struct file_backend *fb = priv;

    if (fb->fd >= 0)
        close(fb->fd);
    free(fb);
}

bool vd_backend_file_open(struct vd_backend_file_opts opts,
                          struct vd_blk_backend *out_backend,
                          uint64_t *out_size) {
    struct file_backend *fb;
    struct stat st;
    uint64_t size;
    int flags;

    if (!opts.path || !out_backend) {
        vd_set_error(EINVAL, "path and out_backend are required");
        return false;
    }

    flags = (opts.readonly ? O_RDONLY : O_RDWR) | O_CLOEXEC;
    if (opts.direct)
        flags |= O_DIRECT;

    fb = calloc(1, sizeof(*fb));
    if (!fb) {
        vd_set_error(ENOMEM, "out of memory");
        return false;
    }

    fb->fd = open(opts.path, flags);
    if (fb->fd < 0) {
        vd_set_error(errno, "open %s: %s", opts.path, strerror(errno));
        goto err;
    }

    if (fstat(fb->fd, &st) < 0) {
        vd_set_error(errno, "fstat %s: %s", opts.path, strerror(errno));
        goto err;
    }

    if (S_ISBLK(st.st_mode)) {
        if (ioctl(fb->fd, BLKGETSIZE64, &size) < 0) {
            vd_set_error(errno, "BLKGETSIZE64 %s: %s", opts.path,
                         strerror(errno));
            goto err;
        }
    } else if (S_ISREG(st.st_mode)) {
        size = (uint64_t)st.st_size;
    } else {
        vd_set_error(EINVAL, "%s is neither a regular file nor a block device",
                     opts.path);
        goto err;
    }

    *out_backend = (struct vd_blk_backend){
        .priv = fb,
        .readv = file_readv,
        .writev = file_writev,
        .flush = file_flush,
        // Hole punching only works on regular files; a block device would
        // need BLKDISCARD, so VIRTIO_BLK_F_DISCARD is not offered there.
        .discard =
            (!opts.readonly && S_ISREG(st.st_mode)) ? file_discard : NULL,
        .destroy = file_destroy,
    };

    if (out_size)
        *out_size = size;

    return true;

err:
    if (fb->fd >= 0)
        close(fb->fd);
    free(fb);
    return false;
}
