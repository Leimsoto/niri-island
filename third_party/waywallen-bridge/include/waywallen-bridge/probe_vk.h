/* waywallen-bridge — Vulkan dispatch table and probe helpers.
 *
 * Bridge does NOT link libvulkan. Function-pointer types are pulled
 * from <vulkan/vulkan.h>; the plugin loads `vkGetInstanceProcAddr`
 * (typically via dlsym from libvulkan, or by chaining through
 * `vkCreateInstance`'s pNext) and hands it to bridge to populate the
 * dispatch table.
 *
 * Pattern mirrors `probe_egl.h`. Plugins pick exactly one backend at
 * init time.
 */
#ifndef WAYWALLEN_BRIDGE_PROBE_VK_H
#define WAYWALLEN_BRIDGE_PROBE_VK_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dispatch table populated by ww_bridge_vk_dt_load. NULL members
 * mean the entry point wasn't resolvable on the supplied
 * instance/device; helpers check before invoking. */
typedef struct ww_bridge_vk_dt {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    /* Physical-device queries. Resolved against the live instance. */
    PFN_vkEnumeratePhysicalDevices           vkEnumeratePhysicalDevices;
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
    PFN_vkGetPhysicalDeviceProperties        vkGetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceProperties2       vkGetPhysicalDeviceProperties2;
    PFN_vkGetPhysicalDeviceFormatProperties2 vkGetPhysicalDeviceFormatProperties2;
} ww_bridge_vk_dt_t;

/* Resolve every dispatch entry by calling `get_instance_proc_addr`
 * with `instance`. Pre-instance entry points (the ones legal with
 * `instance == VK_NULL_HANDLE`) are not part of this table — Vulkan
 * lets you call them on the loader-provided gIPA before
 * vkCreateInstance, and bridge has no use for them.
 *
 * Returns 0 on success, -EINVAL if `dt` or `get_instance_proc_addr`
 * is NULL. Members stay NULL where the entry point isn't
 * resolvable. */
int ww_bridge_vk_dt_load(ww_bridge_vk_dt_t* dt, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                         VkInstance instance);

/* Print a "GPU info" diagnostic block to stderr for the picked
 * physical device:
 *
 *     {prefix}: GPU info
 *       device:    {deviceName}
 *       api ver:   {major.minor.patch}
 *       driver:    {driverName} | {driverInfo}     (when DriverProperties available)
 *
 * Uses `vkGetPhysicalDeviceProperties2` + VkPhysicalDeviceDriverProperties
 * when available; falls back to `vkGetPhysicalDeviceProperties` for
 * the bare-minimum device name + api version. No-op when both are
 * NULL or `phys` is VK_NULL_HANDLE. */
void ww_bridge_vk_log_gpu_info(const char* prefix, const ww_bridge_vk_dt_t* dt,
                               VkPhysicalDevice phys);

/* Resolve a "/dev/dri/renderD*" path to the matching VkPhysicalDevice's
 * 16-byte `VkPhysicalDeviceIDProperties.deviceUUID`, so callers can
 * filter their own device picker by UUID (e.g. wescene's
 * `Instance::ChoosePhysicalDevice`).
 *
 * Walks `vkEnumeratePhysicalDevices(instance)`, filters devices that
 * expose `VK_EXT_physical_device_drm`, then via
 * `vkGetPhysicalDeviceProperties2` (chained with DRM + ID props) finds
 * the one whose `(renderMajor, renderMinor)` matches the major:minor of
 * `stat(render_node_path).st_rdev`.
 *
 * Caller owns `instance`; it must be Vulkan 1.1+ or have
 * `VK_KHR_get_physical_device_properties2` enabled. `dt` must already be
 * populated via `ww_bridge_vk_dt_load(dt, gIPA, instance)`.
 *
 * Returns:
 *   0           on success.
 *  -EINVAL      on NULL args.
 *  -ENOTSUP     dt is missing one of vkEnumeratePhysicalDevices,
 *               vkEnumerateDeviceExtensionProperties, or
 *               vkGetPhysicalDeviceProperties2.
 *  -ENOENT      no physical device matched the render major:minor.
 *  -errno       negative errno from stat(render_node_path) on failure.
 */
int ww_bridge_vk_resolve_render_node(const ww_bridge_vk_dt_t* dt, VkInstance instance,
                                     const char* render_node_path, uint8_t out_uuid[16]);

/* Query VK_EXT_physical_device_drm for `phys` and write the render-node
 * (major, minor) out. Plugins call this to fill
 * `ww_pool_vulkan_init_t.drm_render_{major,minor}` so bridge can ship
 * them in `Ready` / `format_caps`.
 *
 * `dt->vkGetPhysicalDeviceProperties2` must be resolved (Vulkan 1.1+
 * or VK_KHR_get_physical_device_properties2 enabled on the instance);
 * the DRM properties struct is chained into the standard properties2
 * query and quietly ignored by drivers that don't advertise the
 * extension.
 *
 * Returns:
 *   0           on success.
 *  -EINVAL      on NULL args.
 *  -ENOTSUP     dt->vkGetPhysicalDeviceProperties2 is NULL.
 *  -ENOENT      driver returned hasRender=0 (no DRM render-node binding;
 *               typical for software renderers / nvidia<470 / WSL).
 *  -ERANGE      reported renderMajor/renderMinor doesn't fit u32.
 *
 * On any non-zero return `*out_major` and `*out_minor` are written to 0.
 */
int ww_bridge_vk_query_render_node(const ww_bridge_vk_dt_t* dt, VkPhysicalDevice phys,
                                   uint32_t* out_major, uint32_t* out_minor);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WAYWALLEN_BRIDGE_PROBE_VK_H */
