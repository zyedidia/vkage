#include "vduse/bridge.h"

#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --name NAME [--uml PATH --uml-args ARGS] [options]\n"
            "\n"
            "  -n, --name NAME       vduse device name (/dev/vduse/NAME)\n"
            "  -u, --uml PATH        UML kernel to spawn; omit to start it\n"
            "                        yourself with umvirtio.sock=SOCK\n"
            "  -a, --uml-args ARGS   extra UML arguments, space separated\n"
            "  -S, --sock PATH       handshake socket\n"
            "                        (default /tmp/vduse-bridge-NAME.sock)\n"
            "  -t, --timeout MS      handshake timeout (default 30000)\n"
            "  -V, --vdpa-bin PATH   vdpa binary (default \"vdpa\")\n"
            "  -h, --help\n"
            "\n"
            "example:\n"
            "  %s --name nvme0 \\\n"
            "     --uml ../linux-uml/build-nvme/linux \\\n"
            "     --uml-args 'mem=256M vfio_uml.device=0000:01:00.0"
            " umvirtio_blk.disk=259:0'\n",
            argv0, argv0);
}

int main(int argc, char **argv) {
    static const struct option longopts[] = {
        {"name", required_argument, NULL, 'n'},
        {"uml", required_argument, NULL, 'u'},
        {"uml-args", required_argument, NULL, 'a'},
        {"sock", required_argument, NULL, 'S'},
        {"timeout", required_argument, NULL, 't'},
        {"vdpa-bin", required_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };
    static const int sigs[] = {SIGINT, SIGTERM};

    struct vd_bridge_opts opts = {0};
    struct vd_bridge *br = NULL;
    struct vd_loop *loop = NULL;
    int rc = 1;
    int c;

    while ((c = getopt_long(argc, argv, "n:u:a:S:t:V:h", longopts, NULL)) !=
           -1) {
        switch (c) {
        case 'n':
            opts.name = optarg;
            break;
        case 'u':
            opts.uml_bin = optarg;
            break;
        case 'a':
            opts.uml_args = optarg;
            break;
        case 'S':
            opts.sock_path = optarg;
            break;
        case 't':
            opts.handshake_timeout_ms = (unsigned)strtoul(optarg, NULL, 0);
            break;
        case 'V':
            opts.vdpa_bin = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (!opts.name) {
        usage(argv[0]);
        return 1;
    }

    if (!vd_loop_new(&loop)) {
        fprintf(stderr, "loop: %s\n", vd_last_error_msg());
        return 1;
    }

    if (!vd_loop_catch_signals(loop, sigs, 2)) {
        fprintf(stderr, "signals: %s\n", vd_last_error_msg());
        vd_loop_free(loop);
        return 1;
    }

    // Blocks until UML connects and declares what it is.
    if (!vd_bridge_new(opts, &br)) {
        fprintf(stderr, "bridge: %s\n", vd_last_error_msg());
        vd_loop_free(loop);
        return 1;
    }

    printf("created /dev/vduse/%s, virtio device id %" PRIu32 "\n",
           vd_bridge_name(br), vd_bridge_device_id(br));
    fflush(stdout);

    if (!vd_bridge_attach(br, loop)) {
        fprintf(stderr, "attach: %s\n", vd_last_error_msg());
        goto out;
    }

    if (!vd_loop_run(loop)) {
        fprintf(stderr, "run: %s\n", vd_last_error_msg());
        goto out;
    }

    rc = 0;

out:
    // Must precede vd_loop_free(), same as vd_blk_free().
    vd_bridge_free(br);
    vd_loop_free(loop);
    return rc;
}
