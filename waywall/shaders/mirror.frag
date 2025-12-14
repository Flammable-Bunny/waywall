#version 450

layout(location = 0) in vec2 f_uv;
layout(location = 0) out vec4 out_color;

// Storage buffer containing the raw dma-buf pixel data
layout(set = 0, binding = 0) readonly buffer PixelData {
    uint pixels[];
} pixel_data;

// Push constants for mirror rendering
layout(push_constant) uniform PushConstants {
    // Game texture dimensions
    int game_width;
    int game_height;
    int game_stride;

    // Source region in game (pixels)
    int src_x;
    int src_y;
    int src_w;
    int src_h;

    // Color keying
    int color_key_enabled;
    float key_r, key_g, key_b;      // Input color to match
    float out_r, out_g, out_b;      // Output color to replace with
    float tolerance;                 // Color match tolerance
} pc;

void main() {
    // Map UV (0-1) to source region in game
    int px = pc.src_x + int(f_uv.x * float(pc.src_w));
    int py = pc.src_y + int(f_uv.y * float(pc.src_h));

    // Clamp to game bounds
    px = clamp(px, 0, pc.game_width - 1);
    py = clamp(py, 0, pc.game_height - 1);

    // Calculate byte offset using actual dma-buf stride
    int byte_offset = py * pc.game_stride + px * 4;
    int uint_index = byte_offset / 4;

    // Read pixel (XRGB8888 format, little endian: B,G,R,X)
    uint packed = pixel_data.pixels[uint_index];
    float b = float((packed >> 0) & 0xFF) / 255.0;
    float g = float((packed >> 8) & 0xFF) / 255.0;
    float r = float((packed >> 16) & 0xFF) / 255.0;

    // Apply color keying if enabled
    if (pc.color_key_enabled != 0) {
        // Check if this pixel matches the key color (within tolerance)
        float dr = abs(r - pc.key_r);
        float dg = abs(g - pc.key_g);
        float db = abs(b - pc.key_b);

        if (dr <= pc.tolerance && dg <= pc.tolerance && db <= pc.tolerance) {
            // Replace with output color
            r = pc.out_r;
            g = pc.out_g;
            b = pc.out_b;
        }
    }

    out_color = vec4(r, g, b, 1.0);
}
