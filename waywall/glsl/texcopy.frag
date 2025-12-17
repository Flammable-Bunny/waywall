precision highp float;

varying vec2 f_src_pos;
varying vec4 f_src_rgba;
varying vec4 f_dst_rgba;

uniform sampler2D u_texture;

const float threshold = 0.01;

void main() {
    vec4 color = texture2D(u_texture, f_src_pos);

    // DIRECT RENDER DEBUG: Bypass all logic and show the texture
    gl_FragColor = vec4(color.r, color.g, color.b, 1.0); // Force alpha to 1.0 to ensure visibility

    /*
    // Swap Red and Blue channels for cross-GPU compatibility
    color.rb = color.br;
    if (f_dst_rgba.a == 0.0) {
        gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0); // DIAGNOSTIC: force magenta
    } else {
        if (all(lessThan(abs(f_src_rgba.rgb - color.rgb), vec3(threshold)))) {
            gl_FragColor = f_dst_rgba;
        } else {
            gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        }
    }
    */
}

// vim:ft=glsl
