#pragma once

#include <stdbool.h>

// Level-triggered epoll loop.
struct vk_loop;

typedef void (*vk_loop_cb)(int fd, void *user);

bool vk_loop_new(struct vk_loop **out);
void vk_loop_free(struct vk_loop *loop);

bool vk_loop_add(struct vk_loop *loop, int fd, vk_loop_cb cb, void *user);

bool vk_loop_del(struct vk_loop *loop, int fd);

bool vk_loop_catch_signals(struct vk_loop *loop, const int *signals, int n);

bool vk_loop_run(struct vk_loop *loop);

void vk_loop_stop(struct vk_loop *loop);

// Stop the loop and make vk_loop_run() return false, for a callback that
// hit an error rather than a clean shutdown.
void vk_loop_fail(struct vk_loop *loop);
