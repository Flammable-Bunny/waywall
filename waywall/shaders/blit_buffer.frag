#version 450

layout(location = 0) in vec2 f_uv;
layout(location = 0) out vec4 out_color;

// Storage buffer containing the raw dma-buf pixel data
layout(set = 0, binding = 0) readonly buffer PixelData {
    uint pixels[];
} pixel_data;

// Push constants for stride-aware sampling with source cropping
layout(push_constant) uniform PushConstants {
    int width;        // Full game width in pixels
    int height;       // Full game height in pixels
    int stride;       // Actual stride in bytes (from dma-buf)
    int swap_colors;  // 0 = normal, 1 = swap R and B channels
    // Source crop region (for tall/wide resolutions)
    int src_x;        // Source X offset in game pixels
    int src_y;        // Source Y offset in game pixels
    int src_w;        // Source width in game pixels
    int src_h;        // Source height in game pixels
} pc;

void main() {
    // Map UV (0-1) to source crop region in game
    int px = pc.src_x + int(f_uv.x * float(pc.src_w));
    int py = pc.src_y + int(f_uv.y * float(pc.src_h));

    // Clamp to game bounds
    px = clamp(px, 0, pc.width - 1);
    py = clamp(py, 0, pc.height - 1);

    // Calculate byte offset using the actual dma-buf stride
    // stride is in bytes, each pixel is 4 bytes (XRGB8888)
    int byte_offset = py * pc.stride + px * 4;
    int uint_index = byte_offset / 4;

    // Read the pixel as a packed uint32 (XRGB8888 format)
    uint packed = pixel_data.pixels[uint_index];

    // Unpack XRGB8888: memory layout is B, G, R, X (little endian)
    float b = float((packed >> 0) & 0xFF) / 255.0;
    float g = float((packed >> 8) & 0xFF) / 255.0;
    float r = float((packed >> 16) & 0xFF) / 255.0;

    // Apply color swap for dual-GPU if needed
    if (pc.swap_colors != 0) {
        out_color = vec4(b, g, r, 1.0);  // Swap R and B
    } else {
        out_color = vec4(r, g, b, 1.0);
    }
}
