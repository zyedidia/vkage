#include "vduse/error.h"
#include "internal.h"

#include <stdarg.h>
#include <stdio.h>

static _Thread_local int vd_err;
static _Thread_local char vd_msg[256];

int vd_set_error(int err, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(vd_msg, sizeof(vd_msg), fmt, ap);
    va_end(ap);

    vd_err = err;
    return err;
}

void vd_clear_error(void) {
    vd_err = 0;
    vd_msg[0] = '\0';
}

int vd_last_error(void) {
    return vd_err;
}

const char *vd_last_error_msg(void) {
    return vd_msg;
}
