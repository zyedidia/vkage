#include "vkage/blk.h"

#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --name NAME --file PATH [options]\n"
            "\n"
            "  -n, --name NAME       vduse device name (/dev/vduse/NAME)\n"
            "  -f, --file PATH       backing file or block device\n"
            "  -r, --read-only       export read-only\n"
            "  -d, --direct          open the backing file with O_DIRECT\n"
            "  -b, --block-size N    512 or 4096 (default 512)\n"
            "  -q, --queue-size N    power of two <= 1024 (default 256)\n"
            "  -s, --serial STR      serial reported by GET_ID\n"
            "  -c, --capacity BYTES  override the backing file's size\n"
            "  -V, --vdpa-bin PATH   vdpa binary (default \"vdpa\")\n"
            "  -h, --help\n",
            argv0);
}

int main(int argc, char **argv) {
    static const struct option longopts[] = {
        {"name", required_argument, NULL, 'n'},
        {"file", required_argument, NULL, 'f'},
        {"read-only", no_argument, NULL, 'r'},
        {"direct", no_argument, NULL, 'd'},
        {"block-size", required_argument, NULL, 'b'},
        {"queue-size", required_argument, NULL, 'q'},
        {"serial", required_argument, NULL, 's'},
        {"capacity", required_argument, NULL, 'c'},
        {"vdpa-bin", required_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };
    static const int sigs[] = {SIGINT, SIGTERM};

    struct vk_backend_file_opts fopts = {0};
    struct vk_blk_opts bopts = {0};
    struct vk_blk_backend be;
    struct vk_loop *loop = NULL;
    struct vk_blk *blk = NULL;
    uint64_t size = 0;
    int rc = 1;
    int c;

    while ((c = getopt_long(argc, argv, "n:f:rdb:q:s:c:V:h", longopts,
                            NULL)) != -1) {
        switch (c) {
        case 'n':
            bopts.name = optarg;
            break;
        case 'f':
            fopts.path = optarg;
            break;
        case 'r':
            fopts.readonly = true;
            bopts.readonly = true;
            break;
        case 'd':
            fopts.direct = true;
            break;
        case 'b':
            bopts.block_size = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'q':
            bopts.queue_size = (uint16_t)strtoul(optarg, NULL, 0);
            break;
        case 's':
            bopts.serial = optarg;
            break;
        case 'c':
            bopts.capacity = strtoull(optarg, NULL, 0);
            break;
        case 'V':
            bopts.vdpa_bin = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (!bopts.name || !fopts.path) {
        usage(argv[0]);
        return 1;
    }

    if (!vk_backend_file_open(fopts, &be, &size)) {
        fprintf(stderr, "backend: %s\n", vk_last_error_msg());
        return 1;
    }

    if (bopts.capacity == 0)
        bopts.capacity = size;

    if (!vk_loop_new(&loop)) {
        fprintf(stderr, "loop: %s\n", vk_last_error_msg());
        be.destroy(be.priv);
        return 1;
    }

    if (!vk_loop_catch_signals(loop, sigs, 2)) {
        fprintf(stderr, "signals: %s\n", vk_last_error_msg());
        be.destroy(be.priv);
        vk_loop_free(loop);
        return 1;
    }

    // The device takes ownership of the backend from here on.
    if (!vk_blk_new(bopts, be, &blk)) {
        fprintf(stderr, "device: %s\n", vk_last_error_msg());
        be.destroy(be.priv);
        vk_loop_free(loop);
        return 1;
    }

    printf("created /dev/vduse/%s, %" PRIu64 " bytes\n", vk_blk_name(blk),
           vk_blk_capacity(blk));
    fflush(stdout);

    if (!vk_blk_attach(blk, loop)) {
        fprintf(stderr, "attach: %s\n", vk_last_error_msg());
        goto out;
    }

    if (!vk_loop_run(loop)) {
        fprintf(stderr, "run: %s\n", vk_last_error_msg());
        goto out;
    }

    rc = 0;

out:
    // Must precede vk_loop_free(): teardown drives a reset whose
    // disable_queue callback deregisters the kick fd from the loop.
    vk_blk_free(blk);
    vk_loop_free(loop);
    return rc;
}
