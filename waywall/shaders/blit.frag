#version 450

layout(location = 0) in vec2 f_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_texture;

// Push constant for dual-GPU color swap (BGRA <-> RGBA)
layout(push_constant) uniform PushConstants {
    int swap_colors;  // 0 = normal, 1 = swap R and B channels
} pc;

void main() {
    vec4 color = texture(u_texture, f_uv);
    if (pc.swap_colors != 0) {
        out_color = vec4(color.b, color.g, color.r, color.a);
    } else {
        out_color = color;
    }
}
