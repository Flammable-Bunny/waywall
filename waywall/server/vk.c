#define _GNU_SOURCE

#include "server/vk.h"
#include "server/backend.h"
#include "server/buffer.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_compositor.h"
#include "server/wp_linux_dmabuf.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

#include <wayland-client-protocol.h>

#include <drm/drm.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>

// Generated SPIR-V shader bytecode
#include "shader_spirv.h"

// Required instance extensions
static const char *INSTANCE_EXTENSIONS[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
};

// Required device extensions
static const char *DEVICE_EXTENSIONS[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,  // For cross-GPU queue family transfers
};

#define ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

// Logging helper
#define vk_log(lvl, fmt, ...) \
    util_log(lvl, "[vk] " fmt, ##__VA_ARGS__)

#define vk_check(result, msg) \
    do { \
        if ((result) != VK_SUCCESS) { \
            vk_log(LOG_ERROR, "%s: %d", msg, (int)(result)); \
            return false; \
        } \
    } while (0)

// Forward declarations
static void vk_buffer_destroy(struct vk_buffer *buffer);
static struct vk_buffer *vk_buffer_import(struct server_vk *vk, struct server_buffer *buffer);
static void on_surface_commit(struct wl_listener *listener, void *data);
static void on_surface_destroy(struct wl_listener *listener, void *data);
static void on_ui_resize(struct wl_listener *listener, void *data);
static uint32_t find_memory_type(struct server_vk *vk, uint32_t type_filter, VkMemoryPropertyFlags properties);

// ============================================================================
// Instance Creation
// ============================================================================

static bool
check_instance_extensions(void) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);

    VkExtensionProperties *props = zalloc(count, sizeof(*props));
    vkEnumerateInstanceExtensionProperties(NULL, &count, props);

    for (size_t i = 0; i < ARRAY_LEN(INSTANCE_EXTENSIONS); i++) {
        bool found = false;
        for (uint32_t j = 0; j < count; j++) {
            if (strcmp(INSTANCE_EXTENSIONS[i], props[j].extensionName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            vk_log(LOG_ERROR, "missing instance extension: %s", INSTANCE_EXTENSIONS[i]);
            free(props);
            return false;
        }
    }

    free(props);
    return true;
}

static bool
create_instance(struct server_vk *vk) {
    if (!check_instance_extensions()) {
        return false;
    }

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "waywall",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "waywall",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_2,
    };

    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = ARRAY_LEN(INSTANCE_EXTENSIONS),
        .ppEnabledExtensionNames = INSTANCE_EXTENSIONS,
    };

    VkResult result = vkCreateInstance(&create_info, NULL, &vk->instance);
    vk_check(result, "failed to create Vulkan instance");

    vk_log(LOG_INFO, "created Vulkan instance");
    return true;
}

// ============================================================================
// Physical Device Selection
// ============================================================================

static bool
check_device_extensions(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL);

    VkExtensionProperties *props = zalloc(count, sizeof(*props));
    vkEnumerateDeviceExtensionProperties(device, NULL, &count, props);

    size_t found_count = 0;
    for (size_t i = 0; i < ARRAY_LEN(DEVICE_EXTENSIONS); i++) {
        for (uint32_t j = 0; j < count; j++) {
            if (strcmp(DEVICE_EXTENSIONS[i], props[j].extensionName) == 0) {
                found_count++;
                break;
            }
        }
    }

    free(props);
    return found_count == ARRAY_LEN(DEVICE_EXTENSIONS);
}

static bool
find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface,
                    uint32_t *graphics_family, uint32_t *present_family) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);

    VkQueueFamilyProperties *props = zalloc(count, sizeof(*props));
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, props);

    bool found_graphics = false;
    bool found_present = false;

    for (uint32_t i = 0; i < count; i++) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            *graphics_family = i;
            found_graphics = true;
        }

        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present_support);
        if (present_support) {
            *present_family = i;
            found_present = true;
        }

        if (found_graphics && found_present) {
            break;
        }
    }

    free(props);
    return found_graphics && found_present;
}

static bool
select_physical_device(struct server_vk *vk) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(vk->instance, &count, NULL);

    if (count == 0) {
        vk_log(LOG_ERROR, "no Vulkan devices found");
        return false;
    }

    VkPhysicalDevice *devices = zalloc(count, sizeof(*devices));
    vkEnumeratePhysicalDevices(vk->instance, &count, devices);

    // Prefer discrete GPU (AMD), fall back to any suitable device
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    VkPhysicalDevice fallback = VK_NULL_HANDLE;

    for (uint32_t i = 0; i < count; i++) {
        if (!check_device_extensions(devices[i])) {
            continue;
        }

        uint32_t gfx_family, present_family;
        if (!find_queue_families(devices[i], vk->swapchain.surface, &gfx_family, &present_family)) {
            continue;
        }

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);

        vk_log(LOG_INFO, "found suitable device: %s", props.deviceName);

        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            selected = devices[i];
            vk->graphics_family = gfx_family;
            vk->present_family = present_family;
            break;
        } else if (fallback == VK_NULL_HANDLE) {
            fallback = devices[i];
            vk->graphics_family = gfx_family;
            vk->present_family = present_family;
        }
    }

    if (selected == VK_NULL_HANDLE) {
        selected = fallback;
    }

    free(devices);

    if (selected == VK_NULL_HANDLE) {
        vk_log(LOG_ERROR, "no suitable Vulkan device found");
        return false;
    }

    vk->physical_device = selected;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(selected, &props);
    vk_log(LOG_INFO, "selected device: %s", props.deviceName);

    vkGetPhysicalDeviceMemoryProperties(selected, &vk->memory_properties);

    return true;
}

// ============================================================================
// Logical Device Creation
// ============================================================================

static bool
create_device(struct server_vk *vk) {
    // Queue create infos
    float priority = 1.0f;
    uint32_t unique_families[2] = { vk->graphics_family, vk->present_family };
    uint32_t unique_count = (vk->graphics_family == vk->present_family) ? 1 : 2;

    VkDeviceQueueCreateInfo queue_infos[2];
    for (uint32_t i = 0; i < unique_count; i++) {
        queue_infos[i] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = unique_families[i],
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
    }

    VkPhysicalDeviceFeatures features = {0};

    VkDeviceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = unique_count,
        .pQueueCreateInfos = queue_infos,
        .enabledExtensionCount = ARRAY_LEN(DEVICE_EXTENSIONS),
        .ppEnabledExtensionNames = DEVICE_EXTENSIONS,
        .pEnabledFeatures = &features,
    };

    VkResult result = vkCreateDevice(vk->physical_device, &create_info, NULL, &vk->device);
    vk_check(result, "failed to create logical device");

    vkGetDeviceQueue(vk->device, vk->graphics_family, 0, &vk->graphics_queue);
    vkGetDeviceQueue(vk->device, vk->present_family, 0, &vk->present_queue);

    vk_log(LOG_INFO, "created logical device");
    return true;
}

// ============================================================================
// Swapchain Creation
// ============================================================================

static VkSurfaceFormatKHR
choose_surface_format(VkPhysicalDevice device, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, NULL);

    VkSurfaceFormatKHR *formats = zalloc(count, sizeof(*formats));
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, formats);

    // Prefer BGRA8 SRGB
    VkSurfaceFormatKHR selected = formats[0];
    for (uint32_t i = 0; i < count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            selected = formats[i];
            break;
        }
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            selected = formats[i];
        }
    }

    free(formats);
    return selected;
}

static VkPresentModeKHR
choose_present_mode(VkPhysicalDevice device, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, NULL);

    VkPresentModeKHR *modes = zalloc(count, sizeof(*modes));
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, modes);

    // Prefer IMMEDIATE (no vsync) > MAILBOX (low latency) > FIFO (vsync)
    VkPresentModeKHR selected = VK_PRESENT_MODE_FIFO_KHR;  // Always available
    for (uint32_t i = 0; i < count; i++) {
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            selected = VK_PRESENT_MODE_IMMEDIATE_KHR;
            break;
        }
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR && selected != VK_PRESENT_MODE_IMMEDIATE_KHR) {
            selected = VK_PRESENT_MODE_MAILBOX_KHR;
        }
    }

    free(modes);
    vk_log(LOG_INFO, "using present mode: %s",
           selected == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" :
           selected == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO");
    return selected;
}

static bool
create_swapchain(struct server_vk *vk, uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->physical_device, vk->swapchain.surface, &caps);

    VkSurfaceFormatKHR format = choose_surface_format(vk->physical_device, vk->swapchain.surface);
    VkPresentModeKHR present_mode = choose_present_mode(vk->physical_device, vk->swapchain.surface);

    // Clamp extent to surface capabilities
    VkExtent2D extent = { width, height };
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width = (extent.width < caps.minImageExtent.width) ? caps.minImageExtent.width :
                       (extent.width > caps.maxImageExtent.width) ? caps.maxImageExtent.width :
                       extent.width;
        extent.height = (extent.height < caps.minImageExtent.height) ? caps.minImageExtent.height :
                        (extent.height > caps.maxImageExtent.height) ? caps.maxImageExtent.height :
                        extent.height;
    }

    // Request one more image than minimum for triple buffering
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = vk->swapchain.surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE,
    };

    uint32_t queue_families[] = { vk->graphics_family, vk->present_family };
    if (vk->graphics_family != vk->present_family) {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = queue_families;
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VkResult result = vkCreateSwapchainKHR(vk->device, &create_info, NULL, &vk->swapchain.swapchain);
    vk_check(result, "failed to create swapchain");

    vk->swapchain.format = format.format;
    vk->swapchain.extent = extent;

    // Get swapchain images
    vkGetSwapchainImagesKHR(vk->device, vk->swapchain.swapchain, &vk->swapchain.image_count, NULL);
    vk->swapchain.images = zalloc(vk->swapchain.image_count, sizeof(VkImage));
    vkGetSwapchainImagesKHR(vk->device, vk->swapchain.swapchain, &vk->swapchain.image_count,
                            vk->swapchain.images);

    // Create image views
    vk->swapchain.views = zalloc(vk->swapchain.image_count, sizeof(VkImageView));
    for (uint32_t i = 0; i < vk->swapchain.image_count; i++) {
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = vk->swapchain.images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = vk->swapchain.format,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

        result = vkCreateImageView(vk->device, &view_info, NULL, &vk->swapchain.views[i]);
        if (result != VK_SUCCESS) {
            vk_log(LOG_ERROR, "failed to create swapchain image view");
            return false;
        }
    }

    vk_log(LOG_INFO, "created swapchain: %dx%d, %d images",
           extent.width, extent.height, vk->swapchain.image_count);
    return true;
}

// ============================================================================
// Render Pass
// ============================================================================

static bool
create_render_pass(struct server_vk *vk) {
    VkAttachmentDescription color_attachment = {
        .format = vk->swapchain.format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };

    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
    };

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };

    VkResult result = vkCreateRenderPass(vk->device, &create_info, NULL, &vk->render_pass);
    vk_check(result, "failed to create render pass");

    return true;
}

// ============================================================================
// Framebuffers
// ============================================================================

static bool
create_framebuffers(struct server_vk *vk) {
    vk->swapchain.framebuffers = zalloc(vk->swapchain.image_count, sizeof(VkFramebuffer));

    for (uint32_t i = 0; i < vk->swapchain.image_count; i++) {
        VkFramebufferCreateInfo create_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = vk->render_pass,
            .attachmentCount = 1,
            .pAttachments = &vk->swapchain.views[i],
            .width = vk->swapchain.extent.width,
            .height = vk->swapchain.extent.height,
            .layers = 1,
        };

        VkResult result = vkCreateFramebuffer(vk->device, &create_info, NULL,
                                               &vk->swapchain.framebuffers[i]);
        if (result != VK_SUCCESS) {
            vk_log(LOG_ERROR, "failed to create framebuffer");
            return false;
        }
    }

    return true;
}

// ============================================================================
// Command Pool and Buffers
// ============================================================================

static bool
create_command_pool(struct server_vk *vk) {
    VkCommandPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk->graphics_family,
    };

    VkResult result = vkCreateCommandPool(vk->device, &create_info, NULL, &vk->command_pool);
    vk_check(result, "failed to create command pool");

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = VK_MAX_FRAMES_IN_FLIGHT,
    };

    result = vkAllocateCommandBuffers(vk->device, &alloc_info, vk->command_buffers);
    vk_check(result, "failed to allocate command buffers");

    return true;
}

// ============================================================================
// Synchronization Objects
// ============================================================================

static bool
create_sync_objects(struct server_vk *vk) {
    VkSemaphoreCreateInfo sem_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,  // Start signaled
    };

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(vk->device, &sem_info, NULL, &vk->image_available[i]) != VK_SUCCESS ||
            vkCreateSemaphore(vk->device, &sem_info, NULL, &vk->render_finished[i]) != VK_SUCCESS ||
            vkCreateFence(vk->device, &fence_info, NULL, &vk->in_flight[i]) != VK_SUCCESS) {
            vk_log(LOG_ERROR, "failed to create sync objects");
            return false;
        }
    }

    return true;
}

// ============================================================================
// Sampler
// ============================================================================

static bool
create_sampler(struct server_vk *vk) {
    VkSamplerCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .anisotropyEnable = VK_FALSE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable = VK_FALSE,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    };

    VkResult result = vkCreateSampler(vk->device, &create_info, NULL, &vk->sampler);
    vk_check(result, "failed to create sampler");

    return true;
}

// ============================================================================
// Descriptor Pool
// ============================================================================

static bool
create_descriptor_pool(struct server_vk *vk) {
    VkDescriptorPoolSize pool_sizes[] = {
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 100,
        },
        {
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 100,
        },
    };

    VkDescriptorPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 200,
        .poolSizeCount = ARRAY_LEN(pool_sizes),
        .pPoolSizes = pool_sizes,
    };

    VkResult result = vkCreateDescriptorPool(vk->device, &create_info, NULL, &vk->descriptor_pool);
    vk_check(result, "failed to create descriptor pool");

    return true;
}

// ============================================================================
// Fullscreen Quad Vertex Buffer
// ============================================================================

// Simple fullscreen quad vertices (position + texcoord)
struct quad_vertex {
    float pos[2];
    float uv[2];
};

static const struct quad_vertex QUAD_VERTICES[] = {
    // First triangle (top-left, bottom-left, bottom-right)
    {{ -1.0f, -1.0f }, { 0.0f, 0.0f }},
    {{ -1.0f,  1.0f }, { 0.0f, 1.0f }},
    {{  1.0f,  1.0f }, { 1.0f, 1.0f }},
    // Second triangle (top-left, bottom-right, top-right)
    {{ -1.0f, -1.0f }, { 0.0f, 0.0f }},
    {{  1.0f,  1.0f }, { 1.0f, 1.0f }},
    {{  1.0f, -1.0f }, { 1.0f, 0.0f }},
};

static bool
create_quad_vertex_buffer(struct server_vk *vk) {
    VkDeviceSize buffer_size = sizeof(QUAD_VERTICES);

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer_size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VkResult result = vkCreateBuffer(vk->device, &buffer_info, NULL, &vk->quad_vertex_buffer);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create quad vertex buffer");
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(vk->device, vk->quad_vertex_buffer, &mem_reqs);

    uint32_t mem_type = find_memory_type(vk, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (mem_type == UINT32_MAX) {
        vk_log(LOG_ERROR, "no suitable memory type for quad vertex buffer");
        return false;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };

    result = vkAllocateMemory(vk->device, &alloc_info, NULL, &vk->quad_vertex_memory);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to allocate quad vertex memory");
        return false;
    }

    vkBindBufferMemory(vk->device, vk->quad_vertex_buffer, vk->quad_vertex_memory, 0);

    // Copy vertex data
    void *data;
    vkMapMemory(vk->device, vk->quad_vertex_memory, 0, buffer_size, 0, &data);
    memcpy(data, QUAD_VERTICES, buffer_size);
    vkUnmapMemory(vk->device, vk->quad_vertex_memory);

    vk_log(LOG_INFO, "created quad vertex buffer");
    return true;
}

// ============================================================================
// Pipeline Creation
// ============================================================================

// Push constant layout for vertex shader
struct vk_push_constants {
    float src_size[2];
    float dst_size[2];
};

static VkShaderModule
create_shader_module(VkDevice device, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = code,
    };

    VkShaderModule module;
    if (vkCreateShaderModule(device, &create_info, NULL, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

static bool
create_descriptor_set_layout(struct server_vk *vk, VkDescriptorSetLayout *layout) {
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };

    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };

    return vkCreateDescriptorSetLayout(vk->device, &layout_info, NULL, layout) == VK_SUCCESS;
}

static bool
create_texcopy_pipeline(struct server_vk *vk) {
    // Create shader modules
    vk->texcopy_pipeline.vert = create_shader_module(vk->device, texcopy_vert_spv, texcopy_vert_spv_size);
    vk->texcopy_pipeline.frag = create_shader_module(vk->device, texcopy_frag_spv, texcopy_frag_spv_size);

    if (!vk->texcopy_pipeline.vert || !vk->texcopy_pipeline.frag) {
        vk_log(LOG_ERROR, "failed to create shader modules");
        return false;
    }

    // Create descriptor set layout
    if (!create_descriptor_set_layout(vk, &vk->texcopy_pipeline.descriptor_layout)) {
        vk_log(LOG_ERROR, "failed to create descriptor set layout");
        return false;
    }

    // Push constant range
    VkPushConstantRange push_constant = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(struct vk_push_constants),
    };

    // Pipeline layout
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &vk->texcopy_pipeline.descriptor_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant,
    };

    if (vkCreatePipelineLayout(vk->device, &layout_info, NULL, &vk->texcopy_pipeline.layout) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create pipeline layout");
        return false;
    }

    // Shader stages
    VkPipelineShaderStageCreateInfo shader_stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vk->texcopy_pipeline.vert,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = vk->texcopy_pipeline.frag,
            .pName = "main",
        },
    };

    // Vertex input - matches the GL vertex layout
    VkVertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(float) * 12,  // v_src_pos(2) + v_dst_pos(2) + v_src_rgba(4) + v_dst_rgba(4)
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    VkVertexInputAttributeDescription attributes[] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },                    // v_src_pos
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = sizeof(float) * 2 },    // v_dst_pos
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = sizeof(float) * 4 },  // v_src_rgba
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = sizeof(float) * 8 },  // v_dst_rgba
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = ARRAY_LEN(attributes),
        .pVertexAttributeDescriptions = attributes,
    };

    // Input assembly - triangles
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    // Dynamic viewport and scissor
    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = ARRAY_LEN(dynamic_states),
        .pDynamicStates = dynamic_states,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    // Rasterization
    VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
    };

    // Multisampling disabled
    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    // Color blending - standard alpha blending
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };

    // Create the pipeline
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = ARRAY_LEN(shader_stages),
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = vk->texcopy_pipeline.layout,
        .renderPass = vk->render_pass,
        .subpass = 0,
    };

    VkResult result = vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipeline_info,
                                                 NULL, &vk->texcopy_pipeline.pipeline);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create texcopy pipeline: %d", result);
        return false;
    }

    vk_log(LOG_INFO, "created texcopy pipeline");
    return true;
}

static bool
create_text_pipeline(struct server_vk *vk) {
    // Text pipeline uses same vertex shader, different fragment shader
    vk->text_pipeline.vert = vk->texcopy_pipeline.vert;  // Shared
    vk->text_pipeline.frag = create_shader_module(vk->device, text_frag_spv, text_frag_spv_size);

    if (!vk->text_pipeline.frag) {
        vk_log(LOG_ERROR, "failed to create text fragment shader");
        return false;
    }

    // Share descriptor layout with texcopy
    vk->text_pipeline.descriptor_layout = vk->texcopy_pipeline.descriptor_layout;

    // Push constant range - same as texcopy but also used in fragment
    VkPushConstantRange push_constant = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(struct vk_push_constants),
    };

    // Pipeline layout
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &vk->text_pipeline.descriptor_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant,
    };

    if (vkCreatePipelineLayout(vk->device, &layout_info, NULL, &vk->text_pipeline.layout) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create text pipeline layout");
        return false;
    }

    // Shader stages
    VkPipelineShaderStageCreateInfo shader_stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vk->text_pipeline.vert,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = vk->text_pipeline.frag,
            .pName = "main",
        },
    };

    // Same vertex input as texcopy
    VkVertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(float) * 12,
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    VkVertexInputAttributeDescription attributes[] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = sizeof(float) * 2 },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = sizeof(float) * 4 },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = sizeof(float) * 8 },
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = ARRAY_LEN(attributes),
        .pVertexAttributeDescriptions = attributes,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = ARRAY_LEN(dynamic_states),
        .pDynamicStates = dynamic_states,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = ARRAY_LEN(shader_stages),
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = vk->text_pipeline.layout,
        .renderPass = vk->render_pass,
        .subpass = 0,
    };

    VkResult result = vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipeline_info,
                                                 NULL, &vk->text_pipeline.pipeline);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create text pipeline: %d", result);
        return false;
    }

    vk_log(LOG_INFO, "created text pipeline");
    return true;
}

static bool
create_blit_pipeline(struct server_vk *vk) {
    // Create shader modules for simple blit
    vk->blit_pipeline.vert = create_shader_module(vk->device, blit_vert_spv, blit_vert_spv_size);
    vk->blit_pipeline.frag = create_shader_module(vk->device, blit_frag_spv, blit_frag_spv_size);

    if (!vk->blit_pipeline.vert || !vk->blit_pipeline.frag) {
        vk_log(LOG_ERROR, "failed to create blit shader modules");
        return false;
    }

    // Create descriptor set layout (same as texcopy - just a sampler)
    if (!create_descriptor_set_layout(vk, &vk->blit_pipeline.descriptor_layout)) {
        vk_log(LOG_ERROR, "failed to create blit descriptor set layout");
        return false;
    }

    // Push constant for dual-GPU color swap
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(int32_t),  // swap_colors flag
    };

    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &vk->blit_pipeline.descriptor_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };

    if (vkCreatePipelineLayout(vk->device, &layout_info, NULL, &vk->blit_pipeline.layout) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create blit pipeline layout");
        return false;
    }

    // Shader stages
    VkPipelineShaderStageCreateInfo shader_stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vk->blit_pipeline.vert,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = vk->blit_pipeline.frag,
            .pName = "main",
        },
    };

    // Vertex input - simple pos + uv
    VkVertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(struct quad_vertex),  // pos(2) + uv(2)
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    VkVertexInputAttributeDescription attributes[] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },                    // a_pos
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = sizeof(float) * 2 },    // a_uv
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = ARRAY_LEN(attributes),
        .pVertexAttributeDescriptions = attributes,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = ARRAY_LEN(dynamic_states),
        .pDynamicStates = dynamic_states,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    // No blending - opaque copy
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_FALSE,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = ARRAY_LEN(shader_stages),
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = vk->blit_pipeline.layout,
        .renderPass = vk->render_pass,
        .subpass = 0,
    };

    VkResult result = vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipeline_info,
                                                 NULL, &vk->blit_pipeline.pipeline);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create blit pipeline: %d", result);
        return false;
    }

    vk_log(LOG_INFO, "created blit pipeline");
    return true;
}

// Create buffer-based blit pipeline for NATIVE cross-GPU rendering
// Uses storage buffer instead of sampled image to handle stride mismatch
static bool
create_buffer_blit_pipeline(struct server_vk *vk) {
    // Create shader module for buffer-based fragment shader
    vk->buffer_blit.frag = create_shader_module(vk->device, blit_buffer_frag_spv, blit_buffer_frag_spv_size);
    if (!vk->buffer_blit.frag) {
        vk_log(LOG_ERROR, "failed to create buffer blit shader module");
        return false;
    }

    // Create descriptor set layout for storage buffer
    VkDescriptorSetLayoutBinding buffer_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };

    VkDescriptorSetLayoutCreateInfo layout_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &buffer_binding,
    };

    if (vkCreateDescriptorSetLayout(vk->device, &layout_ci, NULL, &vk->buffer_blit.descriptor_layout) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create buffer blit descriptor layout");
        return false;
    }

    // Push constants: width, height, stride, swap_colors
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(int32_t) * 4,  // width, height, stride, swap_colors
    };

    VkPipelineLayoutCreateInfo pipeline_layout_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &vk->buffer_blit.descriptor_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };

    if (vkCreatePipelineLayout(vk->device, &pipeline_layout_ci, NULL, &vk->buffer_blit.layout) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create buffer blit pipeline layout");
        return false;
    }

    // Create graphics pipeline (reuse vertex shader from blit)
    VkPipelineShaderStageCreateInfo shader_stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vk->blit_pipeline.vert,  // Reuse vertex shader
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = vk->buffer_blit.frag,
            .pName = "main",
        },
    };

    VkVertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(struct quad_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    VkVertexInputAttributeDescription attributes[] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = sizeof(float) * 2 },
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = ARRAY_LEN(attributes),
        .pVertexAttributeDescriptions = attributes,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = ARRAY_LEN(dynamic_states),
        .pDynamicStates = dynamic_states,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_FALSE,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };

    VkGraphicsPipelineCreateInfo pipeline_ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = ARRAY_LEN(shader_stages),
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = vk->buffer_blit.layout,
        .renderPass = vk->render_pass,
        .subpass = 0,
    };

    if (vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipeline_ci, NULL, &vk->buffer_blit.pipeline) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create buffer blit pipeline");
        return false;
    }

    vk_log(LOG_INFO, "created buffer blit pipeline (NATIVE cross-GPU)");
    return true;
}

static void
destroy_pipeline(struct server_vk *vk, struct vk_pipeline *pipeline) {
    if (pipeline->pipeline) {
        vkDestroyPipeline(vk->device, pipeline->pipeline, NULL);
    }
    if (pipeline->layout) {
        vkDestroyPipelineLayout(vk->device, pipeline->layout, NULL);
    }
    // Don't destroy shared descriptor layout here
    if (pipeline->frag && pipeline->frag != vk->texcopy_pipeline.frag) {
        vkDestroyShaderModule(vk->device, pipeline->frag, NULL);
    }
    // Vert shader is shared
}

// ============================================================================
// Public API
// ============================================================================

struct server_vk *
server_vk_create(struct server *server, bool dual_gpu) {
    struct server_vk *vk = zalloc(1, sizeof(*vk));
    vk->server = server;
    vk->dual_gpu = dual_gpu;
    vk->drm_fd = -1;

    vk_log(LOG_INFO, "creating Vulkan backend (dual_gpu=%s)", dual_gpu ? "true" : "false");

    wl_list_init(&vk->capture.buffers);
    wl_signal_init(&vk->events.frame);
    wl_list_init(&vk->on_ui_resize.link);

    // Create Vulkan instance
    if (!create_instance(vk)) {
        goto fail;
    }

    // Create Wayland surface as subsurface of UI (need this before selecting physical device)
    vk->swapchain.wl_surface = wl_compositor_create_surface(server->backend->compositor);
    if (!vk->swapchain.wl_surface) {
        vk_log(LOG_ERROR, "failed to create Wayland surface");
        goto fail;
    }

    // Set empty input region (we don't want input events)
    wl_surface_set_input_region(vk->swapchain.wl_surface, server->ui->empty_region);

    // Create subsurface of main UI surface
    vk->swapchain.subsurface = wl_subcompositor_get_subsurface(
        server->backend->subcompositor, vk->swapchain.wl_surface, server->ui->tree.surface);
    if (!vk->swapchain.subsurface) {
        vk_log(LOG_ERROR, "failed to create subsurface");
        goto fail;
    }
    wl_subsurface_set_desync(vk->swapchain.subsurface);
    wl_subsurface_set_position(vk->swapchain.subsurface, 0, 0);
    // Place below the GL surface so UI overlays render on top
    wl_subsurface_place_below(vk->swapchain.subsurface, server->ui->tree.surface);

    VkWaylandSurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = server->backend->display,
        .surface = vk->swapchain.wl_surface,
    };

    if (vkCreateWaylandSurfaceKHR(vk->instance, &surface_info, NULL,
                                   &vk->swapchain.surface) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create Vulkan Wayland surface");
        goto fail;
    }

    // Select physical device and create logical device
    if (!select_physical_device(vk) || !create_device(vk)) {
        goto fail;
    }

    // Create swapchain with initial size
    uint32_t width = server->ui ? server->ui->width : 640;
    uint32_t height = server->ui ? server->ui->height : 480;
    if (!create_swapchain(vk, width, height)) {
        goto fail;
    }

    // Create render pass and framebuffers
    if (!create_render_pass(vk) || !create_framebuffers(vk)) {
        goto fail;
    }

    // Create command pool and sync objects
    if (!create_command_pool(vk) || !create_sync_objects(vk)) {
        goto fail;
    }

    // Create sampler and descriptor pool
    if (!create_sampler(vk) || !create_descriptor_pool(vk)) {
        goto fail;
    }

    // Create pipelines
    if (!create_texcopy_pipeline(vk) || !create_text_pipeline(vk) || !create_blit_pipeline(vk) || !create_buffer_blit_pipeline(vk)) {
        goto fail;
    }

    // Create fullscreen quad vertex buffer
    if (!create_quad_vertex_buffer(vk)) {
        goto fail;
    }

    // Open DRM device for dma-buf operations
    vk->drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (vk->drm_fd >= 0) {
        vk->gbm = gbm_create_device(vk->drm_fd);
        if (!vk->gbm) {
            vk_log(LOG_WARN, "failed to create GBM device");
        }
    }

    // Hook up resize listener
    vk->on_ui_resize.notify = on_ui_resize;
    wl_signal_add(&server->ui->events.resize, &vk->on_ui_resize);

    vk_log(LOG_INFO, "Vulkan backend initialized successfully");
    return vk;

fail:
    server_vk_destroy(vk);
    return NULL;
}

void
server_vk_destroy(struct server_vk *vk) {
    if (!vk) return;

    // Remove UI resize listener
    wl_list_remove(&vk->on_ui_resize.link);

    if (vk->device) {
        vkDeviceWaitIdle(vk->device);
    }

    // Destroy capture buffers
    struct vk_buffer *buf, *tmp;
    wl_list_for_each_safe(buf, tmp, &vk->capture.buffers, link) {
        vk_buffer_destroy(buf);
    }

    // Destroy sync objects
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++) {
        if (vk->image_available[i]) {
            vkDestroySemaphore(vk->device, vk->image_available[i], NULL);
        }
        if (vk->render_finished[i]) {
            vkDestroySemaphore(vk->device, vk->render_finished[i], NULL);
        }
        if (vk->in_flight[i]) {
            vkDestroyFence(vk->device, vk->in_flight[i], NULL);
        }
    }

    if (vk->command_pool) {
        vkDestroyCommandPool(vk->device, vk->command_pool, NULL);
    }

    if (vk->descriptor_pool) {
        vkDestroyDescriptorPool(vk->device, vk->descriptor_pool, NULL);
    }

    if (vk->sampler) {
        vkDestroySampler(vk->device, vk->sampler, NULL);
    }

    // Destroy quad vertex buffer
    if (vk->quad_vertex_buffer) {
        vkDestroyBuffer(vk->device, vk->quad_vertex_buffer, NULL);
    }
    if (vk->quad_vertex_memory) {
        vkFreeMemory(vk->device, vk->quad_vertex_memory, NULL);
    }

    // Destroy pipelines
    destroy_pipeline(vk, &vk->text_pipeline);
    destroy_pipeline(vk, &vk->texcopy_pipeline);
    destroy_pipeline(vk, &vk->blit_pipeline);

    // Destroy buffer blit pipeline resources
    if (vk->buffer_blit.pipeline) {
        vkDestroyPipeline(vk->device, vk->buffer_blit.pipeline, NULL);
    }
    if (vk->buffer_blit.layout) {
        vkDestroyPipelineLayout(vk->device, vk->buffer_blit.layout, NULL);
    }
    if (vk->buffer_blit.descriptor_layout) {
        vkDestroyDescriptorSetLayout(vk->device, vk->buffer_blit.descriptor_layout, NULL);
    }
    if (vk->buffer_blit.frag) {
        vkDestroyShaderModule(vk->device, vk->buffer_blit.frag, NULL);
    }

    // Destroy shader modules and descriptor layouts
    if (vk->texcopy_pipeline.vert) {
        vkDestroyShaderModule(vk->device, vk->texcopy_pipeline.vert, NULL);
    }
    if (vk->texcopy_pipeline.frag) {
        vkDestroyShaderModule(vk->device, vk->texcopy_pipeline.frag, NULL);
    }
    if (vk->texcopy_pipeline.descriptor_layout) {
        vkDestroyDescriptorSetLayout(vk->device, vk->texcopy_pipeline.descriptor_layout, NULL);
    }
    // Destroy blit pipeline's own resources
    if (vk->blit_pipeline.vert) {
        vkDestroyShaderModule(vk->device, vk->blit_pipeline.vert, NULL);
    }
    if (vk->blit_pipeline.frag) {
        vkDestroyShaderModule(vk->device, vk->blit_pipeline.frag, NULL);
    }
    if (vk->blit_pipeline.descriptor_layout) {
        vkDestroyDescriptorSetLayout(vk->device, vk->blit_pipeline.descriptor_layout, NULL);
    }

    // Destroy framebuffers
    if (vk->swapchain.framebuffers) {
        for (uint32_t i = 0; i < vk->swapchain.image_count; i++) {
            if (vk->swapchain.framebuffers[i]) {
                vkDestroyFramebuffer(vk->device, vk->swapchain.framebuffers[i], NULL);
            }
        }
        free(vk->swapchain.framebuffers);
    }

    if (vk->render_pass) {
        vkDestroyRenderPass(vk->device, vk->render_pass, NULL);
    }

    // Destroy swapchain views
    if (vk->swapchain.views) {
        for (uint32_t i = 0; i < vk->swapchain.image_count; i++) {
            if (vk->swapchain.views[i]) {
                vkDestroyImageView(vk->device, vk->swapchain.views[i], NULL);
            }
        }
        free(vk->swapchain.views);
    }
    free(vk->swapchain.images);

    if (vk->swapchain.swapchain) {
        vkDestroySwapchainKHR(vk->device, vk->swapchain.swapchain, NULL);
    }

    if (vk->swapchain.surface) {
        vkDestroySurfaceKHR(vk->instance, vk->swapchain.surface, NULL);
    }

    if (vk->swapchain.subsurface) {
        wl_subsurface_destroy(vk->swapchain.subsurface);
    }

    if (vk->swapchain.wl_surface) {
        wl_surface_destroy(vk->swapchain.wl_surface);
    }

    if (vk->device) {
        vkDestroyDevice(vk->device, NULL);
    }

    if (vk->instance) {
        vkDestroyInstance(vk->instance, NULL);
    }

    if (vk->gbm) {
        gbm_device_destroy(vk->gbm);
    }

    if (vk->drm_fd >= 0) {
        close(vk->drm_fd);
    }

    free(vk);
}

void
server_vk_set_capture(struct server_vk *vk, struct server_surface *surface) {
    if (vk->capture.surface) {
        wl_list_remove(&vk->on_surface_commit.link);
        wl_list_remove(&vk->on_surface_destroy.link);
    }

    vk->capture.surface = surface;
    vk->capture.current = NULL;

    if (!surface) {
        return;
    }

    vk->on_surface_commit.notify = on_surface_commit;
    wl_signal_add(&surface->events.commit, &vk->on_surface_commit);

    vk->on_surface_destroy.notify = on_surface_destroy;
    wl_signal_add(&surface->events.destroy, &vk->on_surface_destroy);
}

VkImageView
server_vk_get_capture(struct server_vk *vk) {
    if (!vk->capture.current) {
        return VK_NULL_HANDLE;
    }
    return vk->capture.current->view;
}

void
server_vk_get_capture_size(struct server_vk *vk, int32_t *width, int32_t *height) {
    if (!vk->capture.current || !vk->capture.current->parent) {
        *width = 0;
        *height = 0;
        return;
    }

    struct server_dmabuf_data *data = vk->capture.current->parent->data;
    *width = data->width;
    *height = data->height;
}

// ============================================================================
// Cross-GPU DMA-BUF Synchronization
// ============================================================================

// Perform kernel-level dma-buf sync before accessing imported buffer
static void
dmabuf_sync_start_read(int fd) {
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ,
    };
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        vk_log(LOG_WARN, "DMA_BUF_SYNC_START failed: %s", strerror(errno));
    }
}

static void
dmabuf_sync_end_read(int fd) {
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ,
    };
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        vk_log(LOG_WARN, "DMA_BUF_SYNC_END failed: %s", strerror(errno));
    }
}

// Transition imported image to shader-read layout with proper cross-GPU barrier
static void
transition_imported_image(struct server_vk *vk, VkImage image, VkCommandBuffer cmd) {
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,  // External GPU writes
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,  // From external GPU
        .dstQueueFamilyIndex = vk->graphics_family,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0, NULL,
        0, NULL,
        1, &barrier);
}

// Release imported image back to external GPU
static void
release_imported_image(struct server_vk *vk, VkImage image, VkCommandBuffer cmd) {
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = vk->graphics_family,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,  // Back to external GPU
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0, NULL,
        0, NULL,
        1, &barrier);
}

// Acquire imported buffer (queue family transfer skipped, relying on dma-buf sync)
static void
acquire_imported_buffer(struct server_vk *vk, VkBuffer buffer, VkCommandBuffer cmd) {
    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0, NULL,
        1, &barrier,
        0, NULL);
}

// Release imported buffer
static void
release_imported_buffer(struct server_vk *vk, VkBuffer buffer, VkCommandBuffer cmd) {
    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = 0,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0, NULL,
        1, &barrier,
        0, NULL);
}

// ============================================================================
// Frame Rendering
// ============================================================================

// Draw the captured texture as a fullscreen quad
static void
draw_captured_frame(struct server_vk *vk, VkCommandBuffer cmd) {
    struct vk_buffer *capture = vk->capture.current;
    if (!capture) {
        return;  // Nothing to draw
    }

        // vk_log(LOG_INFO, "Drawing frame %d: %dx%d stride=%d", vk->current_frame, capture->width, capture->height, capture->stride);

    

        // Set viewport and scissor to cover entire swapchain

        VkViewport viewport = {

            .x = 0.0f,

            .y = 0.0f,

            .width = (float)vk->swapchain.extent.width,

            .height = (float)vk->swapchain.extent.height,

            .minDepth = 0.0f,

            .maxDepth = 1.0f,

        };

        vkCmdSetViewport(cmd, 0, 1, &viewport);

    

        VkRect2D scissor = {

            .offset = { 0, 0 },

            .extent = vk->swapchain.extent,

        };

        vkCmdSetScissor(cmd, 0, 1, &scissor);

    

        // Bind the quad vertex buffer

        VkDeviceSize offset = 0;

        vkCmdBindVertexBuffers(cmd, 0, 1, &vk->quad_vertex_buffer, &offset);

    

        // Check if we should use the storage buffer path (NATIVE cross-GPU)

        if (capture->storage_buffer && capture->buffer_descriptor_set) {

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->buffer_blit.pipeline);

    

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,

                                    vk->buffer_blit.layout, 0, 1,

                                    &capture->buffer_descriptor_set, 0, NULL);

    

            struct {

                int32_t width;

                int32_t height;

                int32_t stride;

                int32_t swap_colors;

            } pc;

    

            pc.width = capture->width;

            pc.height = capture->height;

            pc.stride = capture->stride;

                    // Fix for "colors are reversed": Disable swap since XRGB matches output byte order

                    pc.swap_colors = 1;

            

                    vkCmdPushConstants(cmd, vk->buffer_blit.layout, VK_SHADER_STAGE_FRAGMENT_BIT,

                                       0, sizeof(pc), &pc);

    

            vkCmdDraw(cmd, 6, 1, 0, 0);
    } else if (capture->descriptor_set) {
        // Fallback to image path (Single GPU or compatible stride)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->blit_pipeline.pipeline);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vk->blit_pipeline.layout, 0, 1,
                                &capture->descriptor_set, 0, NULL);

        // Push the color swap flag for dual-GPU mode
        int32_t swap_colors = vk->dual_gpu ? 1 : 0;
        vkCmdPushConstants(cmd, vk->blit_pipeline.layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(int32_t), &swap_colors);

        vkCmdDraw(cmd, 6, 1, 0, 0);
    }
}

void
server_vk_begin_frame(struct server_vk *vk) {
    // Wait for previous frame to finish
    vkWaitForFences(vk->device, 1, &vk->in_flight[vk->current_frame], VK_TRUE, UINT64_MAX);
    vkResetFences(vk->device, 1, &vk->in_flight[vk->current_frame]);

    // Acquire next swapchain image
    vkAcquireNextImageKHR(vk->device, vk->swapchain.swapchain, UINT64_MAX,
                          vk->image_available[vk->current_frame], VK_NULL_HANDLE,
                          &vk->current_image_index);

    // Reset and begin command buffer
    VkCommandBuffer cmd = vk->command_buffers[vk->current_frame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    vkBeginCommandBuffer(cmd, &begin_info);

    // Perform dma-buf sync and image/buffer transition for captured buffer
    if (vk->capture.current && vk->capture.current->dmabuf_fd >= 0) {
        // Kernel-level sync: wait for Intel GPU to finish writing
        dmabuf_sync_start_read(vk->capture.current->dmabuf_fd);

        // GPU-level sync: transition from external GPU to our queue family
        if (vk->capture.current->storage_buffer && vk->capture.current->buffer_descriptor_set) {
            acquire_imported_buffer(vk, vk->capture.current->storage_buffer, cmd);
        } else {
            transition_imported_image(vk, vk->capture.current->image, cmd);
        }
    }

    // Begin render pass
    VkClearValue clear_value = { .color = {{ 0.0f, 0.0f, 0.0f, 1.0f }} };

    VkRenderPassBeginInfo rp_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = vk->render_pass,
        .framebuffer = vk->swapchain.framebuffers[vk->current_image_index],
        .renderArea = {
            .offset = { 0, 0 },
            .extent = vk->swapchain.extent,
        },
        .clearValueCount = 1,
        .pClearValues = &clear_value,
    };

    vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

    // Draw the captured frame as fullscreen quad
    draw_captured_frame(vk, cmd);
}

void
server_vk_end_frame(struct server_vk *vk) {
    VkCommandBuffer cmd = vk->command_buffers[vk->current_frame];

    vkCmdEndRenderPass(cmd);

    // Release imported image/buffer back to external GPU for next frame
    if (vk->capture.current) {
        if (vk->capture.current->storage_buffer && vk->capture.current->buffer_descriptor_set) {
            release_imported_buffer(vk, vk->capture.current->storage_buffer, cmd);
        } else if (vk->capture.current->image) {
            release_imported_image(vk, vk->capture.current->image, cmd);
        }
    }

    vkEndCommandBuffer(cmd);

    // End kernel-level dma-buf sync
    if (vk->capture.current && vk->capture.current->dmabuf_fd >= 0) {
        dmabuf_sync_end_read(vk->capture.current->dmabuf_fd);
    }

    // Submit command buffer
    VkSemaphore wait_semaphores[] = { vk->image_available[vk->current_frame] };
    VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore signal_semaphores[] = { vk->render_finished[vk->current_frame] };

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = wait_semaphores,
        .pWaitDstStageMask = wait_stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = signal_semaphores,
    };

    vkQueueSubmit(vk->graphics_queue, 1, &submit_info, vk->in_flight[vk->current_frame]);

    // Present
    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = signal_semaphores,
        .swapchainCount = 1,
        .pSwapchains = &vk->swapchain.swapchain,
        .pImageIndices = &vk->current_image_index,
    };

    vkQueuePresentKHR(vk->present_queue, &present_info);

    vk->current_frame = (vk->current_frame + 1) % VK_MAX_FRAMES_IN_FLIGHT;
}

// ============================================================================
// Buffer Import (dma-buf from Intel GPU)
// ============================================================================

static void
vk_buffer_destroy(struct vk_buffer *buffer) {
    if (!buffer || buffer->destroyed) return;
    buffer->destroyed = true;

    struct server_vk *vk = buffer->vk;

    if (buffer->descriptor_set) {
        vkFreeDescriptorSets(vk->device, vk->descriptor_pool, 1, &buffer->descriptor_set);
    }
    if (buffer->buffer_descriptor_set) {
        vkFreeDescriptorSets(vk->device, vk->descriptor_pool, 1, &buffer->buffer_descriptor_set);
    }
    if (buffer->view) {
        vkDestroyImageView(vk->device, buffer->view, NULL);
    }
    if (buffer->image) {
        vkDestroyImage(vk->device, buffer->image, NULL);
    }
    if (buffer->storage_buffer) {
        vkDestroyBuffer(vk->device, buffer->storage_buffer, NULL);
    }
    if (buffer->memory) {
        vkFreeMemory(vk->device, buffer->memory, NULL);
    }
    if (buffer->acquire_semaphore) {
        vkDestroySemaphore(vk->device, buffer->acquire_semaphore, NULL);
    }
    if (buffer->parent) {
        server_buffer_unref(buffer->parent);
    }

    if (buffer->link.prev || buffer->link.next) {
        wl_list_remove(&buffer->link);
    }
    free(buffer);
}

static uint32_t
find_memory_type(struct server_vk *vk, uint32_t type_filter, VkMemoryPropertyFlags properties) {
    for (uint32_t i = 0; i < vk->memory_properties.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (vk->memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

static VkFormat
drm_format_to_vk(uint32_t drm_format) {
    switch (drm_format) {
    case 0x34325258:  // DRM_FORMAT_XRGB8888
    case 0x34325241:  // DRM_FORMAT_ARGB8888
        return VK_FORMAT_B8G8R8A8_UNORM;
    case 0x34324258:  // DRM_FORMAT_XBGR8888
    case 0x34324241:  // DRM_FORMAT_ABGR8888
        return VK_FORMAT_R8G8B8A8_UNORM;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

static struct vk_buffer *
vk_buffer_import(struct server_vk *vk, struct server_buffer *buffer) {
    if (strcmp(buffer->impl->name, SERVER_BUFFER_DMABUF) != 0) {
        vk_log(LOG_ERROR, "cannot import non-DMABUF buffer");
        return NULL;
    }

    struct server_dmabuf_data *data = buffer->data;

    VkFormat format = drm_format_to_vk(data->format);
    if (format == VK_FORMAT_UNDEFINED) {
        vk_log(LOG_ERROR, "unsupported DRM format: 0x%x", data->format);
        return NULL;
    }

    struct vk_buffer *vk_buffer = zalloc(1, sizeof(*vk_buffer));
    vk_buffer->vk = vk;
    vk_buffer->parent = server_buffer_ref(buffer);
    vk_buffer->dmabuf_fd = data->planes[0].fd;

    uint64_t modifier = ((uint64_t)data->modifier_hi << 32) | data->modifier_lo;

    // NATIVE GPU-to-GPU dma-buf import via VK_EXT_external_memory_dma_buf
    // Use VK_IMAGE_TILING_LINEAR for cross-GPU sharing - direct VRAM access via ReBAR
    VkExternalMemoryImageCreateInfo ext_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &ext_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { data->width, data->height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
    };

    vk_log(LOG_INFO, "NATIVE dma-buf import: %dx%d, stride=%d, modifier=0x%"PRIx64", format=0x%x",
           data->width, data->height, data->planes[0].stride, modifier, data->format);

    // First create a test image to see what stride RADV wants
    VkResult result = vkCreateImage(vk->device, &image_info, NULL, &vk_buffer->image);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create VkImage for dma-buf: %d", result);
        goto fail;
    }

    // Query what stride Vulkan expects for this LINEAR image
    VkImageSubresource subres = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .arrayLayer = 0 };
    VkSubresourceLayout vk_layout;
    vkGetImageSubresourceLayout(vk->device, vk_buffer->image, &subres, &vk_layout);

    uint32_t dmabuf_stride = data->planes[0].stride;
    vk_log(LOG_INFO, "VK layout: rowPitch=%"PRIu64" | dma-buf stride=%u",
           vk_layout.rowPitch, dmabuf_stride);

    // If strides don't match, recreate image with width adjusted to match dma-buf stride
    if (vk_layout.rowPitch != dmabuf_stride) {
        vkDestroyImage(vk->device, vk_buffer->image, NULL);
        vk_buffer->image = VK_NULL_HANDLE;

        // Calculate effective width from dma-buf stride (4 bytes per pixel for XRGB8888)
        uint32_t effective_width = dmabuf_stride / 4;

        vk_log(LOG_INFO, "STRIDE FIX: adjusting width from %d to %d to match dma-buf stride",
               data->width, effective_width);

        image_info.extent.width = effective_width;

        result = vkCreateImage(vk->device, &image_info, NULL, &vk_buffer->image);
        if (result != VK_SUCCESS) {
            vk_log(LOG_ERROR, "failed to create stride-adjusted VkImage: %d", result);
            goto fail;
        }

        // Verify the new stride matches
        vkGetImageSubresourceLayout(vk->device, vk_buffer->image, &subres, &vk_layout);
        vk_log(LOG_INFO, "After adjustment: VK rowPitch=%"PRIu64" (should match %u)",
               vk_layout.rowPitch, dmabuf_stride);
    }

    // Store actual width for rendering (we'll crop in shader via viewport)
    vk_buffer->stride = dmabuf_stride;

    // Import dma-buf memory
    VkMemoryFdPropertiesKHR fd_props = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };

    PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHR =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(vk->device, "vkGetMemoryFdPropertiesKHR");

    if (!vkGetMemoryFdPropertiesKHR) {
        vk_log(LOG_ERROR, "vkGetMemoryFdPropertiesKHR not available");
        goto fail;
    }

    // Dup the fd since Vulkan takes ownership
    int fd_dup = dup(data->planes[0].fd);
    if (fd_dup < 0) {
        vk_log(LOG_ERROR, "failed to dup dma-buf fd");
        goto fail;
    }

    result = vkGetMemoryFdPropertiesKHR(vk->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                        fd_dup, &fd_props);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to get dma-buf memory properties: %d", result);
        close(fd_dup);
        goto fail;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vk->device, vk_buffer->image, &mem_reqs);

    // Use dedicated allocation for imported images (required by many drivers)
    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = vk_buffer->image,
        .buffer = VK_NULL_HANDLE,
    };

    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated_info,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = fd_dup,
    };

    // For cross-GPU dma-buf import, find any compatible memory type
    // First try DEVICE_LOCAL, then fall back to any available type
    uint32_t compatible_bits = mem_reqs.memoryTypeBits & fd_props.memoryTypeBits;
    uint32_t mem_type_index = find_memory_type(vk, compatible_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (mem_type_index == UINT32_MAX) {
        // Try without DEVICE_LOCAL requirement
        mem_type_index = find_memory_type(vk, compatible_bits, 0);
    }

    if (mem_type_index == UINT32_MAX && fd_props.memoryTypeBits != 0) {
        // Use fd's memory types directly - for cross-GPU the driver manages access
        mem_type_index = find_memory_type(vk, fd_props.memoryTypeBits, 0);
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type_index,
    };

    if (alloc_info.memoryTypeIndex == UINT32_MAX) {
        vk_log(LOG_ERROR, "no suitable memory type for dma-buf import (image=0x%x, fd=0x%x)",
               mem_reqs.memoryTypeBits, fd_props.memoryTypeBits);
        close(fd_dup);
        goto fail;
    }

    result = vkAllocateMemory(vk->device, &alloc_info, NULL, &vk_buffer->memory);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to allocate memory for dma-buf: %d", result);
        goto fail;
    }

    result = vkBindImageMemory(vk->device, vk_buffer->image, vk_buffer->memory, 0);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to bind dma-buf memory: %d", result);
        goto fail;
    }

    // Also create a VkBuffer backed by the same memory for manual stride handling
    // Calculate exact size needed for the data
    VkDeviceSize buffer_size = (VkDeviceSize)data->planes[0].stride * data->height;
    
    // Ensure we don't exceed allocated memory size
    if (buffer_size > mem_reqs.size) {
        buffer_size = mem_reqs.size;
    }

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer_size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    result = vkCreateBuffer(vk->device, &buffer_info, NULL, &vk_buffer->storage_buffer);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create storage buffer for dma-buf: %d", result);
        goto fail;
    }

    // Bind the same imported memory to the buffer
    result = vkBindBufferMemory(vk->device, vk_buffer->storage_buffer, vk_buffer->memory, 0);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to bind storage buffer memory: %d", result);
        goto fail;
    }

    // Allocate descriptor set for buffer path
    VkDescriptorSetAllocateInfo buf_desc_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = vk->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &vk->buffer_blit.descriptor_layout,
    };

    result = vkAllocateDescriptorSets(vk->device, &buf_desc_alloc_info, &vk_buffer->buffer_descriptor_set);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to allocate buffer descriptor set: %d", result);
        goto fail;
    }

    // Update descriptor set with storage buffer
    VkDescriptorBufferInfo buffer_desc = {
        .buffer = vk_buffer->storage_buffer,
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    };

    VkWriteDescriptorSet buf_desc_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = vk_buffer->buffer_descriptor_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .pBufferInfo = &buffer_desc,
    };

    vkUpdateDescriptorSets(vk->device, 1, &buf_desc_write, 0, NULL);

    // Create image view
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = vk_buffer->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    result = vkCreateImageView(vk->device, &view_info, NULL, &vk_buffer->view);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create image view for dma-buf: %d", result);
        goto fail;
    }

    // Store dimensions
    vk_buffer->width = data->width;
    vk_buffer->height = data->height;

    // Allocate descriptor set for this texture
    VkDescriptorSetAllocateInfo desc_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = vk->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &vk->blit_pipeline.descriptor_layout,
    };

    result = vkAllocateDescriptorSets(vk->device, &desc_alloc_info, &vk_buffer->descriptor_set);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to allocate descriptor set: %d", result);
        goto fail;
    }

    // Update descriptor set with this texture
    VkDescriptorImageInfo image_desc = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView = vk_buffer->view,
        .sampler = vk->sampler,
    };

    VkWriteDescriptorSet desc_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = vk_buffer->descriptor_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &image_desc,
    };

    vkUpdateDescriptorSets(vk->device, 1, &desc_write, 0, NULL);

    vk_log(LOG_INFO, "imported dma-buf: %dx%d, format=0x%x, modifier=0x%llx",
           data->width, data->height, data->format, (unsigned long long)modifier);

    wl_list_insert(&vk->capture.buffers, &vk_buffer->link);
    return vk_buffer;

fail:
    vk_buffer_destroy(vk_buffer);
    return NULL;
}

// ============================================================================
// Event Handlers
// ============================================================================

static void
on_surface_commit(struct wl_listener *listener, void *data) {
    struct server_vk *vk = wl_container_of(listener, vk, on_surface_commit);

    struct server_buffer *buffer = server_surface_next_buffer(vk->capture.surface);
    if (!buffer) {
        vk->capture.current = NULL;
        return;
    }

    // Check if buffer is already imported
    struct vk_buffer *vk_buf = NULL;
    struct vk_buffer *iter;
    wl_list_for_each(iter, &vk->capture.buffers, link) {
        if (iter->parent == buffer) {
            vk_buf = iter;
            break;
        }
    }

    // Import new buffer if needed
    if (!vk_buf) {
        vk_buf = vk_buffer_import(vk, buffer);
    }

    if (vk_buf) {
        vk->capture.current = vk_buf;

        // Render frame with Vulkan
        server_vk_begin_frame(vk);
        server_vk_end_frame(vk);
    }

    wl_signal_emit_mutable(&vk->events.frame, NULL);
}

static void
on_surface_destroy(struct wl_listener *listener, void *data) {
    struct server_vk *vk = wl_container_of(listener, vk, on_surface_destroy);

    wl_list_remove(&vk->on_surface_commit.link);
    wl_list_remove(&vk->on_surface_destroy.link);

    vk->capture.surface = NULL;
    vk->capture.current = NULL;
}

static void
cleanup_swapchain(struct server_vk *vk) {
    // Wait for device to be idle before destroying
    vkDeviceWaitIdle(vk->device);

    // Destroy framebuffers
    if (vk->swapchain.framebuffers) {
        for (uint32_t i = 0; i < vk->swapchain.image_count; i++) {
            if (vk->swapchain.framebuffers[i]) {
                vkDestroyFramebuffer(vk->device, vk->swapchain.framebuffers[i], NULL);
            }
        }
        free(vk->swapchain.framebuffers);
        vk->swapchain.framebuffers = NULL;
    }

    // Destroy image views
    if (vk->swapchain.views) {
        for (uint32_t i = 0; i < vk->swapchain.image_count; i++) {
            if (vk->swapchain.views[i]) {
                vkDestroyImageView(vk->device, vk->swapchain.views[i], NULL);
            }
        }
        free(vk->swapchain.views);
        vk->swapchain.views = NULL;
    }

    // Free images array (images are owned by swapchain)
    free(vk->swapchain.images);
    vk->swapchain.images = NULL;

    // Destroy old swapchain
    if (vk->swapchain.swapchain) {
        vkDestroySwapchainKHR(vk->device, vk->swapchain.swapchain, NULL);
        vk->swapchain.swapchain = VK_NULL_HANDLE;
    }
}

static bool
recreate_swapchain(struct server_vk *vk, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return true;  // Skip if minimized
    }

    cleanup_swapchain(vk);

    if (!create_swapchain(vk, width, height)) {
        vk_log(LOG_ERROR, "failed to recreate swapchain");
        return false;
    }

    if (!create_framebuffers(vk)) {
        vk_log(LOG_ERROR, "failed to recreate framebuffers");
        return false;
    }

    vk_log(LOG_INFO, "recreated swapchain: %dx%d", width, height);
    return true;
}

static void
on_ui_resize(struct wl_listener *listener, void *data) {
    struct server_vk *vk = wl_container_of(listener, vk, on_ui_resize);

    uint32_t width = vk->server->ui->width;
    uint32_t height = vk->server->ui->height;

    if (width > 0 && height > 0) {
        recreate_swapchain(vk, width, height);
    }
}
