#version 450

// Inputs from vertex shader
layout(location = 0) in vec2 f_src_pos;
layout(location = 1) in vec4 f_src_rgba;
layout(location = 2) in vec4 f_dst_rgba;

// Output color
layout(location = 0) out vec4 out_color;

// Texture sampler (font atlas)
layout(set = 0, binding = 0) uniform sampler2D u_texture;

void main() {
    float alpha = texture(u_texture, f_src_pos).r;  // Font atlas uses R channel
    if (alpha < 0.01)
        discard;
    // Output pre-multiplied alpha for correct compositing
    float final_alpha = f_dst_rgba.a * alpha;
    out_color = vec4(f_dst_rgba.rgb * final_alpha, final_alpha);
}
