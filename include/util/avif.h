#ifndef WAYWALL_UTIL_AVIF_H
#define WAYWALL_UTIL_AVIF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct util_avif_frame {
    char *data;
    size_t size;
    int32_t width;
    int32_t height;
    double duration;
};

struct util_avif {
    struct util_avif_frame *frames;
    size_t frame_count;
    bool is_animated;
    int32_t loop_count;

    // For single-frame AVIFs, only frames[0] is used
    int32_t width;
    int32_t height;
};

// Decode a single-frame or animated AVIF from a file path
struct util_avif util_avif_decode(const char *path, unsigned int max_size);

// Decode a single-frame or animated AVIF from raw data
struct util_avif util_avif_decode_raw(const char *data, size_t data_size, unsigned int max_size);

// Free all resources associated with a util_avif structure
void util_avif_free(struct util_avif *avif);

#endif
