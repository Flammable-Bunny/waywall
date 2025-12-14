#version 450

// Simple fullscreen quad vertex shader
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;

layout(location = 0) out vec2 f_uv;

void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    f_uv = a_uv;
}
