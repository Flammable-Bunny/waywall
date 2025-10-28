#include "util/sysinfo.h"
#include "util/log.h"
#include "util/prelude.h"
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <wayland-version.h>

#define PATH_SYSCTL "/proc/sys/"

#define PATH_INOTIFY_MAX_QUEUED_EVENTS PATH_SYSCTL "fs/inotify/max_queued_events"
#define PATH_INOTIFY_MAX_USER_INSTANCES PATH_SYSCTL "fs/inotify/max_user_instances"
#define PATH_INOTIFY_MAX_USER_WATCHES PATH_SYSCTL "fs/inotify/max_user_watches"

static long
number_from_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open file '%s'", path);
        return -1;
    }

    char buf[128];
    ssize_t n = read(fd, buf, STATIC_ARRLEN(buf));
    close(fd);
    if (n == -1) {
        ww_log_errno(LOG_ERROR, "failed to read file '%s'", path);
        return -1;
    }
    buf[n] = '\0';

    char *endptr;
    long num = strtol(buf, &endptr, 10);
    if (*endptr && *endptr != '\n') {
        ww_log(LOG_ERROR, "invalid terminator on number '%s'", buf);
        return -1;
    }

    return num;
}

static void
log_inotify_limits() {
    long max_queued_events = number_from_file(PATH_INOTIFY_MAX_QUEUED_EVENTS);
    long max_user_instances = number_from_file(PATH_INOTIFY_MAX_USER_INSTANCES);
    long max_user_watches = number_from_file(PATH_INOTIFY_MAX_USER_WATCHES);

    if (max_queued_events == -1 || max_user_instances == -1 || max_user_watches == -1) {
        ww_log(LOG_ERROR, "failed to get inotify limits");
        return;
    }

    ww_log(LOG_INFO, "inotify max queued events:  %ld", max_queued_events);
    ww_log(LOG_INFO, "inotify max user instances: %ld", max_user_instances);
    ww_log(LOG_INFO, "inotify max user watches:   %ld", max_user_watches);
}

static void
log_max_files() {
    struct rlimit limit;
    ww_assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);

    // There isn't much reason to care about the hard limit because we aren't going to raise the
    // soft limit.
    ww_log(LOG_INFO, "max files: %jd", (intmax_t)limit.rlim_cur);
}

static void
log_uname() {
    struct utsname name;
    ww_assert(uname(&name) == 0);

    ww_log(LOG_INFO, "system:  %s", name.sysname);
    ww_log(LOG_INFO, "release: %s", name.release);
    ww_log(LOG_INFO, "version: %s", name.version);
    ww_log(LOG_INFO, "machine: %s", name.machine);
}

static void
log_wl_version() {
    ww_log(LOG_INFO, "libwayland version: %s", WAYLAND_VERSION);
}

void
sysinfo_dump_log() {
    ww_log(LOG_INFO, "---- SYSTEM INFO");

    log_uname();
    log_max_files();
    log_inotify_limits();
    log_wl_version();

    ww_log(LOG_INFO, "---- END SYSTEM INFO");
}

// NVIDIA-specific VRAM query extension
#ifndef GL_NVX_gpu_memory_info
#define GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX 0x9047
#define GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX 0x9048
#define GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049
#define GL_GPU_MEMORY_INFO_EVICTION_COUNT_NVX 0x904A
#define GL_GPU_MEMORY_INFO_EVICTED_MEMORY_NVX 0x904B
#endif

// AMD-specific VRAM query extension
#ifndef GL_ATI_meminfo
#define GL_VBO_FREE_MEMORY_ATI 0x87FB
#define GL_TEXTURE_FREE_MEMORY_ATI 0x87FC
#define GL_RENDERBUFFER_FREE_MEMORY_ATI 0x87FD
#endif

size_t
sysinfo_query_vram_total() {
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);

    if (!vendor) {
        ww_log(LOG_WARN, "unable to query GL_VENDOR for VRAM detection");
        return 0;
    }

    ww_log(LOG_INFO, "GPU vendor: %s, renderer: %s", vendor, renderer ? renderer : "unknown");

    // Clear any previous GL errors before querying
    while (glGetError() != GL_NO_ERROR)
        ;

    // Try NVIDIA extension (proprietary and nouveau drivers)
    if (strstr(vendor, "NVIDIA") || (renderer && strstr(renderer, "NVIDIA"))) {
        GLint total_kb = 0;

        // Try dedicated VRAM query first
        glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &total_kb);
        GLenum err = glGetError();

        if (err == GL_NO_ERROR && total_kb > 0) {
            ww_log(LOG_INFO, "NVIDIA GPU: %d MB VRAM (dedicated)", total_kb / 1024);
            return (size_t)total_kb * 1024; // Convert KB to bytes
        }

        // Try total available memory as fallback
        glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &total_kb);
        err = glGetError();

        if (err == GL_NO_ERROR && total_kb > 0) {
            ww_log(LOG_INFO, "NVIDIA GPU: %d MB VRAM (total available)", total_kb / 1024);
            return (size_t)total_kb * 1024; // Convert KB to bytes
        }

        // Nouveau driver fallback: may not support extensions
        ww_log(LOG_WARN,
               "NVIDIA GPU detected but unable to query VRAM (nouveau driver or missing extension)");
    }

    // Try AMD extension
    if (strstr(vendor, "AMD") || strstr(vendor, "ATI") || strstr(vendor, "Advanced Micro Devices")) {
        GLint mem_info[4] = {0};
        glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, mem_info);
        GLenum err = glGetError();

        if (err == GL_NO_ERROR && mem_info[0] > 0) {
            // mem_info[0] contains free memory in KB
            // For AMD, we report free as "total" since that's what we can query
            ww_log(LOG_INFO, "AMD GPU: ~%d MB VRAM (currently free)", mem_info[0] / 1024);
            return (size_t)mem_info[0] * 1024; // Convert KB to bytes
        }

        ww_log(LOG_WARN, "AMD GPU detected but unable to query VRAM (missing extension)");
    }

    // Try Intel integrated graphics fallback
    if (strstr(vendor, "Intel")) {
        // Intel iGPUs share system RAM, so we can't accurately query dedicated VRAM
        // Use a conservative estimate based on common configurations
        ww_log(LOG_WARN,
               "Intel integrated GPU detected, using conservative 2GB VRAM estimate (shared memory)");
        return (size_t)2 * 1024 * 1024 * 1024; // 2GB
    }

    ww_log(LOG_WARN, "unable to query VRAM for vendor: %s", vendor);
    return 0;
}

size_t
sysinfo_query_vram_available() {
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);

    if (!vendor) {
        return 0;
    }

    // Clear any previous GL errors
    while (glGetError() != GL_NO_ERROR)
        ;

    // Try NVIDIA extension
    if (strstr(vendor, "NVIDIA") || (renderer && strstr(renderer, "NVIDIA"))) {
        GLint available_kb = 0;
        glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &available_kb);
        GLenum err = glGetError();

        if (err == GL_NO_ERROR && available_kb > 0) {
            return (size_t)available_kb * 1024; // Convert KB to bytes
        }

        // Nouveau fallback: if extension unavailable, return 0 (will use total instead)
        return 0;
    }

    // Try AMD extension
    if (strstr(vendor, "AMD") || strstr(vendor, "ATI") || strstr(vendor, "Advanced Micro Devices")) {
        GLint mem_info[4] = {0};
        glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, mem_info);
        GLenum err = glGetError();

        if (err == GL_NO_ERROR && mem_info[0] > 0) {
            return (size_t)mem_info[0] * 1024; // Convert KB to bytes
        }
    }

    // Intel iGPUs: can't query available VRAM reliably
    if (strstr(vendor, "Intel")) {
        return 0;
    }

    return 0;
}
