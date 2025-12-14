#version 450

// Vertex attributes
layout(location = 0) in vec2 v_src_pos;
layout(location = 1) in vec2 v_dst_pos;
layout(location = 2) in vec4 v_src_rgba;
layout(location = 3) in vec4 v_dst_rgba;

// Push constants for size uniforms
layout(push_constant) uniform PushConstants {
    vec2 u_src_size;
    vec2 u_dst_size;
} pc;

// Outputs to fragment shader
layout(location = 0) out vec2 f_src_pos;
layout(location = 1) out vec4 f_src_rgba;
layout(location = 2) out vec4 f_dst_rgba;

void main() {
    gl_Position.x = 2.0 * (v_dst_pos.x / pc.u_dst_size.x) - 1.0;
    gl_Position.y = 1.0 - 2.0 * (v_dst_pos.y / pc.u_dst_size.y);
    gl_Position.zw = vec2(0.0, 1.0);

    f_src_pos = v_src_pos / pc.u_src_size;
    f_src_rgba = v_src_rgba;
    f_dst_rgba = v_dst_rgba;
}
