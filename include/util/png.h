#ifndef WAYWALL_UTIL_PNG_H
#define WAYWALL_UTIL_PNG_H

#include <stddef.h>
#include <stdint.h>

struct util_png {
    char *data;
    size_t size;

    int32_t width, height;
};

struct util_png util_png_decode(const char *path, unsigned int max_size);
struct util_png util_png_decode_raw(const char *data, size_t data_size, unsigned int max_size);

#endif
