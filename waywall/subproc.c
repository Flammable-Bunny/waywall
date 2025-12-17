#include "subproc.h"
#include "server/server.h"
#include "util/alloc.h"
#include "util/list.h"
#include "util/log.h"
#include "util/syscall.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

LIST_DEFINE_IMPL(struct subproc_entry, list_subproc_entry);

static void destroy_entry(struct subproc *subproc, ssize_t index);

static bool
read_hex_sysfs_u32(const char *path, uint32_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }

    char buf[32] = {0};
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return false;
    }
    fclose(f);

    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(buf, &end, 0);
    if (errno != 0 || end == buf) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool
detect_intel_vulkan_device_id(char out[static 16]) {
    DIR *dir = opendir("/sys/class/drm");
    if (!dir) {
        return false;
    }

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "renderD", 6) != 0) {
            continue;
        }

        char vendor_path[PATH_MAX];
        char device_path[PATH_MAX];
        snprintf(vendor_path, sizeof(vendor_path), "/sys/class/drm/%s/device/vendor", ent->d_name);
        snprintf(device_path, sizeof(device_path), "/sys/class/drm/%s/device/device", ent->d_name);

        uint32_t vendor = 0, device = 0;
        if (!read_hex_sysfs_u32(vendor_path, &vendor) || !read_hex_sysfs_u32(device_path, &device)) {
            continue;
        }

        if (vendor == 0x8086) {
            snprintf(out, 16, "%04x:%04x", vendor, device);
            closedir(dir);
            return true;
        }
    }

    closedir(dir);
    return false;
}

static void
append_vk_instance_layer(const char *layer_name) {
    const char *layers = getenv("VK_INSTANCE_LAYERS");
    if (layers && strstr(layers, layer_name) != NULL) {
        return;
    }

    if (!layers || layers[0] == '\0') {
        setenv("VK_INSTANCE_LAYERS", layer_name, 1);
        return;
    }

    size_t need = strlen(layers) + 1 + strlen(layer_name) + 1;
    char *buf = malloc(need);
    if (!buf) {
        return;
    }
    snprintf(buf, need, "%s:%s", layers, layer_name);
    setenv("VK_INSTANCE_LAYERS", buf, 1);
    free(buf);
}

static int
handle_pidfd(int32_t fd, uint32_t mask, void *data) {
    struct subproc *subproc = data;

    ssize_t entry_index = -1;
    for (ssize_t i = 0; i < subproc->entries.len; i++) {
        if (subproc->entries.data[i].pidfd == fd) {
            entry_index = i;
            break;
        }
    }
    ww_assert(entry_index >= 0);

    struct subproc_entry *entry = &subproc->entries.data[entry_index];
    int status = 0;
    if (waitpid(entry->pid, &status, 0) != entry->pid) {
        ww_log_errno(LOG_ERROR, "failed to waitpid on child process %jd", (intmax_t)entry->pid);
    } else {
        if (WIFEXITED(status)) {
            ww_log(LOG_INFO, "subprocess %jd exited with code %d", (intmax_t)entry->pid,
                   WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            ww_log(LOG_INFO, "subprocess %jd killed by signal %d", (intmax_t)entry->pid,
                   WTERMSIG(status));
        } else {
            ww_log(LOG_INFO, "subprocess %jd exited (status=0x%x)", (intmax_t)entry->pid, status);
        }
    }
    if (pidfd_send_signal(entry->pidfd, SIGKILL, NULL, 0) != 0) {
        if (errno != ESRCH) {
            ww_log_errno(LOG_ERROR, "failed to kill child process %jd", (intmax_t)entry->pid);
        }
    }

    destroy_entry(subproc, entry_index);
    return 0;
}

static void
destroy_entry(struct subproc *subproc, ssize_t index) {
    struct subproc_entry *entry = &subproc->entries.data[index];

    wl_event_source_remove(entry->pidfd_src);
    close(entry->pidfd);

    list_subproc_entry_remove(&subproc->entries, index);
}

struct subproc *
subproc_create(struct server *server) {
    struct subproc *subproc = zalloc(1, sizeof(*subproc));
    subproc->server = server;
    subproc->entries = list_subproc_entry_create();
    return subproc;
}

void
subproc_destroy(struct subproc *subproc) {
    for (ssize_t i = 0; i < subproc->entries.len; i++) {
        struct subproc_entry *entry = &subproc->entries.data[i];

        if (pidfd_send_signal(entry->pidfd, SIGKILL, NULL, 0) != 0) {
            if (errno != ESRCH) {
                ww_log_errno(LOG_ERROR, "failed to kill child process %jd", (intmax_t)entry->pid);
            }
        }

        wl_event_source_remove(entry->pidfd_src);
        close(entry->pidfd);
    }

    list_subproc_entry_destroy(&subproc->entries);
    free(subproc);
}

void
subproc_exec(struct subproc *subproc, char *cmd[static 64]) {
    // Log environment variables for debugging
    const char *display = getenv("DISPLAY");
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    ww_log(LOG_INFO, "subproc_exec: DISPLAY=%s, WAYLAND_DISPLAY=%s, cmd=%s",
           display ? display : "(null)",
           wayland_display ? wayland_display : "(null)",
           cmd[0]);

    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "/tmp/waywall-subproc-%jd.log", (intmax_t)getpid());

        int out = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (out != -1) {
            (void)dup2(out, STDOUT_FILENO);
            (void)dup2(out, STDERR_FILENO);
            close(out);
        } else {
            // Fallback: silence stdout/stderr if log can't be created.
            int null_out = open("/dev/null", O_WRONLY);
            if (null_out != -1) {
                (void)dup2(null_out, STDOUT_FILENO);
                (void)dup2(null_out, STDERR_FILENO);
                close(null_out);
            }
        }

        // Explicitly set environment variables in child to ensure they're correct
        // This fixes issues on some systems (like NixOS) where environment may be modified
        if (display) {
            setenv("DISPLAY", display, 1);
        }
        if (wayland_display) {
            setenv("WAYLAND_DISPLAY", wayland_display, 1);
        }

        // Extend LD_LIBRARY_PATH for NixOS - needed for Java apps to find native libs
        // like libxkbcommon-x11 which JNativeHook depends on
        const char *ld_library_path = getenv("LD_LIBRARY_PATH");
        const char *nixos_sys_lib = "/run/current-system/sw/lib";
        if (ld_library_path) {
            char *new_path = malloc(strlen(ld_library_path) + strlen(nixos_sys_lib) + 2);
            if (new_path) {
                sprintf(new_path, "%s:%s", ld_library_path, nixos_sys_lib);
                setenv("LD_LIBRARY_PATH", new_path, 1);
                free(new_path);
            }
        } else {
            setenv("LD_LIBRARY_PATH", nixos_sys_lib, 1);
        }
        // NOTE: Do NOT set XKB_CONFIG_ROOT - libxkbcommon has the correct NixOS store path
        // compiled in. Setting it to an invalid path breaks xkb_context_new().

        // Force Java AWT/X11 settings for NixOS/Xwayland so GUI apps (e.g., Ninjabrain Bot)
        // create proper X11 buffers.
        setenv("_JAVA_AWT_WM_NONREPARENTING", "1", 1);
        setenv("AWT_TOOLKIT", "XToolkit", 1);
        setenv("GDK_BACKEND", "x11", 1);
        const char *jto = getenv("JAVA_TOOL_OPTIONS");
        const char *awt_flags =
            "-Dswing.defaultlaf=javax.swing.plaf.metal.MetalLookAndFeel "
            "-Dsun.java2d.xrender=true "
            "-Dsun.java2d.opengl=false "
            "-Dsun.awt.nopixmaps=true";
        if (jto && *jto) {
            size_t need = strlen(jto) + 1 + strlen(awt_flags) + 1;
            char *buf = malloc(need);
            if (buf) {
                snprintf(buf, need, "%s %s", jto, awt_flags);
                setenv("JAVA_TOOL_OPTIONS", buf, 1);
                free(buf);
            } else {
                setenv("JAVA_TOOL_OPTIONS", awt_flags, 1);
            }
        } else {
            setenv("JAVA_TOOL_OPTIONS", awt_flags, 1);
        }

        // If explicitly configured, run subprocesses on a specific GPU via DRI_PRIME.
        // Env override: WAYWALL_SUBPROC_DRI_PRIME=0/off/"" disables; any other value forces that GPU.
        const char *env_prime = getenv("WAYWALL_SUBPROC_DRI_PRIME");
        const char *dri_prime = NULL;
        bool prime_disabled = false;
        if (env_prime) {
            if (env_prime[0] == '\0' || strcasecmp(env_prime, "0") == 0 || strcasecmp(env_prime, "off") == 0) {
                prime_disabled = true;
            } else {
                dri_prime = env_prime;
            }
        } else {
            dri_prime = subproc->server->subprocess_dri_prime;
        }

        if (prime_disabled || !dri_prime || !*dri_prime) {
            unsetenv("DRI_PRIME");
        } else {
            setenv("DRI_PRIME", dri_prime, 1);
            // Allow tiled modifiers by default to avoid ReBAR-limited linear paths; opt back into
            // linear with WAYWALL_FORCE_LINEAR_DMABUF if needed for compatibility.
            if (getenv("WAYWALL_FORCE_LINEAR_DMABUF")) {
                setenv("INTEL_MODIFIER_OVERRIDE", "0x0", 1);
            } else {
                unsetenv("INTEL_MODIFIER_OVERRIDE");
            }
            // We intentionally avoid forcing __GLX_VENDOR_LIBRARY_NAME here; let GLX/Vulkan pick
            // the correct driver for the selected GPU.
            ww_log(LOG_INFO, "subprocess: setting DRI_PRIME=%s for cross-GPU rendering", dri_prime);
        }

        // --------------------------------------------------------------------
        // System RAM forcing experiments (Mesa/ANV knobs)
        // --------------------------------------------------------------------
        // Primary knob: enable Mesa's vram_report_limit layer to reduce reported
        // device-local heap sizes for Intel, nudging WSI allocations into sysmem.
        //
        // Usage:
        // - WAYWALL_SUBPROC_VRAM_LIMIT_MIB=<MiB>
        // - Optional: WAYWALL_SUBPROC_VRAM_LIMIT_DEVICE_ID=<vendorID:deviceID>
        //   (auto-detects first Intel render node if unset)
        const char *vram_limit_mib = getenv("WAYWALL_SUBPROC_VRAM_LIMIT_MIB");
        if (vram_limit_mib && vram_limit_mib[0] != '\0') {
            char *end = NULL;
            errno = 0;
            long limit = strtol(vram_limit_mib, &end, 10);
            if (errno == 0 && end && *end == '\0' && limit >= 0) {
                const char *dev_id = getenv("WAYWALL_SUBPROC_VRAM_LIMIT_DEVICE_ID");
                char detected[16] = {0};
                if (!dev_id || dev_id[0] == '\0') {
                    if (detect_intel_vulkan_device_id(detected)) {
                        dev_id = detected;
                    }
                }

                if (dev_id && dev_id[0] != '\0') {
                    append_vk_instance_layer("VK_LAYER_MESA_vram_report_limit");
                    setenv("VK_VRAM_REPORT_LIMIT_DEVICE_ID", dev_id, 1);
                    setenv("VK_VRAM_REPORT_LIMIT_HEAP_SIZE", vram_limit_mib, 1);
                    ww_log(LOG_INFO,
                           "subprocess: enabled VK_LAYER_MESA_vram_report_limit (device=%s, heap=%s MiB)",
                           dev_id, vram_limit_mib);
                } else {
                    ww_log(LOG_WARN,
                           "subprocess: WAYWALL_SUBPROC_VRAM_LIMIT_MIB set but no Intel device id found; set WAYWALL_SUBPROC_VRAM_LIMIT_DEVICE_ID=8086:xxxx");
                }
            } else {
                ww_log(LOG_WARN, "subprocess: invalid WAYWALL_SUBPROC_VRAM_LIMIT_MIB=%s", vram_limit_mib);
            }
        }

        // Secondary knob: pass through ANV_SYS_MEM_LIMIT when requested.
        const char *anv_sys_mem_limit = getenv("WAYWALL_SUBPROC_ANV_SYS_MEM_LIMIT");
        if (anv_sys_mem_limit && anv_sys_mem_limit[0] != '\0') {
            setenv("ANV_SYS_MEM_LIMIT", anv_sys_mem_limit, 1);
            ww_log(LOG_INFO, "subprocess: setting ANV_SYS_MEM_LIMIT=%s", anv_sys_mem_limit);
        }

        execvp(cmd[0], cmd);
        ww_log_errno(LOG_ERROR, "failed to execvp() in child porcess");
        exit(EXIT_FAILURE);
    } else if (pid == -1) {
        // Parent process (error)
        ww_log_errno(LOG_ERROR, "failed to fork() child process");
        return;
    }

    ww_log(LOG_INFO, "subproc_exec: subprocess %jd logs at /tmp/waywall-subproc-%jd.log",
           (intmax_t)pid, (intmax_t)pid);

    int pidfd = pidfd_open(pid, 0);
    if (pidfd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open pidfd for subprocess %jd", (intmax_t)pid);
        return;
    }

    struct wl_event_source *src =
        wl_event_loop_add_fd(wl_display_get_event_loop(subproc->server->display), pidfd,
                             WL_EVENT_READABLE, handle_pidfd, subproc);
    check_alloc(src);

    struct subproc_entry entry = {0};

    entry.pid = pid;
    entry.pidfd = pidfd;
    entry.pidfd_src = src;

    list_subproc_entry_append(&subproc->entries, entry);
}
