#version 450

layout(location = 0) in vec2 f_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex;

void main() {
    vec4 color = texture(tex, f_uv);
    // Output pre-multiplied alpha for correct compositing with transparent background
    out_color = vec4(color.rgb * color.a, color.a);
}
