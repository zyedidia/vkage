#pragma once

#include <stdbool.h>

// Level-triggered epoll loop.
struct vd_loop;

typedef void (*vd_loop_cb)(int fd, void *user);

bool vd_loop_new(struct vd_loop **out);
void vd_loop_free(struct vd_loop *loop);

bool vd_loop_add(struct vd_loop *loop, int fd, vd_loop_cb cb, void *user);

bool vd_loop_del(struct vd_loop *loop, int fd);

bool vd_loop_catch_signals(struct vd_loop *loop, const int *signals, int n);

bool vd_loop_run(struct vd_loop *loop);

void vd_loop_stop(struct vd_loop *loop);

// Stop the loop and make vd_loop_run() return false, for a callback that
// hit an error rather than a clean shutdown.
void vd_loop_fail(struct vd_loop *loop);
