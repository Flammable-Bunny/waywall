precision highp float;

varying vec2 f_src_pos;
varying vec4 f_src_rgba;   // kept for compatibility, not used
varying vec4 f_dst_rgba;

uniform sampler2D u_texture;

void main() {
    float alpha = texture2D(u_texture, f_src_pos).a;

    if (f_dst_rgba.a == 0.0) {
        gl_FragColor = vec4(1.0, 1.0, 1.0, alpha);
    } else {
        gl_FragColor = vec4(f_dst_rgba.rgb, f_dst_rgba.a * alpha);
    }
}

