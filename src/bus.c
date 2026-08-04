#include "bus.h"
#include "internal.h"

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/pidfd.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

// The child blocks in the kernel until the daemon answers the probe, so we
// return a pidfd rather than reaping here. Reaping inline would deadlock:
// the caller is the only thread that can service the control fd.
static int spawn_pidfd(const char *bin, char *const argv[]) {
    pid_t pid;
    int fd, rc;

    rc = posix_spawnp(&pid, bin, NULL, NULL, argv, environ);
    if (rc != 0) {
        vd_set_error(rc, "spawn %s: %s", bin, strerror(rc));
        return -1;
    }

    fd = pidfd_open(pid, 0);
    if (fd < 0) {
        rc = errno;
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        vd_set_error(rc, "pidfd_open: %s", strerror(rc));
        return -1;
    }

    return fd;
}

int vd_bus_spawn_add(const char *bin, const char *name, const char *mgmtdev) {
    char *argv[] = {
        (char *)bin, "dev",     "add",
        "name",      (char *)name,
        "mgmtdev",   (char *)mgmtdev,
        NULL,
    };

    return spawn_pidfd(bin, argv);
}

int vd_bus_spawn_del(const char *bin, const char *name) {
    char *argv[] = {(char *)bin, "dev", "del", (char *)name, NULL};

    return spawn_pidfd(bin, argv);
}

bool vd_bus_reap(int pidfd) {
    siginfo_t si = {0};

    if (waitid(P_PIDFD, (id_t)pidfd, &si, WEXITED) < 0) {
        vd_set_error(errno, "waitid: %s", strerror(errno));
        return false;
    }
    if (si.si_code != CLD_EXITED) {
        vd_set_error(EIO, "vdpa killed by signal %d", si.si_status);
        return false;
    }
    if (si.si_status != 0) {
        vd_set_error(EIO, "vdpa exited with status %d", si.si_status);
        return false;
    }

    return true;
}
