#ifndef WAYWALL_SCENE_H
#define WAYWALL_SCENE_H

#include "config/config.h"
#include "util/box.h"
#include <GLES2/gl2.h>
#include <ft2build.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include FT_FREETYPE_H
#include "server/gl.h"

#define SHADER_SRC_POS_ATTRIB_LOC 0
#define SHADER_DST_POS_ATTRIB_LOC 1
#define SHADER_SRC_RGBA_ATTRIB_LOC 2
#define SHADER_DST_RGBA_ATTRIB_LOC 3

// represents a single character to draw, with color
struct text_char {
    uint32_t c;    // utf8 codepoint
    float rgba[4]; // color
};

// single glyph's data
struct glyph_metadata {
    int width, height;
    int bearingX, bearingY;
    unsigned int advance;

    // atlas position
    int atlas_x, atlas_y;

    uint32_t character;

    bool needs_gpu_upload;
    unsigned char *bitmap_data;
};

// all glyphs for a given font size in a dynamic atlas
struct font_size_obj {
    size_t font_height;

    struct glyph_metadata *glyphs;
    size_t next_glyph_index; // number of glyphs loaded
    size_t glyphs_capacity;  // allocated size of chars array

    // atlas
    GLuint atlas_tex;
    int atlas_width;
    int atlas_height;
    int atlas_x;
    int atlas_y;
    int atlas_row_height;
};

struct Custom_atlas {
    GLuint tex;
    u_int32_t width;
};

struct scene {
    struct server_gl *gl;
    struct server_ui *ui;

    uint32_t image_max_size;

    struct {
        struct scene_shader *data;
        size_t count;
    } shaders;

    struct {
        unsigned int debug;
        size_t debug_vtxcount;

        unsigned int stencil_rect;
    } buffers;

    struct {
        int32_t width, height;
        int32_t tex_width, tex_height;
        uint32_t equal_frames;
    } prev_frame;

    struct {
        struct wl_list sorted; // scene_object.link

        struct wl_list unsorted_images;  // scene_object.link
        struct wl_list unsorted_mirrors; // scene_object.link
        struct wl_list unsorted_text;    // scene_object.link
    } objects;

    int skipped_frames;

    struct wl_listener on_gl_frame;

    struct {
        FT_Library ft;
        FT_Face face;

        size_t last_height; // last height set in freetype

        struct font_size_obj *fonts; // array of font sizes
        size_t fonts_len;
    } font;

    struct Custom_atlas *atlas_arr;
    size_t atlas_arr_len;
};

struct scene_shader {
    struct server_gl_shader *shader;
    int shader_u_src_size, shader_u_dst_size;

    char *name;
};

struct scene_image_options {
    struct box dst;

    int32_t depth;
    char *shader_name;
};

struct scene_image_from_atlas_options {
    struct box dst;
    struct box src;

    struct Custom_atlas *atlas;

    int32_t depth;
    char *shader_name;
};

struct scene_mirror_options {
    struct box src, dst;
    float src_rgba[4];
    float dst_rgba[4];

    int32_t depth;
    char *shader_name;
};

struct scene_text_options {
    int32_t x;
    int32_t y;

    int32_t size;
    int32_t atlas_index;

    int32_t depth;
    char *shader_name;

    int32_t line_spacing;
};

struct scene_object;

struct scene *scene_create(struct config *cfg, struct server_gl *gl, struct server_ui *ui);
void scene_destroy(struct scene *scene);

struct scene_image *scene_add_image(struct scene *scene, const struct scene_image_options *options,
                                    const char *path);
struct scene_image *
scene_add_image_from_atlas(struct scene *scene,
                           const struct scene_image_from_atlas_options *options);
struct scene_mirror *scene_add_mirror(struct scene *scene,
                                      const struct scene_mirror_options *options);
struct scene_text *scene_add_text(struct scene *scene, const char *data,
                                  const struct scene_text_options *options);
struct Custom_atlas *scene_create_atlas(struct scene *scene, const uint32_t width);

void scene_atlas_raw_image(struct scene *scene, struct Custom_atlas *atlas, const char *data,
                           size_t data_len, u_int32_t x, uint32_t y);
void scene_atlas_destroy(struct Custom_atlas *atlas);

void scene_object_destroy(struct scene_object *object);
int32_t scene_object_get_depth(struct scene_object *object);
void scene_object_set_depth(struct scene_object *object, int32_t depth);
void scene_object_hide(struct scene_object *object);
void scene_object_show(struct scene_object *object);

struct advance_ret {
    int32_t x;
    int32_t y;
};

struct advance_ret text_get_advance(struct scene *scene, const char *data, const size_t data_len,
                                    const u_int32_t size);

#endif
