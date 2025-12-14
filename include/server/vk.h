#ifndef WAYWALL_SERVER_VK_H
#define WAYWALL_SERVER_VK_H

#include "util/box.h"
#include <stdbool.h>
#include <vulkan/vulkan.h>
#include <wayland-server-core.h>

// Forward declarations
struct server;
struct server_surface;
struct gbm_device;

// Maximum frames in flight for triple buffering
#define VK_MAX_FRAMES_IN_FLIGHT 2

// Vulkan buffer for imported dma-bufs
struct vk_buffer {
    struct wl_list link;  // server_vk.capture.buffers
    struct server_vk *vk;
    struct server_buffer *parent;

    // Imported dma-buf memory
    VkDeviceMemory memory;

    // Storage buffer for direct stride-aware sampling (NATIVE cross-GPU)
    VkBuffer storage_buffer;
    VkBufferView buffer_view;

    // Image/view (may be unused for cross-GPU with stride mismatch)
    VkImage image;
    VkImageView view;

    // Descriptor set for sampling
    VkDescriptorSet descriptor_set;
    VkDescriptorSet buffer_descriptor_set;

    // Cross-GPU sync
    int dmabuf_fd;
    VkSemaphore acquire_semaphore;  // Wait on this before reading

    // Dimensions and stride (for manual sampling)
    int32_t width, height;
    uint32_t stride;  // Actual dma-buf stride in bytes

    bool destroyed;
};

// Shader pipeline
struct vk_pipeline {
    VkShaderModule vert;
    VkShaderModule frag;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout descriptor_layout;
};

// Main Vulkan context
struct server_vk {
    struct server *server;
    bool dual_gpu;  // Swap color channels for dual-GPU setups

    // Core Vulkan objects
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue graphics_queue;
    VkQueue present_queue;
    uint32_t graphics_family;
    uint32_t present_family;

    // Memory properties for allocation
    VkPhysicalDeviceMemoryProperties memory_properties;

    // Wayland surface and swapchain
    struct {
        struct wl_surface *wl_surface;
        struct wl_subsurface *subsurface;
        VkSurfaceKHR surface;
        VkSwapchainKHR swapchain;
        VkFormat format;
        VkExtent2D extent;

        uint32_t image_count;
        VkImage *images;
        VkImageView *views;
        VkFramebuffer *framebuffers;
    } swapchain;

    // Render pass
    VkRenderPass render_pass;

    // Command pools and buffers
    VkCommandPool command_pool;
    VkCommandBuffer command_buffers[VK_MAX_FRAMES_IN_FLIGHT];

    // Synchronization
    VkSemaphore image_available[VK_MAX_FRAMES_IN_FLIGHT];
    VkSemaphore render_finished[VK_MAX_FRAMES_IN_FLIGHT];
    VkFence in_flight[VK_MAX_FRAMES_IN_FLIGHT];
    uint32_t current_frame;
    uint32_t current_image_index;  // Current swapchain image being rendered to

    // Descriptor pool for texture sampling
    VkDescriptorPool descriptor_pool;
    VkSampler sampler;

    // Pipelines
    struct vk_pipeline texcopy_pipeline;
    struct vk_pipeline text_pipeline;
    struct vk_pipeline blit_pipeline;  // Simple fullscreen blit

    // Buffer-based blit for cross-GPU stride mismatch (NATIVE path)
    struct {
        VkDescriptorSetLayout descriptor_layout;
        VkPipelineLayout layout;
        VkPipeline pipeline;
        VkShaderModule frag;
    } buffer_blit;

    // Vertex buffer for quad rendering
    VkBuffer quad_vertex_buffer;
    VkDeviceMemory quad_vertex_memory;

    // Capture surface (imported from client)
    struct {
        struct server_surface *surface;
        struct wl_list buffers;  // vk_buffer.link
        struct vk_buffer *current;
    } capture;

    // Event listeners
    struct wl_listener on_surface_commit;
    struct wl_listener on_surface_destroy;
    struct wl_listener on_ui_resize;

    // Events
    struct {
        struct wl_signal frame;  // data: NULL
    } events;

    // DRM device for dma-buf operations
    int drm_fd;
    struct gbm_device *gbm;
};

// Public API - mirrors server_gl interface
struct server_vk *server_vk_create(struct server *server, bool dual_gpu);
void server_vk_destroy(struct server_vk *vk);

void server_vk_enter(struct server_vk *vk);
void server_vk_exit(struct server_vk *vk);

VkImageView server_vk_get_capture(struct server_vk *vk);
void server_vk_get_capture_size(struct server_vk *vk, int32_t *width, int32_t *height);
void server_vk_set_capture(struct server_vk *vk, struct server_surface *surface);

void server_vk_begin_frame(struct server_vk *vk);
void server_vk_end_frame(struct server_vk *vk);

// Pipeline management
struct vk_pipeline *server_vk_create_pipeline(struct server_vk *vk,
                                               const uint32_t *vert_code, size_t vert_size,
                                               const uint32_t *frag_code, size_t frag_size);
void server_vk_destroy_pipeline(struct server_vk *vk, struct vk_pipeline *pipeline);

#endif
