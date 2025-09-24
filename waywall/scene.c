#include "config/config.h"
#include "glsl/texcopy.frag.h"
#include "glsl/texcopy.vert.h"
#include "glsl/text.frag.h"

#include "scene.h"
#include "server/gl.h"
#include "server/ui.h"
#include "util/alloc.h"
#include "util/debug.h"
#include "util/log.h"
#include "util/png.h"
#include "util/prelude.h"
#include <GLES2/gl2.h>
#include <ft2build.h>
#include <spng.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <wayland-util.h>
#include FT_FREETYPE_H

#define FONT_ATLAS_WIDTH 1024
#define FONT_ATLAS_HEIGHT 1024

struct vtx_shader {
    float src_pos[2];
    float dst_pos[2];
    float src_rgba[4];
    float dst_rgba[4];
};

enum scene_object_type {
    SCENE_OBJECT_IMAGE,
    SCENE_OBJECT_MIRROR,
    SCENE_OBJECT_TEXT,
};

struct scene_object {
    struct wl_list link;
    struct scene *parent;
    enum scene_object_type type;
    int32_t depth;

    bool enabled;
};

struct scene_image {
    struct scene_object object;
    struct scene *parent;

    size_t shader_index;

    GLuint tex, vbo;

    int32_t width, height;

    bool is_atlas_texture;
};

struct scene_mirror {
    struct scene_object object;
    struct scene *parent;

    size_t shader_index;

    GLuint vbo;

    float src_rgba[4], dst_rgba[4];
};

struct scene_text {
    struct scene_object object;
    struct scene *parent;

    size_t shader_index;

    GLuint vbo, tex;
    size_t vtxcount;

    int32_t x, y;
    uint32_t font_size;
};

static void object_add(struct scene *scene, struct scene_object *object,
                       enum scene_object_type type);
static void object_list_destroy(struct wl_list *list);
static void object_release(struct scene_object *object);
static void object_render(struct scene_object *object);
static void object_sort(struct scene *scene, struct scene_object *object);

static void draw_debug_text(struct scene *scene);
static void draw_frame(struct scene *scene);
static void draw_vertex_list(struct scene_shader *shader, size_t num_vertices);
static void rect_build(struct vtx_shader out[static 6], const struct box *src,
                       const struct box *dst, const float src_rgba[static 4],
                       const float dst_rgba[static 4]);
static inline struct scene_image *scene_image_from_object(struct scene_object *object);
static inline struct scene_mirror *scene_mirror_from_object(struct scene_object *object);
static inline struct scene_text *scene_text_from_object(struct scene_object *object);

static void
image_build(struct scene_image *out, struct scene *scene, const struct scene_image_options *options,
            int32_t width, int32_t height) {
    struct vtx_shader vertices[6] = {0};

    rect_build(vertices, &(struct box){0, 0, width, height}, &options->dst, (float[4]){0, 0, 0, 0},
               (float[4]){0, 0, 0, 0});

    server_gl_with(scene->gl, false) {
        glGenBuffers(1, &out->vbo);
        ww_assert(out->vbo != 0);

        gl_using_buffer(GL_ARRAY_BUFFER, out->vbo) {
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        }
    }
}

static void
image_build_from_atlas(struct scene_image *out, struct scene *scene,
                       const struct scene_image_from_atlas_options *options) {
    struct vtx_shader vertices[6] = {0};

    rect_build(vertices, &options->src, &options->dst, (float[4]){0, 0, 0, 0},
               (float[4]){0, 0, 0, 0});

    server_gl_with(scene->gl, false) {
        glGenBuffers(1, &out->vbo);
        ww_assert(out->vbo != 0);

        gl_using_buffer(GL_ARRAY_BUFFER, out->vbo) {
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        }
    }
}

static void
image_release(struct scene_object *object) {
    struct scene_image *image = scene_image_from_object(object);

    if (image->parent) {
        server_gl_with(image->parent->gl, false) {
            if (!image->is_atlas_texture) {
                glDeleteTextures(1, &image->tex);
            }
            glDeleteBuffers(1, &image->vbo);
        }
    }

    image->parent = NULL;
}

static void
image_render(struct scene_object *object) {
    // The OpenGL context must be current.
    struct scene_image *image = scene_image_from_object(object);
    struct scene *scene = image->parent;

    server_gl_shader_use(scene->shaders.data[image->shader_index].shader);
    glUniform2f(scene->shaders.data[image->shader_index].shader_u_dst_size, scene->ui->width,
                scene->ui->height);
    glUniform2f(scene->shaders.data[image->shader_index].shader_u_src_size, image->width,
                image->height);

    gl_using_buffer(GL_ARRAY_BUFFER, image->vbo) {
        gl_using_texture(GL_TEXTURE_2D, image->tex) {
            // Each image has 6 vertices in its vertex buffer.
            draw_vertex_list(&scene->shaders.data[image->shader_index], 6);
        }
    }
}

static void
mirror_build(struct scene_mirror *mirror, const struct scene_mirror_options *options,
             struct scene *scene) {
    struct vtx_shader vertices[6] = {0};

    rect_build(vertices, &options->src, &options->dst, options->src_rgba, mirror->dst_rgba);

    server_gl_with(scene->gl, false) {
        glGenBuffers(1, &mirror->vbo);
        ww_assert(mirror->vbo != 0);

        gl_using_buffer(GL_ARRAY_BUFFER, mirror->vbo) {
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
        }
    }
}

static void
mirror_release(struct scene_object *object) {
    struct scene_mirror *mirror = scene_mirror_from_object(object);

    if (mirror->parent) {
        server_gl_with(mirror->parent->gl, false) {
            glDeleteBuffers(1, &mirror->vbo);
        }
    }

    mirror->parent = NULL;
}

static void
mirror_render(struct scene_object *object) {
    // The OpenGL context must be current.

    struct scene_mirror *mirror = scene_mirror_from_object(object);
    struct scene *scene = mirror->parent;

    GLuint capture_texture = server_gl_get_capture(scene->gl);
    if (capture_texture == 0) {
        return;
    }

    int32_t width, height;
    server_gl_get_capture_size(scene->gl, &width, &height);

    server_gl_shader_use(scene->shaders.data[mirror->shader_index].shader);
    glUniform2f(scene->shaders.data[mirror->shader_index].shader_u_dst_size, scene->ui->width,
                scene->ui->height);
    glUniform2f(scene->shaders.data[mirror->shader_index].shader_u_src_size, width, height);

    gl_using_buffer(GL_ARRAY_BUFFER, mirror->vbo) {
        gl_using_texture(GL_TEXTURE_2D, capture_texture) {
            // Each mirror has 6 vertices in its vertex buffer.
            draw_vertex_list(&scene->shaders.data[mirror->shader_index], 6);
        }
    }
}

struct glyph_metadata
get_glyph(struct scene *scene, const uint32_t c, const size_t font_height) {
    struct font_size_obj *font_size_obj = NULL;

    // find existing font size object
    for (size_t i = 0; i < scene->font.fonts_len; i++) {
        if (scene->font.fonts[i].font_height == font_height) {
            font_size_obj = &scene->font.fonts[i];
            break;
        }
    }

    // create new font_size_obj if needed
    if (!font_size_obj) {
        size_t new_len = scene->font.fonts_len + 1;
        scene->font.fonts = realloc(scene->font.fonts, new_len * sizeof(struct font_size_obj));
        if (!scene->font.fonts)
            ww_panic("Out of memory");

        font_size_obj = &scene->font.fonts[scene->font.fonts_len];
        memset(font_size_obj, 0, sizeof(*font_size_obj));
        font_size_obj->font_height = font_height;
        font_size_obj->atlas_width = FONT_ATLAS_WIDTH;
        font_size_obj->atlas_height = FONT_ATLAS_HEIGHT;
        font_size_obj->atlas_x = 0;
        font_size_obj->atlas_y = 0;
        font_size_obj->atlas_row_height = 0;
        font_size_obj->glyphs_capacity = 128;
        font_size_obj->glyphs =
            calloc(font_size_obj->glyphs_capacity, sizeof(struct glyph_metadata));
        font_size_obj->next_glyph_index = 0;

        glGenTextures(1, &font_size_obj->atlas_tex);
        gl_using_texture(GL_TEXTURE_2D, font_size_obj->atlas_tex) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, font_size_obj->atlas_width,
                         font_size_obj->atlas_height, 0, GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }

        scene->font.fonts_len = new_len;
    }

    // search for existing glyph
    for (size_t i = 0; i < font_size_obj->next_glyph_index; i++) {
        if (font_size_obj->glyphs[i].character == c)
            return font_size_obj->glyphs[i];
    }

    // render new glyph
    if (scene->font.last_height != font_height) {
        FT_Set_Pixel_Sizes(scene->font.face, 0, font_height);
        scene->font.last_height = font_height;
    }

    if (FT_Load_Char(scene->font.face, c, FT_LOAD_RENDER))
        ww_panic("Failed to load glyph U+%04X\n", c);

    struct glyph_metadata data;
    data.width = (int)scene->font.face->glyph->bitmap.width;
    data.height = (int)scene->font.face->glyph->bitmap.rows;
    data.bearingX = scene->font.face->glyph->bitmap_left;
    data.bearingY = scene->font.face->glyph->bitmap_top;
    data.advance = scene->font.face->glyph->advance.x;
    data.character = c;

    const unsigned char *buffer = scene->font.face->glyph->bitmap.buffer;

    // handle row wrapping
    if (font_size_obj->atlas_x + data.width > font_size_obj->atlas_width) {
        font_size_obj->atlas_x = 0;
        font_size_obj->atlas_y += font_size_obj->atlas_row_height;
        font_size_obj->atlas_row_height = 0;
    }

    // atlas overflow check
    if (font_size_obj->atlas_y + data.height > font_size_obj->atlas_height)
        ww_panic("Atlas full, cannot add glyph U+%04X\n", c);

    data.atlas_x = font_size_obj->atlas_x;
    data.atlas_y = font_size_obj->atlas_y;

    data.needs_gpu_upload = true;
    data.bitmap_data = malloc(data.width * data.height);
    if (data.bitmap_data && buffer) {
        memcpy(data.bitmap_data, buffer, data.width * data.height);
    }

    // update atlas placement
    font_size_obj->atlas_x += data.width;
    if (data.height > font_size_obj->atlas_row_height)
        font_size_obj->atlas_row_height = data.height;

    // store glyph in array
    if (font_size_obj->next_glyph_index >= font_size_obj->glyphs_capacity) {
        font_size_obj->glyphs_capacity *= 2;
        struct glyph_metadata *tmp = realloc(
            font_size_obj->glyphs, font_size_obj->glyphs_capacity * sizeof(struct glyph_metadata));
        if (!tmp)
            ww_panic("Out of memory");
        font_size_obj->glyphs = tmp;
    }
    font_size_obj->glyphs[font_size_obj->next_glyph_index++] = data;

    return data;
}

/*
Decodes a single UTF-8 character into a Unicode codepoint.
Advances *str to the next character.
*/
static uint32_t
utf8_decode(const char **str) {
    const unsigned char *s = (const unsigned char *)*str;
    uint32_t cp;

    if (s[0] < 0x80) {
        // 1-byte ASCII
        cp = s[0];
        *str += 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        // 2-byte
        cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *str += 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        // 3-byte
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *str += 3;
    } else if ((s[0] & 0xF8) == 0xF0) {
        // 4-byte
        cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        *str += 4;
    } else {
        // Invalid byte, skip
        cp = 0xFFFD; // replacement char
        *str += 1;
    }

    return cp;
}

static int
parse_hex_digit(const u_int32_t c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

static int
parse_color(const u_int32_t *s, float rgba[4]) {
    for (int i = 0; i < 4; i++) {
        const int hi = parse_hex_digit(s[i * 2]);
        const int lo = parse_hex_digit(s[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        const int val = (hi << 4) | lo;
        rgba[i] = (float)val / 255.0f;
    }
    return 0;
}

static size_t
text_build(GLuint vbo, struct scene *scene, const char *data, const size_t data_len,
           const struct scene_text_options *options) {
    // The OpenGL context must be current.

    // decode utf8
    size_t capacity = 16;
    size_t next_index = 0;
    u_int32_t *cps = zalloc(capacity, sizeof(u_int32_t));
    const char *c = data;
    while (*c != '\0') {
        if (next_index >= capacity) {
            capacity *= 2;
            u_int32_t *tmp = realloc(cps, capacity * sizeof(u_int32_t));
            if (tmp == NULL) {
                ww_panic("Out of memory");
            }
            cps = tmp;
        }
        cps[next_index++] = utf8_decode(&c);
    }

    const size_t cps_len = next_index;

    // handle color tags
    float current_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    next_index = 0;
    capacity = 16;
    struct text_char *text_chars = zalloc(16, sizeof(struct text_char));

    for (size_t i = 0; i < cps_len; i++) {
        if (i + 1 < cps_len && cps[i] == '<' && cps[i + 1] == '#') {
            // <#FFFFFFFF>
            if (cps_len - i >= 11) {
                if (parse_color(cps + i + 2, current_color) == 0 && cps[i + 10] == '>') {
                    i += 10;
                    continue;
                }
            }
        }

        if (i + 1 < cps_len && cps[i] == '<' && cps[i + 1] == '+') {
            size_t j = i + 2;
            char buf[64];
            size_t k = 0;

            // copy digits until '>' or buffer full
            while (j < cps_len && cps[j] != '>' && k < sizeof(buf) - 1) {
                if (cps[j] < '0' || cps[j] > '9')
                    break;
                buf[k++] = (char)cps[j];
                j++;
            }
            buf[k] = '\0';

            if (cps[j] == '>') {
                int adv = atoi(buf);

                if (next_index >= capacity) {
                    capacity *= 2;
                    struct text_char *tmp = realloc(text_chars, capacity * sizeof *text_chars);
                    if (!tmp)
                        ww_panic("Out of memory");
                    text_chars = tmp;
                }

                text_chars[next_index].c = 0;
                text_chars[next_index].advance = adv;
                memcpy(text_chars[next_index++].rgba, current_color, sizeof(float) * 4);

                i = j;
                continue;
            }
        }

        if (next_index >= capacity) {
            capacity *= 2;
            struct text_char *tmp = realloc(text_chars, capacity * sizeof(struct text_char));
            if (tmp == NULL) {
                ww_panic("Out of memory");
            }
            text_chars = tmp;
        }
        text_chars[next_index].c = cps[i];
        text_chars[next_index].advance = 0;
        memcpy(text_chars[next_index++].rgba, current_color, sizeof(float) * 4);
    }

    free(cps);
    const size_t character_count = next_index;

    // get glyphs
    next_index = 0;
    struct glyph_metadata *glyphs = zalloc(character_count, sizeof(struct glyph_metadata));
    for (size_t i = 0; i < character_count; i++)
        glyphs[i] = get_glyph(scene, text_chars[i].c, options->size);

    // build the VBOs

    size_t vtxcount = character_count * 6;

    struct vtx_shader *vertices = zalloc(vtxcount, sizeof(*vertices));
    struct vtx_shader *ptr = vertices;

    int32_t x = options->x;
    int32_t y = options->y;

    for (size_t i = 0; i < character_count; i++) {
        const struct glyph_metadata g = glyphs[i];

        if (g.character == '\n') {
            y += options->size + options->line_spacing;
            x = options->x;
            continue;
        }

        if (text_chars[i].advance != 0) {
            x += text_chars[i].advance;
            continue;
        }

        struct box src = {
            .x = g.atlas_x,
            .y = g.atlas_y,
            .width = g.width,
            .height = g.height,
        };

        struct box dst = {
            .x = x + g.bearingX,
            .y = y - g.bearingY,
            .width = g.width,
            .height = g.height,
        };

        // src rgba is not used
        rect_build(ptr, &src, &dst, (float[4]){0.0f, 0.0f, 0.0f, 0.0f}, text_chars[i].rgba);
        ptr += 6;

        x += (int)(g.advance >> 6);
    }

    gl_using_buffer(GL_ARRAY_BUFFER, vbo) {
        glBufferData(GL_ARRAY_BUFFER, vtxcount * sizeof(*vertices), vertices, GL_STATIC_DRAW);
    }

    free(text_chars);
    free(glyphs);
    free(vertices);

    return vtxcount;
}

struct advance_ret
text_get_advance(struct scene *scene, const char *data, const size_t data_len,
                 const u_int32_t size) {

    // decode utf8
    size_t capacity = 16;
    size_t next_index = 0;
    u_int32_t *cps = zalloc(capacity, sizeof(u_int32_t));
    const char *c = data;
    while (*c != '\0') {
        if (next_index >= capacity) {
            capacity *= 2;
            u_int32_t *tmp = realloc(cps, capacity * sizeof(u_int32_t));
            if (tmp == NULL) {
                ww_panic("Out of memory");
            }
            cps = tmp;
        }
        cps[next_index++] = utf8_decode(&c);
    }

    const size_t cps_len = next_index;

    // remove color tags
    next_index = 0;
    capacity = 16;
    struct text_char {
        uint32_t c;
        int advance;
    };
    struct text_char *text_chars = zalloc(16, sizeof(struct text_char));
    int pending_advance = 0;

    for (size_t i = 0; i < cps_len; i++) {
        if (i + 1 < cps_len && cps[i] == '<' && cps[i + 1] == '#') {
            if (cps_len - i >= 11) {
                float dummy_color[4];
                if (parse_color(cps + i + 2, dummy_color) == 0 && cps[i + 10] == '>') {
                    i += 10;
                    continue;
                }
            }
        }

        if (i + 1 < cps_len && cps[i] == '<' && cps[i + 1] == '+') {
            size_t j = i + 2;
            char buf[64];
            size_t k = 0;
            while (j < cps_len && cps[j] != '>' && k < sizeof(buf) - 1) {
                if (cps[j] < '0' || cps[j] > '9')
                    break;
                buf[k++] = (char)cps[j];
                j++;
            }
            buf[k] = '\0';
            if (cps[j] == '>') {
                pending_advance = atoi(buf);
                i = j;
                continue;
            }
        }

        if (next_index >= capacity) {
            capacity *= 2;
            struct text_char *tmp = realloc(text_chars, capacity * sizeof(*text_chars));
            if (!tmp)
                ww_panic("Out of memory");
            text_chars = tmp;
        }

        text_chars[next_index].c = cps[i];
        text_chars[next_index].advance = pending_advance;
        next_index++;
        pending_advance = 0;
    }

    free(cps);
    const size_t character_count = next_index;

    // calculate width
    int32_t x = 0;
    int32_t y = 0;

    for (size_t i = 0; i < character_count; i++) {
        const uint32_t ch = text_chars[i].c;

        if (ch == '\n') {
            x = 0;
            y += size;
            continue;
        }

        x += text_chars[i].advance;

        struct glyph_metadata glyph = get_glyph(scene, ch, size);
        x += (int)(glyph.advance >> 6);
    }

    free(text_chars);

    return (struct advance_ret){.x = x, .y = y};
}

static void
text_release(struct scene_object *object) {
    struct scene_text *text = scene_text_from_object(object);

    if (text->parent) {
        server_gl_with(text->parent->gl, false) {
            glDeleteBuffers(1, &text->vbo);
        }
    }

    text->parent = NULL;
}

static void
upload_pending_glyphs(struct scene *scene, struct font_size_obj *font_obj) {
    bool has_pending = false;

    for (size_t i = 0; i < font_obj->next_glyph_index; i++) {
        if (font_obj->glyphs[i].needs_gpu_upload) {
            has_pending = true;
            break;
        }
    }

    if (!has_pending)
        return;

    gl_using_texture(GL_TEXTURE_2D, font_obj->atlas_tex) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        for (size_t i = 0; i < font_obj->next_glyph_index; i++) {
            struct glyph_metadata *glyph = &font_obj->glyphs[i];

            if (glyph->needs_gpu_upload && glyph->bitmap_data) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, glyph->atlas_x, glyph->atlas_y, glyph->width,
                                glyph->height, GL_ALPHA, GL_UNSIGNED_BYTE, glyph->bitmap_data);

                // Clean up
                free(glyph->bitmap_data);
                glyph->bitmap_data = NULL;
                glyph->needs_gpu_upload = false;
            }
        }
    }
}

static void
text_render(struct scene_object *object) {
    // The OpenGL context must be current.
    struct scene_text *text = scene_text_from_object(object);
    struct scene *scene = text->parent;

    // find font obj
    struct font_size_obj *font_obj = NULL;
    for (size_t i = 0; i < scene->font.fonts_len; i++) {
        if (scene->font.fonts[i].font_height == text->font_size) {
            font_obj = &scene->font.fonts[i];
            break;
        }
    }

    if (!font_obj)
        return;

    upload_pending_glyphs(scene, font_obj);

    server_gl_shader_use(scene->shaders.data[text->shader_index].shader);
    glUniform2f(scene->shaders.data[text->shader_index].shader_u_dst_size, scene->ui->width,
                scene->ui->height);
    glUniform2f(scene->shaders.data[text->shader_index].shader_u_src_size, FONT_ATLAS_WIDTH,
                FONT_ATLAS_HEIGHT);

    gl_using_buffer(GL_ARRAY_BUFFER, text->vbo) {
        gl_using_texture(GL_TEXTURE_2D, font_obj->atlas_tex) {
            draw_vertex_list(&scene->shaders.data[text->shader_index], text->vtxcount);
        }
    }
}
static void
on_gl_frame(struct wl_listener *listener, void *data) {
    struct scene *scene = wl_container_of(listener, scene, on_gl_frame);

    server_gl_with(scene->gl, true) {
        draw_frame(scene);
    }
}

static void
object_add(struct scene *scene, struct scene_object *object, enum scene_object_type type) {
    object->parent = scene;
    object->type = type;
    object_sort(scene, object);
}

static void
object_list_destroy(struct wl_list *list) {
    struct scene_object *object, *tmp;

    wl_list_for_each_safe (object, tmp, list, link) {
        wl_list_remove(&object->link);
        wl_list_init(&object->link);

        object_release(object);
    }
}

static void
object_release(struct scene_object *object) {
    switch (object->type) {
    case SCENE_OBJECT_IMAGE:
        image_release(object);
        break;
    case SCENE_OBJECT_MIRROR:
        mirror_release(object);
        break;
    case SCENE_OBJECT_TEXT:
        text_release(object);
        break;
    }
}

static void
object_render(struct scene_object *object) {
    switch (object->type) {
    case SCENE_OBJECT_IMAGE:
        image_render(object);
        break;
    case SCENE_OBJECT_MIRROR:
        mirror_render(object);
        break;
    case SCENE_OBJECT_TEXT:
        text_render(object);
        break;
    }
}

static void
object_sort(struct scene *scene, struct scene_object *object) {
    if (object->depth == 0) {
        switch (object->type) {
        case SCENE_OBJECT_IMAGE:
            wl_list_insert(&scene->objects.unsorted_images, &object->link);
            break;
        case SCENE_OBJECT_MIRROR:
            wl_list_insert(&scene->objects.unsorted_mirrors, &object->link);
            break;
        case SCENE_OBJECT_TEXT:
            wl_list_insert(&scene->objects.unsorted_text, &object->link);
            break;
        }

        return;
    }

    struct scene_object *needle = NULL, *prev = NULL;
    wl_list_for_each (needle, &scene->objects.sorted, link) {
        if (needle->depth >= object->depth) {
            break;
        }

        prev = needle;
    }

    if (prev) {
        wl_list_insert(&prev->link, &object->link);
    } else {
        wl_list_insert(&scene->objects.sorted, &object->link);
    }
}

static void
draw_stencil(struct scene *scene) {
    // The OpenGL context must be current.

    int32_t width, height;
    GLuint tex = server_gl_get_capture(scene->gl);
    if (tex == 0) {
        return;
    }
    server_gl_get_capture_size(scene->gl, &width, &height);

    // It would be possible to listen for resizes instead of checking whether the stencil buffer
    // needs an update every frame, but that would be more complicated and there is also no event
    // for the game being resized (as of writing this comment, at least.)
    //
    // TODO: Is it possible to avoid the frame of leeway? Would also be nice to avoid it in
    // draw_frame. This stuff really should be synchronized with the surface content. Might be worth
    // always doing compositing if scene objects are visible but I'm not very happy with that
    // solution.
    bool stencil_equal = scene->ui->width == scene->prev_frame.width &&
                         scene->ui->height == scene->prev_frame.height &&
                         width == scene->prev_frame.tex_width &&
                         height == scene->prev_frame.tex_height;
    if (stencil_equal) {
        scene->prev_frame.equal_frames++;
        if (scene->prev_frame.equal_frames > 1) {
            return;
        }
    }

    scene->prev_frame.width = scene->ui->width;
    scene->prev_frame.height = scene->ui->height;
    scene->prev_frame.tex_width = width;
    scene->prev_frame.tex_height = height;
    scene->prev_frame.equal_frames = 0;

    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_STENCIL_BUFFER_BIT);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);

    struct box dst = {
        .x = (scene->ui->width / 2) - (width / 2),
        .y = (scene->ui->height / 2) - (height / 2),
        .width = width,
        .height = height,
    };

    struct vtx_shader buf[6];
    rect_build(buf, &(struct box){0, 0, 1, 1}, &dst, (float[4]){0}, (float[4]){0});
    gl_using_buffer(GL_ARRAY_BUFFER, scene->buffers.stencil_rect) {
        gl_using_texture(GL_TEXTURE_2D, tex) {
            glBufferData(GL_ARRAY_BUFFER, sizeof(buf), buf, GL_STATIC_DRAW);
            server_gl_shader_use(scene->shaders.data[0].shader);
            draw_vertex_list(&scene->shaders.data[0], 6);
        }
    }

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glDisable(GL_STENCIL_TEST);
}

static void
draw_debug_text(struct scene *scene) {
    // The OpenGL context must be current.
    server_gl_shader_use(scene->shaders.data[1].shader);
    glUniform2f(scene->shaders.data[1].shader_u_dst_size, scene->ui->width, scene->ui->height);
    glUniform2f(scene->shaders.data[1].shader_u_src_size, FONT_ATLAS_WIDTH, FONT_ATLAS_HEIGHT);

    const char *str = util_debug_str();
    scene->buffers.debug_vtxcount =
        text_build(scene->buffers.debug, scene, str, strlen(str),
                   &(struct scene_text_options){.x = 8, .y = 8, .size = 20, .shader_name = NULL});

    GLuint atlas_texture = 0;
    for (size_t i = 0; i < scene->font.fonts_len; i++) {
        if (scene->font.fonts[i].font_height == 20) {
            atlas_texture = scene->font.fonts[i].atlas_tex;
            break;
        }
    }

    gl_using_buffer(GL_ARRAY_BUFFER, scene->buffers.debug) {
        gl_using_texture(GL_TEXTURE_2D, atlas_texture) {
            draw_vertex_list(&scene->shaders.data[1], scene->buffers.debug_vtxcount);
        }
    }
}

static inline bool
should_draw_frame(struct scene *scene) {
    return util_debug_enabled || wl_list_length(&scene->objects.sorted) ||
           wl_list_length(&scene->objects.unsorted_text) ||
           wl_list_length(&scene->objects.unsorted_mirrors) ||
           wl_list_length(&scene->objects.unsorted_images);
}

static void
draw_frame(struct scene *scene) {
    // The OpenGL context must be current.

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glViewport(0, 0, scene->ui->width, scene->ui->height);

    if (!should_draw_frame(scene)) {
        scene->skipped_frames++;
        if (scene->skipped_frames > 1) {
            return;
        }
    } else {
        scene->skipped_frames = 0;
    }

    draw_stencil(scene);
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

    struct scene_object *object;
    struct wl_list *positive_depth = NULL;
    glEnable(GL_STENCIL_TEST);
    wl_list_for_each (object, &scene->objects.sorted, link) {
        if (object->depth >= 0) {
            positive_depth = object->link.prev;
            break;
        }

        object_render(object);
    }
    glDisable(GL_STENCIL_TEST);

    wl_list_for_each (object, &scene->objects.unsorted_mirrors, link) {
        if (object->enabled)
            mirror_render(object);
    }
    wl_list_for_each (object, &scene->objects.unsorted_images, link) {
        if (object->enabled)
            image_render(object);
    }
    wl_list_for_each (object, &scene->objects.unsorted_text, link) {
        if (object->enabled)
            text_render(object);
    }
    if (positive_depth) {
        wl_list_for_each (object, positive_depth, link) {
            if (object->enabled)
                object_render(object);
        }
    }

    if (util_debug_enabled) {
        draw_debug_text(scene);
    }

    glUseProgram(0);
    server_gl_swap_buffers(scene->gl);
}

static void
draw_vertex_list(struct scene_shader *shader, size_t num_vertices) {
    // The OpenGL context must be current, a texture must be bound to copy from, a vertex buffer
    // with data must be bound, and a valid shader must be in use.

    glVertexAttribPointer(SHADER_SRC_POS_ATTRIB_LOC, 2, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_shader),
                          (const void *)offsetof(struct vtx_shader, src_pos));
    glVertexAttribPointer(SHADER_DST_POS_ATTRIB_LOC, 2, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_shader),
                          (const void *)offsetof(struct vtx_shader, dst_pos));
    glVertexAttribPointer(SHADER_SRC_RGBA_ATTRIB_LOC, 4, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_shader),
                          (const void *)offsetof(struct vtx_shader, src_rgba));
    glVertexAttribPointer(SHADER_DST_RGBA_ATTRIB_LOC, 4, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_shader),
                          (const void *)offsetof(struct vtx_shader, dst_rgba));

    glEnableVertexAttribArray(SHADER_SRC_POS_ATTRIB_LOC);
    glEnableVertexAttribArray(SHADER_DST_POS_ATTRIB_LOC);
    glEnableVertexAttribArray(SHADER_SRC_RGBA_ATTRIB_LOC);
    glEnableVertexAttribArray(SHADER_DST_RGBA_ATTRIB_LOC);

    glDrawArrays(GL_TRIANGLES, 0, num_vertices);

    glDisableVertexAttribArray(SHADER_SRC_POS_ATTRIB_LOC);
    glDisableVertexAttribArray(SHADER_DST_POS_ATTRIB_LOC);
    glDisableVertexAttribArray(SHADER_SRC_RGBA_ATTRIB_LOC);
    glDisableVertexAttribArray(SHADER_DST_RGBA_ATTRIB_LOC);
}

static void
rect_build(struct vtx_shader out[static 6], const struct box *s, const struct box *d,
           const float src_rgba[static 4], const float dst_rgba[static 4]) {
    const struct {
        float src[2];
        float dst[2];
    } data[] = {
        // top-left triangle
        {{s->x, s->y}, {d->x, d->y}},
        {{s->x + s->width, s->y}, {d->x + d->width, d->y}},
        {{s->x, s->y + s->height}, {d->x, d->y + d->height}},

        // bottom-right triangle
        {{s->x + s->width, s->y}, {d->x + d->width, d->y}},
        {{s->x, s->y + s->height}, {d->x, d->y + d->height}},
        {{s->x + s->width, s->y + s->height}, {d->x + d->width, d->y + d->height}},
    };

    for (size_t i = 0; i < STATIC_ARRLEN(data); i++) {
        struct vtx_shader *vtx = &out[i];

        memcpy(vtx->src_pos, data[i].src, sizeof(vtx->src_pos));
        memcpy(vtx->dst_pos, data[i].dst, sizeof(vtx->dst_pos));
        memcpy(vtx->src_rgba, src_rgba, sizeof(vtx->src_rgba));
        memcpy(vtx->dst_rgba, dst_rgba, sizeof(vtx->dst_rgba));
    }
}

static inline struct scene_image *
scene_image_from_object(struct scene_object *object) {
    ww_assert(object->type == SCENE_OBJECT_IMAGE);
    return (struct scene_image *)object;
}

static inline struct scene_mirror *
scene_mirror_from_object(struct scene_object *object) {
    ww_assert(object->type == SCENE_OBJECT_MIRROR);
    return (struct scene_mirror *)object;
}

static inline struct scene_text *
scene_text_from_object(struct scene_object *object) {
    ww_assert(object->type == SCENE_OBJECT_TEXT);
    return (struct scene_text *)object;
}

static bool
image_load(struct scene_image *out, struct scene *scene, const char *path) {
    struct util_png png = util_png_decode(path, scene->image_max_size);
    if (!png.data) {
        return false;
    }

    out->width = png.width;
    out->height = png.height;

    // Upload the decoded image data to a new OpenGL texture.
    server_gl_with(scene->gl, false) {
        glGenTextures(1, &out->tex);
        gl_using_texture(GL_TEXTURE_2D, out->tex) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, png.width, png.height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, png.data);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
    }

    free(png.data);
    return true;
}

static int
shader_find_index(struct scene *scene, const char *key) {
    if (key == NULL) {
        return 0;
    }
    for (size_t i = 1; i < scene->shaders.count; i++) {
        if (strcmp(scene->shaders.data[i].name, key) == 0) {
            return i;
        }
    }
    ww_log(LOG_WARN, "shader %s not found, falling back to default", key);
    return 0;
}

static bool
shader_create(struct server_gl *gl, struct scene_shader *data, char *name, const char *vertex,
              const char *fragment) {
    data->name = name;
    data->shader = server_gl_compile(gl, vertex ? vertex : WAYWALL_GLSL_TEXCOPY_VERT_H,
                                     fragment ? fragment : WAYWALL_GLSL_TEXCOPY_FRAG_H);
    if (!data->shader) {
        return false;
    }

    data->shader_u_src_size = glGetUniformLocation(data->shader->program, "u_src_size");
    data->shader_u_dst_size = glGetUniformLocation(data->shader->program, "u_dst_size");

    return true;
}

struct scene *
scene_create(struct config *cfg, struct server_gl *gl, struct server_ui *ui) {
    struct scene *scene = zalloc(1, sizeof(*scene));

    scene->gl = gl;
    scene->ui = ui;

    // Initialize OpenGL resources.
    server_gl_with(scene->gl, false) {
        GLint tex_size;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &tex_size);

        scene->image_max_size = (uint32_t)tex_size;
        ww_log(LOG_INFO, "max image size: %" PRIu32 "x%" PRIu32, scene->image_max_size,
               scene->image_max_size);

        scene->shaders.count = cfg->shaders.count + 2;
        scene->shaders.data = malloc(sizeof(struct scene_shader) * scene->shaders.count);
        if (!shader_create(scene->gl, &scene->shaders.data[0], strdup("default"), NULL, NULL)) {
            ww_log(LOG_ERROR, "error creating default shader");
            server_gl_exit(scene->gl);
            goto fail_compile_texture_copy;
        }
        if (!shader_create(scene->gl, &scene->shaders.data[1], strdup("text"),
                           WAYWALL_GLSL_TEXCOPY_VERT_H, WAYWALL_GLSL_TEXT_FRAG_H)) {
            ww_log(LOG_ERROR, "error creating text shader");
            server_gl_exit(scene->gl);
            goto fail_compile_texture_copy;
        }
        for (size_t i = 0; i < cfg->shaders.count; i++) {
            if (!shader_create(scene->gl, &scene->shaders.data[i + 2],
                               strdup(cfg->shaders.data[i].name), cfg->shaders.data[i].vertex,
                               cfg->shaders.data[i].fragment)) {
                ww_log(LOG_ERROR, "error creating %s shader", cfg->shaders.data[i].name);
                server_gl_exit(scene->gl);
                goto fail_compile_texture_copy;
            }
            ww_log(LOG_INFO, "created %s shader", cfg->shaders.data[i].name);
        }

        // Initialize vertex buffers.
        glGenBuffers(1, &scene->buffers.debug);
        glGenBuffers(1, &scene->buffers.stencil_rect);

        // Initialize freetype
        if (FT_Init_FreeType(&scene->font.ft)) {
            ww_log(LOG_ERROR, "Failed to init freetype.");
            exit(1);
        }

        if (FT_New_Face(scene->font.ft, cfg->theme.font_path, 0, &scene->font.face)) {
            ww_log(LOG_ERROR, "Failed to load freetype face.");
            exit(1);
        }
    }

    scene->on_gl_frame.notify = on_gl_frame;
    wl_signal_add(&gl->events.frame, &scene->on_gl_frame);

    wl_list_init(&scene->objects.sorted);
    wl_list_init(&scene->objects.unsorted_images);
    wl_list_init(&scene->objects.unsorted_mirrors);
    wl_list_init(&scene->objects.unsorted_text);

    return scene;

fail_compile_texture_copy:
    free(scene);

    return NULL;
}

void
scene_destroy(struct scene *scene) {
    object_list_destroy(&scene->objects.sorted);
    object_list_destroy(&scene->objects.unsorted_images);
    object_list_destroy(&scene->objects.unsorted_mirrors);
    object_list_destroy(&scene->objects.unsorted_text);

    server_gl_with(scene->gl, false) {
        for (size_t i = 0; i < scene->shaders.count; i++) {
            server_gl_shader_destroy(scene->shaders.data[i].shader);
            free(scene->shaders.data[i].name);
        }

        glDeleteBuffers(2, (GLuint[]){scene->buffers.debug, scene->buffers.stencil_rect});
    }
    free(scene->shaders.data);

    wl_list_remove(&scene->on_gl_frame.link);

    FT_Done_Face(scene->font.face);
    FT_Done_FreeType(scene->font.ft);

    for (size_t i = 0; i < scene->font.fonts_len; i++) {
        struct font_size_obj *font_obj = &scene->font.fonts[i];
        for (size_t j = 0; j < font_obj->next_glyph_index; j++) {
            if (font_obj->glyphs[j].bitmap_data) {
                free(font_obj->glyphs[j].bitmap_data);
                font_obj->glyphs[j].bitmap_data = NULL;
            }
        }
        free(font_obj->glyphs);
    }
    free(scene->font.fonts);

    free(scene);
}

struct scene_image *
scene_add_image(struct scene *scene, const struct scene_image_options *options, const char *path) {
    struct scene_image *image = zalloc(1, sizeof(*image));

    image->parent = scene;
    image->is_atlas_texture = false;

    // Load the PNG into an OpenGL texture.
    if (!image_load(image, scene, path)) {
        free(image);
        return NULL;
    }

    // Find correct shader for this image
    image->shader_index = shader_find_index(scene, options->shader_name);

    // Build a vertex buffer containing the data for this image.
    image_build(image, scene, options, image->width, image->height);

    image->object.depth = options->depth;
    object_add(scene, (struct scene_object *)image, SCENE_OBJECT_IMAGE);

    image->object.enabled = true;

    return image;
}

struct scene_image *
scene_add_image_from_atlas(struct scene *scene,
                           const struct scene_image_from_atlas_options *options) {
    struct scene_image *image = zalloc(1, sizeof(*image));

    image->parent = scene;
    image->tex = options->atlas->tex;

    image->width = options->atlas->width;
    image->height = options->atlas->width;

    image->is_atlas_texture = true;

    image->shader_index = shader_find_index(scene, options->shader_name);

    image_build_from_atlas(image, scene, options);

    image->object.depth = options->depth;
    object_add(scene, (struct scene_object *)image, SCENE_OBJECT_IMAGE);

    image->object.enabled = true;

    return image;
}

struct scene_mirror *
scene_add_mirror(struct scene *scene, const struct scene_mirror_options *options) {
    struct scene_mirror *mirror = zalloc(1, sizeof(*mirror));

    mirror->parent = scene;
    memcpy(mirror->src_rgba, options->src_rgba, sizeof(mirror->src_rgba));
    memcpy(mirror->dst_rgba, options->dst_rgba, sizeof(mirror->dst_rgba));

    // Find correct shader for this mirror
    mirror->shader_index = shader_find_index(scene, options->shader_name);

    mirror_build(mirror, options, scene);

    mirror->object.depth = options->depth;
    object_add(scene, (struct scene_object *)mirror, SCENE_OBJECT_MIRROR);

    mirror->object.enabled = true;

    return mirror;
}

struct scene_text *
scene_add_text(struct scene *scene, const char *data, const struct scene_text_options *options) {
    struct scene_text *text = zalloc(1, sizeof(*text));

    text->parent = scene;
    text->x = options->x;
    text->y = options->y;
    text->font_size = options->size;

    if (options->shader_name == NULL) {
        text->shader_index = 1;
    } else {
        text->shader_index = shader_find_index(scene, options->shader_name);
    }

    server_gl_with(scene->gl, false) {
        glGenBuffers(1, &text->vbo);
        ww_assert(text->vbo);

        text->vtxcount = text_build(text->vbo, scene, data, strlen(data), options);
    }

    text->object.depth = options->depth;
    object_add(scene, (struct scene_object *)text, SCENE_OBJECT_TEXT);

    text->object.enabled = true;

    return text;
}

struct Custom_atlas *
scene_create_atlas(struct scene *scene, const uint32_t width, const char *data, size_t len) {
    struct Custom_atlas *atlas = malloc(sizeof(struct Custom_atlas));
    if (!atlas)
        ww_panic("Failed to allocate atlas");

    if (len == 0) {
        atlas->width = width;
        unsigned char *atlasData = (unsigned char *)calloc(width * width * 4, 1);

        server_gl_with(scene->gl, false) {
            glGenTextures(1, &atlas->tex);
            glBindTexture(GL_TEXTURE_2D, atlas->tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, width, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         atlasData);
            glBindTexture(GL_TEXTURE_2D, 0);
            free(atlasData);
        }
    } else {
        if (len < 8) {
            ww_log(LOG_ERROR, "raw dump data too small");
            free(atlas);
            return NULL;
        }

        // dimensions from dump header
        int dump_width = *(int *)data;
        int dump_height = *(int *)(data + 4);
        const char *pixel_data = data + 8;
        size_t pixel_data_size = len - 8;

        if (dump_width != dump_height) {
            ww_log(LOG_ERROR, "atlas must be square (width=%d, height=%d)", dump_width,
                   dump_height);
            free(atlas);
            return NULL;
        }
        if (pixel_data_size != (size_t)dump_width * dump_height * 4) {
            ww_log(LOG_ERROR, "raw dump data size mismatch (expected=%zu, got=%zu)",
                   (size_t)dump_width * dump_height * 4, pixel_data_size);
            free(atlas);
            return NULL;
        }

        atlas->width = dump_width;

        server_gl_with(scene->gl, false) {
            glGenTextures(1, &atlas->tex);
            glBindTexture(GL_TEXTURE_2D, atlas->tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlas->width, atlas->width, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, pixel_data);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }

    return atlas;
}
void
scene_atlas_raw_image(struct scene *scene, struct Custom_atlas *atlas, const char *data,
                      size_t data_len, u_int32_t x, uint32_t y) {

    struct util_png png = util_png_decode_raw(data, data_len, atlas->width);
    if (!png.data)
        return;

    uint32_t blit_width = png.width;
    uint32_t blit_height = png.height;

    if (x + blit_width > atlas->width)
        blit_width = atlas->width - x;
    if (y + blit_height > atlas->width)
        blit_height = atlas->width - y;

    if (blit_width == 0 || blit_height == 0) {
        free(png.data);
        return;
    }

    server_gl_with(scene->gl, false) {
        glBindTexture(GL_TEXTURE_2D, atlas->tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, blit_width, blit_height, GL_RGBA, GL_UNSIGNED_BYTE,
                        png.data);
        glBindTexture(GL_TEXTURE_2D, 0);

        free(png.data);
    }
}

char *
atlas_get_dump(struct scene *scene, struct Custom_atlas *atlas, size_t *out_len) {
    if (!atlas || !out_len) {
        ww_log(LOG_ERROR, "atlas or out_len is NULL");
        if (out_len)
            *out_len = 0;
        return NULL;
    }

    size_t pixel_data_size = atlas->width * atlas->width * 4;
    *out_len = 8 + pixel_data_size;

    char *dump_data = malloc(*out_len);
    check_alloc(dump_data);

    *(int *)dump_data = atlas->width;
    *(int *)(dump_data + 4) = atlas->width;

    char *pixel_data = dump_data + 8;
    server_gl_with(scene->gl, false) {
        // create framebuffer to read from texture
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, atlas->tex, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            // read pixels from framebuffer
            glReadPixels(0, 0, atlas->width, atlas->width, GL_RGBA, GL_UNSIGNED_BYTE, pixel_data);
        } else {
            ww_log(LOG_ERROR, "framebuffer incomplete for atlas dump");
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
    }

    return dump_data;
}

void
scene_atlas_destroy(struct Custom_atlas *atlas) {
    if (!atlas)
        return;

    if (atlas->tex != 0) {
        glDeleteTextures(1, &atlas->tex);
        atlas->tex = 0;
    }

    atlas->width = 0;
    free(atlas);
}

void
scene_object_show(struct scene_object *object) {
    object->enabled = true;
}

void
scene_object_hide(struct scene_object *object) {
    object->enabled = false;
}

void
scene_object_destroy(struct scene_object *object) {
    wl_list_remove(&object->link);
    wl_list_init(&object->link);

    object_release(object);
    free(object);
}

int32_t
scene_object_get_depth(struct scene_object *object) {
    return object->depth;
}

void
scene_object_set_depth(struct scene_object *object, int32_t depth) {
    if (depth == object->depth) {
        return;
    }

    object->depth = depth;
    wl_list_remove(&object->link);
    object_sort(object->parent, object);
}
