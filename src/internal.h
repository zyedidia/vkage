#pragma once

int vd_set_error(int err, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

void vd_clear_error(void);
