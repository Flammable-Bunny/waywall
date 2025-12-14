#version 450

// Inputs from vertex shader
layout(location = 0) in vec2 f_src_pos;
layout(location = 1) in vec4 f_src_rgba;
layout(location = 2) in vec4 f_dst_rgba;

// Output color
layout(location = 0) out vec4 out_color;

// Texture sampler
layout(set = 0, binding = 0) uniform sampler2D u_texture;

void main() {
    vec4 color = texture(u_texture, f_src_pos);
    out_color = color;
}
