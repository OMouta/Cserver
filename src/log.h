#pragma once
#include <stddef.h>

void log_init(const char *path, unsigned long long rotate_size, int rotate_count);
void log_printf(const char *level, const char *fmt, ...);
void log_shutdown(void);
