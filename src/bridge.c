// accept4(2) is a GNU extension and gnu17 alone does not expose it.
#define _GNU_SOURCE

#include "vkage/bridge.h"

#include "bus.h"
#include "internal.h"
#include "umvirtio_proto.h"

#include <endian.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/pidfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/vduse.h>
#include <linux/virtio_ring.h>

#include "libvduse.h"

extern char **environ;

#define DEFAULT_HANDSHAKE_MS 30000u
#define DEFAULT_BUS_TIMEOUT_MS 10000u
#define DEFAULT_VDPA_BIN "vdpa"
#define DEFAULT_MGMTDEV "vduse"

#define MAX_UML_ARGV 64

struct vk_bridge_req {
    // Must stay first; libvduse allocates the iovec arrays after it.
    VduseVirtqElement elem;

    struct vk_bridge_queue *q;
    uint32_t slot;
};

struct vk_bridge_queue {
    struct vk_bridge *br;
    uint32_t index;

    VduseVirtq *vq;

    // Driver side of the UML-facing ring. The memory is UML's; we only
    // ever write len/flags/next and the avail ring. desc->addr is filled
    // in once by UML and must be left alone.
    struct vring vr;
    uint32_t num_desc;

    uint16_t avail_idx;
    uint16_t last_used;

    int kick_fd; // we signal it
    int call_fd; // UML signals it

    struct vk_bridge_req **inflight; // queue_size entries, indexed by slot
    uint32_t *free_slots;
    uint32_t n_free;
};

struct vk_bridge {
    VduseDev *dev;
    struct vk_loop *loop;

    char *name;
    char *sock_path;
    char *vdpa_bin;
    char *mgmtdev;
    unsigned bus_timeout_ms;

    int listen_fd;
    int sock_fd;
    int physmem_fd;

    void *map;
    size_t map_size;

    struct umv_hello hello;

    struct vk_bridge_queue *queues;
    uint32_t num_queues;

    bool start_sent;

    int uml_pidfd;
    int bus_pidfd;
    bool bus_added;
};

// ------------------------------------------------------------------
// Address translation and small helpers
// ------------------------------------------------------------------

// UML kernel virtual address -> pointer into our mapping of its physmem.
static void *umv_ptr(struct vk_bridge *br, uint64_t kva, uint64_t len) {
    uint64_t off;

    if (kva < br->hello.uml_physmem)
        return NULL;

    off = kva - br->hello.uml_physmem;
    if (off > br->map_size || len > br->map_size - off)
        return NULL;

    return (char *)br->map + off;
}

static uint64_t iov_total(const struct iovec *iov, unsigned n) {
    uint64_t total = 0;

    for (unsigned i = 0; i < n; i++)
        total += iov[i].iov_len;

    return total;
}

static void iov_gather(void *dst, const struct iovec *iov, unsigned n) {
    char *p = dst;

    for (unsigned i = 0; i < n; i++) {
        memcpy(p, iov[i].iov_base, iov[i].iov_len);
        p += iov[i].iov_len;
    }
}

static void iov_scatter(const struct iovec *iov, unsigned n, const void *src,
                        uint64_t len) {
    const char *p = src;

    for (unsigned i = 0; i < n && len; i++) {
        uint64_t chunk = iov[i].iov_len < len ? iov[i].iov_len : len;

        memcpy(iov[i].iov_base, p, chunk);
        p += chunk;
        len -= chunk;
    }
}

static bool slot_alloc(struct vk_bridge_queue *q, uint32_t *out) {
    if (!q->n_free)
        return false;

    *out = q->free_slots[--q->n_free];
    return true;
}

static void slot_free(struct vk_bridge_queue *q, uint32_t slot) {
    q->free_slots[q->n_free++] = slot;
}

// ------------------------------------------------------------------
// Ring, driver side
// ------------------------------------------------------------------

static void ring_publish(struct vk_bridge_queue *q, uint16_t head) {
    q->vr.avail->ring[q->avail_idx % q->num_desc] = htole16(head);

    // Descriptor and avail-ring writes must land before the index UML
    // reads to find them.
    __atomic_thread_fence(__ATOMIC_RELEASE);

    q->avail_idx++;
    q->vr.avail->idx = htole16(q->avail_idx);
}

static bool ring_relay(struct vk_bridge_queue *q, struct vk_bridge_req *req) {
    struct vk_bridge *br = q->br;
    VduseVirtqElement *e = &req->elem;
    uint64_t out_len = iov_total(e->out_sg, e->out_num);
    uint64_t in_len = iov_total(e->in_sg, e->in_num);
    uint16_t d_out = (uint16_t)(2 * req->slot);
    uint16_t d_in = (uint16_t)(2 * req->slot + 1);
    uint16_t head;

    if (out_len > br->hello.slot_size || in_len > br->hello.slot_size)
        return false;

    if (out_len) {
        void *dst = umv_ptr(br, le64toh(q->vr.desc[d_out].addr), out_len);

        if (!dst)
            return false;
        iov_gather(dst, e->out_sg, e->out_num);
    }
    if (in_len && !umv_ptr(br, le64toh(q->vr.desc[d_in].addr), in_len))
        return false;

    if (out_len && in_len) {
        q->vr.desc[d_out].len = htole32((uint32_t)out_len);
        q->vr.desc[d_out].flags = htole16(VRING_DESC_F_NEXT);
        q->vr.desc[d_out].next = htole16(d_in);
        q->vr.desc[d_in].len = htole32((uint32_t)in_len);
        q->vr.desc[d_in].flags = htole16(VRING_DESC_F_WRITE);
        head = d_out;
    } else if (out_len) {
        q->vr.desc[d_out].len = htole32((uint32_t)out_len);
        q->vr.desc[d_out].flags = 0;
        head = d_out;
    } else {
        q->vr.desc[d_in].len = htole32((uint32_t)in_len);
        q->vr.desc[d_in].flags = htole16(VRING_DESC_F_WRITE);
        head = d_in;
    }

    q->inflight[req->slot] = req;
    ring_publish(q, head);

    return true;
}

// Move as many pending vduse requests to UML as there are free slots.
static void queue_pump(struct vk_bridge_queue *q) {
    uint64_t one = 1;
    bool kicked = false;

    for (;;) {
        struct vk_bridge_req *req;
        uint32_t slot;

        if (!slot_alloc(q, &slot))
            break;

        req = vduse_queue_pop(q->vq, sizeof(*req));
        if (!req) {
            slot_free(q, slot);
            break;
        }

        req->q = q;
        req->slot = slot;

        if (!ring_relay(q, req)) {
            /*
             * The request does not fit a slot, or points somewhere we
             * cannot reach. A generic relay has no way to signal this
             * properly -- it does not know which byte a status byte is --
             * so the chain goes back unwritten and the guest driver sees
             * a malformed reply. A shim whose advertised seg_max and
             * size_max agree with its slot_size never gets here.
             */
            vk_set_error(EMSGSIZE, "queue %u: request exceeds slot size %llu",
                         q->index, (unsigned long long)q->br->hello.slot_size);
            vduse_queue_push(q->vq, &req->elem, 0);
            vduse_queue_notify(q->vq);
            slot_free(q, slot);
            free(req);
            continue;
        }

        kicked = true;
    }

    if (kicked && write(q->kick_fd, &one, sizeof(one)) < 0)
        vk_set_error(errno, "queue %u: kick failed: %s", q->index,
                     strerror(errno));
}

static void queue_drain_used(struct vk_bridge_queue *q) {
    struct vk_bridge *br = q->br;
    uint16_t used_idx;

    used_idx = le16toh(__atomic_load_n(&q->vr.used->idx, __ATOMIC_ACQUIRE));

    while (q->last_used != used_idx) {
        struct vring_used_elem *ue =
            &q->vr.used->ring[q->last_used % q->num_desc];
        uint32_t id = le32toh(ue->id);
        uint32_t len = le32toh(ue->len);
        struct vk_bridge_req *req;
        uint32_t slot = id / 2;

        q->last_used++;

        if (slot >= br->hello.queue_size)
            continue;

        req = q->inflight[slot];
        if (!req)
            continue;
        q->inflight[slot] = NULL;

        if (len && req->elem.in_num) {
            const void *src = umv_ptr(
                br, le64toh(q->vr.desc[2 * slot + 1].addr), len);

            if (src)
                iov_scatter(req->elem.in_sg, req->elem.in_num, src, len);
            else
                len = 0;
        }

        vduse_queue_push(q->vq, &req->elem, len);
        vduse_queue_notify(q->vq);

        slot_free(q, slot);
        free(req);
    }
}

// ------------------------------------------------------------------
// Loop callbacks
// ------------------------------------------------------------------

static void on_vduse_kick(int fd, void *user) {
    struct vk_bridge_queue *q = user;
    uint64_t counter;

    if (read(fd, &counter, sizeof(counter)) < 0 && errno != EAGAIN)
        return;

    queue_pump(q);
}

static void on_uml_call(int fd, void *user) {
    struct vk_bridge_queue *q = user;
    uint64_t counter;

    if (read(fd, &counter, sizeof(counter)) < 0 && errno != EAGAIN)
        return;

    queue_drain_used(q);

    // Completions freed slots, so anything that stalled can go now.
    queue_pump(q);
}

static void on_ctrl(int fd, void *user) {
    struct vk_bridge *br = user;

    (void)fd;
    if (vduse_dev_handler(br->dev) < 0) {
        vk_set_error(EIO, "vduse control channel failed");
        vk_loop_stop(br->loop);
    }
}

static void on_bus_added(int fd, void *user) {
    struct vk_bridge *br = user;

    vk_loop_del(br->loop, fd);
    br->bus_pidfd = -1;

    if (vk_bus_reap(fd))
        br->bus_added = true;
    else
        vk_loop_stop(br->loop);

    close(fd);
}

static void on_uml_exit(int fd, void *user) {
    struct vk_bridge *br = user;

    vk_loop_del(br->loop, fd);
    vk_set_error(ECHILD, "UML instance exited");
    vk_loop_stop(br->loop);
}

// ------------------------------------------------------------------
// Feature negotiation handoff
// ------------------------------------------------------------------

static bool bridge_send_start(struct vk_bridge *br) {
    struct umv_start msg = {
        .magic = UMV_MAGIC,
        .type = UMV_MSG_START,
    };
    uint64_t features = 0;
    ssize_t rc;

    /*
     * libvduse keeps the negotiated set private, but it exposes the
     * device fd, and VDUSE_DEV_GET_FEATURES is valid once FEATURES_OK is
     * set -- which it is by the time a queue is enabled.
     */
    if (ioctl(vduse_dev_get_fd(br->dev), VDUSE_DEV_GET_FEATURES, &features)) {
        vk_set_error(errno, "VDUSE_DEV_GET_FEATURES: %s", strerror(errno));
        return false;
    }

    msg.features = features;

    rc = send(br->sock_fd, &msg, sizeof(msg), MSG_NOSIGNAL);
    if (rc != (ssize_t)sizeof(msg)) {
        vk_set_error(rc < 0 ? errno : EIO, "sending START to UML failed");
        return false;
    }

    return true;
}

static struct vk_bridge_queue *queue_for(struct vk_bridge *br,
                                         VduseVirtq *vq) {
    for (uint32_t i = 0; i < br->num_queues; i++)
        if (vduse_dev_get_queue(br->dev, (int)i) == vq)
            return &br->queues[i];

    return NULL;
}

static void bridge_enable_queue(VduseDev *dev, VduseVirtq *vq) {
    struct vk_bridge *br = vduse_dev_get_priv(dev);
    struct vk_bridge_queue *q = queue_for(br, vq);

    if (!q || !br->loop)
        return;

    q->vq = vq;

    // UML must know the negotiated set before it touches a descriptor.
    if (!br->start_sent) {
        if (!bridge_send_start(br)) {
            vk_loop_stop(br->loop);
            return;
        }
        br->start_sent = true;
    }

    if (!vk_loop_add(br->loop, vduse_queue_get_fd(vq), on_vduse_kick, q))
        return;

    // The driver may have queued work before we got here.
    queue_pump(q);
}

static void bridge_disable_queue(VduseDev *dev, VduseVirtq *vq) {
    struct vk_bridge *br = vduse_dev_get_priv(dev);

    if (br->loop)
        vk_loop_del(br->loop, vduse_queue_get_fd(vq));
}

static const VduseOps bridge_ops = {
    .enable_queue = bridge_enable_queue,
    .disable_queue = bridge_disable_queue,
};

// ------------------------------------------------------------------
// Handshake
// ------------------------------------------------------------------

static bool listen_socket(struct vk_bridge *br) {
    struct sockaddr_un addr;

    if (strlen(br->sock_path) >= sizeof(addr.sun_path)) {
        vk_set_error(ENAMETOOLONG, "socket path too long");
        return false;
    }

    br->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (br->listen_fd < 0) {
        vk_set_error(errno, "socket: %s", strerror(errno));
        return false;
    }

    unlink(br->sock_path);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, br->sock_path);

    if (bind(br->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        vk_set_error(errno, "bind %s: %s", br->sock_path, strerror(errno));
        return false;
    }
    if (listen(br->listen_fd, 1) < 0) {
        vk_set_error(errno, "listen: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool spawn_uml(struct vk_bridge *br, const char *bin, const char *args) {
    char *argv[MAX_UML_ARGV];
    char sock_arg[160];
    char *copy = NULL;
    int argc = 0;
    pid_t pid;
    int rc;

    argv[argc++] = (char *)bin;

    if (args && *args) {
        char *save = NULL;
        char *tok;

        copy = strdup(args);
        if (!copy) {
            vk_set_error(ENOMEM, "out of memory");
            return false;
        }

        for (tok = strtok_r(copy, " \t", &save); tok;
             tok = strtok_r(NULL, " \t", &save)) {
            if (argc >= MAX_UML_ARGV - 2) {
                vk_set_error(E2BIG, "too many UML arguments");
                free(copy);
                return false;
            }
            argv[argc++] = tok;
        }
    }

    snprintf(sock_arg, sizeof(sock_arg), "umvirtio.sock=%s", br->sock_path);
    argv[argc++] = sock_arg;
    argv[argc] = NULL;

    rc = posix_spawnp(&pid, bin, NULL, NULL, argv, environ);
    free(copy);

    if (rc != 0) {
        vk_set_error(rc, "spawn %s: %s", bin, strerror(rc));
        return false;
    }

    br->uml_pidfd = pidfd_open(pid, 0);
    if (br->uml_pidfd < 0) {
        vk_set_error(errno, "pidfd_open: %s", strerror(errno));
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return false;
    }

    return true;
}

// Accept the UML connection and read HELLO plus its descriptor batch.
static bool recv_hello(struct vk_bridge *br, int *fds, int *nfds_out,
                       unsigned timeout_ms) {
    char control[CMSG_SPACE(sizeof(int) * (1 + 2 * UMV_MAX_QUEUES))];
    struct pollfd pfd = {.fd = br->listen_fd, .events = POLLIN};
    struct cmsghdr *cmsg;
    struct msghdr msg;
    struct iovec iov;
    ssize_t rc;
    size_t got;
    int nfds;

    rc = poll(&pfd, 1, (int)timeout_ms);
    if (rc < 0) {
        vk_set_error(errno, "poll: %s", strerror(errno));
        return false;
    }
    if (rc == 0) {
        vk_set_error(ETIMEDOUT, "UML did not connect to %s within %ums",
                     br->sock_path, timeout_ms);
        return false;
    }

    br->sock_fd = accept4(br->listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (br->sock_fd < 0) {
        vk_set_error(errno, "accept: %s", strerror(errno));
        return false;
    }

    memset(control, 0, sizeof(control));
    memset(&msg, 0, sizeof(msg));

    iov.iov_base = &br->hello;
    iov.iov_len = sizeof(br->hello);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    do {
        rc = recvmsg(br->sock_fd, &msg, 0);
    } while (rc < 0 && errno == EINTR);

    if (rc <= 0) {
        vk_set_error(rc < 0 ? errno : EPIPE, "reading HELLO failed");
        return false;
    }

    // The fds arrive with the first fragment; the body may be short.
    got = (size_t)rc;
    while (got < sizeof(br->hello)) {
        rc = recv(br->sock_fd, (char *)&br->hello + got,
                  sizeof(br->hello) - got, 0);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0) {
            vk_set_error(rc < 0 ? errno : EPIPE, "short HELLO");
            return false;
        }
        got += (size_t)rc;
    }

    cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS) {
        vk_set_error(EPROTO, "HELLO carried no descriptors");
        return false;
    }

    nfds = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
    if (nfds < 1 || nfds > 1 + 2 * UMV_MAX_QUEUES) {
        vk_set_error(EPROTO, "HELLO carried %d descriptors", nfds);
        return false;
    }

    memcpy(fds, CMSG_DATA(cmsg), sizeof(int) * (size_t)nfds);
    *nfds_out = nfds;

    return true;
}

static bool hello_valid(struct vk_bridge *br, int nfds) {
    const struct umv_hello *h = &br->hello;

    if (h->magic != UMV_MAGIC || h->type != UMV_MSG_HELLO) {
        vk_set_error(EPROTO, "bad HELLO magic");
        return false;
    }
    if (h->version != UMV_VERSION) {
        vk_set_error(EPROTO, "UML speaks protocol %u, bridge speaks %u",
                     h->version, UMV_VERSION);
        return false;
    }
    if (h->num_queues < 1 || h->num_queues > UMV_MAX_QUEUES) {
        vk_set_error(EPROTO, "bad queue count %u", h->num_queues);
        return false;
    }
    if (h->queue_size < UMV_MIN_QUEUE_SIZE ||
        h->queue_size > UMV_MAX_QUEUE_SIZE ||
        (h->queue_size & (h->queue_size - 1))) {
        vk_set_error(EPROTO, "bad queue size %u", h->queue_size);
        return false;
    }
    if (!h->slot_size || h->slot_size > UMV_MAX_SLOT_SIZE) {
        vk_set_error(EPROTO, "bad slot size %llu",
                     (unsigned long long)h->slot_size);
        return false;
    }
    if (h->config_size > UMV_MAX_CONFIG) {
        vk_set_error(EPROTO, "config space too large");
        return false;
    }
    if (!h->physmem_size) {
        vk_set_error(EPROTO, "zero physmem size");
        return false;
    }
    if (nfds != 1 + 2 * (int)h->num_queues) {
        vk_set_error(EPROTO, "expected %d descriptors, got %d",
                     1 + 2 * (int)h->num_queues, nfds);
        return false;
    }

    return true;
}

static bool map_and_setup_queues(struct vk_bridge *br, const int *fds) {
    const struct umv_hello *h = &br->hello;

    br->physmem_fd = fds[0];
    br->map_size = h->physmem_size;

    br->map = mmap(NULL, br->map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   br->physmem_fd, 0);
    if (br->map == MAP_FAILED) {
        br->map = NULL;
        vk_set_error(errno, "mmap physmem: %s", strerror(errno));
        return false;
    }

    br->num_queues = h->num_queues;
    br->queues = calloc(br->num_queues, sizeof(*br->queues));
    if (!br->queues) {
        vk_set_error(ENOMEM, "out of memory");
        return false;
    }

    for (uint32_t i = 0; i < br->num_queues; i++) {
        struct vk_bridge_queue *q = &br->queues[i];
        size_t bytes;
        void *ring;

        q->br = br;
        q->index = i;
        q->num_desc = 2 * h->queue_size;
        q->kick_fd = fds[1 + i];
        q->call_fd = fds[1 + h->num_queues + i];

        bytes = vring_size(q->num_desc, UMV_VRING_ALIGN);
        ring = umv_ptr(br, h->vring_kva[i], bytes);
        if (!ring) {
            vk_set_error(EPROTO, "queue %u vring is outside physmem", i);
            return false;
        }

        vring_init(&q->vr, q->num_desc, ring, UMV_VRING_ALIGN);

        /*
         * vringh checks avail->flags for interrupt suppression, so start
         * it at zero; UML would otherwise be free to stop notifying us.
         */
        q->vr.avail->flags = 0;
        q->vr.avail->idx = 0;
        q->avail_idx = 0;
        q->last_used = le16toh(q->vr.used->idx);

        q->inflight = calloc(h->queue_size, sizeof(*q->inflight));
        q->free_slots = calloc(h->queue_size, sizeof(*q->free_slots));
        if (!q->inflight || !q->free_slots) {
            vk_set_error(ENOMEM, "out of memory");
            return false;
        }

        for (uint32_t s = 0; s < h->queue_size; s++)
            q->free_slots[s] = h->queue_size - 1 - s;
        q->n_free = h->queue_size;
    }

    return true;
}

// ------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------

bool vk_bridge_new(struct vk_bridge_opts opts, struct vk_bridge **out) {
    int fds[1 + 2 * UMV_MAX_QUEUES];
    struct vk_bridge *br;
    uint64_t features;
    unsigned timeout;
    char path[128];
    int nfds = 0;

    if (!opts.name || !out) {
        vk_set_error(EINVAL, "name and out are required");
        return false;
    }

    br = calloc(1, sizeof(*br));
    if (!br) {
        vk_set_error(ENOMEM, "out of memory");
        return false;
    }

    br->listen_fd = br->sock_fd = br->physmem_fd = -1;
    br->uml_pidfd = br->bus_pidfd = -1;
    br->bus_timeout_ms =
        opts.bus_timeout_ms ? opts.bus_timeout_ms : DEFAULT_BUS_TIMEOUT_MS;
    timeout = opts.handshake_timeout_ms ? opts.handshake_timeout_ms
                                        : DEFAULT_HANDSHAKE_MS;

    if (!opts.sock_path) {
        snprintf(path, sizeof(path), "/tmp/vkage-bridge-%s.sock", opts.name);
        br->sock_path = strdup(path);
    } else {
        br->sock_path = strdup(opts.sock_path);
    }

    br->name = strdup(opts.name);
    br->vdpa_bin = strdup(opts.vdpa_bin ? opts.vdpa_bin : DEFAULT_VDPA_BIN);
    br->mgmtdev = strdup(opts.mgmtdev ? opts.mgmtdev : DEFAULT_MGMTDEV);
    if (!br->name || !br->sock_path || !br->vdpa_bin || !br->mgmtdev) {
        vk_set_error(ENOMEM, "out of memory");
        goto err;
    }

    if (!listen_socket(br))
        goto err;

    if (opts.uml_bin && !spawn_uml(br, opts.uml_bin, opts.uml_args))
        goto err;

    if (!recv_hello(br, fds, &nfds, timeout))
        goto err;
    if (!hello_valid(br, nfds))
        goto err;
    if (!map_and_setup_queues(br, fds))
        goto err;

    /*
     * UML declares device-type features only. Ring features belong to the
     * vduse side and are libvduse's business, so add them here rather
     * than making every shim know about them.
     */
    features = br->hello.features | vduse_get_virtio_features();

    br->dev = vduse_dev_create(br->name, br->hello.device_id,
                               br->hello.vendor_id, features,
                               (uint16_t)br->num_queues,
                               br->hello.config_size,
                               (char *)br->hello.config, &bridge_ops, br);
    if (!br->dev) {
        vk_set_error(errno ? errno : EIO, "failed to create vduse device %s",
                     br->name);
        goto err;
    }

    for (uint32_t i = 0; i < br->num_queues; i++) {
        if (vduse_dev_setup_queue(br->dev, (int)i,
                                  (int)br->hello.queue_size) < 0) {
            vk_set_error(EIO, "failed to set up queue %u", i);
            goto err;
        }
    }

    *out = br;
    return true;

err:
    vk_bridge_free(br);
    return false;
}

bool vk_bridge_attach(struct vk_bridge *br, struct vk_loop *loop) {
    br->loop = loop;

    if (!vk_loop_add(loop, vduse_dev_get_fd(br->dev), on_ctrl, br))
        return false;

    for (uint32_t i = 0; i < br->num_queues; i++)
        if (!vk_loop_add(loop, br->queues[i].call_fd, on_uml_call,
                         &br->queues[i]))
            return false;

    if (br->uml_pidfd >= 0 &&
        !vk_loop_add(loop, br->uml_pidfd, on_uml_exit, br))
        return false;

    br->bus_pidfd = vk_bus_spawn_add(br->vdpa_bin, br->name, br->mgmtdev);
    if (br->bus_pidfd < 0)
        return false;

    if (!vk_loop_add(loop, br->bus_pidfd, on_bus_added, br)) {
        close(br->bus_pidfd);
        br->bus_pidfd = -1;
        return false;
    }

    return true;
}

// Service the control fd until the child behind pidfd exits. See the same
// pattern in blk.c: `vdpa dev del` drives a reset whose SET_STATUS still
// needs answering, and the caller's loop has usually already stopped.
static void pump_until_exit(struct vk_bridge *br, int pidfd) {
    struct pollfd fds[2] = {
        {.fd = vduse_dev_get_fd(br->dev), .events = POLLIN},
        {.fd = pidfd, .events = POLLIN},
    };
    unsigned waited = 0;

    while (waited < br->bus_timeout_ms) {
        int rc = poll(fds, 2, 100);

        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (rc == 0) {
            waited += 100;
            continue;
        }
        if (fds[1].revents & POLLIN)
            return;
        if (fds[0].revents & POLLIN)
            vduse_dev_handler(br->dev);
    }
}

// How long to let UML exit on SIGTERM before resorting to SIGKILL.
#define UML_EXIT_TIMEOUT_MS 2000

// Stop the UML instance we spawned.
//
// It will not stop by itself. After the handshake there is no host->UML
// message, so nothing on the UML side ever polls that socket, and closing
// it goes unnoticed; with CONFIG_UML_NO_USERSPACE the kernel then idles
// forever rather than exiting. Call this only once the VDUSE device is
// gone, so UML is still able to complete in-flight requests during the
// reset that `vdpa dev del` drives.
static void uml_shutdown(struct vk_bridge *br) {
    struct pollfd pfd;
    siginfo_t si;

    if (br->uml_pidfd < 0)
        return;

    if (br->loop)
        vk_loop_del(br->loop, br->uml_pidfd);

    if (pidfd_send_signal(br->uml_pidfd, SIGTERM, NULL, 0) == 0) {
        pfd.fd = br->uml_pidfd;
        pfd.events = POLLIN;

        if (poll(&pfd, 1, UML_EXIT_TIMEOUT_MS) == 0)
            pidfd_send_signal(br->uml_pidfd, SIGKILL, NULL, 0);
    }

    // Reap, so a bridge embedded in a longer-lived process leaves no zombie.
    memset(&si, 0, sizeof(si));
    waitid(P_PIDFD, (id_t)br->uml_pidfd, &si, WEXITED);

    close(br->uml_pidfd);
    br->uml_pidfd = -1;
}

void vk_bridge_free(struct vk_bridge *br) {
    if (!br)
        return;

    if (br->bus_pidfd >= 0) {
        if (br->loop)
            vk_loop_del(br->loop, br->bus_pidfd);
        pump_until_exit(br, br->bus_pidfd);
        if (vk_bus_reap(br->bus_pidfd))
            br->bus_added = true;
        close(br->bus_pidfd);
        br->bus_pidfd = -1;
    }

    if (br->bus_added && br->dev) {
        int fd = vk_bus_spawn_del(br->vdpa_bin, br->name);

        if (fd >= 0) {
            pump_until_exit(br, fd);
            vk_bus_reap(fd);
            close(fd);
        }
        br->bus_added = false;
    }

    if (br->loop && br->dev)
        vk_loop_del(br->loop, vduse_dev_get_fd(br->dev));

    if (br->dev)
        vduse_dev_destroy(br->dev);

    uml_shutdown(br);

    if (br->queues) {
        for (uint32_t i = 0; i < br->num_queues; i++) {
            struct vk_bridge_queue *q = &br->queues[i];

            if (br->loop)
                vk_loop_del(br->loop, q->call_fd);
            if (q->kick_fd >= 0)
                close(q->kick_fd);
            if (q->call_fd >= 0)
                close(q->call_fd);
            free(q->inflight);
            free(q->free_slots);
        }
        free(br->queues);
    }

    if (br->map)
        munmap(br->map, br->map_size);
    if (br->physmem_fd >= 0)
        close(br->physmem_fd);
    if (br->sock_fd >= 0)
        close(br->sock_fd);
    if (br->listen_fd >= 0)
        close(br->listen_fd);
    if (br->sock_path)
        unlink(br->sock_path);

    free(br->name);
    free(br->sock_path);
    free(br->vdpa_bin);
    free(br->mgmtdev);
    free(br);
}

const char *vk_bridge_name(const struct vk_bridge *br) {
    return br->name;
}

uint32_t vk_bridge_device_id(const struct vk_bridge *br) {
    return br->hello.device_id;
}
