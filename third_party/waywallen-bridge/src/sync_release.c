/* waywallen-bridge — drm_syncobj helpers, kernel uAPI hand-rolled.
 *
 * Equivalent to <drm/drm.h> but redefined here so we don't pull
 * libdrm into the link surface. Kernel layouts MUST match exactly. */
#include "sync_release.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Mirror <drm/drm.h>. */
struct ww_drm_syncobj_create {
    uint32_t handle;
    uint32_t flags;
};
struct ww_drm_syncobj_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t  fd;
    uint32_t pad;
};
struct ww_drm_syncobj_destroy {
    uint32_t handle;
    uint32_t pad;
};
struct ww_drm_syncobj_timeline_wait {
    uint64_t handles;      /* ptr to u32 array */
    uint64_t points;       /* ptr to u64 array */
    int64_t  timeout_nsec; /* absolute CLOCK_MONOTONIC */
    uint32_t count_handles;
    uint32_t flags;
    uint32_t first_signaled;
    uint32_t pad;
};

#ifndef DRM_IOCTL_BASE
#    define DRM_IOCTL_BASE 'd'
#endif
#define WW_DRM_IOCTL_SYNCOBJ_CREATE       _IOWR(DRM_IOCTL_BASE, 0xBF, struct ww_drm_syncobj_create)
#define WW_DRM_IOCTL_SYNCOBJ_DESTROY      _IOWR(DRM_IOCTL_BASE, 0xC0, struct ww_drm_syncobj_destroy)
#define WW_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD _IOWR(DRM_IOCTL_BASE, 0xC1, struct ww_drm_syncobj_handle)
#define WW_DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT \
    _IOWR(DRM_IOCTL_BASE, 0xCA, struct ww_drm_syncobj_timeline_wait)

#define WW_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL        (1u << 0)
#define WW_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1u << 1)

int ww_drm_open_render_node_by_minor(uint32_t minor) {
    if (minor < 128 || minor > 255) return -EINVAL;
    char path[32];
    int  n = snprintf(path, sizeof(path), "/dev/dri/renderD%u", minor);
    if (n < 0 || (size_t)n >= sizeof(path)) return -EINVAL;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -errno;
    return fd;
}

int ww_drm_syncobj_create(int drm_fd, uint32_t* out_handle) {
    if (drm_fd < 0 || ! out_handle) return -EINVAL;
    struct ww_drm_syncobj_create cr = { 0 };
    if (ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_CREATE, &cr) != 0) {
        return -errno;
    }
    *out_handle = cr.handle;
    return 0;
}

int ww_drm_syncobj_export_fd(int drm_fd, uint32_t handle, int* out_fd) {
    if (drm_fd < 0 || handle == 0 || ! out_fd) return -EINVAL;
    struct ww_drm_syncobj_handle h2fd = { 0 };
    h2fd.handle                       = handle;
    h2fd.fd                           = -1;
    if (ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &h2fd) != 0) {
        return -errno;
    }
    *out_fd = h2fd.fd;
    return 0;
}

void ww_drm_syncobj_destroy(int drm_fd, uint32_t handle) {
    if (drm_fd < 0 || handle == 0) return;
    struct ww_drm_syncobj_destroy d = { handle, 0 };
    (void)ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_DESTROY, &d);
}

int ww_drm_syncobj_timeline_wait(int drm_fd, uint32_t handle, uint64_t point, uint32_t timeout_ms) {
    if (drm_fd < 0 || handle == 0) return -EINVAL;
    /* Point 0 is "never submitted" — by convention treat it as
     * already-signaled to avoid blocking on the first frame. */
    if (point == 0) return 0;

    struct timespec ts = { 0 };
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -errno;
    int64_t deadline =
        (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec + (int64_t)timeout_ms * 1000000;

    uint32_t                            handles[1] = { handle };
    uint64_t                            points[1]  = { point };
    struct ww_drm_syncobj_timeline_wait arg        = { 0 };
    arg.handles                                    = (uintptr_t)handles;
    arg.points                                     = (uintptr_t)points;
    arg.timeout_nsec                               = deadline;
    arg.count_handles                              = 1;
    arg.flags = WW_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | WW_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;

    if (ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &arg) != 0) {
        return -errno;
    }
    return 0;
}
