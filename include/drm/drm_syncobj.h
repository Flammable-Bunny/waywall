#pragma once
/* Local stub for systems without libdrm's drm_syncobj.h.
 * We only need the ioctl definitions from drm.h; function prototypes are
 * declared manually in server/gl.c when the real header is missing.
 */
#include <libdrm/drm.h>
