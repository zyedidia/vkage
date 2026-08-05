#include "vkage/error.h"
#include "internal.h"

#include <stdarg.h>
#include <stdio.h>

static _Thread_local int vk_err;
static _Thread_local char vk_msg[256];

int vk_set_error(int err, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(vk_msg, sizeof(vk_msg), fmt, ap);
    va_end(ap);

    vk_err = err;
    return err;
}

void vk_clear_error(void) {
    vk_err = 0;
    vk_msg[0] = '\0';
}

int vk_last_error(void) {
    return vk_err;
}

const char *vk_last_error_msg(void) {
    return vk_msg;
}
