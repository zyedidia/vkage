#include "vduse/loop.h"

#include "internal.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#define MAX_EVENTS 64

struct vd_loop_entry {
    vd_loop_cb cb;
    void *user;
    uint32_t gen;
    bool active;
};

struct vd_loop {
    int epfd;

    // Indexed by fd. epoll_data carries fd plus the generation that was
    // current when it was registered, so an event queued for a descriptor
    // that a callback removed earlier in the same batch is discarded
    // instead of dispatched.
    struct vd_loop_entry *ents;
    int n_ents;
    uint32_t next_gen;

    bool stopping;
    bool failed;

    int sigfd;
    sigset_t oldmask;
    bool mask_saved;
};

static bool ensure_capacity(struct vd_loop *loop, int fd) {
    struct vd_loop_entry *ents;
    int n;

    if (fd < loop->n_ents)
        return true;

    n = loop->n_ents ? loop->n_ents : 16;
    while (n <= fd)
        n *= 2;

    ents = realloc(loop->ents, (size_t)n * sizeof(*ents));
    if (!ents) {
        vd_set_error(ENOMEM, "out of memory");
        return false;
    }

    memset(ents + loop->n_ents, 0,
           (size_t)(n - loop->n_ents) * sizeof(*ents));
    loop->ents = ents;
    loop->n_ents = n;
    return true;
}

bool vd_loop_new(struct vd_loop **out) {
    struct vd_loop *loop;

    if (!out) {
        vd_set_error(EINVAL, "out is NULL");
        return false;
    }

    loop = calloc(1, sizeof(*loop));
    if (!loop) {
        vd_set_error(ENOMEM, "out of memory");
        return false;
    }

    loop->sigfd = -1;
    loop->next_gen = 1;

    loop->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epfd < 0) {
        vd_set_error(errno, "epoll_create1: %s", strerror(errno));
        free(loop);
        return false;
    }

    *out = loop;
    return true;
}

void vd_loop_free(struct vd_loop *loop) {
    if (!loop)
        return;

    if (loop->sigfd >= 0)
        close(loop->sigfd);
    if (loop->mask_saved)
        sigprocmask(SIG_SETMASK, &loop->oldmask, NULL);
    if (loop->epfd >= 0)
        close(loop->epfd);

    free(loop->ents);
    free(loop);
}

bool vd_loop_add(struct vd_loop *loop, int fd, vd_loop_cb cb, void *user) {
    struct epoll_event ev;
    bool present;
    int rc;

    if (fd < 0 || !cb) {
        vd_set_error(EINVAL, "bad fd or callback");
        return false;
    }
    if (!ensure_capacity(loop, fd))
        return false;

    present = loop->ents[fd].active;

    loop->ents[fd].cb = cb;
    loop->ents[fd].user = user;
    loop->ents[fd].gen = loop->next_gen++;
    loop->ents[fd].active = true;

    ev.events = EPOLLIN;
    ev.data.u64 =
        ((uint64_t)loop->ents[fd].gen << 32) | (uint64_t)(uint32_t)fd;

    rc = epoll_ctl(loop->epfd, present ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &ev);

    // A registration goes stale if its fd is closed behind our back, since
    // close() drops it from the epoll set without telling us. If the number
    // is then reused, MOD fails and ADD is what we actually wanted.
    if (rc < 0 && present && errno == ENOENT)
        rc = epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev);

    if (rc < 0) {
        if (!present)
            loop->ents[fd].active = false;
        vd_set_error(errno, "epoll_ctl add: %s", strerror(errno));
        return false;
    }

    return true;
}

bool vd_loop_del(struct vd_loop *loop, int fd) {
    // Tolerate descriptors that were never registered; vd_blk_free() relies
    // on this when unwinding a partially attached device.
    if (fd < 0 || fd >= loop->n_ents || !loop->ents[fd].active)
        return true;

    loop->ents[fd].active = false;

    if (epoll_ctl(loop->epfd, EPOLL_CTL_DEL, fd, NULL) < 0 &&
        errno != EBADF && errno != ENOENT) {
        vd_set_error(errno, "epoll_ctl del: %s", strerror(errno));
        return false;
    }

    return true;
}

static void on_signal(int fd, void *user) {
    struct vd_loop *loop = user;
    struct signalfd_siginfo si;

    while (read(fd, &si, sizeof(si)) == (ssize_t)sizeof(si))
        ;

    vd_loop_stop(loop);
}

bool vd_loop_catch_signals(struct vd_loop *loop, const int *signals, int n) {
    sigset_t mask;

    if (loop->sigfd >= 0) {
        vd_set_error(EEXIST, "signals are already being caught");
        return false;
    }

    sigemptyset(&mask);
    for (int i = 0; i < n; i++)
        sigaddset(&mask, signals[i]);

    if (sigprocmask(SIG_BLOCK, &mask, &loop->oldmask) < 0) {
        vd_set_error(errno, "sigprocmask: %s", strerror(errno));
        return false;
    }
    loop->mask_saved = true;

    loop->sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (loop->sigfd < 0) {
        vd_set_error(errno, "signalfd: %s", strerror(errno));
        return false;
    }

    return vd_loop_add(loop, loop->sigfd, on_signal, loop);
}

bool vd_loop_run(struct vd_loop *loop) {
    struct epoll_event evs[MAX_EVENTS];

    loop->stopping = false;
    loop->failed = false;

    while (!loop->stopping) {
        int n = epoll_wait(loop->epfd, evs, MAX_EVENTS, -1);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            vd_set_error(errno, "epoll_wait: %s", strerror(errno));
            return false;
        }

        for (int i = 0; i < n; i++) {
            int fd = (int)(uint32_t)(evs[i].data.u64 & 0xffffffffu);
            uint32_t gen = (uint32_t)(evs[i].data.u64 >> 32);
            struct vd_loop_entry *e;
            vd_loop_cb cb;
            void *user;

            if (fd < 0 || fd >= loop->n_ents)
                continue;

            e = &loop->ents[fd];
            if (!e->active || e->gen != gen)
                continue;

            // Copied out because the callback may vd_loop_add() and
            // reallocate the table under us.
            cb = e->cb;
            user = e->user;
            cb(fd, user);
        }
    }

    return !loop->failed;
}

void vd_loop_stop(struct vd_loop *loop) {
    loop->stopping = true;
}

void vd_loop_fail(struct vd_loop *loop) {
    loop->failed = true;
    loop->stopping = true;
}

