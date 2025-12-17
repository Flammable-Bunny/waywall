#include "scene.h"
#include "server/vk.h"
#include "util/alloc.h"
#include "util/log.h"
#include <stdlib.h>
#include <string.h>

struct scene *
scene_create(struct config *cfg, struct server_vk *vk, struct server_ui *ui) {
    struct scene *scene = zalloc(1, sizeof(*scene));
    scene->vk = vk;
    scene->ui = ui;
    scene->force_composition = cfg->experimental.force_composition;
    
    wl_list_init(&scene->objects);
    
    // We don't need to initialize GL shaders or buffers anymore as VK handles it.
    
    return scene;
}

void
scene_destroy(struct scene *scene) {
    struct scene_object *obj, *tmp;
    wl_list_for_each_safe(obj, tmp, &scene->objects, link) {
        scene_object_destroy(obj);
    }
    free(scene);
}

void
scene_set_vk_active(struct scene *scene, bool active) {
    scene->vk_active = active;
}

// Helper to add object to scene list
static void
scene_add_object(struct scene *scene, struct scene_object *obj, enum scene_object_type type, void *vk_obj) {
    obj->parent = scene;
    obj->type = type;
    obj->vk_obj = vk_obj;
    wl_list_insert(&scene->objects, &obj->link);
}

struct scene_image *
scene_add_image(struct scene *scene, const struct scene_image_options *options, const char *path) {
    struct vk_image_options vk_opts = {
        .dst = options->dst,
        .depth = options->depth,
    };

    struct vk_image *vk_img = server_vk_add_image(scene->vk, path, &vk_opts);
    if (!vk_img) return NULL;

    struct scene_image *img = zalloc(1, sizeof(*img));
    scene_add_object(scene, &img->object, SCENE_OBJECT_IMAGE, vk_img);
    
    return img;
}

struct scene_image *
scene_add_image_from_atlas(struct scene *scene, const struct scene_image_from_atlas_options *options) {
    struct vk_image_options vk_opts = {
        .dst = options->dst,
        .depth = options->depth,
    };

    struct vk_image *vk_img = server_vk_add_image_from_atlas(scene->vk, options->atlas, options->src, &vk_opts);
    if (!vk_img) return NULL;

    struct scene_image *img = zalloc(1, sizeof(*img));
    scene_add_object(scene, &img->object, SCENE_OBJECT_IMAGE, vk_img);

    return img;
}

struct scene_image *
scene_add_animated_image(struct scene *scene, const struct scene_animated_image_options *options, const char *avif_path) {
    struct vk_image_options vk_opts = {
        .dst = options->dst,
        .depth = options->depth,
    };

    struct vk_image *vk_img = server_vk_add_avif_image(scene->vk, avif_path, &vk_opts);
    if (!vk_img) return NULL;

    struct scene_image *img = zalloc(1, sizeof(*img));
    scene_add_object(scene, &img->object, SCENE_OBJECT_IMAGE, vk_img);

    return img;
}

struct scene_mirror *
scene_add_mirror(struct scene *scene, const struct scene_mirror_options *options) {
    struct vk_mirror_options vk_opts = {
        .src = options->src,
        .dst = options->dst,
        .depth = options->depth,
        .color_key_enabled = options->color_key_enabled,
        .color_key_input = options->color_key_input,
        .color_key_output = options->color_key_output,
        .color_key_tolerance = options->color_key_tolerance,
    };

    struct vk_mirror *vk_mirror = server_vk_add_mirror(scene->vk, &vk_opts);
    if (!vk_mirror) return NULL;

    struct scene_mirror *mirror = zalloc(1, sizeof(*mirror));
    scene_add_object(scene, &mirror->object, SCENE_OBJECT_MIRROR, vk_mirror);

    return mirror;
}

struct scene_text *
scene_add_text(struct scene *scene, const char *data, const struct scene_text_options *options) {
    struct vk_text_options vk_opts = {
        .x = options->x,
        .y = options->y,
        .size = (uint32_t)options->size,
        .line_spacing = options->line_spacing,
        .color = options->color,
        .depth = options->depth,
    };

    struct vk_text *vk_text = server_vk_add_text(scene->vk, data, &vk_opts);
    if (!vk_text) return NULL;

    struct scene_text *text = zalloc(1, sizeof(*text));
    scene_add_object(scene, &text->object, SCENE_OBJECT_TEXT, vk_text);
    
    return text;
}

struct vk_atlas *
scene_create_atlas(struct scene *scene, uint32_t width, const char *data, size_t len) {
    return server_vk_create_atlas(scene->vk, width, data, len);
}

void
scene_atlas_destroy(struct vk_atlas *atlas) {
    server_vk_atlas_unref(atlas);
}

void
scene_atlas_raw_image(struct scene *scene, struct vk_atlas *atlas, const char *data, size_t data_len, uint32_t x, uint32_t y) {
    server_vk_atlas_insert_raw(atlas, data, data_len, x, y);
}

char *
atlas_get_dump(struct scene *scene, struct vk_atlas *atlas, size_t *out_len) {
    return server_vk_atlas_get_dump(atlas, out_len);
}

void
scene_object_destroy(struct scene_object *object) {
    if (!object) return;

    wl_list_remove(&object->link);

    switch (object->type) {
        case SCENE_OBJECT_IMAGE:
            server_vk_remove_image(object->parent->vk, (struct vk_image *)object->vk_obj);
            break;
        case SCENE_OBJECT_MIRROR:
            server_vk_remove_mirror(object->parent->vk, (struct vk_mirror *)object->vk_obj);
            break;
        case SCENE_OBJECT_TEXT:
            server_vk_remove_text(object->parent->vk, (struct vk_text *)object->vk_obj);
            break;
    }
    
    free(object);
}

int32_t
scene_object_get_depth(struct scene_object *object) {
    // We'd need to access the underlying struct. 
    // Since we know the memory layout or can cast, let's just cast for now.
    // However, the vk objects don't share a common 'depth' at the same offset strictly speaking unless valid standard C.
    // But we know concrete types.
    switch (object->type) {
        case SCENE_OBJECT_IMAGE:
            return ((struct vk_image *)object->vk_obj)->depth;
        case SCENE_OBJECT_MIRROR:
            return ((struct vk_mirror *)object->vk_obj)->depth;
        case SCENE_OBJECT_TEXT:
            return ((struct vk_text *)object->vk_obj)->depth;
    }
    return 0;
}

void
scene_object_set_depth(struct scene_object *object, int32_t depth) {
    switch (object->type) {
        case SCENE_OBJECT_IMAGE:
            ((struct vk_image *)object->vk_obj)->depth = depth;
            break;
        case SCENE_OBJECT_MIRROR:
            ((struct vk_mirror *)object->vk_obj)->depth = depth;
            break;
        case SCENE_OBJECT_TEXT:
            ((struct vk_text *)object->vk_obj)->depth = depth;
            break;
    }
}

void
scene_object_hide(struct scene_object *object) {
    switch (object->type) {
        case SCENE_OBJECT_IMAGE:
            server_vk_image_set_enabled((struct vk_image *)object->vk_obj, false);
            break;
        case SCENE_OBJECT_MIRROR:
            server_vk_mirror_set_enabled((struct vk_mirror *)object->vk_obj, false);
            break;
        case SCENE_OBJECT_TEXT:
            server_vk_text_set_enabled((struct vk_text *)object->vk_obj, false);
            break;
    }
}

void
scene_object_show(struct scene_object *object) {
    switch (object->type) {
        case SCENE_OBJECT_IMAGE:
            server_vk_image_set_enabled((struct vk_image *)object->vk_obj, true);
            break;
        case SCENE_OBJECT_MIRROR:
            server_vk_mirror_set_enabled((struct vk_mirror *)object->vk_obj, true);
            break;
        case SCENE_OBJECT_TEXT:
            server_vk_text_set_enabled((struct vk_text *)object->vk_obj, true);
            break;
    }
}

struct advance_ret
text_get_advance(struct scene *scene, const char *data, const size_t data_len, const u_int32_t size) {
    struct vk_advance_ret ret = server_vk_text_advance(scene->vk, data, data_len, size);
    return (struct advance_ret){ .x = ret.x, .y = ret.y };
}
