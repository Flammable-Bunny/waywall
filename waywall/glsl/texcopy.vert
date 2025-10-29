attribute vec2 a_src_pos;
attribute vec2 a_dst_pos;
attribute vec4 a_src_rgba;
attribute vec4 a_dst_rgba;

uniform vec2 u_src_size;
uniform vec2 u_dst_size;

varying vec2 f_src_pos;
varying vec4 f_src_rgba;
varying vec4 f_dst_rgba;

void main() {
    gl_Position.x = 2.0 * (a_dst_pos.x / u_dst_size.x) - 1.0;
    gl_Position.y = 1.0 - 2.0 * (a_dst_pos.y / u_dst_size.y);
    gl_Position.zw = vec2(1.0);

    f_src_pos = a_src_pos / u_src_size;
    f_src_rgba = a_src_rgba;
    f_dst_rgba = a_dst_rgba;
}

// vim:ft=glsl
