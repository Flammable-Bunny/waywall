#version 450

layout(location = 0) in vec2 f_uv;
layout(location = 0) out vec4 out_color;

// Storage buffer containing the raw dma-buf pixel data
layout(set = 0, binding = 0) readonly buffer PixelData {
    uint pixels[];
} pixel_data;

// Push constants for stride-aware sampling and dual-GPU color swap
layout(push_constant) uniform PushConstants {
    int width;        // Image width in pixels
    int height;       // Image height in pixels
    int stride;       // Actual stride in bytes (from dma-buf)
    int swap_colors;  // 0 = normal, 1 = swap R and B channels
} pc;

void main() {
    // Calculate pixel coordinates from UV
    int px = int(f_uv.x * float(pc.width));
    int py = int(f_uv.y * float(pc.height));

    // Clamp to valid range
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
