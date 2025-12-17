#ifndef WAYWALL_SERVER_WP_LINUX_DRM_SYNCOBJ_H
#define WAYWALL_SERVER_WP_LINUX_DRM_SYNCOBJ_H

#include "server/server.h"
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

struct server_drm_syncobj_manager {
    struct wl_global *global;
    struct wl_list surfaces; // wl_resource link

    struct wp_linux_drm_syncobj_manager_v1 *remote;

    struct wl_listener on_display_destroy;
};

struct server_drm_syncobj_surface {
    struct wl_resource *resource;
    struct server_drm_syncobj_manager *manager;

    struct server_surface *parent;
    struct wp_linux_drm_syncobj_surface_v1 *remote;

    VkSemaphore vk_sem;
    int imported_fd;

    VkSemaphore vk_sem_release;
    int imported_release_fd;

    struct wl_listener on_surface_destroy;

    struct server_drm_syncobj_point {
        // A dup() of the client's timeline fd. This stays stable across
        // set_{acquire,release}_point calls for the same timeline.
        int32_t fd;
        // The original timeline fd (as seen by the client/server object) that
        // `fd` was duplicated from. Used to avoid dup()/close() churn every
        // frame when only the point value changes.
        int32_t timeline_fd;
        uint32_t point_hi, point_lo;
    } acquire, release;
};

struct server_drm_syncobj_timeline {
    struct wl_resource *resource;
    struct wp_linux_drm_syncobj_timeline_v1 *remote;

    int32_t fd;
};

struct server_drm_syncobj_manager *server_drm_syncobj_manager_create(struct server *server);

#endif
