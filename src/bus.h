#pragma once

#include <stdbool.h>

// Spawn `$bin dev add name $name mgmtdev $mgmtdev`. Returns a pidfd.
int vd_bus_spawn_add(const char *bin, const char *name, const char *mgmtdev);

// Spawn `$bin dev del $name`.
int vd_bus_spawn_del(const char *bin, const char *name);

// Reap the child behind pidfd.
bool vd_bus_reap(int pidfd);
