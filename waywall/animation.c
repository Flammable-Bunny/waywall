#include "animation.h"
#include "scene.h"
#include "server/gl.h"
#include "util/alloc.h"
#include "util/avif.h"
#include "util/log.h"
#include "util/prelude.h"
#include "util/sysinfo.h"
#include <GLES2/gl2.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Configuration constants
#define DEFAULT_MIN_FRAMES_PER_ANIM 2
#define DEFAULT_MAX_FRAMES_PER_ANIM 20
#define FALLBACK_VRAM_BUDGET_MB 1500
#define MAX_VRAM_BUDGET_MB 10000

// Format: {total_vram_mb, budget_mb, frames_per_anim}
struct vram_tier {
    size_t total_vram_mb;
    size_t budget_mb;
    size_t frames_per_anim;
};

static const struct vram_tier VRAM_TIERS[] = {
    {6 * 1024, 1500, 6},    // 6GB VRAM → 1.5GB budget, 6 frames/emote
    {8 * 1024, 2400, 8},    // 8GB VRAM → 2.4GB budget, 8 frames/emote
    {12 * 1024, 4000, 12},  // 12GB VRAM → 4GB budget, 12 frames/emote
    {16 * 1024, 5500, 15},  // 16GB VRAM → 5.5GB budget, 15 frames/emote
    {24 * 1024, 8000, 18},  // 24GB VRAM → 8GB budget, 18 frames/emote
    {32 * 1024, 10000, 20}, // 32GB VRAM → 10GB budget, 20 frames/emote
};

#define VRAM_TIER_COUNT (sizeof(VRAM_TIERS) / sizeof(VRAM_TIERS[0]))

static void upload_frame_to_vram(struct animation_manager *mgr, struct animation *anim,
                                 struct anim_vram_frame *vram_frame, size_t source_frame_idx) {
    if (source_frame_idx >= anim->avif.frame_count) {
        ww_log(LOG_ERROR, "invalid frame index %zu", source_frame_idx);
        return;
    }

    struct util_avif_frame *src_frame = &anim->avif.frames[source_frame_idx];

    // Create texture if needed
    server_gl_with(mgr->gl, false) {
        if (vram_frame->texture == 0) {
            glGenTextures(1, &vram_frame->texture);
        }

        glBindTexture(GL_TEXTURE_2D, vram_frame->texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, src_frame->width, src_frame->height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, src_frame->data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        vram_frame->frame_index = source_frame_idx;
        vram_frame->is_uploaded = true;

        // Update VRAM usage accounting
        size_t frame_size = src_frame->width * src_frame->height * 4;
        mgr->vram_used += frame_size;
    }
}

static void evict_frame_from_vram(struct animation_manager *mgr, struct anim_vram_frame *vram_frame,
                                  int32_t frame_width, int32_t frame_height) {
    if (!vram_frame->is_uploaded) {
        return;
    }

    server_gl_with(mgr->gl, false) {
        if (vram_frame->texture != 0) {
            glDeleteTextures(1, &vram_frame->texture);
            vram_frame->texture = 0;
        }

        vram_frame->is_uploaded = false;

        // Update VRAM usage accounting
        size_t frame_size = frame_width * frame_height * 4;
        if (mgr->vram_used >= frame_size) {
            mgr->vram_used -= frame_size;
        } else {
            mgr->vram_used = 0;
        }
    }
}

struct animation_manager *animation_manager_create(struct scene *scene, struct server_gl *gl) {
    struct animation_manager *mgr = zalloc(1, sizeof(*mgr));

    mgr->scene = scene;
    mgr->gl = gl;

    // Query VRAM and calculate budget using tiered system
    server_gl_with(gl, false) {
        mgr->vram_total = sysinfo_query_vram_total();
    }

    if (mgr->vram_total == 0) {
        ww_log(LOG_WARN, "unable to query VRAM, using fallback budget of %d MB",
               FALLBACK_VRAM_BUDGET_MB);
        mgr->vram_budget = (size_t)FALLBACK_VRAM_BUDGET_MB * 1024 * 1024;
        mgr->frames_per_anim = 5;
    } else {
        size_t vram_total_mb = mgr->vram_total / (1024 * 1024);

        // Find appropriate tier
        const struct vram_tier *selected_tier = NULL;
        for (size_t i = 0; i < VRAM_TIER_COUNT; i++) {
            if (vram_total_mb <= VRAM_TIERS[i].total_vram_mb) {
                selected_tier = &VRAM_TIERS[i];
                break;
            }
        }

        // If VRAM exceeds highest tier, use the highest tier
        if (!selected_tier) {
            selected_tier = &VRAM_TIERS[VRAM_TIER_COUNT - 1];
        }

        mgr->vram_budget = (size_t)selected_tier->budget_mb * 1024 * 1024;
        mgr->frames_per_anim = selected_tier->frames_per_anim;

        // Apply hard cap
        size_t max_budget = (size_t)MAX_VRAM_BUDGET_MB * 1024 * 1024;
        if (mgr->vram_budget > max_budget) {
            mgr->vram_budget = max_budget;
        }

        // Log current VRAM availability
        size_t vram_available = sysinfo_query_vram_available();
        if (vram_available > 0) {
            size_t vram_available_mb = vram_available / (1024 * 1024);
            ww_log(LOG_INFO, "detected %zu MB total VRAM, %zu MB currently available",
                   vram_total_mb, vram_available_mb);
        } else {
            ww_log(LOG_INFO, "detected %zu MB total VRAM (available memory query unavailable)",
                   vram_total_mb);
        }

        ww_log(LOG_INFO, "animation budget: %zu MB (%.1f%%), %zu frames per emote",
               selected_tier->budget_mb,
               (selected_tier->budget_mb * 100.0) / vram_total_mb,
               selected_tier->frames_per_anim);
    }

    mgr->min_frames_per_anim = DEFAULT_MIN_FRAMES_PER_ANIM;
    mgr->max_frames_per_anim = DEFAULT_MAX_FRAMES_PER_ANIM;

    mgr->animations = NULL;
    mgr->animation_count = 0;
    mgr->animation_capacity = 0;
    mgr->vram_used = 0;

    return mgr;
}

void animation_manager_destroy(struct animation_manager *mgr) {
    if (!mgr) {
        return;
    }

    // Free all animations
    for (size_t i = 0; i < mgr->animation_count; i++) {
        struct animation *anim = mgr->animations[i];
        if (anim) {
            // Force cleanup regardless of ref count
            anim->ref_count = 0;
            animation_unref(mgr, anim);
        }
    }

    free(mgr->animations);
    free(mgr);
}

struct animation *animation_create(struct animation_manager *mgr, const char *avif_path) {
    struct animation *anim = zalloc(1, sizeof(*anim));

    // Load AVIF
    anim->avif = util_avif_decode(avif_path, 4096);
    if (!anim->avif.frames || anim->avif.frame_count == 0) {
        ww_log(LOG_ERROR, "failed to load AVIF: %s", avif_path);
        free(anim);
        return NULL;
    }

    anim->avif_path = strdup(avif_path);
    anim->width = anim->avif.width;
    anim->height = anim->avif.height;
    anim->current_frame = 0;
    anim->last_update_time = 0.0;
    anim->is_playing = true;
    anim->ref_count = 1;

    // Allocate VRAM frame ring buffer
    anim->vram_frame_count = mgr->frames_per_anim;
    if (anim->vram_frame_count > anim->avif.frame_count) {
        anim->vram_frame_count = anim->avif.frame_count;
    }

    anim->vram_frames = zalloc(anim->vram_frame_count, sizeof(struct anim_vram_frame));
    anim->vram_frame_head = 0;

    // Allocate RAM cache (1.5x VRAM buffer size)
    anim->ram_cache_capacity = anim->vram_frame_count + (anim->vram_frame_count / 2);
    if (anim->ram_cache_capacity > anim->avif.frame_count) {
        anim->ram_cache_capacity = anim->avif.frame_count;
    }
    anim->ram_cache = zalloc(anim->ram_cache_capacity, sizeof(struct anim_ram_frame));
    anim->ram_cache_count = 0;

    // Upload initial frames
    for (size_t i = 0; i < anim->vram_frame_count && i < anim->avif.frame_count; i++) {
        upload_frame_to_vram(mgr, anim, &anim->vram_frames[i], i);
    }

    // Add to manager
    if (mgr->animation_count >= mgr->animation_capacity) {
        mgr->animation_capacity = mgr->animation_capacity == 0 ? 8 : mgr->animation_capacity * 2;
        mgr->animations = realloc(mgr->animations, mgr->animation_capacity * sizeof(struct animation *));
        check_alloc(mgr->animations);
    }

    mgr->animations[mgr->animation_count++] = anim;

    // Recalculate budget based on new animation count
    animation_manager_recalculate_budget(mgr);

    ww_log(LOG_INFO, "created animation: %s (%d frames, %dx%d) - VRAM usage: %.2f MB / %.2f MB",
           avif_path, (int)anim->avif.frame_count, anim->width, anim->height,
           mgr->vram_used / (1024.0 * 1024.0), mgr->vram_budget / (1024.0 * 1024.0));

    return anim;
}

void animation_ref(struct animation *anim) {
    if (anim) {
        anim->ref_count++;
    }
}

void animation_unref(struct animation_manager *mgr, struct animation *anim) {
    if (!anim) {
        return;
    }

    anim->ref_count--;
    if (anim->ref_count > 0) {
        return;
    }

    // Remove from manager
    for (size_t i = 0; i < mgr->animation_count; i++) {
        if (mgr->animations[i] == anim) {
            // Shift remaining animations
            memmove(&mgr->animations[i], &mgr->animations[i + 1],
                    (mgr->animation_count - i - 1) * sizeof(struct animation *));
            mgr->animation_count--;
            break;
        }
    }

    // Free VRAM frames
    for (size_t i = 0; i < anim->vram_frame_count; i++) {
        evict_frame_from_vram(mgr, &anim->vram_frames[i], anim->width, anim->height);
    }
    free(anim->vram_frames);

    // Free RAM cache
    for (size_t i = 0; i < anim->ram_cache_count; i++) {
        free(anim->ram_cache[i].data);
    }
    free(anim->ram_cache);

    // Free AVIF data
    util_avif_free(&anim->avif);

    free(anim->avif_path);
    free(anim);

    // Recalculate budget after removing animation
    animation_manager_recalculate_budget(mgr);
}

void animation_manager_update(struct animation_manager *mgr, double current_time) {
    for (size_t i = 0; i < mgr->animation_count; i++) {
        struct animation *anim = mgr->animations[i];
        if (!anim || !anim->is_playing || anim->avif.frame_count <= 1) {
            continue;
        }

        // Check if it's time to advance to the next frame
        double frame_duration = anim->avif.frames[anim->current_frame].duration;
        if (current_time - anim->last_update_time >= frame_duration) {
            anim->current_frame = (anim->current_frame + 1) % anim->avif.frame_count;
            anim->last_update_time = current_time;

            // Preload next frames if needed
            size_t next_frame = (anim->current_frame + anim->vram_frame_count) % anim->avif.frame_count;

            // Check if next frame is already uploaded
            bool found = false;
            for (size_t j = 0; j < anim->vram_frame_count; j++) {
                if (anim->vram_frames[j].is_uploaded &&
                    anim->vram_frames[j].frame_index == next_frame) {
                    found = true;
                    break;
                }
            }

            // If not found, upload it (evict oldest if needed)
            if (!found) {
                size_t victim_idx = anim->vram_frame_head;
                anim->vram_frame_head = (anim->vram_frame_head + 1) % anim->vram_frame_count;

                evict_frame_from_vram(mgr, &anim->vram_frames[victim_idx], anim->width,
                                      anim->height);
                upload_frame_to_vram(mgr, anim, &anim->vram_frames[victim_idx], next_frame);
            }
        }
    }
}

GLuint animation_get_current_texture(struct animation_manager *mgr, struct animation *anim) {
    if (!anim || anim->avif.frame_count == 0) {
        return 0;
    }

    size_t target_frame = anim->current_frame;

    for (size_t i = 0; i < anim->vram_frame_count; i++) {
        if (anim->vram_frames[i].is_uploaded && anim->vram_frames[i].frame_index == target_frame) {
            return anim->vram_frames[i].texture;
        }
    }

    if (anim->vram_frame_count == 0) {
        return 0;
    }

    size_t upload_idx = anim->vram_frame_head;
    evict_frame_from_vram(mgr, &anim->vram_frames[upload_idx], anim->width, anim->height);
    upload_frame_to_vram(mgr, anim, &anim->vram_frames[upload_idx], target_frame);
    anim->vram_frame_head = (anim->vram_frame_head + 1) % anim->vram_frame_count;

    return anim->vram_frames[upload_idx].texture;
}

void animation_play(struct animation *anim) {
    if (anim) {
        anim->is_playing = true;
    }
}

void animation_pause(struct animation *anim) {
    if (anim) {
        anim->is_playing = false;
    }
}

void animation_reset(struct animation *anim) {
    if (anim) {
        anim->current_frame = 0;
        anim->last_update_time = 0.0;
    }
}

void animation_manager_recalculate_budget(struct animation_manager *mgr) {
    if (mgr->animation_count == 0) {
        mgr->frames_per_anim = mgr->max_frames_per_anim;
        return;
    }

    // Estimate average frame size (assume 32x32 RGBA)
    size_t avg_frame_size = 32 * 32 * 4;

    // Calculate frames per animation based on budget
    size_t budget_per_anim = mgr->vram_budget / mgr->animation_count;
    size_t calculated_frames = budget_per_anim / avg_frame_size;

    // Clamp to min/max
    if (calculated_frames < mgr->min_frames_per_anim) {
        calculated_frames = mgr->min_frames_per_anim;
    } else if (calculated_frames > mgr->max_frames_per_anim) {
        calculated_frames = mgr->max_frames_per_anim;
    }

    mgr->frames_per_anim = calculated_frames;
}