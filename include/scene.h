#ifndef WAYWALL_SCENE_H
#define WAYWALL_SCENE_H

#include "config/config.h"
#include "util/box.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "server/vk.h"

struct scene {
    struct server_vk *vk;
    struct server_ui *ui;

    bool force_composition; // Draw capture as background (for cross-GPU support)
    bool vk_active;         // Vulkan backend is handling game rendering (skip game background)

    struct wl_list objects; // scene_object.link
};

enum scene_object_type {
    SCENE_OBJECT_IMAGE,
    SCENE_OBJECT_MIRROR,
    SCENE_OBJECT_TEXT,
};

struct scene_object {
    struct wl_list link; // scene.objects
    struct scene *parent;
    enum scene_object_type type;

    // Pointer to underlying Vulkan object (vk_image, vk_mirror, vk_text)
    void *vk_obj;
};

struct scene_image {
    struct scene_object object;
};

struct scene_mirror {
    struct scene_object object;
};

struct scene_text {
    struct scene_object object;
};

struct scene_image_options {
    struct box dst;
    int32_t depth;
};

struct scene_image_from_atlas_options {
    struct box dst;
    struct box src;
    struct vk_atlas *atlas;
    int32_t depth;
};

struct scene_mirror_options {
    struct box src, dst;
    float src_rgba[4];
    float dst_rgba[4];
    int32_t depth;
    bool color_key_enabled;
    uint32_t color_key_input;
    uint32_t color_key_output;
    float color_key_tolerance;
};

struct scene_text_options {
    int32_t x;
    int32_t y;
    int32_t size;
    int32_t line_spacing;
    uint32_t color; // RGBA
    int32_t depth;
};

struct scene_animated_image_options {
    struct box dst;
    int32_t depth;
};

struct scene *scene_create(struct config *cfg, struct server_vk *vk, struct server_ui *ui);
void scene_destroy(struct scene *scene);

struct scene_image *scene_add_image(struct scene *scene, const struct scene_image_options *options,
                                    const char *path);
struct scene_image *
scene_add_image_from_atlas(struct scene *scene,
                           const struct scene_image_from_atlas_options *options);
struct scene_image *scene_add_animated_image(struct scene *scene,
                                             const struct scene_animated_image_options *options,
                                             const char *avif_path);
struct scene_mirror *scene_add_mirror(struct scene *scene,
                                      const struct scene_mirror_options *options);
struct scene_text *scene_add_text(struct scene *scene, const char *data,
                                  const struct scene_text_options *options);

struct vk_atlas *scene_create_atlas(struct scene *scene, uint32_t width, const char *data, size_t len);
void scene_atlas_destroy(struct vk_atlas *atlas);
void scene_atlas_raw_image(struct scene *scene, struct vk_atlas *atlas, const char *data,
                           size_t data_len, uint32_t x, uint32_t y);
char *atlas_get_dump(struct scene *scene, struct vk_atlas *atlas, size_t *out_len);

void scene_object_destroy(struct scene_object *object);
int32_t scene_object_get_depth(struct scene_object *object);
void scene_object_set_depth(struct scene_object *object, int32_t depth);
void scene_object_hide(struct scene_object *object);
void scene_object_show(struct scene_object *object);


struct advance_ret {
    int32_t x;
    int32_t y;
};

struct advance_ret text_get_advance(struct scene *scene, const char *data, const size_t data_len, const uint32_t size);

void scene_set_vk_active(struct scene *scene, bool active);

#endif
