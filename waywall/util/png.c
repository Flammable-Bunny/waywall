#include "util/png.h"
#include "util/alloc.h"
#include "util/log.h"
#include <fcntl.h>
#include <spng.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct util_png
util_png_decode(const char *data, size_t data_size, int max_size) {
    struct util_png result = {0};

    struct spng_ctx *ctx = spng_ctx_new(0);
    if (!ctx) {
        ww_log(LOG_ERROR, "failed to create spng context");
        return result;
    }
    spng_set_image_limits(ctx, max_size, max_size);

    int err = spng_set_png_buffer(ctx, data, data_size);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to set PNG buffer: %s", spng_strerror(err));
        goto fail_set_buffer;
    }

    struct spng_ihdr ihdr;
    err = spng_get_ihdr(ctx, &ihdr);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to get image header: %s", spng_strerror(err));
        goto fail_get_ihdr;
    }

    err = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &result.size);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to get size of decoded image: %s", spng_strerror(err));
        goto fail_get_size;
    }

    result.data = malloc(result.size);
    check_alloc(result.data);

    err = spng_decode_image(ctx, result.data, result.size, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to decode image: %s", spng_strerror(err));
        goto fail_decode;
    }

    result.width = ihdr.width;
    result.height = ihdr.height;

    spng_ctx_free(ctx);
    return result;

fail_decode:
    free(result.data);

fail_get_size:
fail_get_ihdr:
fail_set_buffer:
    spng_ctx_free(ctx);

    result.data = NULL;
    return result;
}
