precision mediump float;

varying vec2 f_src_pos;
varying vec4 f_src_rgba; // unused, kept just incase
varying vec4 f_dst_rgba;
uniform sampler2D u_texture;

void main() {
    float a = texture2D(u_texture, f_src_pos).a;
    if (a < 0.01)
        discard;
    gl_FragColor = vec4(f_dst_rgba.rgb, f_dst_rgba.a * a);
}
