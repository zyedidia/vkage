#pragma once

#include <stdbool.h>

// Spawn `$bin dev add name $name mgmtdev $mgmtdev`. Returns a pidfd.
int vk_bus_spawn_add(const char *bin, const char *name, const char *mgmtdev);

// Spawn `$bin dev del $name`.
int vk_bus_spawn_del(const char *bin, const char *name);

// Reap the child behind pidfd.
bool vk_bus_reap(int pidfd);
