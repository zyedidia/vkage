#pragma once

int vk_set_error(int err, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

void vk_clear_error(void);
