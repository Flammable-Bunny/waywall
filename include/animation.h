#ifndef WAYWALL_ANIMATION_H
#define WAYWALL_ANIMATION_H

#include "util/avif.h"
#include <GLES2/gl2.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// Forward declarations
struct scene;
struct server_gl;

// Represents a single frame in VRAM
struct anim_vram_frame {
    GLuint texture;
    bool is_uploaded;
    size_t frame_index;
};

// Represents a decoded frame in RAM (not yet uploaded to VRAM)
struct anim_ram_frame {
    char *data;
    size_t size;
    size_t frame_index;
    uint64_t last_used;
};

// Animation instance
struct animation {
    char *avif_path;
    struct util_avif avif;

    // Current playback state
    size_t current_frame;
    double last_update_time;
    bool is_playing;

    // VRAM frame ring buffer
    struct anim_vram_frame *vram_frames;
    size_t vram_frame_count;
    size_t vram_frame_head;

    // RAM cache for decoded frames
    struct anim_ram_frame *ram_cache;
    size_t ram_cache_capacity;
    size_t ram_cache_count;

    // Dimensions
    int32_t width;
    int32_t height;

    int ref_count;
};

// Global animation manager
struct animation_manager {
    struct scene *scene;
    struct server_gl *gl;

    // Array of all animations
    struct animation **animations;
    size_t animation_count;
    size_t animation_capacity;

    // VRAM budget management
    size_t vram_total;
    size_t vram_budget;
    size_t vram_used;
    size_t frames_per_anim;

    // Configuration
    size_t min_frames_per_anim;
    size_t max_frames_per_anim;
};

// Initialize the animation manager
// Returns NULL on failure
struct animation_manager *animation_manager_create(struct scene *scene, struct server_gl *gl);

// Destroy the animation manager and all animations
void animation_manager_destroy(struct animation_manager *mgr);

// Create a new animation from an AVIF file
// Returns NULL on failure
struct animation *animation_create(struct animation_manager *mgr, const char *avif_path);

// Increment reference count
void animation_ref(struct animation *anim);

// Decrement reference count and free if reaches zero
void animation_unref(struct animation_manager *mgr, struct animation *anim);

// Updates frame indices based on timing
void animation_manager_update(struct animation_manager *mgr, double current_time);

// Uploads frames to VRAM as needed
GLuint animation_get_current_texture(struct animation_manager *mgr, struct animation *anim);

// Start playing an animation
void animation_play(struct animation *anim);

// Pause an animation
void animation_pause(struct animation *anim);

// Reset animation to frame 0
void animation_reset(struct animation *anim);

// Recalculate VRAM budget based on current system state
void animation_manager_recalculate_budget(struct animation_manager *mgr);

#endif
