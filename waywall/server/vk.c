#define _GNU_SOURCE

#include "server/vk.h"
#include "config/config.h"
#include "server/backend.h"
#include "server/buffer.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_compositor.h"
#include "server/wp_linux_drm_syncobj.h"
#include "server/wp_linux_dmabuf.h"
#include "util/alloc.h"
#include "util/avif.h"
#include "util/log.h"
#include "util/png.h"
#include "util/prelude.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

static PFN_vkImportSemaphoreFdKHR pfn_vkImportSemaphoreFdKHR = NULL;

#include <wayland-client-protocol.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>

static uint64_t
now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int32_t
refresh_mhz_to_ms(int32_t refresh_mhz) {
    if (refresh_mhz <= 0) {
        return 16; // fallback ~60 Hz
    }
    // refresh_mhz is milli-Hz, so period_ms = 1e6 / refresh_mhz.
    int32_t ms = (int32_t)(1000000 / refresh_mhz);
    if (ms < 1) ms = 1;
    if (ms > 1000) ms = 1000;
    return ms;
}

// Forward decls for helpers used before definition
static void on_ui_refresh(struct wl_listener *listener, void *data);
static int handle_overlay_tick(void *data);

// Forward decls for helpers used before definition
static uint32_t find_memory_type(struct server_vk *vk, uint32_t type_filter, VkMemoryPropertyFlags properties);
static VkFormat drm_format_to_vk(uint32_t drm_format);

static void
destroy_double_buffered_optimal(struct server_vk *vk, struct vk_buffer *buf) {
    for (int i = 0; i < 2; i++) {
        if (buf->optimal_descriptors[i]) {
            vkFreeDescriptorSets(vk->device, vk->descriptor_pool, 1, &buf->optimal_descriptors[i]);
            buf->optimal_descriptors[i] = VK_NULL_HANDLE;
        }
        if (buf->optimal_views[i]) {
            vkDestroyImageView(vk->device, buf->optimal_views[i], NULL);
            buf->optimal_views[i] = VK_NULL_HANDLE;
        }
        if (buf->optimal_images[i]) {
            vkDestroyImage(vk->device, buf->optimal_images[i], NULL);
            buf->optimal_images[i] = VK_NULL_HANDLE;
        }
        if (buf->optimal_memories[i]) {
            vkFreeMemory(vk->device, buf->optimal_memories[i], NULL);
            buf->optimal_memories[i] = VK_NULL_HANDLE;
        }
    }
    if (buf->copy_fence) {
        vkDestroyFence(vk->device, buf->copy_fence, NULL);
        buf->copy_fence = VK_NULL_HANDLE;
    }
    buf->async_optimal_valid = false;
    buf->copy_pending = false;
}

static void
destroy_optimal_copy(struct server_vk *vk, struct vk_buffer *buf) {
    if (buf->optimal_view) {
        vkDestroyImageView(vk->device, buf->optimal_view, NULL);
        buf->optimal_view = VK_NULL_HANDLE;
    }
    if (buf->optimal_image) {
        vkDestroyImage(vk->device, buf->optimal_image, NULL);
        buf->optimal_image = VK_NULL_HANDLE;
    }
    if (buf->optimal_memory) {
        vkFreeMemory(vk->device, buf->optimal_memory, NULL);
        buf->optimal_memory = VK_NULL_HANDLE;
    }
    buf->optimal_valid = false;
}

static bool
create_optimal_copy(struct server_vk *vk, struct vk_buffer *src_buf) {
    // Create an optimal-tiling image on AMD and copy from the imported linear image.
    VkImageCreateInfo img_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = src_buf->view ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_UNDEFINED,
        .extent = { (uint32_t)src_buf->width, (uint32_t)src_buf->height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkResult res = vkCreateImage(vk->device, &img_info, NULL, &src_buf->optimal_image);
    if (res != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vk->device, src_buf->optimal_image, &mem_reqs);

    uint32_t mem_type_index = find_memory_type(vk, mem_reqs.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type_index == UINT32_MAX) {
        vkDestroyImage(vk->device, src_buf->optimal_image, NULL);
        src_buf->optimal_image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type_index,
    };

    res = vkAllocateMemory(vk->device, &alloc_info, NULL, &src_buf->optimal_memory);
    if (res != VK_SUCCESS) {
        vkDestroyImage(vk->device, src_buf->optimal_image, NULL);
        src_buf->optimal_image = VK_NULL_HANDLE;
        return false;
    }

    res = vkBindImageMemory(vk->device, src_buf->optimal_image, src_buf->optimal_memory, 0);
    if (res != VK_SUCCESS) {
        destroy_optimal_copy(vk, src_buf);
        return false;
    }

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = src_buf->optimal_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = img_info.format,
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

    res = vkCreateImageView(vk->device, &view_info, NULL, &src_buf->optimal_view);
    if (res != VK_SUCCESS) {
        destroy_optimal_copy(vk, src_buf);
        return false;
    }

    src_buf->optimal_valid = true;
    return true;
}

static bool
copy_to_optimal(struct server_vk *vk, struct vk_buffer *buf) {
    if (!buf->optimal_valid || !buf->optimal_image || !buf->image) {
        return false;
    }

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vk->device, &alloc_info, &cmd) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &begin_info);

    // Transition layouts for copy
    VkImageMemoryBarrier barriers[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = buf->image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = buf->optimal_image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        },
    };

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 2, barriers);

    VkImageCopy region = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffset = { 0, 0, 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffset = { 0, 0, 0 },
        .extent = { (uint32_t)buf->width, (uint32_t)buf->height, 1 },
    };
    vkCmdCopyImage(cmd,
                   buf->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   buf->optimal_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region);

    VkImageMemoryBarrier post_barriers[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = buf->image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = buf->optimal_image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        },
    };

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 2, post_barriers);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VkResult submit_res = vkQueueSubmit(vk->graphics_queue, 1, &submit, VK_NULL_HANDLE);
    if (submit_res == VK_SUCCESS) {
        vkQueueWaitIdle(vk->graphics_queue);
    }
    vkFreeCommandBuffers(vk->device, vk->command_pool, 1, &cmd);

    return submit_res == VK_SUCCESS;
}

static bool
create_double_buffered_optimal(struct server_vk *vk, struct vk_buffer *src_buf) {
    if (!vk->async_pipelining_enabled) {
        return false;
    }

    VkFormat format = src_buf->view ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_UNDEFINED;
    if (format == VK_FORMAT_UNDEFINED) {
        return false;
    }

    // Create fence for async copy synchronization
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,  // Start signaled
    };
    VkResult res = vkCreateFence(vk->device, &fence_info, NULL, &src_buf->copy_fence);
    if (res != VK_SUCCESS) {
        return false;
    }

    // Create two optimal images for double-buffering
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo img_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = { (uint32_t)src_buf->width, (uint32_t)src_buf->height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        res = vkCreateImage(vk->device, &img_info, NULL, &src_buf->optimal_images[i]);
        if (res != VK_SUCCESS) {
            destroy_double_buffered_optimal(vk, src_buf);
            return false;
        }

        VkMemoryRequirements mem_reqs;
        vkGetImageMemoryRequirements(vk->device, src_buf->optimal_images[i], &mem_reqs);

        uint32_t mem_type_index = find_memory_type(vk, mem_reqs.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mem_type_index == UINT32_MAX) {
            destroy_double_buffered_optimal(vk, src_buf);
            return false;
        }

        VkMemoryAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mem_reqs.size,
            .memoryTypeIndex = mem_type_index,
        };

        res = vkAllocateMemory(vk->device, &alloc_info, NULL, &src_buf->optimal_memories[i]);
        if (res != VK_SUCCESS) {
            destroy_double_buffered_optimal(vk, src_buf);
            return false;
        }

        res = vkBindImageMemory(vk->device, src_buf->optimal_images[i], src_buf->optimal_memories[i], 0);
        if (res != VK_SUCCESS) {
            destroy_double_buffered_optimal(vk, src_buf);
            return false;
        }

        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = src_buf->optimal_images[i],
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

        res = vkCreateImageView(vk->device, &view_info, NULL, &src_buf->optimal_views[i]);
        if (res != VK_SUCCESS) {
            destroy_double_buffered_optimal(vk, src_buf);
            return false;
        }

        // Allocate descriptor set for this buffer
        VkDescriptorSetAllocateInfo desc_alloc = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = vk->descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &vk->blit_pipeline.descriptor_layout,
        };

        res = vkAllocateDescriptorSets(vk->device, &desc_alloc, &src_buf->optimal_descriptors[i]);
        if (res != VK_SUCCESS) {
            destroy_double_buffered_optimal(vk, src_buf);
            return false;
        }

        VkDescriptorImageInfo image_desc = {
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = src_buf->optimal_views[i],
            .sampler = vk->sampler,
        };

        VkWriteDescriptorSet desc_write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = src_buf->optimal_descriptors[i],
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &image_desc,
        };

        vkUpdateDescriptorSets(vk->device, 1, &desc_write, 0, NULL);
    }

    src_buf->optimal_read_index = 0;
    src_buf->optimal_write_index = 1;
    src_buf->copy_pending = false;
    src_buf->async_optimal_valid = true;

    // Note: vk_log not available here, will log in vk_buffer_import
    return true;
}

static void
start_async_copy_to_optimal(struct server_vk *vk, struct vk_buffer *buf) {
    if (!buf->async_optimal_valid || buf->copy_pending || !buf->image) {
        return;
    }

    int write_idx = buf->optimal_write_index;

    // Allocate command buffer from transfer pool
    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk->transfer_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vk->device, &alloc_info, &cmd) != VK_SUCCESS) {
        return;
    }

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &begin_info);

    // Transition layouts for copy
    VkImageMemoryBarrier barriers[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = buf->image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = buf->optimal_images[write_idx],
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        },
    };

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 2, barriers);

    VkImageCopy region = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffset = { 0, 0, 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffset = { 0, 0, 0 },
        .extent = { (uint32_t)buf->width, (uint32_t)buf->height, 1 },
    };
    vkCmdCopyImage(cmd,
                   buf->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   buf->optimal_images[write_idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region);

    // Transition to shader read
    VkImageMemoryBarrier post_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = buf->optimal_images[write_idx],
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &post_barrier);

    vkEndCommandBuffer(cmd);

    // Check if previous copy is done (NON-BLOCKING) - skip if still in progress
    VkResult fence_status = vkGetFenceStatus(vk->device, buf->copy_fence);
    if (fence_status != VK_SUCCESS) {
        // Previous copy still running, skip this one to avoid stalling
        vkFreeCommandBuffers(vk->device, vk->transfer_pool, 1, &cmd);
        return;
    }
    vkResetFences(vk->device, 1, &buf->copy_fence);

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VkResult submit_res = vkQueueSubmit(vk->transfer_queue, 1, &submit, buf->copy_fence);

    vkFreeCommandBuffers(vk->device, vk->transfer_pool, 1, &cmd);

    if (submit_res == VK_SUCCESS) {
        buf->copy_pending = true;
    }
}

static void
try_swap_optimal_buffers(struct server_vk *vk, struct vk_buffer *buf) {
    if (!buf->async_optimal_valid || !buf->copy_pending) {
        return;
    }

    // Check if copy finished (non-blocking)
    VkResult result = vkGetFenceStatus(vk->device, buf->copy_fence);
    if (result == VK_SUCCESS) {
        // Swap indices
        int tmp = buf->optimal_read_index;
        buf->optimal_read_index = buf->optimal_write_index;
        buf->optimal_write_index = tmp;

        // Update descriptor to point to new read buffer
        buf->descriptor_set = buf->optimal_descriptors[buf->optimal_read_index];

        buf->copy_pending = false;
    }
}

static bool
vk_image_write_rgba(struct server_vk *vk, struct vk_image *image, const unsigned char *rgba, uint32_t width,
                    uint32_t height) {
    if (!vk || !vk->device || !image || !image->owns_image || !image->image || !image->memory || !rgba) {
        return false;
    }
    if (width == 0 || height == 0) {
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vk->device, image->image, &mem_reqs);

    void *mapped = NULL;
    if (vkMapMemory(vk->device, image->memory, 0, mem_reqs.size, 0, &mapped) != VK_SUCCESS) {
        return false;
    }

    VkImageSubresource subres = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(vk->device, image->image, &subres, &layout);

    const unsigned char *src = rgba;
    unsigned char *dst = (unsigned char *)mapped + layout.offset;
    const size_t src_row = (size_t)width * 4;

    for (uint32_t y = 0; y < height; y++) {
        memcpy(dst, src, src_row);
        src += src_row;
        dst += layout.rowPitch;
    }

    vkUnmapMemory(vk->device, image->memory);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmd_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(vk->device, &cmd_alloc, &cmd) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &begin_info);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    vkQueueSubmit(vk->graphics_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk->graphics_queue);
    vkFreeCommandBuffers(vk->device, vk->command_pool, 1, &cmd);
    return true;
}

static void
vk_update_animated_images(struct server_vk *vk) {
    if (!vk || !vk->device) return;

    const uint64_t now = now_ms();
    bool waited = false;
    struct vk_image *image;
    wl_list_for_each(image, &vk->images, link) {
        if (!image->enabled) continue;
        if (!image->owns_image) continue;
        if (!image->frames || image->frame_count <= 1) continue;
        if (now < image->next_frame_ms) continue;

        if (!waited) {
            vkDeviceWaitIdle(vk->device);
            waited = true;
        }

        image->frame_index = (image->frame_index + 1) % image->frame_count;
        struct util_avif_frame *frame = &image->frames[image->frame_index];
        (void)vk_image_write_rgba(vk, image, (const unsigned char *)frame->data, (uint32_t)frame->width,
                                  (uint32_t)frame->height);

        double dur_s = frame->duration;
        if (!(dur_s > 0.0)) dur_s = 0.1;
        uint64_t dur_ms = (uint64_t)llround(dur_s * 1000.0);
        if (dur_ms == 0) dur_ms = 1;
        image->next_frame_ms = now + dur_ms;
    }
}

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
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
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

// Text rendering forward declarations
static bool init_font_system(struct server_vk *vk, const char *font_path, uint32_t base_size);
static void destroy_font_system(struct server_vk *vk);
static bool create_text_vk_pipeline(struct server_vk *vk);
// static void draw_texts(struct server_vk *vk, VkCommandBuffer cmd);

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
                    uint32_t *graphics_family, uint32_t *present_family, uint32_t *transfer_family) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);

    VkQueueFamilyProperties *props = zalloc(count, sizeof(*props));
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, props);

    bool found_graphics = false;
    bool found_present = false;
    bool found_transfer = false;

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

        // Look for dedicated transfer queue (without graphics bit)
        if ((props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            !found_transfer) {
            *transfer_family = i;
            found_transfer = true;
        }

        if (found_graphics && found_present) {
            break;
        }
    }

    // If no dedicated transfer queue, fall back to graphics queue
    if (!found_transfer && found_graphics) {
        *transfer_family = *graphics_family;
        found_transfer = true;
    }

    free(props);
    return found_graphics && found_present && found_transfer;
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

    // Prefer AMD discrete GPU (0x1002). If env WAYWALL_VK_VENDOR is set to "amd"/"intel",
    // honor it. Default: use legacy selection (last discrete wins) to avoid FPS cap regression;
    // set WAYWALL_GPU_SELECT_STRICT=1 to enable the new AMD-first selection by default.
    // Env WAYWALL_GPU_SELECT_LEGACY explicitly forces the legacy path.
    const char *env_vendor = getenv("WAYWALL_VK_VENDOR");
    bool prefer_amd = true;
    bool prefer_intel = false;
    bool use_legacy_select = true;
    if (env_vendor) {
        if (strcasecmp(env_vendor, "intel") == 0) {
            prefer_amd = false;
            prefer_intel = true;
            use_legacy_select = false;  // explicit vendor request
        } else if (strcasecmp(env_vendor, "amd") == 0) {
            prefer_amd = true;
            prefer_intel = false;
            use_legacy_select = false;  // explicit vendor request
        }
    } else {
        if (getenv("WAYWALL_GPU_SELECT_STRICT")) {
            use_legacy_select = false; // opt in to AMD-first without vendor env
        } else if (getenv("WAYWALL_GPU_SELECT_LEGACY")) {
            use_legacy_select = true;
        } else {
            use_legacy_select = true; // default to legacy
        }
    }

    // Prefer AMD discrete GPU (0x1002). If not present, prefer first discrete GPU.
    VkPhysicalDevice preferred_amd = VK_NULL_HANDLE;
    VkPhysicalDevice preferred_discrete = VK_NULL_HANDLE;
    VkPhysicalDevice preferred_intel = VK_NULL_HANDLE;
    VkPhysicalDevice fallback = VK_NULL_HANDLE;

    // Legacy path variables
    VkPhysicalDevice legacy_selected = VK_NULL_HANDLE;
    VkPhysicalDevice legacy_fallback = VK_NULL_HANDLE;
    bool legacy_prefer_amd_last = getenv("WAYWALL_PREFER_AMD_LEGACY") != NULL;

    uint32_t amd_gfx = 0, amd_present = 0, amd_transfer = 0;
    uint32_t discrete_gfx = 0, discrete_present = 0, discrete_transfer = 0;
    uint32_t fb_gfx = 0, fb_present = 0, fb_transfer = 0;

    uint32_t legacy_gfx = 0, legacy_present = 0, legacy_transfer = 0;
    uint32_t legacy_fb_gfx = 0, legacy_fb_present = 0, legacy_fb_transfer = 0;

    bool has_amd = false;
    bool has_intel = false;

    uint32_t passes = (use_legacy_select && legacy_prefer_amd_last) ? 2 : 1;
    for (uint32_t pass = 0; pass < passes; pass++) {
        bool prefer_amd_pass = legacy_prefer_amd_last && pass == 1;

        for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);

        vk_log(LOG_INFO, "Device found: %s (vendor=0x%x)", props.deviceName, props.vendorID);

        if (props.vendorID == 0x1002) has_amd = true;
        if (props.vendorID == 0x8086) has_intel = true;

        if (!check_device_extensions(devices[i])) {
            continue;
        }

        uint32_t gfx_family, present_family, transfer_family;
        if (!find_queue_families(devices[i], vk->swapchain.surface, &gfx_family, &present_family, &transfer_family)) {
            continue;
        }

        vk_log(LOG_INFO, "found suitable device: %s", props.deviceName);

        // Legacy path: last discrete wins, fallback is first suitable.
        // If WAYWALL_PREFER_AMD_LEGACY is set, we iterate twice and place AMD last.
        if (use_legacy_select) {
            if (legacy_prefer_amd_last) {
                if (prefer_amd_pass && props.vendorID != 0x1002) {
                    continue; // skip non-AMD in AMD pass
                }
                if (!prefer_amd_pass && props.vendorID == 0x1002) {
                    continue; // skip AMD in non-AMD pass
                }
            }

            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                legacy_selected = devices[i];
                legacy_gfx = gfx_family;
                legacy_present = present_family;
                legacy_transfer = transfer_family;
            } else if (legacy_fallback == VK_NULL_HANDLE) {
                legacy_fallback = devices[i];
                legacy_fb_gfx = gfx_family;
                legacy_fb_present = present_family;
                legacy_fb_transfer = transfer_family;
            }
            continue;
        }

        // New path: AMD > first discrete > fallback
        if (props.vendorID == 0x1002 && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            // Strong preference: AMD dGPU
            preferred_amd = devices[i];
            amd_gfx = gfx_family;
            amd_present = present_family;
            amd_transfer = transfer_family;
            // Keep scanning to record detection flags, but selection is decided.
            continue;
        }

        if (props.vendorID == 0x8086 && preferred_intel == VK_NULL_HANDLE &&
            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            preferred_intel = devices[i];
            discrete_gfx = gfx_family;
            discrete_present = present_family;
            discrete_transfer = transfer_family;
        }

        if (preferred_discrete == VK_NULL_HANDLE &&
            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            preferred_discrete = devices[i];
            discrete_gfx = gfx_family;
            discrete_present = present_family;
            discrete_transfer = transfer_family;
        }

        if (fallback == VK_NULL_HANDLE) {
            fallback = devices[i];
            fb_gfx = gfx_family;
            fb_present = present_family;
            fb_transfer = transfer_family;
        }
        }
    }

    VkPhysicalDevice selected = VK_NULL_HANDLE;
    if (use_legacy_select) {
        selected = legacy_selected ? legacy_selected : legacy_fallback;
        if (selected) {
            vk->graphics_family = legacy_selected ? legacy_gfx : legacy_fb_gfx;
            vk->present_family = legacy_selected ? legacy_present : legacy_fb_present;
            vk->transfer_family = legacy_selected ? legacy_transfer : legacy_fb_transfer;
        }
        if (selected) {
            vk_log(LOG_INFO, "Legacy GPU selection enabled (WAYWALL_GPU_SELECT_LEGACY)");
        }
    } else {
        if (prefer_amd && preferred_amd != VK_NULL_HANDLE) {
            selected = preferred_amd;
            vk->graphics_family = amd_gfx;
            vk->present_family = amd_present;
            vk->transfer_family = amd_transfer;
        } else if (prefer_intel && preferred_intel != VK_NULL_HANDLE) {
            selected = preferred_intel;
            vk->graphics_family = discrete_gfx;
            vk->present_family = discrete_present;
            vk->transfer_family = discrete_transfer;
        } else if (preferred_discrete != VK_NULL_HANDLE) {
            selected = preferred_discrete;
            vk->graphics_family = discrete_gfx;
            vk->present_family = discrete_present;
            vk->transfer_family = discrete_transfer;
        } else if (fallback != VK_NULL_HANDLE) {
            selected = fallback;
            vk->graphics_family = fb_gfx;
            vk->present_family = fb_present;
            vk->transfer_family = fb_transfer;
        }
    }

    free(devices);

    vk_log(LOG_INFO, "Detection result: has_amd=%d, has_intel=%d", has_amd, has_intel);

    if (selected == VK_NULL_HANDLE) {
        vk_log(LOG_ERROR, "no suitable Vulkan device found");
        return false;
    }

    vk->physical_device = selected;
    vk->dual_gpu = has_amd && has_intel;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(selected, &props);
    vk_log(LOG_INFO, "selected device: %s (dual_gpu=%s)", props.deviceName, vk->dual_gpu ? "true" : "false");
    vk_log(LOG_INFO, "queue families: graphics=%u, present=%u, transfer=%u%s",
           vk->graphics_family, vk->present_family, vk->transfer_family,
           vk->transfer_family != vk->graphics_family ? " (dedicated)" : "");

    vkGetPhysicalDeviceMemoryProperties(selected, &vk->memory_properties);

    // Enable async pipelining if dual-GPU and env var is set
    vk->async_pipelining_enabled = vk->dual_gpu && getenv("WAYWALL_ASYNC_PIPELINING") != NULL;
    if (vk->async_pipelining_enabled) {
        vk_log(LOG_INFO, "Async pipelining ENABLED for dual-GPU setup");
    }

    return true;
}

// ============================================================================
// Logical Device Creation
// ============================================================================

static bool
create_device(struct server_vk *vk) {
    // Queue create infos - support up to 3 unique families
    float priority = 1.0f;
    uint32_t unique_families[3] = { vk->graphics_family, vk->present_family, vk->transfer_family };
    uint32_t unique_count = 1;

    // Add present family if different from graphics
    if (vk->present_family != vk->graphics_family) {
        unique_families[unique_count++] = vk->present_family;
    }

    // Add transfer family if different from both graphics and present
    if (vk->transfer_family != vk->graphics_family && vk->transfer_family != vk->present_family) {
        unique_families[unique_count++] = vk->transfer_family;
    }

    VkDeviceQueueCreateInfo queue_infos[3];
    for (uint32_t i = 0; i < unique_count; i++) {
        queue_infos[i] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = unique_families[i],
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
    }

    VkPhysicalDeviceFeatures features = {0};

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .timelineSemaphore = VK_TRUE,
    };

    VkDeviceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &timeline_features,
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
    vkGetDeviceQueue(vk->device, vk->transfer_family, 0, &vk->transfer_queue);

    pfn_vkImportSemaphoreFdKHR = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(vk->device, "vkImportSemaphoreFdKHR");
    if (!pfn_vkImportSemaphoreFdKHR) {
        vk_log(LOG_WARN, "failed to load vkImportSemaphoreFdKHR - explicit sync disabled");
    }

    // Create transfer command pool if async pipelining is enabled
    if (vk->async_pipelining_enabled) {
        VkCommandPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = vk->transfer_family,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        };
        result = vkCreateCommandPool(vk->device, &pool_info, NULL, &vk->transfer_pool);
        if (result != VK_SUCCESS) {
            vk_log(LOG_ERROR, "failed to create transfer command pool: %d", result);
            vk->async_pipelining_enabled = false;
        } else {
            vk_log(LOG_INFO, "created transfer command pool for async pipelining");
        }
    }

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

    // Prefer BGRA8 UNORM (pass-through sRGB values without re-encoding)
    VkSurfaceFormatKHR selected = formats[0];
    for (uint32_t i = 0; i < count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            selected = formats[i];
            break;
        }
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB) {
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

    // Optional override via env: WAYWALL_PRESENT_MODE=IMMEDIATE|MAILBOX|FIFO
    VkPresentModeKHR forced = VK_PRESENT_MODE_MAX_ENUM_KHR;
    const char *env_mode = getenv("WAYWALL_PRESENT_MODE");
    if (env_mode) {
        if (strcasecmp(env_mode, "IMMEDIATE") == 0) forced = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (strcasecmp(env_mode, "MAILBOX") == 0) forced = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (strcasecmp(env_mode, "FIFO") == 0) forced = VK_PRESENT_MODE_FIFO_KHR;
    }

    // Prefer IMMEDIATE (no vsync) > MAILBOX (low latency) > FIFO (vsync)
    VkPresentModeKHR selected = VK_PRESENT_MODE_FIFO_KHR;  // Always available
    for (uint32_t i = 0; i < count; i++) {
        if (forced != VK_PRESENT_MODE_MAX_ENUM_KHR && modes[i] == forced) {
            selected = modes[i];
            break;
        }
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            selected = VK_PRESENT_MODE_IMMEDIATE_KHR;
            break;
        }
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR && selected != VK_PRESENT_MODE_IMMEDIATE_KHR) {
            selected = VK_PRESENT_MODE_MAILBOX_KHR;
        }
    }

    const char *sel_name = "UNKNOWN";
    switch (selected) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: sel_name = "IMMEDIATE"; break;
    case VK_PRESENT_MODE_MAILBOX_KHR: sel_name = "MAILBOX"; break;
    case VK_PRESENT_MODE_FIFO_KHR: sel_name = "FIFO"; break;
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: sel_name = "FIFO_RELAXED"; break;
    default: break;
    }
    if (forced != VK_PRESENT_MODE_MAX_ENUM_KHR) {
        vk_log(LOG_INFO, "Present mode: forced %s (selected=%s)", env_mode ? env_mode : "?", sel_name);
    } else {
        vk_log(LOG_INFO, "Present mode: selected=%s", sel_name);
    }

    free(modes);
    return selected;
}

static bool
create_swapchain(struct server_vk *vk, uint32_t width, uint32_t height, VkSwapchainKHR old_swapchain) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->physical_device, vk->swapchain.surface, &caps);

    // Prefer BGRA8 UNORM (pass-through sRGB values without re-encoding)
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

    // Check for transparent compositing support (for background visibility)
    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
        composite_alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
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
        .compositeAlpha = composite_alpha,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = old_swapchain,
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

    // Push constants: width, height, stride, swap_colors, src_x, src_y, src_w, src_h
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(int32_t) * 8,  // 8 ints for source cropping
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

// Mirror push constants structure (must match shader)
struct mirror_push_constants {
    int32_t game_width;
    int32_t game_height;
    int32_t game_stride;
    int32_t src_x;
    int32_t src_y;
    int32_t src_w;
    int32_t src_h;
    int32_t color_key_enabled;
    float key_r, key_g, key_b;
    float out_r, out_g, out_b;
    float tolerance;
};

static bool
create_mirror_pipeline(struct server_vk *vk) {
    // Create shader module for mirror fragment shader
    vk->mirror_pipeline.frag = create_shader_module(vk->device, mirror_frag_spv, mirror_frag_spv_size);
    if (!vk->mirror_pipeline.frag) {
        vk_log(LOG_ERROR, "failed to create mirror shader module");
        return false;
    }

    // Push constants for mirror parameters
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(struct mirror_push_constants),
    };

    VkPipelineLayoutCreateInfo pipeline_layout_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &vk->buffer_blit.descriptor_layout,  // Reuse storage buffer layout
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };

    if (vkCreatePipelineLayout(vk->device, &pipeline_layout_ci, NULL, &vk->mirror_pipeline.layout) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create mirror pipeline layout");
        return false;
    }

    // Create graphics pipeline (reuse vertex shader from blit)
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
            .module = vk->mirror_pipeline.frag,
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

    // Pre-multiplied alpha blending for mirrors
    // Mirror outputs either opaque (alpha=1) or fully transparent (alpha=0), both are pre-multiplied
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,  // Pre-multiplied alpha
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
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
        .layout = vk->mirror_pipeline.layout,
        .renderPass = vk->render_pass,
        .subpass = 0,
    };

    if (vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipeline_ci, NULL, &vk->mirror_pipeline.pipeline) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create mirror pipeline");
        return false;
    }

    vk_log(LOG_INFO, "created mirror pipeline");
    return true;
}

static bool
create_image_pipeline(struct server_vk *vk) {
    // Create descriptor set layout for images (same as blit - combined image sampler)
    if (!create_descriptor_set_layout(vk, &vk->image_pipeline.descriptor_layout)) {
        vk_log(LOG_ERROR, "failed to create image descriptor set layout");
        return false;
    }

    // Pipeline layout (no push constants needed - viewport controls position)
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &vk->image_pipeline.descriptor_layout,
    };

    if (vkCreatePipelineLayout(vk->device, &layout_info, NULL, &vk->image_pipeline.layout) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create image pipeline layout");
        return false;
    }

    // Create shader module for image fragment shader
    VkShaderModule image_frag = create_shader_module(vk->device, image_frag_spv, image_frag_spv_size);
    if (!image_frag) {
        vk_log(LOG_ERROR, "failed to create image shader module");
        return false;
    }

    // Shader stages (reuse vertex shader from blit)
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
            .module = image_frag,
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

    // Pre-multiplied alpha blending for images with transparency
    // Shader outputs pre-multiplied colors (rgb * a, a), so use ONE for srcColor
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,  // Pre-multiplied: colors already include alpha
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
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
        .layout = vk->image_pipeline.layout,
        .renderPass = vk->render_pass,
        .subpass = 0,
    };

    VkResult result = vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipeline_ci, NULL, &vk->image_pipeline.pipeline);

    // Clean up shader module (don't need to keep it)
    vkDestroyShaderModule(vk->device, image_frag, NULL);

    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create image pipeline: %d", result);
        return false;
    }

    vk_log(LOG_INFO, "created image pipeline");
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
server_vk_create(struct server *server, struct config *cfg) {
    struct server_vk *vk = zalloc(1, sizeof(*vk));
    vk->server = server;
    vk->drm_fd = -1;
    vk->fps_last_time_ms = now_ms();
    vk->fps_frame_count = 0;
    vk->disable_capture_sync_wait = getenv("WAYWALL_DISABLE_CAPTURE_SYNC_WAIT") != NULL;
    // Prefer modifier-based dma-buf imports when we know we're doing cross-GPU (subprocess offload)
    // to avoid ReBAR-limited linear paths. Env can still force it.
    bool env_allow_mods = getenv("WAYWALL_DMABUF_ALLOW_MODIFIERS") != NULL;
    vk->allow_modifiers =
        env_allow_mods || (server->linux_dmabuf && server->linux_dmabuf->allow_modifiers);
    vk->proxy_game = getenv("WAYWALL_VK_PROXY_GAME") != NULL;
    vk->overlay_tick = NULL;
    vk->overlay_tick_ms = refresh_mhz_to_ms(server->ui ? server->ui->refresh_mhz : 0);

    vk_log(LOG_INFO, "creating Vulkan backend");

    wl_list_init(&vk->capture.buffers);
    wl_list_init(&vk->mirrors);
    wl_list_init(&vk->images);
    wl_list_init(&vk->atlases);
    wl_list_init(&vk->texts);
    wl_list_init(&vk->views);
    wl_signal_init(&vk->events.frame);
    wl_list_init(&vk->on_ui_resize.link);
    wl_list_init(&vk->on_ui_refresh.link);

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
    // Match GL behavior - don't call place_below, let subsurface stack naturally

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
    if (!create_swapchain(vk, width, height, VK_NULL_HANDLE)) {
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
    if (!create_texcopy_pipeline(vk) || !create_text_pipeline(vk) || !create_blit_pipeline(vk) || !create_buffer_blit_pipeline(vk) || !create_mirror_pipeline(vk) || !create_image_pipeline(vk) || !create_text_vk_pipeline(vk)) {
        goto fail;
    }

    // Initialize font system for text rendering
    const char *font_path = cfg ? cfg->theme.font_path : NULL;
    uint32_t font_size = 1;  // Size is specified per-text (pixels)
    if (font_path && font_path[0]) {
        if (!init_font_system(vk, font_path, font_size)) {
            vk_log(LOG_WARN, "font system initialization failed, text rendering disabled");
        }
    } else {
        vk_log(LOG_INFO, "no font path configured, text rendering disabled");
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

    if (vk->proxy_game) {
        VkCommandBufferAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = vk->command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = DMABUF_EXPORT_MAX,
        };
        VkResult result =
            vkAllocateCommandBuffers(vk->device, &alloc_info, vk->proxy_copy.command_buffers);
        if (result != VK_SUCCESS) {
            vk_log(LOG_ERROR, "failed to allocate proxy copy command buffers: %d", result);
            goto fail;
        }

        VkFenceCreateInfo fence_info = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        for (uint32_t i = 0; i < DMABUF_EXPORT_MAX; i++) {
            result = vkCreateFence(vk->device, &fence_info, NULL, &vk->proxy_copy.fences[i]);
            if (result != VK_SUCCESS) {
                vk_log(LOG_ERROR, "failed to create proxy copy fence: %d", result);
                goto fail;
            }
        }
        vk->proxy_copy.index = 0;

        // Drive overlay rendering independently of the game’s surface commits.
        vk->on_ui_refresh.notify = on_ui_refresh;
        wl_signal_add(&server->ui->events.refresh, &vk->on_ui_refresh);

        struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
        vk->overlay_tick = wl_event_loop_add_timer(loop, handle_overlay_tick, vk);
        check_alloc(vk->overlay_tick);
        wl_event_source_timer_update(vk->overlay_tick, vk->overlay_tick_ms);
    }

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
    wl_list_remove(&vk->on_ui_refresh.link);

    if (vk->overlay_tick) {
        wl_event_source_remove(vk->overlay_tick);
        vk->overlay_tick = NULL;
    }

    if (vk->device) {
        for (uint32_t i = 0; i < DMABUF_EXPORT_MAX; i++) {
            if (vk->proxy_copy.fences[i]) {
                vkDestroyFence(vk->device, vk->proxy_copy.fences[i], NULL);
                vk->proxy_copy.fences[i] = VK_NULL_HANDLE;
            }
        }

        vkDeviceWaitIdle(vk->device);
    }

    // Destroy capture buffers
    struct vk_buffer *buf, *tmp;
    wl_list_for_each_safe(buf, tmp, &vk->capture.buffers, link) {
        vk_buffer_destroy(buf);
    }

    // Destroy text objects
    struct vk_text *text, *text_tmp;
    wl_list_for_each_safe(text, text_tmp, &vk->texts, link) {
        if (text->vertex_buffer) {
            vkFreeMemory(vk->device, text->vertex_memory, NULL);
            vkDestroyBuffer(vk->device, text->vertex_buffer, NULL);
        }
        wl_list_remove(&text->link);
        free(text->text);
        free(text);
    }

    // Destroy font system
    destroy_font_system(vk);

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

    // Destroy mirror pipeline
    if (vk->mirror_pipeline.pipeline) {
        vkDestroyPipeline(vk->device, vk->mirror_pipeline.pipeline, NULL);
    }
    if (vk->mirror_pipeline.layout) {
        vkDestroyPipelineLayout(vk->device, vk->mirror_pipeline.layout, NULL);
    }
    if (vk->mirror_pipeline.frag) {
        vkDestroyShaderModule(vk->device, vk->mirror_pipeline.frag, NULL);
    }

    // Destroy mirrors
    struct vk_mirror *mirror, *tmp_mirror;
    wl_list_for_each_safe(mirror, tmp_mirror, &vk->mirrors, link) {
        wl_list_remove(&mirror->link);
        free(mirror);
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

// Draw the captured texture centered in the window (matching original waywall behavior)
// Called from begin_frame after validating capture is ready
static void
draw_captured_frame(struct server_vk *vk, VkCommandBuffer cmd) {
    struct vk_buffer *capture = vk->capture.current;
    // Note: capture is validated in begin_frame, so we assert here
    ww_assert(capture);

    int32_t game_width = capture->width;
    int32_t game_height = capture->height;
    int32_t window_width = (int32_t)vk->swapchain.extent.width;
    int32_t window_height = (int32_t)vk->swapchain.extent.height;

    // Calculate centered position (same logic as layout_centered in ui.c)
    int32_t x = (window_width / 2) - (game_width / 2);
    int32_t y = (window_height / 2) - (game_height / 2);

    // Calculate viewport position and source crop region for oversized games
    int32_t vp_x, vp_y, vp_width, vp_height;
    int32_t src_x = 0, src_y = 0, src_w = game_width, src_h = game_height;

    if (x >= 0 && y >= 0) {
        // Game fits entirely within window - center it
        vp_x = x;
        vp_y = y;
        vp_width = game_width;
        vp_height = game_height;
        // No source cropping needed - show entire game
    } else {
        // Game is larger than window in one or both dimensions
        // Match layout_centered from ui.c: crop from CENTER of game
        int32_t crop_width = (x >= 0) ? game_width : window_width;
        int32_t crop_height = (y >= 0) ? game_height : window_height;

        // Source crop from center of game (same as OpenGL viewporter)
        src_x = (game_width / 2) - (crop_width / 2);
        src_y = (game_height / 2) - (crop_height / 2);
        src_w = crop_width;
        src_h = crop_height;

        // Viewport position (clamp to window bounds)
        vp_x = (x >= 0) ? x : 0;
        vp_y = (y >= 0) ? y : 0;
        vp_width = crop_width;
        vp_height = crop_height;
    }

    // Set viewport to the visible area
    VkViewport viewport = {
        .x = (float)vp_x,
        .y = (float)vp_y,
        .width = (float)vp_width,
        .height = (float)vp_height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    // Scissor clips to window bounds
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
            int32_t src_x;
            int32_t src_y;
            int32_t src_w;
            int32_t src_h;
        } pc;

        pc.width = game_width;
        pc.height = game_height;
        pc.stride = capture->stride;
        pc.swap_colors = 0;  // Buffer path uses consistent unpacking
        pc.src_x = src_x;
        pc.src_y = src_y;
        pc.src_w = src_w;
        pc.src_h = src_h;

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

// Sorting types
enum render_item_type { ITEM_MIRROR, ITEM_IMAGE, ITEM_TEXT, ITEM_VIEW };

struct render_item {
    int32_t depth;
    enum render_item_type type;
    void *obj;
};

static int compare_render_items(const void *a, const void *b) {
    const struct render_item *ia = a;
    const struct render_item *ib = b;
    if (ia->depth != ib->depth) {
        return ia->depth - ib->depth;
    }
    // Stable sort by type to minimize pipeline switches
    return (int)ia->type - (int)ib->type;
}

static void
draw_mirror_single(struct server_vk *vk, VkCommandBuffer cmd, struct vk_mirror *mirror) {
    struct vk_buffer *capture = vk->capture.current;
    if (!capture || !capture->storage_buffer || !capture->buffer_descriptor_set) {
        return;
    }

    // Set viewport for this mirror's destination
    VkViewport viewport = {
        .x = (float)mirror->dst.x,
        .y = (float)mirror->dst.y,
        .width = (float)mirror->dst.width,
        .height = (float)mirror->dst.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    // Scissor to window bounds
    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = vk->swapchain.extent,
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Prepare push constants
    struct mirror_push_constants pc = {
        .game_width = capture->width,
        .game_height = capture->height,
        .game_stride = capture->stride,
        .src_x = mirror->src.x,
        .src_y = mirror->src.y,
        .src_w = mirror->src.width,
        .src_h = mirror->src.height,
        .color_key_enabled = mirror->color_key_enabled ? 1 : 0,
        .key_r = ((mirror->color_key_input >> 16) & 0xFF) / 255.0f,
        .key_g = ((mirror->color_key_input >> 8) & 0xFF) / 255.0f,
        .key_b = (mirror->color_key_input & 0xFF) / 255.0f,
        .out_r = ((mirror->color_key_output >> 16) & 0xFF) / 255.0f,
        .out_g = ((mirror->color_key_output >> 8) & 0xFF) / 255.0f,
        .out_b = (mirror->color_key_output & 0xFF) / 255.0f,
        .tolerance = mirror->color_key_tolerance,
    };

    vkCmdPushConstants(cmd, vk->mirror_pipeline.layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

static void
draw_image_single(struct server_vk *vk, VkCommandBuffer cmd, struct vk_image *image) {
    if (!image->enabled || image->descriptor_set == VK_NULL_HANDLE) {
        return;
    }

    VkDeviceSize offset = 0;
    VkBuffer vertex_buffer = image->vertex_buffer ? image->vertex_buffer : vk->quad_vertex_buffer;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &offset);

    // Bind this image's descriptor set
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk->image_pipeline.layout, 0, 1,
                            &image->descriptor_set, 0, NULL);

    // Set viewport for this image's destination
    VkViewport viewport = {
        .x = (float)image->dst.x,
        .y = (float)image->dst.y,
        .width = (float)image->dst.width,
        .height = (float)image->dst.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    // Scissor to window bounds
    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = vk->swapchain.extent,
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdDraw(cmd, 6, 1, 0, 0);
}

static void
draw_text_single(struct server_vk *vk, VkCommandBuffer cmd, struct vk_text *text) {
    if (!text->enabled || !text->font || text->vertex_count == 0) {
        return;
    }

    struct vk_push_constants pc = {
        .src_size = { (float)text->font->atlas_width, (float)text->font->atlas_height },
        .dst_size = { (float)vk->swapchain.extent.width, (float)vk->swapchain.extent.height },
    };
    vkCmdPushConstants(cmd, vk->text_vk_pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(pc), &pc);

    // Bind font atlas descriptor
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk->text_vk_pipeline.layout, 0, 1,
                            &text->font->atlas_descriptor, 0, NULL);

    // Bind vertex buffer
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &text->vertex_buffer, &offset);

    // Set viewport to cover entire screen
    VkViewport viewport = {
        .x = 0, .y = 0,
        .width = (float)vk->swapchain.extent.width,
        .height = (float)vk->swapchain.extent.height,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = vk->swapchain.extent,
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdDraw(cmd, text->vertex_count, 1, 0, 0);
}

static void
draw_view_single(struct server_vk *vk, VkCommandBuffer cmd, struct vk_view *view) {
    vk_log(LOG_INFO, "draw_view_single: view=%p, enabled=%d, buffer=%p", 
           (void*)view, view->enabled, (void*)view->current_buffer);
    if (!view->enabled || !view->current_buffer) {
        vk_log(LOG_INFO, "draw_view_single: skipping (not ready)");
        return;
    }

    struct vk_buffer *buf = view->current_buffer;

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vk->quad_vertex_buffer, &offset);

    VkViewport viewport = {
        .x = (float)view->dst.x,
        .y = (float)view->dst.y,
        .width = (float)view->dst.width,
        .height = (float)view->dst.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = vk->swapchain.extent,
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Use buffer_blit path for cross-GPU with stride mismatch (NATIVE path)
    if (buf->storage_buffer && buf->buffer_descriptor_set) {
        vk_log(LOG_INFO, "draw_view_single: using buffer_blit path, width=%d height=%d stride=%u",
               buf->width, buf->height, buf->stride);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->buffer_blit.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vk->buffer_blit.layout, 0, 1,
                                &buf->buffer_descriptor_set, 0, NULL);

        // Buffer blit uses stride-aware sampling via push constants
        struct {
            int32_t width;
            int32_t height;
            int32_t stride;
            int32_t swap_colors;
            int32_t src_x;
            int32_t src_y;
            int32_t src_w;
            int32_t src_h;
        } pc = {
            .width = buf->width,
            .height = buf->height,
            .stride = (int32_t)buf->stride,
            .swap_colors = vk->dual_gpu ? 1 : 0,  // Try swapping for dual GPU
            .src_x = 0,
            .src_y = 0,
            .src_w = buf->width,
            .src_h = buf->height,
        };

        vkCmdPushConstants(cmd, vk->buffer_blit.layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    } else if (buf->descriptor_set) {
        // Fallback to image path (no stride mismatch or single GPU)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->blit_pipeline.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vk->blit_pipeline.layout, 0, 1,
                                &buf->descriptor_set, 0, NULL);

        // Set push constants for blit shader
        int32_t swap_colors = vk->dual_gpu ? 1 : 0;
        vkCmdPushConstants(cmd, vk->blit_pipeline.layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(int32_t), &swap_colors);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    } else {
        vk_log(LOG_WARN, "draw_view_single: no valid descriptor set");
    }
}

static void
draw_sorted_objects(struct server_vk *vk, VkCommandBuffer cmd) {
    // Collect all enabled objects
    size_t count = 0;
    struct vk_mirror *m;
    wl_list_for_each(m, &vk->mirrors, link) { if (m->enabled) count++; }
    struct vk_image *i;
    wl_list_for_each(i, &vk->images, link) { if (i->enabled) count++; }
    struct vk_text *t;
    wl_list_for_each(t, &vk->texts, link) { if (t->enabled) count++; }
    struct vk_view *v;
    wl_list_for_each(v, &vk->views, link) { if (v->enabled && v->current_buffer) count++; }

    if (count == 0) return;

    struct render_item *items = malloc(count * sizeof(*items));
    if (!items) return;

    size_t idx = 0;
    wl_list_for_each(m, &vk->mirrors, link) {
        if (m->enabled) items[idx++] = (struct render_item){ .depth = m->depth, .type = ITEM_MIRROR, .obj = m };
    }
    wl_list_for_each(i, &vk->images, link) {
        if (i->enabled) items[idx++] = (struct render_item){ .depth = i->depth, .type = ITEM_IMAGE, .obj = i };
    }
    wl_list_for_each(t, &vk->texts, link) {
        if (t->enabled) items[idx++] = (struct render_item){ .depth = t->depth, .type = ITEM_TEXT, .obj = t };
    }
    wl_list_for_each(v, &vk->views, link) {
        if (v->enabled && v->current_buffer) items[idx++] = (struct render_item){ .depth = v->depth, .type = ITEM_VIEW, .obj = v };
    }

    qsort(items, count, sizeof(*items), compare_render_items);

    // Draw sorted items
    VkPipeline last_pipeline = VK_NULL_HANDLE;
    struct vk_buffer *capture = vk->capture.current;

    for (size_t k = 0; k < count; k++) {
        struct render_item *item = &items[k];
        switch (item->type) {
        case ITEM_MIRROR:
            if (last_pipeline != vk->mirror_pipeline.pipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->mirror_pipeline.pipeline);
                // Mirrors rely on capture buffer descriptor
                if (capture && capture->storage_buffer && capture->buffer_descriptor_set) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            vk->mirror_pipeline.layout, 0, 1,
                                            &capture->buffer_descriptor_set, 0, NULL);
                }
                // Bind quad vertex buffer (mirrors use shared quad)
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &vk->quad_vertex_buffer, &offset);
                last_pipeline = vk->mirror_pipeline.pipeline;
            }
            draw_mirror_single(vk, cmd, item->obj);
            break;

        case ITEM_IMAGE:
            if (last_pipeline != vk->image_pipeline.pipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->image_pipeline.pipeline);
                last_pipeline = vk->image_pipeline.pipeline;
            }
            draw_image_single(vk, cmd, item->obj);
            break;

        case ITEM_TEXT:
            if (last_pipeline != vk->text_vk_pipeline.pipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->text_vk_pipeline.pipeline);
                last_pipeline = vk->text_vk_pipeline.pipeline;
            }
            draw_text_single(vk, cmd, item->obj);
            break;

        case ITEM_VIEW:
            draw_view_single(vk, cmd, item->obj);
            last_pipeline = VK_NULL_HANDLE;
            break;
        }
    }

    free(items);
}

bool
server_vk_begin_frame(struct server_vk *vk) {
    struct vk_buffer *capture = vk->capture.current;
    bool has_capture = false;
    if (capture) {
        bool has_buffer_path = capture->storage_buffer && capture->buffer_descriptor_set;
        bool has_image_path = capture->descriptor_set != VK_NULL_HANDLE;
        has_capture = has_buffer_path || has_image_path;

        // Try to swap optimal buffers if async copy is ready.
        if (vk->async_pipelining_enabled && capture->async_optimal_valid) {
            try_swap_optimal_buffers(vk, capture);
        }
    }

    // If there is no capture buffer, we can still render overlays (proxy_game mode).
    bool has_anything = has_capture;
    if (!has_anything) {
        struct vk_image *img;
        wl_list_for_each(img, &vk->images, link) {
            if (img->enabled) {
                has_anything = true;
                break;
            }
        }
    }
    if (!has_anything) {
        struct vk_text *txt;
        wl_list_for_each(txt, &vk->texts, link) {
            if (txt->enabled) {
                has_anything = true;
                break;
            }
        }
    }
    if (!has_anything) {
        struct vk_view *v;
        wl_list_for_each(v, &vk->views, link) {
            if (v->enabled && v->current_buffer) {
                has_anything = true;
                break;
            }
        }
    }
    if (!has_anything) {
        return false;
    }

    // Wait for the previous frame on this slot to finish (avoid dropping frames)
    vkWaitForFences(vk->device, 1, &vk->in_flight[vk->current_frame], VK_TRUE, UINT64_MAX);
    // vkResetFences moved to after acquire

    // Acquire next swapchain image (non-blocking)
    VkResult result = vkAcquireNextImageKHR(vk->device, vk->swapchain.swapchain, 0,
                          vk->image_available[vk->current_frame], VK_NULL_HANDLE,
                          &vk->current_image_index);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return false;
    }

    vkResetFences(vk->device, 1, &vk->in_flight[vk->current_frame]);

    // Reset and begin command buffer
    VkCommandBuffer cmd = vk->command_buffers[vk->current_frame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    vkBeginCommandBuffer(cmd, &begin_info);

    // Perform dma-buf sync and image/buffer transition for captured buffer
    if (has_capture && vk->capture.current && vk->capture.current->dmabuf_fd >= 0) {
        // Kernel-level sync: wait for Intel GPU to finish writing
        // dmabuf_sync_start_read(vk->capture.current->dmabuf_fd);

        // GPU-level sync: transition from external GPU to our queue family
        if (vk->capture.current->storage_buffer && vk->capture.current->buffer_descriptor_set) {
            acquire_imported_buffer(vk, vk->capture.current->storage_buffer, cmd);
        } else {
            transition_imported_image(vk, vk->capture.current->image, cmd);
        }
    }

    // Begin render pass with transparent clear color (for background visibility)
    VkClearValue clear_value = { .color = {{ 0.0f, 0.0f, 0.0f, 0.0f }} };

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

    // Draw the captured frame centered in window (Game Background)
    if (has_capture) {
        draw_captured_frame(vk, cmd);
    }

    // Draw all overlays sorted by depth (Mirrors, Images, Text)
    draw_sorted_objects(vk, cmd);

    return true;
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
        // dmabuf_sync_end_read(vk->capture.current->dmabuf_fd);
    }

    // Explicit sync (timeline semaphore)
    VkSemaphore wait_semaphores[2];
    uint64_t wait_values[2] = {0, 0};
    VkPipelineStageFlags wait_stages[2] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT };
    uint32_t wait_count = 1;

    wait_semaphores[0] = vk->image_available[vk->current_frame];

    if (!vk->disable_capture_sync_wait &&
        pfn_vkImportSemaphoreFdKHR && vk->capture.surface && vk->capture.surface->syncobj) {
        struct server_drm_syncobj_surface *sync = vk->capture.surface->syncobj;
        if (sync->acquire.fd != -1) {
            if (sync->vk_sem == VK_NULL_HANDLE) {
                VkSemaphoreTypeCreateInfo type_info = {
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                    .initialValue = 0,
                };
                VkSemaphoreCreateInfo info = {
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                    .pNext = &type_info,
                };
                vkCreateSemaphore(vk->device, &info, NULL, &sync->vk_sem);
            }

            if (sync->imported_fd != sync->acquire.fd) {
                int fd_dup = dup(sync->acquire.fd);
                VkImportSemaphoreFdInfoKHR import = {
                    .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
                    .semaphore = sync->vk_sem,
                    .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
                    .fd = fd_dup,
                };
                if (pfn_vkImportSemaphoreFdKHR(vk->device, &import) == VK_SUCCESS) {
                    sync->imported_fd = sync->acquire.fd;
                } else {
                    close(fd_dup);
                }
            }

            if (sync->imported_fd == sync->acquire.fd) {
                wait_semaphores[wait_count] = sync->vk_sem;
                wait_values[wait_count] = ((uint64_t)sync->acquire.point_hi << 32) | sync->acquire.point_lo;
                wait_count++;
            }
        }
    }

    VkTimelineSemaphoreSubmitInfo timeline_info = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = wait_count,
        .pWaitSemaphoreValues = wait_values,
    };
    if (vk->disable_capture_sync_wait && wait_count == 1) {
        // Optional warning when explicit sync is disabled (dual-GPU tear risk)
        static bool warned = false;
        if (!warned) {
            vk_log(LOG_WARN, "capture sync wait disabled (WAYWALL_DISABLE_CAPTURE_SYNC_WAIT set) - may improve FPS but risk tearing");
            warned = true;
        }
    }

    VkSemaphore signal_semaphores[2];
    uint64_t signal_values[2] = {0, 0};
    uint32_t signal_count = 1;

    signal_semaphores[0] = vk->render_finished[vk->current_frame];

    // Handle explicit release (Signal)
    if (!vk->disable_capture_sync_wait &&
        pfn_vkImportSemaphoreFdKHR && vk->capture.surface && vk->capture.surface->syncobj) {
        struct server_drm_syncobj_surface *sync = vk->capture.surface->syncobj;
        if (sync->release.fd != -1) {
            if (sync->vk_sem_release == VK_NULL_HANDLE) {
                VkSemaphoreTypeCreateInfo type_info = {
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                    .initialValue = 0,
                };
                VkSemaphoreCreateInfo info = {
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                    .pNext = &type_info,
                };
                vkCreateSemaphore(vk->device, &info, NULL, &sync->vk_sem_release);
            }

            if (sync->imported_release_fd != sync->release.fd) {
                int fd_dup = dup(sync->release.fd);
                VkImportSemaphoreFdInfoKHR import = {
                    .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
                    .semaphore = sync->vk_sem_release,
                    .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
                    .fd = fd_dup,
                };
                if (pfn_vkImportSemaphoreFdKHR(vk->device, &import) == VK_SUCCESS) {
                    sync->imported_release_fd = sync->release.fd;
                } else {
                    close(fd_dup);
                }
            }

            if (sync->imported_release_fd == sync->release.fd) {
                signal_semaphores[signal_count] = sync->vk_sem_release;
                signal_values[signal_count] = ((uint64_t)sync->release.point_hi << 32) | sync->release.point_lo;
                signal_count++;
            }
        }
    }

    timeline_info.signalSemaphoreValueCount = signal_count;
    timeline_info.pSignalSemaphoreValues = signal_values;

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline_info,
        .waitSemaphoreCount = wait_count,
        .pWaitSemaphores = wait_semaphores,
        .pWaitDstStageMask = wait_stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = signal_count,
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

    // FPS logging (every 100 ms)
    vk->fps_frame_count++;
    uint64_t now = now_ms();
    uint64_t delta = now - vk->fps_last_time_ms;
    if (delta >= 100) {
        double fps = (double)vk->fps_frame_count * 1000.0 / (double)delta;
        int32_t cap_w = 0, cap_h = 0;
        server_vk_get_capture_size(vk, &cap_w, &cap_h);
        vk_log(LOG_INFO, "FPS: %.1f (capture=%dx%d, swap=%ux%u)",
               fps, cap_w, cap_h, vk->swapchain.extent.width, vk->swapchain.extent.height);
        vk->fps_frame_count = 0;
        vk->fps_last_time_ms = now;
    }

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

    for (uint32_t i = 0; i < buffer->export_count; i++) {
        if (buffer->export_images[i]) {
            vkDestroyImage(vk->device, buffer->export_images[i], NULL);
            buffer->export_images[i] = VK_NULL_HANDLE;
        }
        if (buffer->export_memories[i]) {
            vkFreeMemory(vk->device, buffer->export_memories[i], NULL);
            buffer->export_memories[i] = VK_NULL_HANDLE;
        }
    }

    // Clean up async double-buffered optimal first (if used)
    // This must happen before freeing descriptor_set to avoid double-free
    // since descriptor_set may point to one of optimal_descriptors[]
    if (buffer->async_optimal_valid) {
        destroy_double_buffered_optimal(vk, buffer);
        // descriptor_set was pointing to optimal_descriptors, now freed
        buffer->descriptor_set = VK_NULL_HANDLE;
    }

    if (buffer->descriptor_set) {
        vkFreeDescriptorSets(vk->device, vk->descriptor_pool, 1, &buffer->descriptor_set);
    }
    if (buffer->buffer_descriptor_set) {
        vkFreeDescriptorSets(vk->device, vk->descriptor_pool, 1, &buffer->buffer_descriptor_set);
    }
    // Clean up legacy optimal copy path
    destroy_optimal_copy(vk, buffer);
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
        // Remove parent destroy listener if active
        if (buffer->on_parent_destroy.link.prev || buffer->on_parent_destroy.link.next) {
            wl_list_remove(&buffer->on_parent_destroy.link);
        }
        server_buffer_unref(buffer->parent);
    }

    if (buffer->link.prev || buffer->link.next) {
        wl_list_remove(&buffer->link);
    }
    free(buffer);
}

static bool
vk_import_dmabuf_image(struct server_vk *vk, int32_t width, int32_t height, uint32_t drm_format,
                       uint32_t stride, uint32_t offset, uint64_t modifier, int fd,
                       VkImageUsageFlags usage, VkImage *out_image, VkDeviceMemory *out_memory,
                       bool *out_prepared) {
    *out_image = VK_NULL_HANDLE;
    *out_memory = VK_NULL_HANDLE;
    *out_prepared = false;

    VkFormat format = drm_format_to_vk(drm_format);
    if (format == VK_FORMAT_UNDEFINED) {
        vk_log(LOG_ERROR, "unsupported DRM format for dmabuf image: 0x%x", drm_format);
        return false;
    }

    bool use_modifier_path = modifier != DRM_FORMAT_MOD_INVALID && modifier != DRM_FORMAT_MOD_LINEAR;

    VkResult result;
    if (use_modifier_path) {
        VkSubresourceLayout plane_layout = {
            .offset = offset,
            .rowPitch = stride,
        };

        VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .drmFormatModifier = modifier,
            .drmFormatModifierPlaneCount = 1,
            .pPlaneLayouts = &plane_layout,
        };

        VkExternalMemoryImageCreateInfo ext_info_mod = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = &mod_info,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };

        VkImageCreateInfo image_info_mod = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &ext_info_mod,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = { (uint32_t)width, (uint32_t)height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        result = vkCreateImage(vk->device, &image_info_mod, NULL, out_image);
        if (result != VK_SUCCESS) {
            vk_log(LOG_ERROR, "failed to create dmabuf VkImage (modifier path): %d", result);
            return false;
        }
    } else {
        VkExternalMemoryImageCreateInfo ext_info = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };

        VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &ext_info,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = { (uint32_t)width, (uint32_t)height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_LINEAR,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
        };

        result = vkCreateImage(vk->device, &image_info, NULL, out_image);
        if (result != VK_SUCCESS) {
            vk_log(LOG_ERROR, "failed to create dmabuf VkImage (linear): %d", result);
            return false;
        }

        VkImageSubresource subres = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .arrayLayer = 0,
        };
        VkSubresourceLayout vk_layout;
        vkGetImageSubresourceLayout(vk->device, *out_image, &subres, &vk_layout);

        if (vk_layout.rowPitch != stride) {
            vkDestroyImage(vk->device, *out_image, NULL);
            *out_image = VK_NULL_HANDLE;

            uint32_t effective_width = stride / 4;
            image_info.extent.width = effective_width;

            result = vkCreateImage(vk->device, &image_info, NULL, out_image);
            if (result != VK_SUCCESS) {
                vk_log(LOG_ERROR, "failed to create stride-adjusted dmabuf VkImage: %d", result);
                return false;
            }
        }
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vk->device, *out_image, &mem_reqs);

    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = dup(fd),
    };
    if (import_info.fd < 0) {
        vk_log(LOG_ERROR, "failed to dup dmabuf fd");
        vkDestroyImage(vk->device, *out_image, NULL);
        *out_image = VK_NULL_HANDLE;
        return false;
    }

    uint32_t memory_type = find_memory_type(vk, mem_reqs.memoryTypeBits, 0);
    if (memory_type == UINT32_MAX) {
        vk_log(LOG_ERROR, "no suitable memory type for dmabuf image");
        close(import_info.fd);
        vkDestroyImage(vk->device, *out_image, NULL);
        *out_image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = memory_type,
    };

    result = vkAllocateMemory(vk->device, &alloc_info, NULL, out_memory);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to allocate dmabuf memory: %d", result);
        close(import_info.fd);
        vkDestroyImage(vk->device, *out_image, NULL);
        *out_image = VK_NULL_HANDLE;
        return false;
    }

    result = vkBindImageMemory(vk->device, *out_image, *out_memory, 0);
    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to bind dmabuf image memory: %d", result);
        vkFreeMemory(vk->device, *out_memory, NULL);
        *out_memory = VK_NULL_HANDLE;
        vkDestroyImage(vk->device, *out_image, NULL);
        *out_image = VK_NULL_HANDLE;
        return false;
    }

    return true;
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

static void
on_parent_buffer_destroy(struct wl_listener *listener, void *data) {
    struct vk_buffer *vk_buf = wl_container_of(listener, vk_buf, on_parent_destroy);
    struct server_vk *vk = vk_buf->vk;

    destroy_optimal_copy(vk, vk_buf);

    // If this is the current buffer, clear the reference
    if (vk->capture.current == vk_buf) {
        vk->capture.current = NULL;
    }

    // Remove the listener before destroying (parent is already being destroyed)
    wl_list_remove(&vk_buf->on_parent_destroy.link);
    vk_buf->parent = NULL;  // Parent is being destroyed, don't try to unref

    vk_buffer_destroy(vk_buf);
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
    vk_buffer->source_prepared = false;

    if (data->proxy_export && data->export_count > 0) {
        vk_buffer->export_count = data->export_count;
        if (vk_buffer->export_count > DMABUF_EXPORT_MAX) {
            vk_buffer->export_count = DMABUF_EXPORT_MAX;
        }

        for (uint32_t i = 0; i < vk_buffer->export_count; i++) {
            uint64_t exp_mod = ((uint64_t)data->exports[i].modifier_hi << 32) | data->exports[i].modifier_lo;
            if (!vk_import_dmabuf_image(vk, data->width, data->height, data->format,
                                        data->exports[i].stride, data->exports[i].offset, exp_mod,
                                        data->exports[i].fd,
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                        &vk_buffer->export_images[i], &vk_buffer->export_memories[i],
                                        &vk_buffer->export_prepared[i])) {
                vk_log(LOG_ERROR, "failed to import proxy export target %u", i);
                goto fail;
            }
        }
    }

    uint64_t modifier = ((uint64_t)data->modifier_hi << 32) | data->modifier_lo;
    bool allow_modifiers = vk->allow_modifiers || getenv("WAYWALL_DMABUF_ALLOW_MODIFIERS") != NULL;
    bool use_modifier_path = allow_modifiers &&
                             data->num_planes == 1 &&
                             modifier != DRM_FORMAT_MOD_INVALID &&
                             modifier != DRM_FORMAT_MOD_LINEAR;

    // ------------------------------------------------------------------------
    // Modifier-based import (tiled) when allowed and modifier is non-linear.
    // ------------------------------------------------------------------------
    if (use_modifier_path) {
        VkSubresourceLayout plane_layout = {
            .offset = data->planes[0].offset,
            .size = 0,
            .rowPitch = data->planes[0].stride,
            .arrayPitch = 0,
            .depthPitch = 0,
        };

        VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .drmFormatModifier = modifier,
            .drmFormatModifierPlaneCount = 1,
            .pPlaneLayouts = &plane_layout,
        };

        VkExternalMemoryImageCreateInfo ext_info_mod = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = &mod_info,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };

        VkImageCreateInfo image_info_mod = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &ext_info_mod,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = { data->width, data->height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        vk_log(LOG_INFO, "MODIFIER dma-buf import: %dx%d, stride=%d, modifier=0x%"PRIx64", format=0x%x",
               data->width, data->height, data->planes[0].stride, modifier, data->format);

        VkResult result = vkCreateImage(vk->device, &image_info_mod, NULL, &vk_buffer->image);
        if (result == VK_SUCCESS) {
            // Import dma-buf memory
            VkMemoryFdPropertiesKHR fd_props = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
            };

            PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHR =
                (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(vk->device, "vkGetMemoryFdPropertiesKHR");

            int fd_dup = dup(vk_buffer->dmabuf_fd);

            result = vkGetMemoryFdPropertiesKHR(vk->device,
                                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                                fd_dup, &fd_props);
            if (result != VK_SUCCESS) {
                vk_log(LOG_ERROR, "failed to get dma-buf memory properties: %d", result);
                close(fd_dup);
                goto fail;
            }

            VkMemoryRequirements mem_reqs;
            vkGetImageMemoryRequirements(vk->device, vk_buffer->image, &mem_reqs);

            // Dedicated allocation for imported images
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

            uint32_t compatible_bits = mem_reqs.memoryTypeBits & fd_props.memoryTypeBits;
            uint32_t mem_type_index = find_memory_type(vk, compatible_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mem_type_index == UINT32_MAX) {
                mem_type_index = find_memory_type(vk, compatible_bits, 0);
            }
            if (mem_type_index == UINT32_MAX && fd_props.memoryTypeBits != 0) {
                mem_type_index = find_memory_type(vk, fd_props.memoryTypeBits, 0);
            }

            VkMemoryAllocateInfo alloc_info = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &import_info,
                .allocationSize = mem_reqs.size,
                .memoryTypeIndex = mem_type_index,
            };

            if (alloc_info.memoryTypeIndex == UINT32_MAX) {
                vk_log(LOG_ERROR, "no suitable memory type for dma-buf import (modifier path)");
                close(fd_dup);
                goto fail;
            }

            result = vkAllocateMemory(vk->device, &alloc_info, NULL, &vk_buffer->memory);
            if (result != VK_SUCCESS) {
                vk_log(LOG_ERROR, "failed to allocate memory for dma-buf (modifier path): %d", result);
                goto fail;
            }

            result = vkBindImageMemory(vk->device, vk_buffer->image, vk_buffer->memory, 0);
            if (result != VK_SUCCESS) {
                vk_log(LOG_ERROR, "failed to bind dma-buf memory (modifier path): %d", result);
                goto fail;
            }

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
                vk_log(LOG_ERROR, "failed to create image view for dma-buf (modifier path): %d", result);
                goto fail;
            }

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
                vk_log(LOG_ERROR, "failed to allocate descriptor set (modifier path): %d", result);
                goto fail;
            }

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

            vk_log(LOG_INFO, "imported dma-buf (modifier path): %dx%d, format=0x%x, modifier=0x%llx",
                   data->width, data->height, data->format, (unsigned long long)modifier);

            // Listen for parent buffer destruction to clean up
            vk_buffer->on_parent_destroy.notify = on_parent_buffer_destroy;
            wl_signal_add(&buffer->events.resource_destroy, &vk_buffer->on_parent_destroy);

            wl_list_insert(&vk->capture.buffers, &vk_buffer->link);
            return vk_buffer;
        } else {
            vk_log(LOG_WARN, "modifier import failed (%d), falling back to LINEAR path", result);
            vkDestroyImage(vk->device, vk_buffer->image, NULL);
            vk_buffer->image = VK_NULL_HANDLE;
        }
    }

    // ------------------------------------------------------------------------
    // Legacy LINEAR import path (stride fix + optional storage buffer).
    // ------------------------------------------------------------------------

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

    // Attempt to create optimal-tiling copy to avoid linear peer-read throttling
    if (!use_modifier_path && vk_buffer->view) {
        VkDescriptorSet linear_desc = vk_buffer->descriptor_set;

        // Use async pipelined double-buffering if enabled, otherwise use synchronous copy
        if (vk->async_pipelining_enabled) {
            if (create_double_buffered_optimal(vk, vk_buffer)) {
                // Use the first buffer's descriptor initially
                vk_buffer->descriptor_set = vk_buffer->optimal_descriptors[vk_buffer->optimal_read_index];
                vk_log(LOG_INFO, "created double-buffered optimal for async pipelining");
                // Free linear descriptor set to avoid leaks
                if (linear_desc) {
                    vkFreeDescriptorSets(vk->device, vk->descriptor_pool, 1, &linear_desc);
                }

                // Perform initial synchronous copy to populate first buffer
                // We'll do a simple blocking copy here to get the first frame ready
                VkCommandBufferAllocateInfo alloc_info_init = {
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                    .commandPool = vk->command_pool,
                    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                    .commandBufferCount = 1,
                };
                VkCommandBuffer init_cmd = VK_NULL_HANDLE;
                if (vkAllocateCommandBuffers(vk->device, &alloc_info_init, &init_cmd) == VK_SUCCESS) {
                    VkCommandBufferBeginInfo begin_info_init = {
                        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                    };
                    vkBeginCommandBuffer(init_cmd, &begin_info_init);

                    // Copy to first optimal buffer
                    VkImageMemoryBarrier init_barriers[2] = {
                        {
                            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                            .srcAccessMask = 0,
                            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                            .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
                            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .image = vk_buffer->image,
                            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
                        },
                        {
                            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                            .srcAccessMask = 0,
                            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .image = vk_buffer->optimal_images[0],
                            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
                        },
                    };
                    vkCmdPipelineBarrier(init_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, init_barriers);

                    VkImageCopy init_region = {
                        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffset = {0, 0, 0},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffset = {0, 0, 0},
                        .extent = {(uint32_t)vk_buffer->width, (uint32_t)vk_buffer->height, 1},
                    };
                    vkCmdCopyImage(init_cmd, vk_buffer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   vk_buffer->optimal_images[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &init_region);

                    VkImageMemoryBarrier init_post = {
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = vk_buffer->optimal_images[0],
                        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
                    };
                    vkCmdPipelineBarrier(init_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &init_post);

                    vkEndCommandBuffer(init_cmd);
                    VkSubmitInfo init_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &init_cmd};
                    vkQueueSubmit(vk->graphics_queue, 1, &init_submit, VK_NULL_HANDLE);
                    vkQueueWaitIdle(vk->graphics_queue);
                    vkFreeCommandBuffers(vk->device, vk->command_pool, 1, &init_cmd);
                    vk_log(LOG_INFO, "initial sync copy to optimal buffer complete");
                }
            } else {
                vk_buffer->descriptor_set = linear_desc;
            }
        } else {
            // Legacy synchronous optimal copy path
            if (create_optimal_copy(vk, vk_buffer)) {
                // Record descriptor set for optimal view
                VkDescriptorSetAllocateInfo desc_alloc_info_opt = {
                    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                    .descriptorPool = vk->descriptor_pool,
                    .descriptorSetCount = 1,
                    .pSetLayouts = &vk->blit_pipeline.descriptor_layout,
                };

                VkDescriptorSet opt_desc = VK_NULL_HANDLE;
                if (vkAllocateDescriptorSets(vk->device, &desc_alloc_info_opt, &opt_desc) == VK_SUCCESS) {
                    VkDescriptorImageInfo opt_image_desc = {
                        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        .imageView = vk_buffer->optimal_view,
                        .sampler = vk->sampler,
                    };

                    VkWriteDescriptorSet opt_write = {
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = opt_desc,
                        .dstBinding = 0,
                        .dstArrayElement = 0,
                        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        .descriptorCount = 1,
                        .pImageInfo = &opt_image_desc,
                    };

                    vkUpdateDescriptorSets(vk->device, 1, &opt_write, 0, NULL);

                    if (copy_to_optimal(vk, vk_buffer)) {
                        // Prefer optimal descriptor
                        vk_buffer->descriptor_set = opt_desc;
                        vk_log(LOG_INFO, "created optimal-tiling copy for dma-buf import");
                        // Free linear descriptor set to avoid leaks
                        if (linear_desc) {
                            vkFreeDescriptorSets(vk->device, vk->descriptor_pool, 1, &linear_desc);
                        }
                    } else {
                        // Copy failed, keep linear descriptor
                        vkFreeDescriptorSets(vk->device, vk->descriptor_pool, 1, &opt_desc);
                        destroy_optimal_copy(vk, vk_buffer);
                        vk_buffer->descriptor_set = linear_desc;
                    }
                } else {
                    destroy_optimal_copy(vk, vk_buffer);
                    vk_buffer->descriptor_set = linear_desc;
                }
            } else {
                vk_buffer->descriptor_set = linear_desc;
            }
        }
    }

    // Listen for parent buffer destruction to clean up
    vk_buffer->on_parent_destroy.notify = on_parent_buffer_destroy;
    wl_signal_add(&buffer->events.resource_destroy, &vk_buffer->on_parent_destroy);

    wl_list_insert(&vk->capture.buffers, &vk_buffer->link);
    return vk_buffer;

fail:
    vk_buffer_destroy(vk_buffer);
    return NULL;
}

// ============================================================================
// Event Handlers
// ============================================================================

static bool
vk_proxy_copy_to_export(struct server_vk *vk, struct vk_buffer *src, struct server_dmabuf_data *data,
                        uint32_t export_index) {
    if (export_index >= src->export_count || export_index >= data->export_count) {
        return false;
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint32_t slot = UINT32_MAX;

    for (uint32_t attempt = 0; attempt < DMABUF_EXPORT_MAX; attempt++) {
        uint32_t idx = (vk->proxy_copy.index + attempt) % DMABUF_EXPORT_MAX;
        if (vkGetFenceStatus(vk->device, vk->proxy_copy.fences[idx]) == VK_SUCCESS) {
            slot = idx;
            break;
        }
    }
    if (slot == UINT32_MAX) {
        vk_log(LOG_WARN, "proxy copy: no available command slot (dropping frame)");
        return false;
    }

    vk->proxy_copy.index = (slot + 1) % DMABUF_EXPORT_MAX;
    cmd = vk->proxy_copy.command_buffers[slot];
    fence = vk->proxy_copy.fences[slot];

    vkResetFences(vk->device, 1, &fence);
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "proxy copy: vkBeginCommandBuffer failed");
        return false;
    }

    VkImageMemoryBarrier barriers[2] = {0};

    barriers[0] = (VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src->source_prepared ? VK_ACCESS_TRANSFER_READ_BIT : 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = src->source_prepared ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = src->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    barriers[1] = (VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src->export_prepared[export_index] ? VK_ACCESS_TRANSFER_WRITE_BIT : 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = src->export_prepared[export_index] ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = src->export_images[export_index],
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 2, barriers);

    VkImageCopy copy = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .extent = { (uint32_t)data->width, (uint32_t)data->height, 1 },
    };
    vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL,
                   src->export_images[export_index], VK_IMAGE_LAYOUT_GENERAL, 1, &copy);

    VkImageMemoryBarrier dst_release = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = src->export_images[export_index],
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, NULL, 0, NULL, 1, &dst_release);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "proxy copy: vkEndCommandBuffer failed");
        return false;
    }

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };

    VkResult res = vkQueueSubmit(vk->graphics_queue, 1, &submit, fence);
    if (res != VK_SUCCESS) {
        vk_log(LOG_ERROR, "proxy copy: vkQueueSubmit failed: %d", res);
        return false;
    }

    // Ensure the export dmabuf contents are complete before Wayland commits the wl_buffer.
    // This avoids presenting partially-copied frames when the host compositor samples it.
    // This wait is CPU-blocking, but it avoids the swapchain-present timing stall that caps FPS.
    res = vkWaitForFences(vk->device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (res != VK_SUCCESS) {
        vk_log(LOG_ERROR, "proxy copy: vkWaitForFences failed: %d", res);
        return false;
    }

    src->source_prepared = true;
    src->export_prepared[export_index] = true;
    return true;
}

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

        if (vk->proxy_game) {
            struct server_dmabuf_data *data = buffer->data;
            if (data && data->proxy_export && data->export_count > 0) {
                uint32_t export_index = 0;
                bool found = false;
                for (uint32_t i = 0; i < data->export_count && i < DMABUF_EXPORT_MAX; i++) {
                    if (!data->exports[i].busy) {
                        export_index = i;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    export_index = 0;
                }

                data->exports[export_index].busy = true;
                buffer->remote = data->exports[export_index].remote;
                vk_buf->export_index = export_index;
                (void)vk_proxy_copy_to_export(vk, vk_buf, data, export_index);
            }

            wl_signal_emit_mutable(&vk->events.frame, NULL);
            return;
        }

        // Start async copy to optimal if pipelining is enabled
        if (vk->async_pipelining_enabled && vk_buf->async_optimal_valid) {
            start_async_copy_to_optimal(vk, vk_buf);
        }

        // Advance animated overlays (e.g., AVIF emotes) on frame ticks.
        vk_update_animated_images(vk);

        // Update all floating view buffers before rendering
        struct vk_view *v;
        wl_list_for_each(v, &vk->views, link) {
            if (v->view && v->view->surface) {
                struct server_buffer *view_buffer = server_surface_next_buffer(v->view->surface);
                if (view_buffer) {
                    // Find or import the buffer
                    struct vk_buffer *vb = NULL;
                    struct vk_buffer *it;
                    wl_list_for_each(it, &vk->capture.buffers, link) {
                        if (it->parent == view_buffer) {
                            vb = it;
                            break;
                        }
                    }
                    if (!vb) {
                        vb = vk_buffer_import(vk, view_buffer);
                        if (vb) {
                            vk_log(LOG_INFO, "imported floating view buffer: %dx%d", vb->width, vb->height);
                        }
                    }
                    if (vb && v->current_buffer != vb) {
                        v->current_buffer = vb;
                        // Update geometry from buffer dimensions
                        // Position at top-left corner (no margin)
                        // TODO: Read anchor from config
                        v->dst.x = 0;
                        v->dst.y = 0;
                        v->dst.width = vb->width;
                        v->dst.height = vb->height;
                        vk_log(LOG_INFO, "view buffer updated: pos=(%d,%d) size=(%d,%d)", 
                               v->dst.x, v->dst.y, v->dst.width, v->dst.height);
                    }
                }
            }
        }

        // Render frame with Vulkan
        if (server_vk_begin_frame(vk)) {
            server_vk_end_frame(vk);
        }
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
cleanup_swapchain(struct server_vk *vk, bool destroy_swapchain) {
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
    if (destroy_swapchain && vk->swapchain.swapchain) {
        vkDestroySwapchainKHR(vk->device, vk->swapchain.swapchain, NULL);
        vk->swapchain.swapchain = VK_NULL_HANDLE;
    }
}

static bool
recreate_swapchain(struct server_vk *vk, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return true;  // Skip if minimized
    }

    VkSwapchainKHR old_swapchain = vk->swapchain.swapchain;
    cleanup_swapchain(vk, false);

    if (!create_swapchain(vk, width, height, old_swapchain)) {
        vk_log(LOG_ERROR, "failed to recreate swapchain");
        return false;
    }

    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(vk->device, old_swapchain, NULL);
    }

    if (!create_framebuffers(vk)) {
        vk_log(LOG_ERROR, "failed to recreate framebuffers");
        return false;
    }

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

static void
on_ui_refresh(struct wl_listener *listener, void *data) {
    (void)data;
    struct server_vk *vk = wl_container_of(listener, vk, on_ui_refresh);
    vk->overlay_tick_ms = refresh_mhz_to_ms(vk->server->ui ? vk->server->ui->refresh_mhz : 0);

    if (vk->overlay_tick) {
        wl_event_source_timer_update(vk->overlay_tick, vk->overlay_tick_ms);
    }
}

static int
handle_overlay_tick(void *data) {
    struct server_vk *vk = data;

    if (server_vk_begin_frame(vk)) {
        server_vk_end_frame(vk);
    }

    if (vk->overlay_tick) {
        wl_event_source_timer_update(vk->overlay_tick, vk->overlay_tick_ms);
    }

    return 0;
}

// ============================================================================
// Mirror API
// ============================================================================

struct vk_mirror *
server_vk_add_mirror(struct server_vk *vk, const struct vk_mirror_options *options) {
    struct vk_mirror *mirror = zalloc(1, sizeof(*mirror));

    mirror->src = options->src;
    mirror->dst = options->dst;
    mirror->color_key_enabled = options->color_key_enabled;
    mirror->color_key_input = options->color_key_input;
    mirror->color_key_output = options->color_key_output;
    mirror->color_key_tolerance = options->color_key_tolerance > 0.0f ? options->color_key_tolerance : 0.1f;
    mirror->depth = options->depth;
    mirror->enabled = true;

    wl_list_insert(&vk->mirrors, &mirror->link);

    // Count total mirrors
    int count = 0;
    struct vk_mirror *m;
    wl_list_for_each(m, &vk->mirrors, link) count++;

    vk_log(LOG_INFO, "added mirror #%d: src(%d,%d %dx%d) -> dst(%d,%d %dx%d) color_key=%d",
           count,
           mirror->src.x, mirror->src.y, mirror->src.width, mirror->src.height,
           mirror->dst.x, mirror->dst.y, mirror->dst.width, mirror->dst.height,
           mirror->color_key_enabled);

    return mirror;
}

void
server_vk_remove_mirror(struct server_vk *vk, struct vk_mirror *mirror) {
    if (!mirror) return;

    vk_log(LOG_INFO, "removing mirror: src(%d,%d %dx%d)",
           mirror->src.x, mirror->src.y, mirror->src.width, mirror->src.height);

    wl_list_remove(&mirror->link);
    free(mirror);

    // Count remaining
    int count = 0;
    struct vk_mirror *m;
    wl_list_for_each(m, &vk->mirrors, link) count++;
    vk_log(LOG_INFO, "mirrors remaining: %d", count);
}

void
server_vk_mirror_set_enabled(struct vk_mirror *mirror, bool enabled) {
    if (mirror) {
        mirror->enabled = enabled;
    }
}

// ============================================================================
// Image API
// ============================================================================

static struct vk_image *
server_vk_add_rgba_image(struct server_vk *vk, const char *debug_name, uint32_t width, uint32_t height,
                         const unsigned char *rgba, const struct vk_image_options *options) {
    if (!rgba || width == 0 || height == 0) {
        return NULL;
    }

    struct vk_image *image = zalloc(1, sizeof(*image));
    image->width = (int32_t)width;
    image->height = (int32_t)height;
    image->dst = options->dst;
    image->depth = options->depth;
    image->enabled = true;
    image->owns_descriptor_set = true;
    image->owns_image = true;

    vk_log(LOG_INFO, "loading image: %s (%ux%u)", debug_name ? debug_name : "(raw)", width, height);

    VkImageCreateInfo image_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,  // For direct CPU access
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
    };

    if (vkCreateImage(vk->device, &image_ci, NULL, &image->image) != VK_SUCCESS) {
        free(image);
        return NULL;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vk->device, image->image, &mem_reqs);

    uint32_t mem_type = find_memory_type(vk, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type == UINT32_MAX) {
        vkDestroyImage(vk->device, image->image, NULL);
        free(image);
        return NULL;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };
    if (vkAllocateMemory(vk->device, &alloc_info, NULL, &image->memory) != VK_SUCCESS) {
        vkDestroyImage(vk->device, image->image, NULL);
        free(image);
        return NULL;
    }

    vkBindImageMemory(vk->device, image->image, image->memory, 0);

    void *mapped;
    if (vkMapMemory(vk->device, image->memory, 0, mem_reqs.size, 0, &mapped) != VK_SUCCESS) {
        vkFreeMemory(vk->device, image->memory, NULL);
        vkDestroyImage(vk->device, image->image, NULL);
        free(image);
        return NULL;
    }

    VkImageSubresource subres = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(vk->device, image->image, &subres, &layout);

    const unsigned char *src = rgba;
    unsigned char *dst = (unsigned char *)mapped + layout.offset;
    size_t src_row_size = (size_t)width * 4;

    for (uint32_t y = 0; y < height; y++) {
        memcpy(dst, src, src_row_size);
        src += src_row_size;
        dst += layout.rowPitch;
    }

    vkUnmapMemory(vk->device, image->memory);

    // Transition to shader-read layout
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cmd_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    vkAllocateCommandBuffers(vk->device, &cmd_alloc, &cmd);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &begin_info);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    vkQueueSubmit(vk->graphics_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk->graphics_queue);
    vkFreeCommandBuffers(vk->device, vk->command_pool, 1, &cmd);

    VkImageViewCreateInfo view_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    if (vkCreateImageView(vk->device, &view_ci, NULL, &image->view) != VK_SUCCESS) {
        vkFreeMemory(vk->device, image->memory, NULL);
        vkDestroyImage(vk->device, image->image, NULL);
        free(image);
        return NULL;
    }

    VkDescriptorSetAllocateInfo ds_alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = vk->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &vk->image_pipeline.descriptor_layout,
    };
    if (vkAllocateDescriptorSets(vk->device, &ds_alloc, &image->descriptor_set) != VK_SUCCESS) {
        vkDestroyImageView(vk->device, image->view, NULL);
        vkFreeMemory(vk->device, image->memory, NULL);
        vkDestroyImage(vk->device, image->image, NULL);
        free(image);
        return NULL;
    }

    VkDescriptorImageInfo img_info = {
        .sampler = vk->sampler,
        .imageView = image->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = image->descriptor_set,
        .dstBinding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &img_info,
    };
    vkUpdateDescriptorSets(vk->device, 1, &write, 0, NULL);

    wl_list_insert(&vk->images, &image->link);
    return image;
}

struct vk_atlas *
server_vk_create_atlas(struct server_vk *vk, uint32_t width, const char *rgba_data, size_t rgba_len) {
    if (!vk || width == 0) {
        return NULL;
    }

    const unsigned char *bytes = NULL;
    size_t pixel_len = 0;
    uint32_t height = 0;
    bool need_free = false;

    if (rgba_data && rgba_len > 0) {
        // waywall's atlas.raw format is:
        // - 8-byte header: uint32_le width, uint32_le height
        // - followed by width*height*4 bytes of RGBA data
        bytes = (const unsigned char *)rgba_data;
        pixel_len = rgba_len;
        if (rgba_len >= 8) {
            uint32_t header_w = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
                                ((uint32_t)bytes[3] << 24);
            uint32_t header_h = (uint32_t)bytes[4] | ((uint32_t)bytes[5] << 8) | ((uint32_t)bytes[6] << 16) |
                                ((uint32_t)bytes[7] << 24);

            size_t header_pixels = (size_t)header_w * (size_t)header_h * 4;
            if (header_w > 0 && header_h > 0 && rgba_len == header_pixels + 8) {
                // Prefer the header dimensions.
                if (width != header_w) {
                    vk_log(LOG_WARN, "atlas width param (%u) != header width (%u), using header", width, header_w);
                }
                width = header_w;
                pixel_len = header_pixels;
                bytes += 8;
            }
        }

        if (pixel_len % ((size_t)width * 4) != 0) {
            vk_log(LOG_ERROR, "atlas raw size mismatch: width=%u len=%zu", width, pixel_len);
            return NULL;
        }

        height = (uint32_t)(pixel_len / ((size_t)width * 4));
    } else {
        // No initial data - create empty atlas (square, using width as both dimensions)
        height = width;
        pixel_len = (size_t)width * height * 4;
        bytes = zalloc(1, pixel_len);  // Allocate zeroed (black/transparent) pixel data
        if (!bytes) {
            return NULL;
        }
        need_free = true;
        vk_log(LOG_INFO, "creating empty atlas: %ux%u", width, height);
    }

    struct vk_atlas *atlas = zalloc(1, sizeof(*atlas));
    atlas->vk = vk;
    atlas->width = width;
    atlas->height = height;
    atlas->refcount = 1;
    wl_list_init(&atlas->link);
    wl_list_insert(&vk->atlases, &atlas->link);

    struct vk_image_options opts = { .dst = {0} };
    struct vk_image *tmp = server_vk_add_rgba_image(vk, "atlas.raw", width, height, bytes, &opts);
    
    // Free temp buffer if we allocated it for empty atlas
    if (need_free) {
        free((void*)bytes);
        bytes = NULL;
    }
    
    if (!tmp) {
        free(atlas);
        return NULL;
    }

    // Steal GPU resources from temp image; this atlas isn't part of vk->images list.
    atlas->image = tmp->image;
    atlas->memory = tmp->memory;
    atlas->view = tmp->view;
    atlas->descriptor_set = tmp->descriptor_set;

    // Prevent temp image cleanup from freeing atlas resources.
    tmp->owns_descriptor_set = false;
    tmp->owns_image = false;
    tmp->image = VK_NULL_HANDLE;
    tmp->memory = VK_NULL_HANDLE;
    tmp->view = VK_NULL_HANDLE;
    tmp->descriptor_set = VK_NULL_HANDLE;
    server_vk_remove_image(vk, tmp);

    vk_log(LOG_INFO, "created atlas: %ux%u", width, height);
    return atlas;
}

void
server_vk_atlas_ref(struct vk_atlas *atlas) {
    if (atlas) {
        atlas->refcount++;
    }
}

void
server_vk_atlas_unref(struct vk_atlas *atlas) {
    if (!atlas) return;
    if (atlas->refcount == 0) return;

    atlas->refcount--;
    if (atlas->refcount != 0) return;

    struct server_vk *vk = atlas->vk;
    if (vk && vk->device) {
        vkDeviceWaitIdle(vk->device);
    }

    wl_list_remove(&atlas->link);
    wl_list_init(&atlas->link);

    if (vk && atlas->descriptor_set != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(vk->device, vk->descriptor_pool, 1, &atlas->descriptor_set);
    }
    if (vk && atlas->view) {
        vkDestroyImageView(vk->device, atlas->view, NULL);
    }
    if (vk && atlas->memory) {
        vkFreeMemory(vk->device, atlas->memory, NULL);
    }
    if (vk && atlas->image) {
        vkDestroyImage(vk->device, atlas->image, NULL);
    }

    free(atlas);
}

bool
server_vk_atlas_insert_raw(struct vk_atlas *atlas, const char *data, size_t data_len, uint32_t x, uint32_t y) {
    if (!atlas || !atlas->vk || !atlas->vk->device || !data || data_len == 0) {
        return false;
    }

    struct server_vk *vk = atlas->vk;

    struct util_png png = util_png_decode_raw(data, data_len, atlas->width);
    if (!png.data || png.width <= 0 || png.height <= 0) {
        return false;
    }

    uint32_t blit_width = (uint32_t)png.width;
    uint32_t blit_height = (uint32_t)png.height;

    if (x + blit_width > atlas->width) blit_width = atlas->width - x;
    if (y + blit_height > atlas->height) blit_height = atlas->height - y;
    if (blit_width == 0 || blit_height == 0) {
        free(png.data);
        return false;
    }

    vkDeviceWaitIdle(vk->device);

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vk->device, atlas->image, &mem_reqs);

    void *mapped = NULL;
    if (vkMapMemory(vk->device, atlas->memory, 0, mem_reqs.size, 0, &mapped) != VK_SUCCESS) {
        free(png.data);
        return false;
    }

    VkImageSubresource subres = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(vk->device, atlas->image, &subres, &layout);

    const unsigned char *src = (const unsigned char *)png.data;
    unsigned char *dst0 = (unsigned char *)mapped + layout.offset;
    for (uint32_t row = 0; row < blit_height; row++) {
        unsigned char *dst = dst0 + ((y + row) * (uint32_t)layout.rowPitch) + x * 4;
        memcpy(dst, src + (size_t)row * (size_t)png.width * 4, (size_t)blit_width * 4);
    }

    vkUnmapMemory(vk->device, atlas->memory);
    free(png.data);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmd_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(vk->device, &cmd_alloc, &cmd) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &begin_info);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = atlas->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    vkQueueSubmit(vk->graphics_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk->graphics_queue);
    vkFreeCommandBuffers(vk->device, vk->command_pool, 1, &cmd);

    return true;
}

char *
server_vk_atlas_get_dump(struct vk_atlas *atlas, size_t *out_len) {
    if (!atlas || !atlas->vk || !atlas->vk->device || !out_len) {
        return NULL;
    }

    struct server_vk *vk = atlas->vk;

    size_t pixel_data_size = (size_t)atlas->width * (size_t)atlas->height * 4;
    *out_len = 8 + pixel_data_size;

    char *dump_data = malloc(*out_len);
    check_alloc(dump_data);

    // Write little-endian header (width, height)
    dump_data[0] = (char)(atlas->width & 0xFF);
    dump_data[1] = (char)((atlas->width >> 8) & 0xFF);
    dump_data[2] = (char)((atlas->width >> 16) & 0xFF);
    dump_data[3] = (char)((atlas->width >> 24) & 0xFF);
    dump_data[4] = (char)(atlas->height & 0xFF);
    dump_data[5] = (char)((atlas->height >> 8) & 0xFF);
    dump_data[6] = (char)((atlas->height >> 16) & 0xFF);
    dump_data[7] = (char)((atlas->height >> 24) & 0xFF);

    vkDeviceWaitIdle(vk->device);

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vk->device, atlas->image, &mem_reqs);

    void *mapped = NULL;
    if (vkMapMemory(vk->device, atlas->memory, 0, mem_reqs.size, 0, &mapped) != VK_SUCCESS) {
        free(dump_data);
        *out_len = 0;
        return NULL;
    }

    VkImageSubresource subres = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(vk->device, atlas->image, &subres, &layout);

    unsigned char *out_pixels = (unsigned char *)dump_data + 8;
    const unsigned char *src0 = (const unsigned char *)mapped + layout.offset;
    size_t row_bytes = (size_t)atlas->width * 4;

    for (uint32_t row = 0; row < atlas->height; row++) {
        memcpy(out_pixels + (size_t)row * row_bytes, src0 + (size_t)row * layout.rowPitch, row_bytes);
    }

    vkUnmapMemory(vk->device, atlas->memory);
    return dump_data;
}

struct vk_image *
server_vk_add_image_from_atlas(struct server_vk *vk, struct vk_atlas *atlas, struct box src,
                               const struct vk_image_options *options) {
    if (!vk || !atlas || !options) return NULL;

    struct vk_image *image = zalloc(1, sizeof(*image));
    image->atlas = atlas;
    server_vk_atlas_ref(atlas);

    image->dst = options->dst;
    image->depth = options->depth;
    image->enabled = true;
    image->owns_descriptor_set = false;
    image->owns_image = false;
    image->descriptor_set = atlas->descriptor_set;
    image->width = src.width;
    image->height = src.height;

    float u0 = (float)src.x / (float)atlas->width;
    float v0 = (float)src.y / (float)atlas->height;
    float u1 = (float)(src.x + src.width) / (float)atlas->width;
    float v1 = (float)(src.y + src.height) / (float)atlas->height;

    struct quad_vertex verts[6] = {
        {{ -1.0f, -1.0f }, { u0, v0 }},
        {{ -1.0f,  1.0f }, { u0, v1 }},
        {{  1.0f,  1.0f }, { u1, v1 }},
        {{ -1.0f, -1.0f }, { u0, v0 }},
        {{  1.0f,  1.0f }, { u1, v1 }},
        {{  1.0f, -1.0f }, { u1, v0 }},
    };

    VkDeviceSize buffer_size = sizeof(verts);
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer_size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(vk->device, &buffer_info, NULL, &image->vertex_buffer) != VK_SUCCESS) {
        server_vk_remove_image(vk, image);
        return NULL;
    }

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(vk->device, image->vertex_buffer, &mem_reqs);
    uint32_t mem_type = find_memory_type(vk, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type == UINT32_MAX) {
        server_vk_remove_image(vk, image);
        return NULL;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };
    if (vkAllocateMemory(vk->device, &alloc_info, NULL, &image->vertex_memory) != VK_SUCCESS) {
        server_vk_remove_image(vk, image);
        return NULL;
    }

    vkBindBufferMemory(vk->device, image->vertex_buffer, image->vertex_memory, 0);

    void *mapped;
    if (vkMapMemory(vk->device, image->vertex_memory, 0, buffer_size, 0, &mapped) == VK_SUCCESS) {
        memcpy(mapped, verts, sizeof(verts));
        vkUnmapMemory(vk->device, image->vertex_memory);
    }

    wl_list_insert(&vk->images, &image->link);
    return image;
}

struct vk_image *
server_vk_add_image(struct server_vk *vk, const char *path, const struct vk_image_options *options) {
    // Load PNG file
    struct util_png png = util_png_decode(path, 8192);  // max 8192x8192
    if (!png.data || png.width <= 0 || png.height <= 0) {
        vk_log(LOG_ERROR, "failed to load PNG: %s", path);
        return NULL;
    }

    vk_log(LOG_INFO, "loading image: %s (%dx%d)", path, png.width, png.height);

    struct vk_image *image = zalloc(1, sizeof(*image));
    image->width = png.width;
    image->height = png.height;
    image->dst = options->dst;
    image->enabled = true;
    image->owns_descriptor_set = true;
    image->owns_image = true;

    // Create Vulkan image
    VkImageCreateInfo image_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,  // PNG is RGBA
        .extent = { png.width, png.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,  // For direct CPU access
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
    };

    if (vkCreateImage(vk->device, &image_ci, NULL, &image->image) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create image for PNG");
        free(png.data);
        free(image);
        return NULL;
    }

    // Get memory requirements
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vk->device, image->image, &mem_reqs);

    // Find host-visible memory type for direct mapping
    uint32_t mem_type = find_memory_type(vk, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (mem_type == UINT32_MAX) {
        vk_log(LOG_ERROR, "no suitable memory type for image");
        vkDestroyImage(vk->device, image->image, NULL);
        free(png.data);
        free(image);
        return NULL;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };

    if (vkAllocateMemory(vk->device, &alloc_info, NULL, &image->memory) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to allocate image memory");
        vkDestroyImage(vk->device, image->image, NULL);
        free(png.data);
        free(image);
        return NULL;
    }

    vkBindImageMemory(vk->device, image->image, image->memory, 0);

    // Map memory and copy PNG data
    void *mapped;
    if (vkMapMemory(vk->device, image->memory, 0, mem_reqs.size, 0, &mapped) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to map image memory");
        vkFreeMemory(vk->device, image->memory, NULL);
        vkDestroyImage(vk->device, image->image, NULL);
        free(png.data);
        free(image);
        return NULL;
    }

    // Get image subresource layout for proper row pitch
    VkImageSubresource subres = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(vk->device, image->image, &subres, &layout);

    // Copy row by row to handle potential padding
    const char *src = png.data;
    char *dst = (char *)mapped + layout.offset;
    size_t src_row_size = png.width * 4;  // RGBA = 4 bytes per pixel

    for (int y = 0; y < png.height; y++) {
        memcpy(dst, src, src_row_size);
        src += src_row_size;
        dst += layout.rowPitch;
    }

    vkUnmapMemory(vk->device, image->memory);
    free(png.data);

    // Transition image layout (need command buffer)
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cmd_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    vkAllocateCommandBuffers(vk->device, &cmd_alloc, &cmd);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &begin_info);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    vkQueueSubmit(vk->graphics_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk->graphics_queue);
    vkFreeCommandBuffers(vk->device, vk->command_pool, 1, &cmd);

    // Create image view
    VkImageViewCreateInfo view_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    if (vkCreateImageView(vk->device, &view_ci, NULL, &image->view) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create image view");
        vkFreeMemory(vk->device, image->memory, NULL);
        vkDestroyImage(vk->device, image->image, NULL);
        free(image);
        return NULL;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo ds_alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = vk->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &vk->image_pipeline.descriptor_layout,
    };

    if (vkAllocateDescriptorSets(vk->device, &ds_alloc, &image->descriptor_set) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to allocate image descriptor set");
        vkDestroyImageView(vk->device, image->view, NULL);
        vkFreeMemory(vk->device, image->memory, NULL);
        vkDestroyImage(vk->device, image->image, NULL);
        free(image);
        return NULL;
    }

    // Update descriptor set
    VkDescriptorImageInfo img_info = {
        .sampler = vk->sampler,
        .imageView = image->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = image->descriptor_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &img_info,
    };

    vkUpdateDescriptorSets(vk->device, 1, &write, 0, NULL);

    // Add to list
    wl_list_insert(&vk->images, &image->link);

    vk_log(LOG_INFO, "added image: %dx%d -> dst(%d,%d %dx%d)",
           image->width, image->height,
           image->dst.x, image->dst.y, image->dst.width, image->dst.height);

    return image;
}

struct vk_image *
server_vk_add_avif_image(struct server_vk *vk, const char *path, const struct vk_image_options *options) {
    struct util_avif avif = util_avif_decode(path, 4096);
    if (!avif.frames || avif.frame_count == 0 || avif.width == 0 || avif.height == 0) {
        util_avif_free(&avif);
        vk_log(LOG_ERROR, "failed to load AVIF: %s", path);
        return NULL;
    }

    struct util_avif_frame *frame0 = &avif.frames[0];
    struct vk_image *image = server_vk_add_rgba_image(vk, path, (uint32_t)avif.width, (uint32_t)avif.height,
                                                      (const unsigned char *)frame0->data, options);
    if (!image) {
        util_avif_free(&avif);
        return NULL;
    }

    if (avif.is_animated && avif.frame_count > 1) {
        // Transfer ownership of decoded frames to the image for per-frame updates.
        image->frames = avif.frames;
        image->frame_count = avif.frame_count;
        image->frame_index = 0;

        double dur_s = frame0->duration;
        if (!(dur_s > 0.0)) dur_s = 0.1;
        uint64_t dur_ms = (uint64_t)llround(dur_s * 1000.0);
        if (dur_ms == 0) dur_ms = 1;
        image->next_frame_ms = now_ms() + dur_ms;

        avif.frames = NULL;
        avif.frame_count = 0;
    }

    util_avif_free(&avif);
    return image;
}

void
server_vk_remove_image(struct server_vk *vk, struct vk_image *image) {
    if (!image) return;

    vk_log(LOG_INFO, "removing image: %dx%d", image->width, image->height);

    // Wait for GPU to finish using this image
    vkDeviceWaitIdle(vk->device);

    if (image->frames) {
        for (size_t i = 0; i < image->frame_count; i++) {
            free(image->frames[i].data);
        }
        free(image->frames);
        image->frames = NULL;
        image->frame_count = 0;
        image->frame_index = 0;
        image->next_frame_ms = 0;
    }

    // Free descriptor set back to pool
    if (image->owns_descriptor_set && image->descriptor_set != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(vk->device, vk->descriptor_pool, 1, &image->descriptor_set);
    }

    if (image->vertex_buffer) {
        vkFreeMemory(vk->device, image->vertex_memory, NULL);
        vkDestroyBuffer(vk->device, image->vertex_buffer, NULL);
    }

    // Destroy Vulkan resources (if owned by this image)
    if (image->owns_image) {
        if (image->view) {
            vkDestroyImageView(vk->device, image->view, NULL);
        }
        if (image->memory) {
            vkFreeMemory(vk->device, image->memory, NULL);
        }
        if (image->image) {
            vkDestroyImage(vk->device, image->image, NULL);
        }
    }

    if (image->atlas) {
        server_vk_atlas_unref(image->atlas);
        image->atlas = NULL;
    }

    wl_list_remove(&image->link);
    free(image);
}

void
server_vk_image_set_enabled(struct vk_image *image, bool enabled) {
    if (image) {
        image->enabled = enabled;
    }
}

// ============================================================================
// Text Rendering
// ============================================================================

#define VK_FONT_ATLAS_SIZE 1024
#define VK_MAX_TEXT_BYTES 16384
#define VK_MAX_ADVANCE_BYTES 16384

// Safe bounded strdup for text payloads
static char *
vk_strdup_bounded(const char *src) {
    if (!src) {
        return strdup("");
    }
    size_t len = strlen(src);
    if (len > VK_MAX_TEXT_BYTES) {
        len = VK_MAX_TEXT_BYTES;
    }
    char *dst = malloc(len + 1);
    if (!dst) {
        return NULL;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

// Text vertex structure (matches text.frag expectations)
struct text_vertex {
    // Matches the shared texcopy vertex shader inputs:
    // layout(location=0) v_src_pos (pixels)
    // layout(location=1) v_dst_pos (pixels)
    // layout(location=2) v_src_rgba (unused)
    // layout(location=3) v_dst_rgba (text color)
    float src_pos[2];   // Pixel coords in font atlas
    float dst_pos[2];   // Pixel coords on output surface
    float src_rgba[4];  // Unused
    float dst_rgba[4];  // Text color with alpha
};

// UTF-8 decode helper
static uint32_t
vk_utf8_decode_bounded(const char **str, const char *end) {
    if (!str || !*str || *str >= end) {
        return 0;
    }

    const unsigned char *s = (const unsigned char *)*str;
    const size_t remaining = (size_t)(end - *str);

    uint32_t cp = 0xFFFD;
    size_t len = 1;

    unsigned char b0 = s[0];
    if (b0 < 0x80) {
        cp = b0;
        len = 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        if (remaining < 2) goto done;
        unsigned char b1 = s[1];
        if ((b1 & 0xC0) != 0x80) goto done;
        cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(b1 & 0x3F);
        if (cp < 0x80) cp = 0xFFFD; // overlong
        len = 2;
    } else if ((b0 & 0xF0) == 0xE0) {
        if (remaining < 3) goto done;
        unsigned char b1 = s[1], b2 = s[2];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) goto done;
        cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(b1 & 0x3F) << 6) | (uint32_t)(b2 & 0x3F);
        if (cp < 0x800) cp = 0xFFFD; // overlong
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; // surrogate
        len = 3;
    } else if ((b0 & 0xF8) == 0xF0) {
        if (remaining < 4) goto done;
        unsigned char b1 = s[1], b2 = s[2], b3 = s[3];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) goto done;
        cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(b1 & 0x3F) << 12) |
             ((uint32_t)(b2 & 0x3F) << 6) | (uint32_t)(b3 & 0x3F);
        if (cp < 0x10000) cp = 0xFFFD; // overlong
        if (cp > 0x10FFFF) cp = 0xFFFD;
        len = 4;
    }

done:
    *str += (ptrdiff_t)len;
    return cp;
}

// Initialize FreeType font system
static bool
init_font_system(struct server_vk *vk, const char *font_path, uint32_t base_size) {
    FT_Library ft;
    if (FT_Init_FreeType(&ft) != 0) {
        vk_log(LOG_ERROR, "failed to initialize FreeType");
        return false;
    }
    vk->font.ft_library = ft;

    FT_Face face;
    if (FT_New_Face(ft, font_path, 0, &face) != 0) {
        vk_log(LOG_ERROR, "failed to load font: %s", font_path);
        FT_Done_FreeType(ft);
        vk->font.ft_library = NULL;
        return false;
    }
    vk->font.ft_face = face;
    vk->font.base_font_size = base_size > 0 ? base_size : 16;
    vk->font.sizes = NULL;
    vk->font.sizes_count = 0;
    vk->font.sizes_capacity = 0;

    vk_log(LOG_INFO, "initialized font system: %s (base size %u)", font_path, vk->font.base_font_size);
    return true;
}

static void
destroy_font_system(struct server_vk *vk) {
    // Destroy all font size caches
    for (size_t i = 0; i < vk->font.sizes_count; i++) {
        struct vk_font_size *fs = &vk->font.sizes[i];
        if (fs->atlas_view) vkDestroyImageView(vk->device, fs->atlas_view, NULL);
        if (fs->atlas_memory) vkFreeMemory(vk->device, fs->atlas_memory, NULL);
        if (fs->atlas_image) vkDestroyImage(vk->device, fs->atlas_image, NULL);
        free(fs->glyphs);
    }
    free(vk->font.sizes);
    vk->font.sizes = NULL;
    vk->font.sizes_count = 0;

    if (vk->font.ft_face) {
        FT_Done_Face((FT_Face)vk->font.ft_face);
        vk->font.ft_face = NULL;
    }
    if (vk->font.ft_library) {
        FT_Done_FreeType((FT_Library)vk->font.ft_library);
        vk->font.ft_library = NULL;
    }
}

// Get or create font size cache
static struct vk_font_size *
get_font_size(struct server_vk *vk, uint32_t size) {
    // Look for existing
    for (size_t i = 0; i < vk->font.sizes_count; i++) {
        if (vk->font.sizes[i].size == size) {
            return &vk->font.sizes[i];
        }
    }

    // Create new
    if (vk->font.sizes_count >= vk->font.sizes_capacity) {
        size_t new_cap = vk->font.sizes_capacity ? vk->font.sizes_capacity * 2 : 4;
        struct vk_font_size *new_sizes = realloc(vk->font.sizes, new_cap * sizeof(*new_sizes));
        if (!new_sizes) return NULL;
        vk->font.sizes = new_sizes;
        vk->font.sizes_capacity = new_cap;
    }

    struct vk_font_size *fs = &vk->font.sizes[vk->font.sizes_count++];
    memset(fs, 0, sizeof(*fs));
    fs->size = size;
    fs->glyph_capacity = 128;
    fs->glyphs = zalloc(fs->glyph_capacity, sizeof(struct vk_glyph));
    fs->atlas_width = VK_FONT_ATLAS_SIZE;
    fs->atlas_height = VK_FONT_ATLAS_SIZE;

    // Set FreeType size
    FT_Face face = (FT_Face)vk->font.ft_face;
    FT_Set_Pixel_Sizes(face, 0, size);

    // Create atlas image
    VkImageCreateInfo image_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,  // Single channel for font glyphs
        .extent = { fs->atlas_width, fs->atlas_height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
    };

    if (vkCreateImage(vk->device, &image_ci, NULL, &fs->atlas_image) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create font atlas image");
        vk->font.sizes_count--;
        return NULL;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vk->device, fs->atlas_image, &mem_reqs);

    uint32_t mem_type = find_memory_type(vk, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };

    if (vkAllocateMemory(vk->device, &alloc_info, NULL, &fs->atlas_memory) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to allocate font atlas memory");
        vkDestroyImage(vk->device, fs->atlas_image, NULL);
        vk->font.sizes_count--;
        return NULL;
    }

    vkBindImageMemory(vk->device, fs->atlas_image, fs->atlas_memory, 0);

    // Clear atlas to zero
    void *mapped;
    if (vkMapMemory(vk->device, fs->atlas_memory, 0, mem_reqs.size, 0, &mapped) == VK_SUCCESS) {
        memset(mapped, 0, mem_reqs.size);
        vkUnmapMemory(vk->device, fs->atlas_memory);
    }

    // Create image view
    VkImageViewCreateInfo view_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = fs->atlas_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    if (vkCreateImageView(vk->device, &view_ci, NULL, &fs->atlas_view) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create font atlas view");
        vkFreeMemory(vk->device, fs->atlas_memory, NULL);
        vkDestroyImage(vk->device, fs->atlas_image, NULL);
        vk->font.sizes_count--;
        return NULL;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo ds_alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = vk->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &vk->text_vk_pipeline.descriptor_layout,
    };

    if (vkAllocateDescriptorSets(vk->device, &ds_alloc, &fs->atlas_descriptor) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to allocate font atlas descriptor");
        vkDestroyImageView(vk->device, fs->atlas_view, NULL);
        vkFreeMemory(vk->device, fs->atlas_memory, NULL);
        vkDestroyImage(vk->device, fs->atlas_image, NULL);
        vk->font.sizes_count--;
        return NULL;
    }

    // Update descriptor set
    VkDescriptorImageInfo img_info = {
        .sampler = vk->sampler,
        .imageView = fs->atlas_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = fs->atlas_descriptor,
        .dstBinding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &img_info,
    };

    vkUpdateDescriptorSets(vk->device, 1, &write, 0, NULL);
    fs->atlas_initialized = true;

    vk_log(LOG_INFO, "created font size cache: %u px", size);
    return fs;
}

// Get or render glyph
static struct vk_glyph *
get_glyph(struct server_vk *vk, struct vk_font_size *fs, uint32_t codepoint) {
    // Look for existing
    for (size_t i = 0; i < fs->glyph_count; i++) {
        if (fs->glyphs[i].codepoint == codepoint) {
            return &fs->glyphs[i];
        }
    }

    // Render new glyph
    FT_Face face = (FT_Face)vk->font.ft_face;
    FT_Set_Pixel_Sizes(face, 0, fs->size);

    if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER) != 0) {
        return NULL;  // Glyph not available
    }

    FT_GlyphSlot g = face->glyph;

    // Check if we need to expand glyphs array
    if (fs->glyph_count >= fs->glyph_capacity) {
        fs->glyph_capacity *= 2;
        fs->glyphs = realloc(fs->glyphs, fs->glyph_capacity * sizeof(struct vk_glyph));
    }

    // Check if glyph fits in current row
    if (fs->atlas_x + (int)g->bitmap.width > fs->atlas_width) {
        fs->atlas_x = 0;
        fs->atlas_y += fs->atlas_row_height + 1;
        fs->atlas_row_height = 0;
    }

    // Check if atlas is full
    if (fs->atlas_y + (int)g->bitmap.rows > fs->atlas_height) {
        vk_log(LOG_WARN, "font atlas full for size %u", fs->size);
        return NULL;
    }

    struct vk_glyph *glyph = &fs->glyphs[fs->glyph_count++];
    glyph->codepoint = codepoint;
    glyph->width = g->bitmap.width;
    glyph->height = g->bitmap.rows;
    glyph->bearing_x = g->bitmap_left;
    glyph->bearing_y = g->bitmap_top;
    glyph->advance = g->advance.x >> 6;
    glyph->atlas_x = fs->atlas_x;
    glyph->atlas_y = fs->atlas_y;

    // Copy glyph bitmap to atlas
    if (g->bitmap.buffer && g->bitmap.width > 0 && g->bitmap.rows > 0) {
        void *mapped;
        VkMemoryRequirements mem_reqs;
        vkGetImageMemoryRequirements(vk->device, fs->atlas_image, &mem_reqs);

        if (vkMapMemory(vk->device, fs->atlas_memory, 0, mem_reqs.size, 0, &mapped) == VK_SUCCESS) {
            VkImageSubresource subres = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
            VkSubresourceLayout layout;
            vkGetImageSubresourceLayout(vk->device, fs->atlas_image, &subres, &layout);

            unsigned char *dst = (unsigned char *)mapped + layout.offset;
            for (unsigned int y = 0; y < g->bitmap.rows; y++) {
                unsigned char *row = dst + ((fs->atlas_y + y) * layout.rowPitch) + fs->atlas_x;
                memcpy(row, g->bitmap.buffer + y * g->bitmap.pitch, g->bitmap.width);
            }
            vkUnmapMemory(vk->device, fs->atlas_memory);
        }
    }

    // Update packing position
    fs->atlas_x += glyph->width + 1;
    if (glyph->height > fs->atlas_row_height) {
        fs->atlas_row_height = glyph->height;
    }

    return glyph;
}

// Create text pipeline
static bool
create_text_vk_pipeline(struct server_vk *vk) {
    // Create descriptor set layout (same as image - combined image sampler)
    if (!create_descriptor_set_layout(vk, &vk->text_vk_pipeline.descriptor_layout)) {
        vk_log(LOG_ERROR, "failed to create text descriptor set layout");
        return false;
    }

    // Push constant range (shared texcopy vertex shader expects this)
    VkPushConstantRange push_constant = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(struct vk_push_constants),
    };

    // Pipeline layout
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &vk->text_vk_pipeline.descriptor_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant,
    };

    if (vkCreatePipelineLayout(vk->device, &layout_info, NULL, &vk->text_vk_pipeline.layout) != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create text pipeline layout");
        return false;
    }

    // Use the text.frag shader
    VkShaderModule text_frag = create_shader_module(vk->device, text_frag_spv, text_frag_spv_size);
    if (!text_frag) {
        vk_log(LOG_ERROR, "failed to create text shader module");
        return false;
    }

    // Shader stages (reuse texcopy vertex shader)
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
            .module = text_frag,
            .pName = "main",
        },
    };

    // Vertex input - matches shared texcopy vertex shader inputs
    VkVertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(struct text_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    VkVertexInputAttributeDescription attributes[] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(struct text_vertex, src_pos) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(struct text_vertex, dst_pos) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(struct text_vertex, src_rgba) },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(struct text_vertex, dst_rgba) },
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

    // Pre-multiplied alpha blending for text
    // Shader outputs pre-multiplied colors (rgb * a, a)
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,  // Pre-multiplied alpha
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
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
        .layout = vk->text_vk_pipeline.layout,
        .renderPass = vk->render_pass,
        .subpass = 0,
    };

    VkResult result = vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipeline_ci, NULL, &vk->text_vk_pipeline.pipeline);
    vkDestroyShaderModule(vk->device, text_frag, NULL);

    if (result != VK_SUCCESS) {
        vk_log(LOG_ERROR, "failed to create text pipeline: %d", result);
        return false;
    }

    vk_log(LOG_INFO, "created text pipeline");
    return true;
}

// Draw all text objects
// Note: This logic has been moved to draw_text_single and draw_sorted_objects.
// Keeping this comment as a placeholder where the old function was.

// Build text vertex buffer
static bool
build_text_vertices(struct server_vk *vk, struct vk_text *text) {
    if (!text->text || text->text[0] == '\0' || !text->font) {
        text->vertex_count = 0;
        return true;
    }
    static const float UNUSED_RGBA[4] = { 0.f, 0.f, 0.f, 0.f };

    const size_t total_len = strlen(text->text);
    const size_t used_len = total_len > VK_MAX_TEXT_BYTES ? VK_MAX_TEXT_BYTES : total_len;
    if (total_len > VK_MAX_TEXT_BYTES) {
        vk_log(LOG_WARN, "text truncated for rendering (%zu bytes > %u)", total_len, VK_MAX_TEXT_BYTES);
    }

    // Worst-case: every byte is a glyph -> 6 vertices per byte.
    size_t max_vertices = used_len * 6;
    struct text_vertex *vertices = zalloc(max_vertices ? max_vertices : 6, sizeof(*vertices));

    // Parse inline tags compatible with the OpenGL text renderer:
    // - "<#RRGGBBAA>" changes the current color
    // - "<+N>" advances by N pixels (used to reserve emote space)
    uint32_t current_color_u32 = text->color;
    float current_color[4] = {
        ((current_color_u32 >> 24) & 0xFF) / 255.0f,
        ((current_color_u32 >> 16) & 0xFF) / 255.0f,
        ((current_color_u32 >> 8) & 0xFF) / 255.0f,
        (current_color_u32 & 0xFF) / 255.0f,
    };

    int32_t x = text->x;
    int32_t y = text->y; // baseline y in pixels (top-left origin, y down)
    size_t vtx_idx = 0;

    const char *p = text->text;
    const char *end = text->text + used_len;
    while (p < end && *p) {
        // Color tag: <#RRGGBBAA> or <#RRGGBB>
        if (p[0] == '<' && p[1] == '#') {
            uint32_t rgba = 0;
            int hex_len = 0;
            const char *q = p + 2;
            while (hex_len < 8) {
                char c = q[hex_len];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                    break;
                }
                hex_len++;
            }
            if ((hex_len == 6 || hex_len == 8) && q[hex_len] == '>') {
                for (int i = 0; i < hex_len; i++) {
                    char c = q[i];
                    uint32_t val = 0;
                    if (c >= '0' && c <= '9') val = (uint32_t)(c - '0');
                    else if (c >= 'a' && c <= 'f') val = (uint32_t)(10 + c - 'a');
                    else if (c >= 'A' && c <= 'F') val = (uint32_t)(10 + c - 'A');
                    rgba = (rgba << 4) | val;
                }
                if (hex_len == 6) {
                    rgba = (rgba << 8) | 0xFF;
                }
                current_color_u32 = rgba;
                current_color[0] = ((current_color_u32 >> 24) & 0xFF) / 255.0f;
                current_color[1] = ((current_color_u32 >> 16) & 0xFF) / 255.0f;
                current_color[2] = ((current_color_u32 >> 8) & 0xFF) / 255.0f;
                current_color[3] = (current_color_u32 & 0xFF) / 255.0f;
                p = q + hex_len + 1;
                continue;
            }
        }

        // Advance tag: <+N>
        if (p[0] == '<' && p[1] == '+') {
            char *q = NULL;
            double adv = strtod(p + 2, &q);
            if (q && q < end && *q == '>') {
                x += (int32_t)llround(adv);
                p = q + 1;
                continue;
            }
            // Don't render raw "<+...>" if it's malformed; just skip it.
            const char *skip = p + 2;
            while (skip < end && *skip && *skip != '>') skip++;
            if (skip < end && *skip == '>') {
                p = skip + 1;
                continue;
            }
        }

        uint32_t cp = vk_utf8_decode_bounded(&p, end);
        if (cp == '\n') {
            x = text->x;
            y += (int32_t)text->size + text->line_spacing;
            continue;
        }

        struct vk_glyph *g = get_glyph(vk, text->font, cp);
        if (!g) {
            continue;
        }

        float px = (float)(x + g->bearing_x);
        float py = (float)(y - g->bearing_y);
        float pw = (float)g->width;
        float ph = (float)g->height;

        float u0 = (float)g->atlas_x;
        float v0 = (float)g->atlas_y;
        float u1 = (float)(g->atlas_x + g->width);
        float v1 = (float)(g->atlas_y + g->height);

        float x0 = px;
        float y0 = py;
        float x1 = px + pw;
        float y1 = py + ph;

        if (vtx_idx + 6 > max_vertices) {
            // Shouldn't happen due to conservative allocation, but guard anyway.
            break;
        }

        // Triangle 1
        vertices[vtx_idx].src_pos[0] = u0; vertices[vtx_idx].src_pos[1] = v0;
        vertices[vtx_idx].dst_pos[0] = x0; vertices[vtx_idx].dst_pos[1] = y0;
        memcpy(vertices[vtx_idx].src_rgba, UNUSED_RGBA, sizeof(UNUSED_RGBA));
        memcpy(vertices[vtx_idx].dst_rgba, current_color, sizeof(current_color));
        vtx_idx++;

        vertices[vtx_idx].src_pos[0] = u1; vertices[vtx_idx].src_pos[1] = v0;
        vertices[vtx_idx].dst_pos[0] = x1; vertices[vtx_idx].dst_pos[1] = y0;
        memcpy(vertices[vtx_idx].src_rgba, UNUSED_RGBA, sizeof(UNUSED_RGBA));
        memcpy(vertices[vtx_idx].dst_rgba, current_color, sizeof(current_color));
        vtx_idx++;

        vertices[vtx_idx].src_pos[0] = u1; vertices[vtx_idx].src_pos[1] = v1;
        vertices[vtx_idx].dst_pos[0] = x1; vertices[vtx_idx].dst_pos[1] = y1;
        memcpy(vertices[vtx_idx].src_rgba, UNUSED_RGBA, sizeof(UNUSED_RGBA));
        memcpy(vertices[vtx_idx].dst_rgba, current_color, sizeof(current_color));
        vtx_idx++;

        // Triangle 2
        vertices[vtx_idx].src_pos[0] = u0; vertices[vtx_idx].src_pos[1] = v0;
        vertices[vtx_idx].dst_pos[0] = x0; vertices[vtx_idx].dst_pos[1] = y0;
        memcpy(vertices[vtx_idx].src_rgba, UNUSED_RGBA, sizeof(UNUSED_RGBA));
        memcpy(vertices[vtx_idx].dst_rgba, current_color, sizeof(current_color));
        vtx_idx++;

        vertices[vtx_idx].src_pos[0] = u1; vertices[vtx_idx].src_pos[1] = v1;
        vertices[vtx_idx].dst_pos[0] = x1; vertices[vtx_idx].dst_pos[1] = y1;
        memcpy(vertices[vtx_idx].src_rgba, UNUSED_RGBA, sizeof(UNUSED_RGBA));
        memcpy(vertices[vtx_idx].dst_rgba, current_color, sizeof(current_color));
        vtx_idx++;

        vertices[vtx_idx].src_pos[0] = u0; vertices[vtx_idx].src_pos[1] = v1;
        vertices[vtx_idx].dst_pos[0] = x0; vertices[vtx_idx].dst_pos[1] = y1;
        memcpy(vertices[vtx_idx].src_rgba, UNUSED_RGBA, sizeof(UNUSED_RGBA));
        memcpy(vertices[vtx_idx].dst_rgba, current_color, sizeof(current_color));
        vtx_idx++;

        x += g->advance;
    }

    text->vertex_count = vtx_idx;

    // Create or update vertex buffer
    if (text->vertex_buffer) {
        vkDeviceWaitIdle(vk->device);
        vkFreeMemory(vk->device, text->vertex_memory, NULL);
        vkDestroyBuffer(vk->device, text->vertex_buffer, NULL);
        text->vertex_buffer = VK_NULL_HANDLE;
        text->vertex_memory = VK_NULL_HANDLE;
    }

    if (vtx_idx == 0) {
        free(vertices);
        return true;
    }

    VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = vtx_idx * sizeof(struct text_vertex),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    if (vkCreateBuffer(vk->device, &buffer_ci, NULL, &text->vertex_buffer) != VK_SUCCESS) {
        free(vertices);
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(vk->device, text->vertex_buffer, &mem_reqs);

    uint32_t mem_type = find_memory_type(vk, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };

    if (vkAllocateMemory(vk->device, &alloc_info, NULL, &text->vertex_memory) != VK_SUCCESS) {
        vkDestroyBuffer(vk->device, text->vertex_buffer, NULL);
        text->vertex_buffer = VK_NULL_HANDLE;
        free(vertices);
        return false;
    }

    vkBindBufferMemory(vk->device, text->vertex_buffer, text->vertex_memory, 0);

    void *mapped;
    if (vkMapMemory(vk->device, text->vertex_memory, 0, buffer_ci.size, 0, &mapped) == VK_SUCCESS) {
        memcpy(mapped, vertices, vtx_idx * sizeof(struct text_vertex));
        vkUnmapMemory(vk->device, text->vertex_memory);
    }

    free(vertices);
    text->dirty = false;
    return true;
}

// Text API implementation
struct vk_text *
server_vk_add_text(struct server_vk *vk, const char *str, const struct vk_text_options *options) {
    if (!vk->font.ft_face) {
        vk_log(LOG_ERROR, "font not initialized");
        return NULL;
    }

    struct vk_text *text = zalloc(1, sizeof(*text));
    text->vk = vk;
    text->text = vk_strdup_bounded(str ? str : "");
    if (!text->text) {
        free(text);
        return NULL;
    }
    text->x = options->x;
    text->y = options->y;
    text->size = options->size > 0 ? options->size : 1;
    text->line_spacing = options->line_spacing;
    text->color = options->color;
    text->depth = options->depth;
    text->enabled = true;
    text->dirty = true;

    // Get font size cache (size is pixel size)
    uint32_t font_size = text->size;
    text->font = get_font_size(vk, font_size);
    if (!text->font) {
        free(text->text);
        free(text);
        return NULL;
    }

    // Build initial vertices
    if (!build_text_vertices(vk, text)) {
        free(text->text);
        free(text);
        return NULL;
    }

    wl_list_insert(&vk->texts, &text->link);

    vk_log(LOG_INFO, "added text: \"%s\" at (%d,%d) size=%u color=0x%08x",
           text->text, text->x, text->y, text->size, text->color);

    return text;
}

void
server_vk_remove_text(struct server_vk *vk, struct vk_text *text) {
    if (!text) return;

    vk_log(LOG_INFO, "removing text: \"%s\"", text->text);

    vkDeviceWaitIdle(vk->device);

    if (text->vertex_buffer) {
        vkFreeMemory(vk->device, text->vertex_memory, NULL);
        vkDestroyBuffer(vk->device, text->vertex_buffer, NULL);
    }

    wl_list_remove(&text->link);
    free(text->text);
    free(text);
}

void
server_vk_text_set_enabled(struct vk_text *text, bool enabled) {
    if (text) {
        text->enabled = enabled;
    }
}

void
server_vk_text_set_text(struct vk_text *text, const char *new_text) {
    if (!text) return;

    free(text->text);
    text->text = vk_strdup_bounded(new_text ? new_text : "");
    if (!text->text) {
        text->text = strdup("");
    }
    text->dirty = true;

    // Rebuild vertices immediately
    build_text_vertices(text->vk, text);
}

void
server_vk_text_set_color(struct vk_text *text, uint32_t color) {
    if (!text) return;

    text->color = color;
    text->dirty = true;

    // Rebuild vertices immediately
    build_text_vertices(text->vk, text);
}

struct vk_advance_ret
server_vk_text_advance(struct server_vk *vk, const char *data, size_t data_len, uint32_t size) {
    if (!vk || !vk->font.ft_face || !data || data_len == 0 || size == 0) {
        return (struct vk_advance_ret){ .x = 0, .y = 0 };
    }

    if (data_len > VK_MAX_ADVANCE_BYTES) {
        data_len = VK_MAX_ADVANCE_BYTES;
    }

    struct vk_font_size *fs = get_font_size(vk, size);
    if (!fs) {
        return (struct vk_advance_ret){ .x = 0, .y = 0 };
    }

    int32_t x = 0;
    int32_t y = 0;

    const char *p = data;
    const char *end = data + data_len;
    while (p < end && *p) {
        // Skip color tags: <#RRGGBBAA> / <#RRGGBB>
        if (p + 3 < end && p[0] == '<' && p[1] == '#') {
            int hex_len = 0;
            const char *q = p + 2;
            while ((q + hex_len) < end && hex_len < 8) {
                char c = q[hex_len];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                    break;
                }
                hex_len++;
            }
            if ((hex_len == 6 || hex_len == 8) && (q + hex_len) < end && q[hex_len] == '>') {
                p = q + hex_len + 1;
                continue;
            }
        }

        // Handle advance-only tags: <+N>
        if (p + 3 < end && p[0] == '<' && p[1] == '+') {
            const char *q = p + 2;
            char buf[64];
            size_t n = 0;
            while (q < end && *q != '>' && n + 1 < sizeof(buf)) {
                buf[n++] = *q++;
            }
            if (q < end && *q == '>' && n > 0) {
                buf[n] = '\0';
                char *endptr = NULL;
                double adv = strtod(buf, &endptr);
                if (endptr && endptr != buf && *endptr == '\0') {
                    x += (int32_t)llround(adv);
                    p = q + 1;
                    continue;
                }
            }
        }

        // If this looks like an advance tag but we failed to parse it, skip it as text.
        // This prevents raw "<+...>" from leaking into chat when Lua emits floats.
        if (p + 2 < end && p[0] == '<' && p[1] == '+') {
            const char *q = p + 2;
            while (q < end && *q && *q != '>') q++;
            if (q < end && *q == '>') {
                p = q + 1;
                continue;
            }
        }

        uint32_t cp = vk_utf8_decode_bounded(&p, end);
        if (cp == '\n') {
            x = 0;
            y += (int32_t)size;
            continue;
        }

        struct vk_glyph *g = get_glyph(vk, fs, cp);
        if (!g) continue;
        x += g->advance;
    }

    return (struct vk_advance_ret){ .x = x, .y = y };
}

// ============================================================================
// Floating View API
// ============================================================================

struct vk_view *
server_vk_add_view(struct server_vk *vk, struct server_view *view) {
    struct vk_view *v = zalloc(1, sizeof(*v));
    v->vk = vk;
    v->view = view;
    v->enabled = true;
    wl_list_insert(&vk->views, &v->link);
    return v;
}

void
server_vk_remove_view(struct server_vk *vk, struct vk_view *view) {
    wl_list_remove(&view->link);
    free(view);
}

void
server_vk_view_set_buffer(struct vk_view *view, struct server_buffer *buffer) {
    if (!buffer) {
        view->current_buffer = NULL;
        return;
    }
    
    struct vk_buffer *b = NULL;
    struct vk_buffer *iter;
    wl_list_for_each(iter, &view->vk->capture.buffers, link) {
        if (iter->parent == buffer) {
            b = iter;
            break;
        }
    }
    
    // If buffer not found in capture list, import it
    if (!b) {
        b = vk_buffer_import(view->vk, buffer);
        if (b) {
            vk_log(LOG_INFO, "imported floating view buffer: %dx%d", b->width, b->height);
        }
    }
    
    view->current_buffer = b;
}

void
server_vk_view_set_geometry(struct vk_view *view, int32_t x, int32_t y, int32_t width, int32_t height) {
    view->dst.x = x;
    view->dst.y = y;
    view->dst.width = width;
    view->dst.height = height;
}

void
server_vk_view_set_enabled(struct vk_view *view, bool enabled) {
    view->enabled = enabled;
}
