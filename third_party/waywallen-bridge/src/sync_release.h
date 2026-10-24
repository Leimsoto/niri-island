/* waywallen-bridge — internal drm_syncobj helpers (NOT public).
 *
 * Used by pool.c / pool_egl_gbm.c / pool_vulkan.c to own the
 * producer's release timeline. Mirrors the kernel uAPI we need from
 * <drm/drm.h> without pulling in libdrm: layouts must match
 * <drm/drm.h> exactly.
 *
 * All functions return 0 on success, a negated errno on failure
 * (typed as `int`). The drm_fd may be the bridge's own /dev/dri/render*
 * or one shared with the plugin's EGL/GBM stack — the kernel handle
 * is per-fd, so the same fd must be used across create / export /
 * destroy / wait. */
#ifndef WAYWALLEN_BRIDGE_SYNC_RELEASE_H
#define WAYWALLEN_BRIDGE_SYNC_RELEASE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open `/dev/dri/renderD<minor>` for the given DRM minor (must be in
 * the render-node range 128..). Returns fd on success, -EINVAL if the
 * minor is out of range, or a negated errno from open(2). The plugin
 * is expected to know its own render-node minor — see
 * ww_bridge_vk_query_render_node in probe_vk.h for the canonical way
 * to derive it from a VkPhysicalDevice. */
int ww_drm_open_render_node_by_minor(uint32_t minor);

/* Create a fresh drm_syncobj. The new handle starts unsignaled at
 * timeline value 0. Use as both binary and timeline (the kernel
 * doesn't distinguish — points are just u64s, with 0 reserved as
 * "binary unsignaled"). */
int ww_drm_syncobj_create(int drm_fd, uint32_t* out_handle);

/* OPAQUE_FD export. Caller owns the returned fd and must close() it
 * (the kernel dup'd it). */
int ww_drm_syncobj_export_fd(int drm_fd, uint32_t handle, int* out_fd);

/* DESTROY. Idempotent on (handle == 0). */
void ww_drm_syncobj_destroy(int drm_fd, uint32_t handle);

/* TIMELINE_WAIT on a single (handle, point) with WAIT_FOR_SUBMIT.
 * `timeout_ms` is wall-clock from now; passing 0 polls. Returns 0 on
 * signal, -ETIME on timeout, other negated errno on ioctl failure. */
int ww_drm_syncobj_timeline_wait(int drm_fd, uint32_t handle, uint64_t point, uint32_t timeout_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WAYWALLEN_BRIDGE_SYNC_RELEASE_H */
