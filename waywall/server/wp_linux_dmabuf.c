#define _GNU_SOURCE  // For memfd_create
#include "server/wp_linux_dmabuf.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-dmabuf-v1-server-protocol.h"
#include "server/backend.h"
#include "server/buffer.h"
#include "server/server.h"
#include "server/wl_compositor.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <drm/drm_fourcc.h>
#include <gbm.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <linux/memfd.h>
#include <sys/syscall.h>
#include <unistd.h>

// memfd_create wrapper (in case libc doesn't provide it)
static inline int ww_memfd_create(const char *name, unsigned int flags) {
    return (int)syscall(SYS_memfd_create, name, flags);
}
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-util.h>

#define SRV_LINUX_DMABUF_VERSION 4

static const struct zwp_linux_buffer_params_v1_listener linux_buffer_params_listener;

static void
destroy_dmabuf_buffer_data(struct server_dmabuf_data *data) {
    for (size_t i = 0; i < data->export_count; i++) {
        if (data->exports[i].remote) {
            wl_buffer_destroy(data->exports[i].remote);
            data->exports[i].remote = NULL;
        }
        if (data->exports[i].fd != -1) {
            close(data->exports[i].fd);
            data->exports[i].fd = -1;
        }
    }

    for (size_t i = 0; i < data->num_planes; i++) {
        close(data->planes[i].fd);
    }

    free(data);
}

static void
dmabuf_buffer_destroy(void *data) {
    struct server_dmabuf_data *buffer_data = data;
    destroy_dmabuf_buffer_data(buffer_data);
}

static void
dmabuf_buffer_size(void *data, int32_t *width, int32_t *height) {
    struct server_dmabuf_data *buffer_data = data;

    *width = buffer_data->width;
    *height = buffer_data->height;
}

static const struct server_buffer_impl dmabuf_buffer_impl = {
    .name = SERVER_BUFFER_DMABUF,

    .destroy = dmabuf_buffer_destroy,
    .size = dmabuf_buffer_size,
};

static void
on_export_wl_buffer_release(void *data, struct wl_buffer *wl_buffer) {
    (void)wl_buffer;
    bool *busy = data;
    *busy = false;
}

static const struct wl_buffer_listener export_wl_buffer_listener = {
    .release = on_export_wl_buffer_release,
};

static struct wl_buffer *
create_export_wl_buffer(struct server_linux_dmabuf *linux_dmabuf, int32_t width, int32_t height,
                        uint32_t format, uint32_t flags, int fd, uint32_t stride,
                        uint32_t modifier_hi, uint32_t modifier_lo) {
    struct server_linux_buffer_params params = {0};
    params.parent = linux_dmabuf;
    params.status = BUFFER_PARAMS_STATUS_UNKNOWN;

    params.remote = zwp_linux_dmabuf_v1_create_params(linux_dmabuf->remote);
    check_alloc(params.remote);
    zwp_linux_buffer_params_v1_add_listener(params.remote, &linux_buffer_params_listener, &params);

    zwp_linux_buffer_params_v1_add(params.remote, fd, 0, 0, stride, modifier_hi, modifier_lo);
    zwp_linux_buffer_params_v1_create(params.remote, width, height, format, flags);

    wl_display_roundtrip_queue(linux_dmabuf->remote_display, linux_dmabuf->queue);

    if (params.status != BUFFER_PARAMS_STATUS_OK || !params.ok_buffer) {
        if (params.remote) {
            zwp_linux_buffer_params_v1_destroy(params.remote);
        }
        return NULL;
    }

    wl_proxy_set_queue((struct wl_proxy *)params.ok_buffer, linux_dmabuf->main_queue);

    struct wl_buffer *ok = params.ok_buffer;
    params.ok_buffer = NULL;
    zwp_linux_buffer_params_v1_destroy(params.remote);
    return ok;
}

static bool
dmabuf_setup_export_buffers(struct server_linux_dmabuf *linux_dmabuf, struct server_dmabuf_data *data) {
    // Currently only needed/implemented for the common 1-plane RGB formats.
    if (data->num_planes != 1) {
        ww_log(LOG_ERROR, "proxy export: unsupported plane count: %u", data->num_planes);
        return false;
    }

    if (!linux_dmabuf->export_gbm) {
        ww_log(LOG_ERROR, "proxy export: no GBM device available");
        return false;
    }

    data->proxy_export = true;
    data->export_count = 0;
    for (size_t i = 0; i < STATIC_ARRLEN(data->exports); i++) {
        data->exports[i].fd = -1;
    }

    for (uint32_t i = 0; i < DMABUF_EXPORT_MAX; i++) {
        struct gbm_bo *bo = gbm_bo_create(linux_dmabuf->export_gbm, data->width, data->height,
                                          data->format, GBM_BO_USE_RENDERING);
        if (!bo) {
            ww_log(LOG_ERROR, "proxy export: gbm_bo_create failed for %dx%d format=0x%x",
                   data->width, data->height, data->format);
            break;
        }

        int bo_fd = gbm_bo_get_fd(bo);
        uint32_t bo_stride = gbm_bo_get_stride(bo);
        uint64_t bo_mod = gbm_bo_get_modifier(bo);
        gbm_bo_destroy(bo);

        if (bo_fd < 0 || bo_stride == 0) {
            ww_log(LOG_ERROR, "proxy export: gbm_bo_get_fd/stride failed");
            if (bo_fd >= 0) {
                close(bo_fd);
            }
            break;
        }

        struct wl_buffer *wl_buf = create_export_wl_buffer(
            linux_dmabuf, data->width, data->height, data->format, data->flags, bo_fd, bo_stride,
            (uint32_t)(bo_mod >> 32), (uint32_t)bo_mod);
        if (!wl_buf) {
            ww_log(LOG_ERROR, "proxy export: failed to create wl_buffer on host compositor");
            close(bo_fd);
            break;
        }

        data->exports[i].fd = bo_fd;
        data->exports[i].offset = 0;
        data->exports[i].stride = bo_stride;
        data->exports[i].modifier_hi = (uint32_t)(bo_mod >> 32);
        data->exports[i].modifier_lo = (uint32_t)bo_mod;
        data->exports[i].remote = wl_buf;
        data->exports[i].busy = false;
        wl_buffer_add_listener(wl_buf, &export_wl_buffer_listener, &data->exports[i].busy);

        data->export_count++;
    }

    if (data->export_count == 0) {
        data->proxy_export = false;
        return false;
    }

    return true;
}

static bool
check_buffer_params(struct server_linux_buffer_params *buffer_params) {
    if (buffer_params->used) {
        wl_resource_post_error(buffer_params->resource,
                               ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "cannot call create on the same zwp_linux_buffer_params twice");
        return false;
    }

    if (buffer_params->data->num_planes == 0) {
        wl_resource_post_error(buffer_params->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                               "zwp_linux_buffer_params has no planes");
        return false;
    }

    for (size_t i = 0; i < buffer_params->data->num_planes; i++) {
        if (buffer_params->data->planes[i].fd == -1) {

            wl_resource_post_error(buffer_params->resource,
                                   ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                                   "zwp_linux_buffer_params has gap at plane %zu", i);
            return false;
        }
    }

    return true;
}

static void
create_buffer(struct server_linux_buffer_params *buffer_params, struct wl_resource *buffer_resource,
              int32_t width, int32_t height, uint32_t format, uint32_t flags) {
    buffer_params->data->width = width;
    buffer_params->data->height = height;
    buffer_params->data->format = format;
    buffer_params->data->flags = flags;

    if (buffer_params->parent->proxy_game) {
        ww_log(LOG_INFO,
               "dmabuf proxy create: %dx%d format=0x%x flags=%u planes=%u modifier=%#" PRIx64,
               width, height, format, flags, buffer_params->data->num_planes,
               ((uint64_t)buffer_params->data->modifier_hi << 32) | buffer_params->data->modifier_lo);

        // Proxy-export mode: allocate exportable buffers on the compositor GPU and use Vulkan
        // to copy the client dmabuf into those buffers. This avoids requiring the host compositor
        // to import the client’s cross-GPU dmabuf directly.
        if (!dmabuf_setup_export_buffers(buffer_params->parent, buffer_params->data)) {
            buffer_params->status = BUFFER_PARAMS_STATUS_NOT_OK;
            return;
        }

        buffer_params->buffer =
            server_buffer_create(buffer_resource, buffer_params->data->exports[0].remote,
                                 &dmabuf_buffer_impl, buffer_params->data);
        buffer_params->status = BUFFER_PARAMS_STATUS_OK;
        return;
    }

    // Default (composition) mode: create a "local-only" buffer that stores the dma-buf data but
    // does not have a corresponding wl_buffer on the parent compositor. The Vulkan backend will
    // import the dma-buf directly.
    ww_log(LOG_INFO, "creating local-only dmabuf: %dx%d, format=0x%x, modifier=0x%llx",
           width, height, format,
           (unsigned long long)(((uint64_t)buffer_params->data->modifier_hi << 32) |
                                buffer_params->data->modifier_lo));

    buffer_params->buffer =
        server_buffer_create(buffer_resource, NULL, &dmabuf_buffer_impl, buffer_params->data);
    buffer_params->status = BUFFER_PARAMS_STATUS_OK;
}

static void
on_linux_buffer_params_created(void *data, struct zwp_linux_buffer_params_v1 *wl,
                               struct wl_buffer *buffer) {
    struct server_linux_buffer_params *buffer_params = data;

    // Move the created buffer off of the linux_dmabuf queue and onto the main display queue.
    wl_proxy_set_queue((struct wl_proxy *)buffer, buffer_params->parent->main_queue);

    ww_log(LOG_INFO, "dmabuf params created by host compositor (proxy_game=%d)",
           buffer_params->parent->proxy_game);

    buffer_params->ok_buffer = buffer;
    buffer_params->status = BUFFER_PARAMS_STATUS_OK;
}

static void
on_linux_buffer_params_failed(void *data, struct zwp_linux_buffer_params_v1 *wl) {
    struct server_linux_buffer_params *buffer_params = data;

    if (buffer_params->data) {
        ww_log(LOG_ERROR,
               "dmabuf params FAILED in host compositor (proxy_game=%d): %dx%d format=0x%x flags=%u modifier=%#" PRIx64
               " planes=%u stride0=%u",
               buffer_params->parent->proxy_game, buffer_params->data->width, buffer_params->data->height,
               buffer_params->data->format, buffer_params->data->flags,
               ((uint64_t)buffer_params->data->modifier_hi << 32) | buffer_params->data->modifier_lo,
               buffer_params->data->num_planes,
               buffer_params->data->num_planes ? buffer_params->data->planes[0].stride : 0);
    } else {
        ww_log(LOG_ERROR, "dmabuf params FAILED in host compositor (proxy_game=%d)",
               buffer_params->parent->proxy_game);
    }

    buffer_params->status = BUFFER_PARAMS_STATUS_NOT_OK;
}

static const struct zwp_linux_buffer_params_v1_listener linux_buffer_params_listener = {
    .created = on_linux_buffer_params_created,
    .failed = on_linux_buffer_params_failed,
};

static void
on_linux_dmabuf_feedback_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *wl) {
    struct server_linux_dmabuf_feedback *feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_done(feedback->resource);
}

// Format table entry structure (16 bytes per entry)
struct format_table_entry {
    uint32_t format;
    uint32_t padding;
    uint64_t modifier;
};

static void
on_linux_dmabuf_feedback_format_table(void *data, struct zwp_linux_dmabuf_feedback_v1 *wl,
                                      int32_t fd, uint32_t size) {
    struct server_linux_dmabuf_feedback *feedback = data;

    if (feedback->parent->proxy_game) {
        ww_log(LOG_INFO, "dmabuf feedback: passing through format table (proxy_game=1)");
        zwp_linux_dmabuf_feedback_v1_send_format_table(feedback->resource, fd, size);
        close(fd);
        return;
    }

    if (feedback->parent->force_intel_feedback) {
        // Override with a modifier-capable table (include a sample tiled modifier to hint clients)
        struct format_table_entry entries[] = {
            { DRM_FORMAT_XRGB8888, 0, DRM_FORMAT_MOD_LINEAR },
            { DRM_FORMAT_ARGB8888, 0, DRM_FORMAT_MOD_LINEAR },
            { DRM_FORMAT_XBGR8888, 0, DRM_FORMAT_MOD_LINEAR },
            { DRM_FORMAT_ABGR8888, 0, DRM_FORMAT_MOD_LINEAR },
            // Add explicit Intel tiling modifier to hint clients
            { DRM_FORMAT_XRGB8888, 0, fourcc_mod_code(INTEL, 1) }, // INTEL_X_TILED
        };

        size_t table_size = sizeof(entries);
        close(fd); // drop host table

        int new_fd = ww_memfd_create("dmabuf-format-table", MFD_CLOEXEC);
        if (new_fd < 0) {
            ww_log(LOG_ERROR, "failed to create memfd for modifier format table");
            return;
        }
        if (ftruncate(new_fd, table_size) < 0) {
            ww_log(LOG_ERROR, "failed to truncate modifier format table fd");
            close(new_fd);
            return;
        }
        void *map = mmap(NULL, table_size, PROT_READ | PROT_WRITE, MAP_SHARED, new_fd, 0);
        if (map == MAP_FAILED) {
            ww_log(LOG_ERROR, "failed to mmap modifier format table");
            close(new_fd);
            return;
        }
        memcpy(map, entries, table_size);
        munmap(map, table_size);

        ww_log(LOG_INFO, "dmabuf feedback: overriding format table for Intel (mods + linear)");
        zwp_linux_dmabuf_feedback_v1_send_format_table(feedback->resource, new_fd, table_size);
        close(new_fd);
        return;
    }

    if (feedback->parent->allow_modifiers) {
        ww_log(LOG_INFO, "dmabuf feedback: passing through modifier table (no LINEAR override)");
        zwp_linux_dmabuf_feedback_v1_send_format_table(feedback->resource, fd, size);
        close(fd);
        return;
    }

    // Close AMD's format table - we'll send our own with LINEAR formats only
    close(fd);

    // Create our own format table with LINEAR formats for cross-GPU compatibility
    // These formats work across Intel, AMD, and NVIDIA with LINEAR modifier
    struct format_table_entry entries[] = {
        { DRM_FORMAT_XRGB8888, 0, DRM_FORMAT_MOD_LINEAR },
        { DRM_FORMAT_ARGB8888, 0, DRM_FORMAT_MOD_LINEAR },
        { DRM_FORMAT_XBGR8888, 0, DRM_FORMAT_MOD_LINEAR },
        { DRM_FORMAT_ABGR8888, 0, DRM_FORMAT_MOD_LINEAR },
    };

    size_t table_size = sizeof(entries);

    // Create a memfd for our format table
    int new_fd = ww_memfd_create("dmabuf-format-table", MFD_CLOEXEC);
    if (new_fd < 0) {
        ww_log(LOG_ERROR, "failed to create memfd for LINEAR format table");
        return;
    }

    if (ftruncate(new_fd, table_size) < 0) {
        ww_log(LOG_ERROR, "failed to truncate LINEAR format table fd");
        close(new_fd);
        return;
    }

    void *map = mmap(NULL, table_size, PROT_READ | PROT_WRITE, MAP_SHARED, new_fd, 0);
    if (map == MAP_FAILED) {
        ww_log(LOG_ERROR, "failed to mmap LINEAR format table");
        close(new_fd);
        return;
    }

    memcpy(map, entries, table_size);
    munmap(map, table_size);

    ww_log(LOG_INFO, "sending LINEAR-only format table for cross-GPU compatibility");
    zwp_linux_dmabuf_feedback_v1_send_format_table(feedback->resource, new_fd, table_size);
    close(new_fd);
}

static void
on_linux_dmabuf_feedback_main_device(void *data, struct zwp_linux_dmabuf_feedback_v1 *wl,
                                     struct wl_array *device) {
    struct server_linux_dmabuf_feedback *feedback = data;

    if (feedback->parent->proxy_game) {
        zwp_linux_dmabuf_feedback_v1_send_main_device(feedback->resource, device);
        return;
    }

    if (feedback->parent->force_intel_feedback) {
        struct wl_array dev_arr;
        wl_array_init(&dev_arr);
        dev_t *dev = wl_array_add(&dev_arr, sizeof(dev_t));
        *dev = makedev(226, 129); // Intel renderD129
        ww_log(LOG_INFO, "dmabuf feedback: overriding main_device to renderD129 (Intel)");
        zwp_linux_dmabuf_feedback_v1_send_main_device(feedback->resource, &dev_arr);
        wl_array_release(&dev_arr);
    } else {
        // Pass through upstream device feedback unchanged.
        zwp_linux_dmabuf_feedback_v1_send_main_device(feedback->resource, device);
    }
}

static void
on_linux_dmabuf_feedback_tranche_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *wl) {
    struct server_linux_dmabuf_feedback *feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback->resource);
}

static void
on_linux_dmabuf_feedback_tranche_flags(void *data, struct zwp_linux_dmabuf_feedback_v1 *wl,
                                       uint32_t flags) {
    struct server_linux_dmabuf_feedback *feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback->resource, flags);
}

static void
on_linux_dmabuf_feedback_tranche_formats(void *data, struct zwp_linux_dmabuf_feedback_v1 *wl,
                                         struct wl_array *indices) {
    struct server_linux_dmabuf_feedback *feedback = data;

    if (feedback->parent->proxy_game) {
        zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback->resource, indices);
        return;
    }

    if (feedback->parent->force_intel_feedback) {
        // Allow all provided formats (including modifiers) when forcing Intel
        ww_log(LOG_INFO, "dmabuf feedback: passing through tranche_formats (Intel forced)");
        zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback->resource, indices);
        return;
    }

    if (feedback->parent->allow_modifiers) {
        ww_log(LOG_INFO, "dmabuf feedback: passing through tranche formats (modifiers allowed)");
        zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback->resource, indices);
        return;
    }

    // Always override to use indices 0-3 (our 4 LINEAR formats).
    // Waywall's GL pipeline can only handle LINEAR modifiers, regardless of compositor mode.
    struct wl_array override_indices;
    wl_array_init(&override_indices);

    // Add indices 0, 1, 2, 3 for our 4 LINEAR format entries
    for (uint16_t i = 0; i < 4; i++) {
        uint16_t *idx = wl_array_add(&override_indices, sizeof(uint16_t));
        *idx = i;
    }

    ww_log(LOG_INFO, "dmabuf feedback: overriding tranche_formats with LINEAR-only indices (0-3)");

    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback->resource, &override_indices);
    wl_array_release(&override_indices);
}

static void
on_linux_dmabuf_feedback_tranche_target_device(void *data, struct zwp_linux_dmabuf_feedback_v1 *wl,
                                               struct wl_array *device) {
    struct server_linux_dmabuf_feedback *feedback = data;

    if (feedback->parent->proxy_game) {
        zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback->resource, device);
        return;
    }

    if (feedback->parent->force_intel_feedback) {
        struct wl_array dev_arr;
        wl_array_init(&dev_arr);
        dev_t *dev = wl_array_add(&dev_arr, sizeof(dev_t));
        *dev = makedev(226, 129); // Intel renderD129
        ww_log(LOG_INFO, "dmabuf feedback: overriding tranche target_device to renderD129 (Intel)");
        zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback->resource, &dev_arr);
        wl_array_release(&dev_arr);
    } else {
        // Pass through upstream device feedback unchanged.
        zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback->resource, device);
    }
}

static const struct zwp_linux_dmabuf_feedback_v1_listener linux_dmabuf_feedback_listener = {
    .done = on_linux_dmabuf_feedback_done,
    .format_table = on_linux_dmabuf_feedback_format_table,
    .main_device = on_linux_dmabuf_feedback_main_device,
    .tranche_done = on_linux_dmabuf_feedback_tranche_done,
    .tranche_flags = on_linux_dmabuf_feedback_tranche_flags,
    .tranche_formats = on_linux_dmabuf_feedback_tranche_formats,
    .tranche_target_device = on_linux_dmabuf_feedback_tranche_target_device,
};

static void
linux_buffer_params_resource_destroy(struct wl_resource *resource) {
    struct server_linux_buffer_params *buffer_params = wl_resource_get_user_data(resource);

    if (buffer_params->status != BUFFER_PARAMS_STATUS_OK) {
        destroy_dmabuf_buffer_data(buffer_params->data);
    }

    if (buffer_params->remote) {
        zwp_linux_buffer_params_v1_destroy(buffer_params->remote);
    }
    free(buffer_params);
}

static void
linux_buffer_params_add(struct wl_client *client, struct wl_resource *resource, int32_t fd,
                        uint32_t plane_idx, uint32_t offset, uint32_t stride, uint32_t modifier_hi,
                        uint32_t modifier_lo) {
    struct server_linux_buffer_params *buffer_params = wl_resource_get_user_data(resource);

    if (plane_idx >= DMABUF_MAX_PLANES) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "plane %" PRIu32 " exceeds max of %d", plane_idx, DMABUF_MAX_PLANES);
        close(fd);
        return;
    }

    if (buffer_params->data->planes[plane_idx].fd != -1) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                               "plane %" PRIu32 " already set", plane_idx);
        close(fd);
        return;
    }

    bool eq_modifier = (modifier_lo == buffer_params->data->modifier_lo &&
                        modifier_hi == buffer_params->data->modifier_hi);
    if (buffer_params->data->num_planes > 0 && !eq_modifier) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                               "modifier of plane %" PRIu32 " does not match", plane_idx);
        close(fd);
        return;
    }

    buffer_params->data->planes[plane_idx].fd = fd;
    buffer_params->data->planes[plane_idx].offset = offset;
    buffer_params->data->planes[plane_idx].stride = stride;

    // Store the original modifier from Intel
    buffer_params->data->modifier_lo = modifier_lo;
    buffer_params->data->modifier_hi = modifier_hi;

    buffer_params->data->num_planes++;

    ww_log(LOG_INFO, "dmabuf add plane: fd=%d, plane_idx=%d, offset=%u, stride=%u, modifier=%#" PRIx64,
           fd, plane_idx, offset, stride, ((uint64_t)modifier_hi << 32) | modifier_lo);

    if (!buffer_params->parent->proxy_game) {
        zwp_linux_buffer_params_v1_add(buffer_params->remote, fd, plane_idx, offset, stride,
                                       modifier_hi, modifier_lo);
    }
}

static void
linux_buffer_params_create(struct wl_client *client, struct wl_resource *resource, int32_t width,
                           int32_t height, uint32_t format, uint32_t flags) {
    struct server_linux_buffer_params *buffer_params = wl_resource_get_user_data(resource);

    if (!check_buffer_params(buffer_params)) {
        return;
    }

    ww_log(LOG_INFO, "dmabuf create_immed: width=%d, height=%d, format=0x%x, flags=%u, num_planes=%u",
           width, height, format, flags, buffer_params->data->num_planes);
    for (uint32_t i = 0; i < buffer_params->data->num_planes; i++) {
        ww_log(LOG_INFO, "  plane[%u]: fd=%d, offset=%u, stride=%u",
               i, buffer_params->data->planes[i].fd, buffer_params->data->planes[i].offset,
               buffer_params->data->planes[i].stride);
    }

    struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface, 1, 0);
    check_alloc(buffer_resource);

    create_buffer(buffer_params, buffer_resource, width, height, format, flags);
    switch (buffer_params->status) {
    case BUFFER_PARAMS_STATUS_UNKNOWN:
        // There is nothing to do. A protocol error was already sent.
        break;
    case BUFFER_PARAMS_STATUS_OK:
        zwp_linux_buffer_params_v1_send_created(buffer_params->resource, buffer_resource);
        break;
    case BUFFER_PARAMS_STATUS_NOT_OK:
        zwp_linux_buffer_params_v1_send_failed(buffer_params->resource);
        wl_resource_destroy(buffer_resource);
        break;
    }
}

static void
linux_buffer_params_create_immed(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t id, int32_t width, int32_t height, uint32_t format,
                                 uint32_t flags) {
    struct server_linux_buffer_params *buffer_params = wl_resource_get_user_data(resource);

    if (!check_buffer_params(buffer_params)) {
        return;
    }

    ww_log(LOG_INFO,
           "dmabuf create_immed: id=%u width=%d height=%d format=0x%x flags=%u num_planes=%u modifier=%#" PRIx64,
           id, width, height, format, flags, buffer_params->data->num_planes,
           ((uint64_t)buffer_params->data->modifier_hi << 32) | buffer_params->data->modifier_lo);

    struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    check_alloc(buffer_resource);

    create_buffer(buffer_params, buffer_resource, width, height, format, flags);
    switch (buffer_params->status) {
    case BUFFER_PARAMS_STATUS_UNKNOWN:
        // There is nothing to do. A protocol error was already sent.
        break;
    case BUFFER_PARAMS_STATUS_OK:
        // There is nothing to do, since the client already has the wl_buffer.
        break;
    case BUFFER_PARAMS_STATUS_NOT_OK:
        // Implementations are allowed to kill the client if create_immed fails, so we can just do
        // that.
        wl_resource_post_error(buffer_params->resource,
                               ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
                               "failed to create dmabuf");
        break;
    }
}

static void
linux_buffer_params_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwp_linux_buffer_params_v1_interface linux_buffer_params_impl = {
    .add = linux_buffer_params_add,
    .create = linux_buffer_params_create,
    .create_immed = linux_buffer_params_create_immed,
    .destroy = linux_buffer_params_destroy,
};

static void
linux_dmabuf_feedback_resource_destroy(struct wl_resource *resource) {
    struct server_linux_dmabuf_feedback *feedback = wl_resource_get_user_data(resource);

    zwp_linux_dmabuf_feedback_v1_destroy(feedback->remote);
    free(feedback);
}

static void
linux_dmabuf_feedback_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface linux_dmabuf_feedback_impl = {
    .destroy = linux_dmabuf_feedback_destroy,
};

static void
linux_dmabuf_resource_destroy(struct wl_resource *resource) {
    // Unused.
}

static void
linux_dmabuf_create_params(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_linux_dmabuf *linux_dmabuf = wl_resource_get_user_data(resource);

    struct server_dmabuf_data *buffer_data = zalloc(1, sizeof(*buffer_data));
    for (size_t i = 0; i < STATIC_ARRLEN(buffer_data->planes); i++) {
        buffer_data->planes[i].fd = -1;
    }

    struct server_linux_buffer_params *buffer_params = zalloc(1, sizeof(*buffer_params));
    buffer_params->data = buffer_data;
    buffer_params->parent = linux_dmabuf;

    buffer_params->resource = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
                                                 wl_resource_get_version(resource), id);
    check_alloc(buffer_params->resource);
    wl_resource_set_implementation(buffer_params->resource, &linux_buffer_params_impl,
                                   buffer_params, linux_buffer_params_resource_destroy);

    if (!linux_dmabuf->proxy_game) {
        buffer_params->remote = zwp_linux_dmabuf_v1_create_params(linux_dmabuf->remote);
        check_alloc(buffer_params->remote);

        zwp_linux_buffer_params_v1_add_listener(buffer_params->remote, &linux_buffer_params_listener,
                                                buffer_params);
        wl_display_roundtrip_queue(linux_dmabuf->remote_display, linux_dmabuf->queue);
    }

    return;
}

static void
linux_dmabuf_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

// Send synthetic dmabuf feedback for cross-GPU composition mode
static void
send_synthetic_feedback(struct wl_resource *feedback_resource) {
    // Create a format table with basic LINEAR formats
    // These formats are universally supported across Intel, AMD, and NVIDIA
    struct format_table_entry entries[] = {
        { DRM_FORMAT_XRGB8888, 0, DRM_FORMAT_MOD_LINEAR },
        { DRM_FORMAT_ARGB8888, 0, DRM_FORMAT_MOD_LINEAR },
        { DRM_FORMAT_XBGR8888, 0, DRM_FORMAT_MOD_LINEAR },
        { DRM_FORMAT_ABGR8888, 0, DRM_FORMAT_MOD_LINEAR },
    };

    size_t table_size = sizeof(entries);

    // Create a memfd for the format table
    int fd = ww_memfd_create("dmabuf-format-table", MFD_CLOEXEC);
    if (fd < 0) {
        ww_log(LOG_ERROR, "failed to create memfd for format table");
        return;
    }

    if (ftruncate(fd, table_size) < 0) {
        ww_log(LOG_ERROR, "failed to truncate format table fd");
        close(fd);
        return;
    }

    void *map = mmap(NULL, table_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ww_log(LOG_ERROR, "failed to mmap format table");
        close(fd);
        return;
    }

    memcpy(map, entries, table_size);
    munmap(map, table_size);

    // Send format table
    zwp_linux_dmabuf_feedback_v1_send_format_table(feedback_resource, fd, table_size);
    close(fd);

    // Send main device as Intel B580 (renderD128 = major 226, minor 128)
    // This tells the client (Minecraft) to use Intel GPU for rendering
    struct wl_array device_arr;
    wl_array_init(&device_arr);
    dev_t *dev = wl_array_add(&device_arr, sizeof(dev_t));
    *dev = makedev(226, 128);  // Intel B580 renderD128
    zwp_linux_dmabuf_feedback_v1_send_main_device(feedback_resource, &device_arr);
    wl_array_release(&device_arr);

    // Send a single tranche with all LINEAR formats targeting Intel GPU
    wl_array_init(&device_arr);
    dev = wl_array_add(&device_arr, sizeof(dev_t));
    *dev = makedev(226, 128);  // Intel B580 renderD128
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback_resource, &device_arr);
    wl_array_release(&device_arr);

    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback_resource, 0);

    // Send indices for all 4 formats
    struct wl_array indices;
    wl_array_init(&indices);
    for (uint16_t i = 0; i < 4; i++) {
        uint16_t *idx = wl_array_add(&indices, sizeof(uint16_t));
        *idx = i;
    }
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback_resource, &indices);
    wl_array_release(&indices);

    zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback_resource);
    zwp_linux_dmabuf_feedback_v1_send_done(feedback_resource);

    ww_log(LOG_INFO, "sent synthetic dmabuf feedback with LINEAR-only formats for cross-GPU");
}

// Resource destroy handler for synthetic feedback (no remote to clean up)
static void
linux_dmabuf_feedback_synthetic_destroy(struct wl_resource *resource) {
    struct server_linux_dmabuf_feedback *feedback = wl_resource_get_user_data(resource);
    // No remote feedback to destroy in synthetic mode
    free(feedback);
}

static void
linux_dmabuf_get_default_feedback(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t id) {
    struct server_linux_dmabuf *linux_dmabuf = wl_resource_get_user_data(resource);

    struct server_linux_dmabuf_feedback *feedback = zalloc(1, sizeof(*feedback));

    feedback->resource = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
                                            wl_resource_get_version(resource), id);
    check_alloc(feedback->resource);
    feedback->parent = linux_dmabuf;

    // Always proxy feedback from Hyprland - let Hyprland tell the client what formats it supports
    // Hyprland handles cross-GPU buffer import via its own mechanisms
    wl_resource_set_implementation(feedback->resource, &linux_dmabuf_feedback_impl, feedback,
                                   linux_dmabuf_feedback_resource_destroy);

    feedback->remote = zwp_linux_dmabuf_v1_get_default_feedback(linux_dmabuf->remote);
    check_alloc(feedback->remote);

    zwp_linux_dmabuf_feedback_v1_add_listener(feedback->remote, &linux_dmabuf_feedback_listener,
                                              feedback);
    wl_display_roundtrip_queue(linux_dmabuf->remote_display, linux_dmabuf->queue);
}

static void
linux_dmabuf_get_surface_feedback(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t id, struct wl_resource *surface_resource) {
    struct server_linux_dmabuf *linux_dmabuf = wl_resource_get_user_data(resource);
    struct server_surface *surface = server_surface_from_resource(surface_resource);

    struct server_linux_dmabuf_feedback *feedback = zalloc(1, sizeof(*feedback));

    feedback->resource = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
                                            wl_resource_get_version(resource), id);
    check_alloc(feedback->resource);
    feedback->parent = linux_dmabuf;

    // Always proxy feedback from Hyprland - let Hyprland tell the client what formats it supports
    // Hyprland handles cross-GPU buffer import via its own mechanisms
    wl_resource_set_implementation(feedback->resource, &linux_dmabuf_feedback_impl, feedback,
                                   linux_dmabuf_feedback_resource_destroy);

    feedback->remote =
        zwp_linux_dmabuf_v1_get_surface_feedback(linux_dmabuf->remote, surface->remote);
    check_alloc(feedback->remote);

    zwp_linux_dmabuf_feedback_v1_add_listener(feedback->remote, &linux_dmabuf_feedback_listener,
                                              feedback);
    wl_display_roundtrip_queue(linux_dmabuf->remote_display, linux_dmabuf->queue);
}

static const struct zwp_linux_dmabuf_v1_interface linux_dmabuf_impl = {
    .create_params = linux_dmabuf_create_params,
    .destroy = linux_dmabuf_destroy,
    .get_default_feedback = linux_dmabuf_get_default_feedback,
    .get_surface_feedback = linux_dmabuf_get_surface_feedback,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_LINUX_DMABUF_VERSION);

    if (version < ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION) {
        // Supporting versions older than v4 would require us to send the `format` and
        // `modifier` events which would become a bit of a hassle.
        wl_client_post_implementation_error(client,
                                            "zwp_linux_dmabuf versions below 4 are unsupported");
        return;
    }

    struct server_linux_dmabuf *linux_dmabuf = data;

    struct wl_resource *resource =
        wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);
    check_alloc(resource);
    wl_resource_set_implementation(resource, &linux_dmabuf_impl, linux_dmabuf,
                                   linux_dmabuf_resource_destroy);
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_linux_dmabuf *linux_dmabuf =
        wl_container_of(listener, linux_dmabuf, on_display_destroy);

    wl_global_destroy(linux_dmabuf->global);

    wl_proxy_wrapper_destroy(linux_dmabuf->remote);
    wl_event_queue_destroy(linux_dmabuf->queue);

    if (linux_dmabuf->export_gbm) {
        gbm_device_destroy(linux_dmabuf->export_gbm);
        linux_dmabuf->export_gbm = NULL;
    }
    if (linux_dmabuf->export_drm_fd >= 0) {
        close(linux_dmabuf->export_drm_fd);
        linux_dmabuf->export_drm_fd = -1;
    }

    wl_list_remove(&linux_dmabuf->on_display_destroy.link);

    free(linux_dmabuf);
}

struct server_linux_dmabuf *
server_linux_dmabuf_create(struct server *server) {
    struct server_linux_dmabuf *linux_dmabuf = zalloc(1, sizeof(*linux_dmabuf));
    linux_dmabuf->server = server;
    linux_dmabuf->export_drm_fd = -1;

    linux_dmabuf->global = wl_global_create(server->display, &zwp_linux_dmabuf_v1_interface,
                                            SRV_LINUX_DMABUF_VERSION, linux_dmabuf, on_global_bind);
    check_alloc(linux_dmabuf->global);

    linux_dmabuf->remote_display = server->backend->display;

    // Setup the event queue and create the necessary proxy wrappers.
    linux_dmabuf->queue =
        wl_display_create_queue_with_name(server->backend->display, "linux_dmabuf");
    check_alloc(linux_dmabuf->queue);

    linux_dmabuf->main_queue = wl_proxy_get_queue((struct wl_proxy *)server->backend->display);
    ww_assert(linux_dmabuf->main_queue);

    linux_dmabuf->remote = wl_proxy_create_wrapper(server->backend->linux_dmabuf);
    check_alloc(linux_dmabuf->remote);
    wl_proxy_set_queue((struct wl_proxy *)linux_dmabuf->remote, linux_dmabuf->queue);

    // Proxy mode must create dmabufs that the *parent compositor* can import. Overriding dmabuf
    // feedback to “Intel” (or pushing Intel-only modifiers) can cause the parent compositor to
    // reject wl_buffer creation entirely.
    linux_dmabuf->proxy_game = getenv("WAYWALL_VK_PROXY_GAME") != NULL;

    // In proxy mode we need a GBM device to allocate export dma-bufs on the compositor GPU.
    const char *gbm_path = getenv("GBM_DEVICE");
    if (!gbm_path || !*gbm_path) {
        gbm_path = "/dev/dri/renderD128";
    }
    linux_dmabuf->export_drm_fd = open(gbm_path, O_RDWR | O_CLOEXEC);
    if (linux_dmabuf->export_drm_fd >= 0) {
        linux_dmabuf->export_gbm = gbm_create_device(linux_dmabuf->export_drm_fd);
        if (!linux_dmabuf->export_gbm) {
            ww_log(LOG_WARN, "proxy export: failed to create GBM device for %s", gbm_path);
            close(linux_dmabuf->export_drm_fd);
            linux_dmabuf->export_drm_fd = -1;
        }
    } else {
        ww_log(LOG_WARN, "proxy export: failed to open GBM device %s", gbm_path);
    }

    // If we're intentionally offloading the subprocess to another GPU (DRI_PRIME set), pass
    // through modifiers by default to avoid forced LINEAR/CPU fallbacks. Still allow an explicit
    // env override for troubleshooting.
    linux_dmabuf->allow_modifiers =
        getenv("WAYWALL_DMABUF_ALLOW_MODIFIERS") || server->subprocess_dri_prime;

    // Force Intel render node feedback if requested; default renderD129 (226:129).
    linux_dmabuf->force_intel_feedback = getenv("WAYWALL_DMABUF_FORCE_INTEL") != NULL;

    if (linux_dmabuf->proxy_game) {
        linux_dmabuf->force_intel_feedback = false;
    }

    ww_log(LOG_INFO, "dmabuf: allow_modifiers=%d force_intel_feedback=%d (proxy_game=%d)",
           linux_dmabuf->allow_modifiers, linux_dmabuf->force_intel_feedback,
           linux_dmabuf->proxy_game);

    linux_dmabuf->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &linux_dmabuf->on_display_destroy);

    return linux_dmabuf;
}
