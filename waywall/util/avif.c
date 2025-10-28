#include "util/avif.h"
#include "util/alloc.h"
#include "util/log.h"
#include <avif/avif.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct util_avif
util_avif_decode(const char *path, unsigned int max_size) {
    struct util_avif result = {0};

    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open AVIF");
        return result;
    }

    struct stat stat;
    if (fstat(fd, &stat) != 0) {
        ww_log_errno(LOG_ERROR, "failed to stat AVIF");
        goto fail_stat;
    }

    void *buf = mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (buf == MAP_FAILED) {
        ww_log_errno(LOG_ERROR, "failed to mmap AVIF (size %ju)", (uintmax_t)stat.st_size);
        goto fail_mmap;
    }

    result = util_avif_decode_raw(buf, stat.st_size, max_size);

    munmap(buf, stat.st_size);
    close(fd);
    return result;

fail_mmap:
fail_stat:
    close(fd);
    return result;
}

struct util_avif
util_avif_decode_raw(const char *data, size_t data_size, unsigned int max_size) {
    struct util_avif result = {0};

    avifDecoder *decoder = avifDecoderCreate();
    if (!decoder) {
        ww_log(LOG_ERROR, "failed to create AVIF decoder");
        return result;
    }

    // Set size limits
    decoder->imageSizeLimit = max_size * max_size;
    decoder->imageDimensionLimit = max_size;

    // Parse the AVIF data
    avifResult res =
        avifDecoderSetIOMemory(decoder, (const uint8_t *)data, data_size);
    if (res != AVIF_RESULT_OK) {
        ww_log(LOG_ERROR, "failed to set AVIF IO: %s", avifResultToString(res));
        goto fail_decoder;
    }

    res = avifDecoderParse(decoder);
    if (res != AVIF_RESULT_OK) {
        ww_log(LOG_ERROR, "failed to parse AVIF: %s", avifResultToString(res));
        goto fail_decoder;
    }

    result.width = decoder->image->width;
    result.height = decoder->image->height;
    result.frame_count = decoder->imageCount;
    result.is_animated = (decoder->imageCount > 1);
    result.loop_count = decoder->repetitionCount;

    if (result.frame_count == 0) {
        ww_log(LOG_ERROR, "AVIF has no frames");
        goto fail_decoder;
    }

    // Allocate frame array
    result.frames = zalloc(result.frame_count, sizeof(struct util_avif_frame));
    if (!result.frames) {
        ww_log(LOG_ERROR, "failed to allocate frame array");
        goto fail_decoder;
    }

    // Decode all frames
    for (size_t i = 0; i < result.frame_count; i++) {
        res = avifDecoderNextImage(decoder);
        if (res != AVIF_RESULT_OK) {
            ww_log(LOG_ERROR, "failed to decode AVIF frame %zu: %s", i, avifResultToString(res));
            goto fail_frames;
        }

        avifImage *image = decoder->image;

        // Calculate frame buffer size (RGBA8)
        size_t frame_size = image->width * image->height * 4;
        result.frames[i].data = malloc(frame_size);
        if (!result.frames[i].data) {
            ww_log(LOG_ERROR, "failed to allocate frame %zu data", i);
            goto fail_frames;
        }

        result.frames[i].width = image->width;
        result.frames[i].height = image->height;
        result.frames[i].size = frame_size;
        result.frames[i].duration = decoder->imageTiming.duration;

        // Convert to RGBA8
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, image);
        rgb.format = AVIF_RGB_FORMAT_RGBA;
        rgb.depth = 8;
        rgb.pixels = (uint8_t *)result.frames[i].data;
        rgb.rowBytes = image->width * 4;

        res = avifImageYUVToRGB(image, &rgb);
        if (res != AVIF_RESULT_OK) {
            ww_log(LOG_ERROR, "failed to convert AVIF frame %zu to RGB: %s", i,
                   avifResultToString(res));
            free(result.frames[i].data);
            result.frames[i].data = NULL;
            goto fail_frames;
        }
    }

    avifDecoderDestroy(decoder);
    return result;

fail_frames:
    // Clean up any allocated frames
    for (size_t i = 0; i < result.frame_count; i++) {
        free(result.frames[i].data);
    }
    free(result.frames);
    result.frames = NULL;

fail_decoder:
    avifDecoderDestroy(decoder);
    memset(&result, 0, sizeof(result));
    return result;
}

void
util_avif_free(struct util_avif *avif) {
    if (!avif) {
        return;
    }

    if (avif->frames) {
        for (size_t i = 0; i < avif->frame_count; i++) {
            free(avif->frames[i].data);
        }
        free(avif->frames);
    }

    memset(avif, 0, sizeof(*avif));
}
