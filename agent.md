# Waywall Agent Context Document

> **IMPORTANT**: This document should be updated by any agent working on this codebase. Track your current work, findings, and progress here.

## Last Updated
- **Date**: 2025-12-17
- **Agent**: GPT-5.2 (Codex CLI) — Secondary (Agent 2)
- **Current Task**: Vulkan “proxy game via subsurface” mode + wl_surface.frame forwarding (per user request)

---

## Current Status (Agent 2)
- I’m actively working on the Vulkan `WAYWALL_VK_PROXY_GAME=1` subsurface-proxying path.
- Latest fix landed: stop `wp_linux_dmabuf` from rewriting dmabuf feedback (LINEAR-only tables / tranche indices) in proxy mode, since that can make the host compositor reject `wl_buffer` creation (`failed to create dmabuf`).
- New finding (blocking): even with full dmabuf feedback passthrough, Hyprland rejects the game’s dmabuf `wl_buffer` creation in proxy mode:
  - `dmabuf params FAILED in host compositor (proxy_game=1): 854x480 format=XR24 modifier=0 stride=3416`
  - `error 7: failed to create dmabuf`
- Implementing next step: Vulkan re-export copy
  - On dmabuf creation (proxy mode), allocate N export dmabufs on the compositor GPU via GBM, create host-compositor `wl_buffer`s for them.
  - On each surface commit, Vulkan imports the client dmabuf and copies it into an available export dmabuf, then the surface commit attaches that export `wl_buffer` to Hyprland (avoids `vkQueuePresentKHR` stall).

## Project Overview

**Waywall** is a Wayland compositor wrapper designed for speedrunning Minecraft. This fork converts the original OpenGL backend to **Vulkan** for better performance and dual-GPU support.

### Goal
Run Minecraft on **Intel GPU** while offloading waywall compositing to **AMD GPU**, achieving unlimited FPS without ReBAR bandwidth limitations.

### Current Problem
When AMD reads Intel's LINEAR dmabuf memory via PCIe ReBAR, FPS caps at ~64-96 due to bandwidth limitations. The Zink branch achieves unlimited FPS but has constant screen tearing.

### Constraint
**NATIVE GPU-TO-GPU ONLY** - No CPU fallback paths. The solution must use direct GPU-to-GPU memory transfer mechanisms.

---

## Repository Structure

```
waywall/
├── agent.md                    # THIS FILE - Agent context and TODO tracking
├── flake.nix                   # Nix flake for building waywall
├── meson.build                 # Root build configuration
├── configs/                    # User configuration files (Lua)
│   ├── init.lua               # Main config
│   ├── keybinds.lua           # Keybind configuration
│   └── chat.lua               # Chat overlay config
│
├── include/                    # Header files
│   ├── config/
│   │   └── config.h           # Configuration structures
│   ├── server/
│   │   ├── vk.h               # [CRITICAL] Vulkan backend structures
│   │   ├── backend.h          # Wayland backend interface
│   │   ├── ui.h               # UI rendering structures
│   │   ├── wl_compositor.h    # Wayland compositor protocol
│   │   ├── wl_output.h        # Output/display handling
│   │   ├── wp_linux_dmabuf.h  # DMA-buf protocol structures
│   │   └── wp_linux_drm_syncobj.h # DRM sync object structures
│   ├── scene.h                # Scene graph for overlays
│   ├── wrap.h                 # Wrapper command handling
│   └── util/
│       ├── debug.h            # Debug utilities
│       └── log.h              # Logging macros
│
├── waywall/                    # Source files
│   ├── main.c                 # Entry point
│   ├── meson.build            # Build configuration
│   │
│   ├── server/
│   │   ├── vk.c               # [CRITICAL] Vulkan backend (~4500 lines)
│   │   ├── backend.c          # Wayland backend implementation
│   │   ├── server.c           # Server core logic
│   │   ├── ui.c               # UI rendering
│   │   ├── wl_compositor.c    # Compositor protocol
│   │   ├── wl_output.c        # Output handling
│   │   ├── wl_data_device_manager.c # Data device (clipboard)
│   │   ├── wp_linux_dmabuf.c  # [CRITICAL] DMA-buf handling
│   │   ├── wp_linux_drm_syncobj.c # [CRITICAL] Timeline sync
│   │   ├── xwm.c              # X11 window manager (XWayland)
│   │   └── bunny.c            # Placeholder file (unused)
│   │
│   ├── shaders/               # GLSL shaders (compiled to SPIR-V)
│   │   ├── blit.frag          # Simple texture blit
│   │   ├── blit.vert          # Blit vertex shader
│   │   ├── blit_buffer.frag   # [CRITICAL] Stride-aware buffer sampling
│   │   ├── texcopy.frag       # Texture copy shader
│   │   ├── texcopy.vert       # Texcopy vertex shader
│   │   ├── mirror.frag        # Mirror with color key
│   │   ├── image.frag         # Image overlay shader
│   │   └── text.frag          # Text rendering shader
│   │
│   ├── config/
│   │   └── api.c              # Lua config API
│   │
│   ├── scene.c                # Scene graph rendering
│   ├── subproc.c              # Subprocess (Minecraft) management
│   ├── wrap.c                 # Wrapper command handling
│   │
│   └── util/
│       └── debug.c            # Debug utilities
│
└── waywall-zink/              # Alternative Zink/OpenGL branch
    └── waywall/
        └── server/
            └── gl.c           # [REFERENCE] OpenGL backend with working cross-GPU
```

---

## Critical Files Deep Dive

### 1. `waywall/server/vk.c` - Vulkan Backend

**Purpose**: Core rendering engine, handles dmabuf import, GPU memory management, and presentation.

**Key Structures**:
```c
struct vk_buffer {
    struct server_vk *vk;
    struct server_buffer *parent;
    int dmabuf_fd;                    // Source dmabuf file descriptor

    VkImage image;                    // Imported LINEAR image
    VkDeviceMemory memory;            // Imported memory (via ReBAR)
    VkImageView view;
    VkDescriptorSet descriptor_set;

    VkBuffer storage_buffer;          // Storage buffer for stride-aware sampling
    VkDescriptorSet buffer_descriptor_set;

    VkImage optimal_image;            // OPTIMAL tiled copy on AMD
    VkDeviceMemory optimal_memory;
    VkImageView optimal_view;
    bool optimal_valid;

    int32_t width, height;
    uint32_t stride;
};

struct server_vk {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;

    bool dual_gpu;                    // True when AMD+Intel detected
    bool allow_modifiers;             // Allow tiled modifier imports
    bool disable_capture_sync_wait;   // Skip sync wait (tearing risk)

    // Swapchain, pipelines, command buffers...
};
```

**Key Functions**:
| Function | Line | Purpose |
|----------|------|---------|
| `select_physical_device()` | 579-769 | GPU selection (prefers AMD for rendering) |
| `choose_present_mode()` | 853-902 | Swapchain present mode (prefers IMMEDIATE) |
| `create_swapchain()` | 904-1006 | Swapchain creation |
| `vk_buffer_import()` | 3490-4100 | [CRITICAL] Dmabuf import with modifier/LINEAR paths |
| `create_optimal_copy()` | 73-152 | Create OPTIMAL tiled copy |
| `copy_to_optimal()` | 154-290 | GPU copy from LINEAR to OPTIMAL |
| `begin_frame()` | ~3150 | Start frame rendering |
| `end_frame()` | ~3230 | Submit and present frame |

**Import Paths** (in `vk_buffer_import()`):
1. **Modifier Path** (lines 3525-3713): Uses `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`
   - Best for same-GPU or compatible modifiers
   - Fails for cross-GPU without shared modifier support

2. **LINEAR Path** (lines 3715-4050): Uses `VK_IMAGE_TILING_LINEAR`
   - Cross-GPU via ReBAR direct VRAM access
   - **THIS IS THE BOTTLENECK** - ReBAR bandwidth limits FPS

### 2. `waywall/server/wp_linux_dmabuf.c` - DMA-buf Protocol

**Purpose**: Handles `zwp_linux_dmabuf_v1` protocol, format negotiation, buffer creation.

**Key Variables**:
```c
linux_dmabuf->allow_modifiers    // Enable modifier-based imports
linux_dmabuf->force_intel_feedback // Force Intel render node
```

**Environment Variables**:
- `WAYWALL_DMABUF_ALLOW_MODIFIERS` - Enable modifier imports
- `WAYWALL_DMABUF_FORCE_INTEL` - Force Intel feedback

### 3. `waywall/server/wp_linux_drm_syncobj.c` - Timeline Sync

**Purpose**: Explicit GPU synchronization using DRM timeline semaphores.

**Flow**:
1. Client (Minecraft) sets acquire point before writing to buffer
2. Waywall waits on acquire point before reading
3. Waywall signals release point after compositing
4. Client waits on release point before reusing buffer

### 4. `waywall-zink/waywall/server/gl.c` - Reference Implementation

**Why Zink Works** (for reference, but we want NATIVE GPU path):
- Uses `eglSwapInterval(0)` - no vsync stalls
- GBM import fallback for cross-GPU
- Has CPU copy fallback (disabled by user request)
- Proxies complex transfers to parent compositor (Hyprland)

---

## Dual-GPU Setup

### Hardware Configuration
- **Intel Arc B580** (Battlemage, discrete): Runs Minecraft (game rendering)
- **AMD Radeon RX 7900 XTX** (RDNA3, discrete): Runs waywall (compositing + display)

Both GPUs are **discrete** with dedicated transfer queues, which enables true async copy operations.

### Current Data Flow (SLOW)
```
Intel GPU renders Minecraft
    |
    v
Dmabuf created (Intel VRAM, LINEAR layout)
    |
    v
AMD imports dmabuf via VK_EXT_external_memory_dma_buf
    |
    v
AMD reads from Intel VRAM via PCIe ReBAR  <-- BOTTLENECK (~8-12 GB/s practical)
    |
    v
AMD composites and displays
```

### Desired Data Flow (FAST)
```
Intel GPU renders Minecraft
    |
    v
Dmabuf created (system RAM or shared memory)
    |
    v
AMD imports dmabuf via peer memory / shared pool
    |
    v
AMD reads from local/shared memory  <-- NO ReBAR BOTTLENECK
    |
    v
AMD composites and displays
```

---

## Environment Variables

| Variable | Purpose | Current State |
|----------|---------|---------------|
| `WAYWALL_VK_VENDOR=amd\|intel` | Force GPU vendor selection | Available |
| `WAYWALL_PRESENT_MODE=IMMEDIATE\|MAILBOX\|FIFO` | Force present mode | Available |
| `WAYWALL_DMABUF_ALLOW_MODIFIERS=1` | Enable tiled modifier imports | Available |
| `WAYWALL_DMABUF_FORCE_INTEL=1` | Force Intel feedback | Available |
| `WAYWALL_DISABLE_CAPTURE_SYNC_WAIT=1` | Skip sync (tearing) | Available |
| `WAYWALL_ASYNC_PIPELINING=1` | Enable async double-buffered optimal copy | Available |
| `WAYWALL_GPU_SELECT_LEGACY=1` | Legacy GPU selection | Available |
| `DRI_PRIME=1` | Mesa GPU selection for subprocess | Available |

---

## Current Agent Work

### Agent 1 - Async Pipelining Implementation
**Status**: IMPLEMENTED, but ineffective on this hardware (still bandwidth-limited)
**Task**: Implemented double-buffered async copy with dedicated transfer queue
**Files Modified**:
- `include/server/vk.h` - Added double-buffer fields to vk_buffer, transfer queue to server_vk
- `waywall/server/vk.c` - Added transfer queue init, async copy functions, frame loop integration

**Implementation Details**:
1. Added `optimal_images[2]`, `optimal_memories[2]`, `optimal_views[2]`, `optimal_descriptors[2]` to vk_buffer
2. Added `transfer_queue`, `transfer_family`, `transfer_pool`, `async_pipelining_enabled` to server_vk
3. Implemented `create_double_buffered_optimal()`, `destroy_double_buffered_optimal()`, `start_async_copy_to_optimal()`, `try_swap_optimal_buffers()`
4. Modified `find_queue_families()` to detect dedicated transfer queue (falls back to graphics queue)
5. Modified `select_physical_device()` and `create_device()` to request transfer queue
6. Integrated async copy into `on_surface_commit()` and buffer swapping into `begin_frame()`
7. Added environment variable `WAYWALL_ASYNC_PIPELINING=1` to enable the feature

**How it works**:
- When enabled (dual-GPU + env var), creates two OPTIMAL tiled images instead of one
- On surface commit, starts async copy from LINEAR to write buffer on transfer queue
- On begin frame, checks if copy is complete (non-blocking) and swaps read/write indices
- Renders from the read buffer while copying to the write buffer in parallel
- Effectively hides ReBAR read latency by overlapping copy with rendering previous frame

**To test**: Set `WAYWALL_ASYNC_PIPELINING=1` environment variable before running waywall

**Note (2025-12-17)**: Reported to not improve FPS on the target dual-dGPU setup (likely hard PCIe/ReBAR throughput limit).

---

### Agent 2 - System RAM Forcing Research
**Status**: PAUSED (switching to subsurface proxy approach)
**Task**: Investigate whether Minecraft/Mesa can be influenced to allocate dma-bufs in system RAM (vs Intel VRAM) to reduce cross-GPU read throttling

**Progress / Findings (so far)**:
1. `zwp_linux_dmabuf_feedback_v1` can steer *which DRM device* is preferred and *which format/modifier pairs* are preferred, but it does not provide a direct “system RAM” allocation target (it’s always a DRM device `dev_t` + tranche info).
2. For “system RAM dma-buf”, typical approaches are allocator-driven (e.g., `dma-heap` / `udmabuf`) or explicit Vulkan choices (e.g., host-visible memory) — both are client-controlled and likely not influenced reliably by Wayland feedback alone.
3. Inspected Mesa binaries in this environment (Mesa `25.3.1` via `/run/opengl-driver`) and found Intel Vulkan (ANV) env vars: `ANV_SYS_MEM_LIMIT`, `ANV_QUEUE_OVERRIDE`, `ANV_DEBUG*`, etc. Notably, `ANV_ENABLE_EXTERNAL_MEMORY_HOST` is **not** present in `libvulkan_intel.so` here (may be outdated/wrong for this Mesa version).
4. Mesa’s explicit Vulkan layer `VK_LAYER_MESA_vram_report_limit` exposes `VK_VRAM_REPORT_LIMIT_DEVICE_ID` (`vendorID:deviceID`) + `VK_VRAM_REPORT_LIMIT_HEAP_SIZE` (MiB) to clamp reported heap size (potentially useful to test if WSI buffer placement changes under constrained “VRAM” reporting).
5. Implemented subprocess env injection in `waywall/subproc.c` so Waywall can automatically enable the layer + set settings when launching Minecraft (opt-in via `WAYWALL_SUBPROC_VRAM_LIMIT_MIB`).
6. If Waywall selects the wrong Vulkan GPU (e.g., Intel), force AMD selection with `WAYWALL_VK_VENDOR=amd` or `WAYWALL_GPU_SELECT_STRICT=1` (GLX/GBM env vars do not affect Vulkan device selection).

**Recommended next step**:
- Agent 1’s async copy/pipelining doesn’t overcome the cross-GPU throughput limit on this hardware; next best low-risk experiment is forcing Intel WSI allocations into sysmem via `VK_LAYER_MESA_vram_report_limit` and/or ANV knobs, then re-measuring FPS.

**Instructions for Agent 2**:
1. Research Mesa ANV environment variables that control memory allocation
2. Test these env vars with waywall:
   - `WAYWALL_SUBPROC_VRAM_LIMIT_MIB=<MiB>` (enables `VK_LAYER_MESA_vram_report_limit` for the launched subprocess)
   - Optional override: `WAYWALL_SUBPROC_VRAM_LIMIT_DEVICE_ID=<vendorID:deviceID>` (otherwise auto-detect first Intel render node)
   - Optional pass-through: `WAYWALL_SUBPROC_ANV_SYS_MEM_LIMIT=<value>` → `ANV_SYS_MEM_LIMIT=<value>`
   - `MESA_VK_WSI_PRESENT_MODE=mailbox`
   - Other Intel-specific variables
3. Modify `waywall/server/wp_linux_dmabuf.c` to send feedback preferring system RAM
4. Update `flake.nix` to add environment variable wrappers if needed
5. Document findings in this file

**Key Files for Agent 2**:
- `waywall/server/wp_linux_dmabuf.c` - Dmabuf feedback handling
- `flake.nix` - Build configuration
- Mesa docs: https://docs.mesa3d.org/envvars.html

**Goal**: Force Intel to allocate in system RAM so AMD can read without ReBAR bottleneck

---

### Agent 2 - Vulkan Proxy Game (New Approach)
**Status**: IN PROGRESS (GPT-5.2 / Codex CLI)
**Task**: Implement Zink-like passthrough by proxying the game buffer to Hyprland via a wl_subsurface, and render Vulkan UI/overlays on a separate surface (no swapchain-driven throttling of the game).

**Work items**:
1. Backport Zink’s `wl_surface.frame` forwarding (so the client’s frame pacing is driven by the parent compositor, not by the Vulkan render loop).
2. Add opt-in env `WAYWALL_VK_PROXY_GAME=1`:
   - Centered (game) view gets a subsurface even when `force_composition` is enabled.
   - `wrap` does **not** set Vulkan capture to the game surface.
   - Vulkan keeps rendering overlays on its own subsurface (transparent background).
   - Floating windows remain direct subsurfaces (skip Vulkan re-composition in this mode).

**Coordination note (for Agent 1)**: I’m actively modifying `waywall/server/wl_compositor.c`, `waywall/server/vk.c`, `waywall/server/ui.c`, and `waywall/wrap.c` to implement proxy mode and frame callback forwarding. Please avoid overlapping edits in these files until I mark this section COMPLETE.

## TODO List

### IMMEDIATE PRIORITY - Native GPU-to-GPU ReBAR Bypass

#### Phase 1: Investigation & Analysis
- [x] Explore main waywall Vulkan codebase
- [x] Explore waywall-zink branch for reference
- [x] Understand dual-GPU dmabuf flow
- [x] Identify ReBAR as the bottleneck
- [x] Create implementation plan
- [ ] **Investigate Vulkan peer memory extensions**
  - `VK_KHR_external_memory`
  - `VK_EXT_external_memory_host`
  - `VK_AMD_buffer_marker` / peer memory features
- [ ] **Research DMA-BUF heaps for system RAM allocation**
  - Can we force Minecraft to allocate in system RAM instead of Intel VRAM?
  - `DMA_HEAP_IOCTL_ALLOC` for system-ram heap

#### Phase 2: Native GPU Solutions (NO CPU FALLBACK)

##### Option A: Force System RAM Allocation
- [ ] Research `MESA_GLSL_CACHE_DIR` and Mesa dmabuf allocation hints
- [ ] Test `MESA_VK_WSI_FORCE_SWAPCHAIN_TO_CURRENT_EXTENT` effects
- [ ] Investigate `VK_EXT_external_memory_host` for host-visible dmabuf
- [ ] Modify wp_linux_dmabuf.c feedback to hint system RAM preference
- [ ] Test with different modifier combinations

##### Option B: Shared Memory Pool
- [ ] Investigate DRM PRIME with explicit memory type selection
- [ ] Research `VK_EXT_device_memory_report` for memory diagnostics
- [ ] Test `VK_AMD_memory_overallocation_behavior`
- [ ] Implement shared memory pool between Intel and AMD

##### Option C: Asynchronous Copy with Pipelining
- [ ] Implement double-buffered dmabuf import
- [ ] Use separate transfer queue for async copies
- [ ] Pipeline copy and render operations
- [ ] Minimize stalls by overlapping operations

##### Option D: Modifier-Based Cross-GPU (if supported)
- [ ] Query supported modifiers on both Intel and AMD
- [ ] Find common LINEAR modifier that works cross-GPU
- [ ] Test `I915_FORMAT_MOD_Y_TILED` on AMD (may work via mesa)
- [ ] Implement modifier negotiation in dmabuf feedback

#### Phase 3: Implementation

##### vk.h Changes
- [ ] Add peer memory support structures to `vk_buffer`
- [ ] Add transfer queue fields to `server_vk`
- [ ] Add async copy synchronization primitives

##### vk.c Changes
- [ ] Implement `vk_buffer_import_peer_memory()` - native GPU path
- [ ] Add async transfer queue initialization
- [ ] Implement double-buffered import with fence sync
- [ ] Add memory type selection for cross-GPU optimization
- [ ] Modify `copy_to_optimal()` to use async transfer

##### wp_linux_dmabuf.c Changes
- [ ] Modify format feedback to prefer system RAM modifiers
- [ ] Add memory hint in buffer params
- [ ] Implement custom feedback for cross-GPU scenarios

#### Phase 4: Testing & Validation
- [ ] Benchmark FPS before/after changes
- [ ] Test with various Minecraft resolutions
- [ ] Verify no visual artifacts
- [ ] Test with compositor protocols (tearing control)
- [ ] Validate on different AMD/Intel GPU combinations

### SECONDARY - Zink Tearing Fix (Fallback Option)
- [ ] Implement `wp_tearing_control_v1` in Zink branch
- [ ] Add configurable vsync toggle
- [ ] Test frame pacing with adaptive sync

### TERTIARY - General Improvements
- [ ] Add runtime switching between import paths
- [ ] Implement FPS/latency monitoring overlay
- [ ] Add automatic ReBAR detection and path selection
- [ ] Document environment variables comprehensively

---

## Technical Notes

### ReBAR (Resizable BAR) Explained
ReBAR allows the CPU to access the full GPU VRAM over PCIe. In cross-GPU scenarios:
- AMD GPU reads Intel's VRAM via PCIe BAR window
- Bandwidth is limited by PCIe lane speed (~15 GB/s for x16 Gen4, less practical)
- Random access patterns reduce effective bandwidth significantly
- Results in FPS cap at seemingly random numbers (64, 96, etc.)

### Why Zink Works (for reference)
1. **EGL dynamic swap interval**: `eglSwapInterval(0)` per-frame, not per-swapchain
2. **GBM fallback**: More robust cross-GPU import path
3. **Compositor proxy**: Lets Hyprland handle complex GPU-GPU transfers
4. **But causes tearing**: No vsync = visible tearing

### Native GPU-to-GPU Approaches
1. **System RAM allocation**: Force dmabuf to system RAM, both GPUs access equally
2. **Peer memory**: Direct GPU-to-GPU memory access (like NVLink but for AMD/Intel)
3. **Shared modifier**: Find a tiling format both GPUs understand natively
4. **Async pipelining**: Hide latency by overlapping operations

---

## Commands for Testing

```bash
# Run with AMD GPU forced, immediate present mode
WAYWALL_VK_VENDOR=amd WAYWALL_PRESENT_MODE=IMMEDIATE waywall

# Run Minecraft on Intel, waywall on AMD
DRI_PRIME=0 minecraft &  # Intel
DRI_PRIME=1 waywall      # AMD

# Enable modifier imports
WAYWALL_DMABUF_ALLOW_MODIFIERS=1 waywall

# Disable sync wait (may cause tearing but tests raw performance)
WAYWALL_DISABLE_CAPTURE_SYNC_WAIT=1 waywall

# Build with Nix
nix build .#waywall

# Run with full debugging
WAYWALL_DEBUG=1 waywall 2>&1 | tee waywall.log

# Current Wrapper Command 
env WAYWALL_DMABUF_FORCE_INTEL=1 WAYWALL_DMABUF_ALLOW_MODIFIERS=1 __GLX_VENDOR_LIBRARY_NAME=am GBM_DEVICE=/dev/dri/renderD128 AMD_DEBUG=forcegtt,nodcc,nohyperz,nowc /home/bunny/IdeaProjects/waywall/builddir/waywall/waywall wrap -- env __GLX_VENDOR_LIBRARY_NAME=intel GBM_DEVICE=/dev/dri/renderD129 DRI_PRIME=1 $GAME_SCRIPT
```

---

## Agent Instructions

When working on this codebase:

1. **Update this file** with your current task and findings
2. **Mark TODO items** as complete when done
3. **Add new findings** to Technical Notes
4. **Coordinate** via the "Current Agent Work" section
5. **Prioritize native GPU-to-GPU** - avoid CPU fallback paths
6. **Test incrementally** - verify each change before proceeding

---

## Final Goal

Achieve **unlimited FPS** (300+) in Minecraft running on Intel GPU while waywall composites on AMD GPU, using **native GPU-to-GPU memory transfer** without CPU involvement or ReBAR bandwidth limitations. No screen tearing.

---

## References

- [Vulkan External Memory Extensions](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_KHR_external_memory.html)
- [DMA-BUF Heaps](https://www.kernel.org/doc/html/latest/driver-api/dma-buf.html)
- [Mesa Vulkan WSI](https://docs.mesa3d.org/envvars.html)
- [Original Waywall](https://github.com/Tesselslate/waywall)
