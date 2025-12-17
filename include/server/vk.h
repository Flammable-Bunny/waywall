#ifndef WAYWALL_SERVER_VK_H
#define WAYWALL_SERVER_VK_H

#include "server/wp_linux_dmabuf.h"
#include "util/box.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <wayland-server-core.h>

// Forward declarations
struct config;
struct server;
struct server_surface;
struct gbm_device;
struct util_avif_frame;

// Mirror with optional color keying
struct vk_mirror {
    struct wl_list link;  // server_vk.mirrors

    // Source region in game (pixels)
    struct box src;

    // Destination region on screen (pixels)
    struct box dst;

    int32_t depth;

    // Color keying (replace input_color with output_color)
    bool color_key_enabled;
    uint32_t color_key_input;   // RGB color to match (0xRRGGBB)
    uint32_t color_key_output;  // RGB color to replace with (0xRRGGBB)
    float color_key_tolerance;  // How close colors must match (0.0-1.0)

    bool enabled;
};

// Raw RGBA atlas used for emote rendering (e.g. 7TV atlas.raw)
struct vk_atlas {
    struct wl_list link;  // server_vk.atlases

    struct server_vk *vk;
    uint32_t width;
    uint32_t height;

    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet descriptor_set;

    uint32_t refcount;
};

// Image overlay (loaded from PNG file)
struct vk_image {
    struct wl_list link;  // server_vk.images

    // Optional atlas backing (shared texture + descriptor set)
    struct vk_atlas *atlas;

    // Vulkan resources
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet descriptor_set;

    // Optional per-image quad vertex buffer (for atlas UVs)
    VkBuffer vertex_buffer;
    VkDeviceMemory vertex_memory;

    // Image dimensions
    int32_t width, height;

    // Optional animated frames (AVIF). Only valid when owns_image=true.
    struct util_avif_frame *frames;
    size_t frame_count;
    size_t frame_index;
    uint64_t next_frame_ms;

    // Destination region on screen (pixels)
    struct box dst;

    int32_t depth;

    bool owns_descriptor_set;
    bool owns_image;
    bool enabled;
};

// Glyph metadata for font atlas
struct vk_glyph {
    uint32_t codepoint;
    int width, height;
    int bearing_x, bearing_y;
    int advance;
    int atlas_x, atlas_y;
};

// Font size cache
struct vk_font_size {
    uint32_t size;
    struct vk_glyph *glyphs;
    size_t glyph_count;
    size_t glyph_capacity;

    // Atlas for this font size
    VkImage atlas_image;
    VkDeviceMemory atlas_memory;
    VkImageView atlas_view;
    VkDescriptorSet atlas_descriptor;
    int atlas_width, atlas_height;
    int atlas_x, atlas_y;  // Current packing position
    int atlas_row_height;
    bool atlas_initialized;
};

// Text overlay
struct vk_text {
    struct wl_list link;  // server_vk.texts

    struct server_vk *vk;

    // Text content and style
    char *text;
    int32_t x, y;
    uint32_t size;  // Font size (px)
    int32_t line_spacing;  // Extra spacing between lines (px)
    uint32_t color;  // Default RGBA

    int32_t depth;

    // Vertex buffer for glyph quads
    VkBuffer vertex_buffer;
    VkDeviceMemory vertex_memory;
    size_t vertex_count;

    // Reference to font size cache
    struct vk_font_size *font;

    bool enabled;
    bool dirty;  // Needs rebuild
};

// Floating view (window)
struct vk_view {
    struct wl_list link;  // server_vk.views
    struct server_vk *vk;
    struct server_view *view;  // Logic view
    struct vk_buffer *current_buffer;  // Currently imported buffer
    struct box dst;  // Position and size on screen
    int32_t depth;
    bool enabled;
};

// Maximum frames in flight for triple buffering
#define VK_MAX_FRAMES_IN_FLIGHT 2

// Vulkan buffer for imported dma-bufs
struct vk_buffer {
    struct wl_list link;  // server_vk.capture.buffers
    struct server_vk *vk;
    struct server_buffer *parent;
    struct wl_listener on_parent_destroy;  // Cleanup when parent buffer is destroyed

    // Optional optimal-tiling copy on AMD (legacy synchronous path)
    VkImage optimal_image;
    VkDeviceMemory optimal_memory;
    VkImageView optimal_view;
    bool optimal_valid;

    // Double-buffered optimal-tiling copy for async pipelining
    VkImage optimal_images[2];
    VkDeviceMemory optimal_memories[2];
    VkImageView optimal_views[2];
    VkDescriptorSet optimal_descriptors[2];
    int optimal_read_index;    // Index being read (rendered)
    int optimal_write_index;   // Index being written (copy target)
    VkFence copy_fence;        // Fence for async copy completion
    bool copy_pending;         // True if async copy in progress
    bool async_optimal_valid;  // True if double-buffered optimal is ready

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

    // Proxy-game export targets (dma-bufs allocated on compositor GPU)
    uint32_t export_count;
    VkImage export_images[DMABUF_EXPORT_MAX];
    VkDeviceMemory export_memories[DMABUF_EXPORT_MAX];
    bool export_prepared[DMABUF_EXPORT_MAX];
    uint32_t export_index;

    // Dimensions and stride (for manual sampling)
    int32_t width, height;
    uint32_t stride;  // Actual dma-buf stride in bytes
    bool source_prepared;

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
    bool proxy_game; // Proxy game buffers to parent compositor (no Vulkan capture)

    // Core Vulkan objects
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue graphics_queue;
    VkQueue present_queue;
    VkQueue transfer_queue;
    uint32_t graphics_family;
    uint32_t present_family;
    uint32_t transfer_family;
    VkCommandPool transfer_pool;
    bool async_pipelining_enabled;

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

    // Proxy-game copy submission (separate from swapchain rendering)
    struct {
        VkCommandBuffer command_buffers[DMABUF_EXPORT_MAX];
        VkFence fences[DMABUF_EXPORT_MAX];
        uint32_t index;
    } proxy_copy;

    // Synchronization
    VkSemaphore image_available[VK_MAX_FRAMES_IN_FLIGHT];
    VkSemaphore render_finished[VK_MAX_FRAMES_IN_FLIGHT];
    VkFence in_flight[VK_MAX_FRAMES_IN_FLIGHT];
    uint32_t current_frame;
    uint32_t current_image_index;  // Current swapchain image being rendered to
    uint64_t fps_last_time_ms;
    uint32_t fps_frame_count;
    bool disable_capture_sync_wait;
    bool allow_modifiers;  // Allow tiled modifier imports (better cross-GPU perf)

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

    // Mirror pipeline (samples game with color keying)
    struct {
        VkPipelineLayout layout;
        VkPipeline pipeline;
        VkShaderModule frag;
    } mirror_pipeline;

    // Vertex buffer for quad rendering
    VkBuffer quad_vertex_buffer;
    VkDeviceMemory quad_vertex_memory;

    // Mirrors list
    struct wl_list mirrors;  // vk_mirror.link

    // Images list
    struct wl_list images;  // vk_image.link

    // Atlases list
    struct wl_list atlases;  // vk_atlas.link

    // Texts list
    struct wl_list texts;  // vk_text.link

    // Floating views list
    struct wl_list views;  // vk_view.link

    // Image pipeline (simple textured quad)
    struct {
        VkPipelineLayout layout;
        VkPipeline pipeline;
        VkDescriptorSetLayout descriptor_layout;
    } image_pipeline;

    // Text pipeline (font atlas sampling)
    struct {
        VkPipelineLayout layout;
        VkPipeline pipeline;
        VkDescriptorSetLayout descriptor_layout;
    } text_vk_pipeline;

    // Font rendering (FreeType)
    struct {
        void *ft_library;  // FT_Library
        void *ft_face;     // FT_Face
        struct vk_font_size *sizes;
        size_t sizes_count;
        size_t sizes_capacity;
        uint32_t base_font_size;
    } font;

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
    struct wl_listener on_ui_refresh;

    // Optional overlay tick (used when proxy_game is enabled)
    struct wl_event_source *overlay_tick;
    int32_t overlay_tick_ms;

    // Events
    struct {
        struct wl_signal frame;  // data: NULL
    } events;

    // DRM device for dma-buf operations
    int drm_fd;
    struct gbm_device *gbm;
};

// Public API - mirrors server_gl interface
struct server_vk *server_vk_create(struct server *server, struct config *cfg);
void server_vk_destroy(struct server_vk *vk);

void server_vk_enter(struct server_vk *vk);
void server_vk_exit(struct server_vk *vk);

VkImageView server_vk_get_capture(struct server_vk *vk);
void server_vk_get_capture_size(struct server_vk *vk, int32_t *width, int32_t *height);
void server_vk_set_capture(struct server_vk *vk, struct server_surface *surface);

bool server_vk_begin_frame(struct server_vk *vk);
void server_vk_end_frame(struct server_vk *vk);

// Pipeline management
struct vk_pipeline *server_vk_create_pipeline(struct server_vk *vk,
                                               const uint32_t *vert_code, size_t vert_size,
                                               const uint32_t *frag_code, size_t frag_size);
void server_vk_destroy_pipeline(struct server_vk *vk, struct vk_pipeline *pipeline);

// Mirror options for creating mirrors
struct vk_mirror_options {
    struct box src;
    struct box dst;
    int32_t depth;

    // Optional color keying
    bool color_key_enabled;
    uint32_t color_key_input;   // 0xRRGGBB
    uint32_t color_key_output;  // 0xRRGGBB
    float color_key_tolerance;  // 0.0-1.0, default 0.1
};

// Mirror API
struct vk_mirror *server_vk_add_mirror(struct server_vk *vk, const struct vk_mirror_options *options);
void server_vk_remove_mirror(struct server_vk *vk, struct vk_mirror *mirror);
void server_vk_mirror_set_enabled(struct vk_mirror *mirror, bool enabled);

// Image options for creating images
struct vk_image_options {
    struct box dst;
    int32_t depth;
};

// Image API
struct vk_image *server_vk_add_image(struct server_vk *vk, const char *path, const struct vk_image_options *options);
struct vk_image *server_vk_add_avif_image(struct server_vk *vk, const char *path, const struct vk_image_options *options);
void server_vk_remove_image(struct server_vk *vk, struct vk_image *image);
void server_vk_image_set_enabled(struct vk_image *image, bool enabled);

// Text options for creating text
struct vk_text_options {
    int32_t x, y;
    uint32_t size;   // Font size (px)
    int32_t line_spacing;  // Extra spacing between lines (px)
    uint32_t color;  // RGBA (0xRRGGBBAA)
    int32_t depth;
};

// Text API
struct vk_text *server_vk_add_text(struct server_vk *vk, const char *text, const struct vk_text_options *options);
void server_vk_remove_text(struct server_vk *vk, struct vk_text *text);
void server_vk_text_set_enabled(struct vk_text *text, bool enabled);
void server_vk_text_set_text(struct vk_text *text, const char *new_text);
void server_vk_text_set_color(struct vk_text *text, uint32_t color);

struct vk_advance_ret {
    int32_t x;
    int32_t y;
};

struct vk_advance_ret server_vk_text_advance(struct server_vk *vk, const char *data, size_t data_len, uint32_t size);

// Atlas / atlas image API (Vulkan-only mode)
struct vk_atlas *server_vk_create_atlas(struct server_vk *vk, uint32_t width, const char *rgba_data,
                                        size_t rgba_len);
void server_vk_atlas_ref(struct vk_atlas *atlas);
void server_vk_atlas_unref(struct vk_atlas *atlas);
bool server_vk_atlas_insert_raw(struct vk_atlas *atlas, const char *data, size_t data_len, uint32_t x,
                                uint32_t y);
char *server_vk_atlas_get_dump(struct vk_atlas *atlas, size_t *out_len);

struct vk_image *server_vk_add_image_from_atlas(struct server_vk *vk, struct vk_atlas *atlas, struct box src,
                                                const struct vk_image_options *options);

// Floating view API
struct vk_view *server_vk_add_view(struct server_vk *vk, struct server_view *view);
void server_vk_remove_view(struct server_vk *vk, struct vk_view *view);
void server_vk_view_set_buffer(struct vk_view *view, struct server_buffer *buffer);
void server_vk_view_set_geometry(struct vk_view *view, int32_t x, int32_t y, int32_t width, int32_t height);
void server_vk_view_set_enabled(struct vk_view *view, bool enabled);

#endif
