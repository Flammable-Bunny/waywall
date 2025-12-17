#ifndef WAYWALL_SERVER_WP_LINUX_DMABUF_H
#define WAYWALL_SERVER_WP_LINUX_DMABUF_H

#include "server/server.h"
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client-core.h>
#include <wayland-server-core.h>

#define DMABUF_MAX_PLANES 4
#define DMABUF_EXPORT_MAX 3

struct gbm_device;

struct server_linux_dmabuf {
    struct wl_global *global;
    struct server *server;

    struct wl_display *remote_display;
    struct zwp_linux_dmabuf_v1 *remote; // wrapped (server_linux_dmabuf.queue)
    struct wl_event_queue *main_queue;  // main queue for backend wl_display
    struct wl_event_queue *queue;       // queue for proxy wrappers

    struct wl_listener on_display_destroy;

    // When true, the Vulkan backend is in "proxy game" mode and game surfaces are committed
    // directly to the host compositor via subsurfaces. In this mode, dma-buf feedback must be
    // passed through unchanged so buffers can actually be imported by the host compositor.
    bool proxy_game;

    // GBM device used to allocate export dma-bufs on the compositor GPU (Waywall process GPU).
    int export_drm_fd;
    struct gbm_device *export_gbm;

    // When true, pass through dmabuf modifiers/format tables from the host compositor.
    bool allow_modifiers;
    // When true, override dmabuf feedback devices to point to the Intel render node (renderD129)
    // so clients allocate on the Intel GPU even if the host compositor is on AMD.
    bool force_intel_feedback;
};

struct server_linux_buffer_params {
    struct wl_resource *resource;

    struct server_linux_dmabuf *parent;
    struct zwp_linux_buffer_params_v1 *remote; // on server_linux_dmabuf.queue

    struct server_dmabuf_data *data;
    bool used; // whether or not a create/create_immed request has been issued

    struct wl_buffer *ok_buffer; // created wl_buffer (for create only)
    enum server_linux_buffer_params_status {
        BUFFER_PARAMS_STATUS_UNKNOWN,
        BUFFER_PARAMS_STATUS_OK,
        BUFFER_PARAMS_STATUS_NOT_OK,
    } status;

    struct server_buffer *buffer;
};

struct server_linux_dmabuf_feedback {
    struct wl_resource *resource;

    struct server_linux_dmabuf *parent;
    struct zwp_linux_dmabuf_feedback_v1 *remote;
};

struct server_dmabuf_data {
    int32_t width, height;
    uint32_t format, flags;
    uint32_t modifier_lo, modifier_hi;

    uint32_t num_planes;
    struct {
        int32_t fd;
        uint32_t offset, stride;
    } planes[DMABUF_MAX_PLANES];

    // Proxy-game export buffers (allocated on the compositor GPU) used for
    // "import → copy → attach/commit" proxying.
    bool proxy_export;
    uint32_t export_count;
    struct {
        int32_t fd;
        uint32_t offset, stride;
        uint32_t modifier_lo, modifier_hi;
        struct wl_buffer *remote;
        bool busy;
    } exports[DMABUF_EXPORT_MAX];
};

struct server_linux_dmabuf *server_linux_dmabuf_create(struct server *server);

#endif
