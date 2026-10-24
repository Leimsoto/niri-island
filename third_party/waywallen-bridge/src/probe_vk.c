/* waywallen-bridge — Vulkan dispatch loader + GPU info logger.
 *
 * Implements probe_vk.h. No libvulkan linkage; the dispatch table is
 * populated by the plugin-supplied vkGetInstanceProcAddr against a
 * live VkInstance.
 */
#include <waywallen-bridge/probe_vk.h>
#include <waywallen-bridge/bridge.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

int ww_bridge_vk_dt_load(ww_bridge_vk_dt_t* dt, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                         VkInstance instance) {
    if (! dt || ! get_instance_proc_addr) return -EINVAL;
    memset(dt, 0, sizeof(*dt));

    dt->vkGetInstanceProcAddr = get_instance_proc_addr;

    /* Physical-device queries are resolved against `instance`. With
     * VK_NULL_HANDLE only the pre-instance entry points resolve, so
     * pass a real instance to populate the rest. Per-field explicit
     * casts keep the unit free of `typeof` under -Wpedantic. */
    dt->vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)get_instance_proc_addr(
        instance, "vkEnumeratePhysicalDevices");
    dt->vkEnumerateDeviceExtensionProperties =
        (PFN_vkEnumerateDeviceExtensionProperties)get_instance_proc_addr(
            instance, "vkEnumerateDeviceExtensionProperties");
    dt->vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)get_instance_proc_addr(
        instance, "vkGetPhysicalDeviceProperties");
    dt->vkGetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)get_instance_proc_addr(
        instance, "vkGetPhysicalDeviceProperties2");
    dt->vkGetPhysicalDeviceFormatProperties2 =
        (PFN_vkGetPhysicalDeviceFormatProperties2)get_instance_proc_addr(
            instance, "vkGetPhysicalDeviceFormatProperties2");
    return 0;
}

void ww_bridge_vk_log_gpu_info(const char* prefix, const ww_bridge_vk_dt_t* dt,
                               VkPhysicalDevice phys) {
    if (! dt || phys == VK_NULL_HANDLE) return;
    if (! dt->vkGetPhysicalDeviceProperties && ! dt->vkGetPhysicalDeviceProperties2) return;

    /* Prefer Properties2 + DriverProperties for richer driver info;
     * fall back to plain Properties when the 1.1+ chain isn't loaded. */
    VkPhysicalDeviceProperties props;
    memset(&props, 0, sizeof(props));

    char drv_buf[256];
    drv_buf[0] = '\0';

    if (dt->vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceDriverProperties drv;
        memset(&drv, 0, sizeof(drv));
        drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

        VkPhysicalDeviceProperties2 p2;
        memset(&p2, 0, sizeof(p2));
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &drv;

        dt->vkGetPhysicalDeviceProperties2(phys, &p2);
        props = p2.properties;

        if (drv.driverName[0] || drv.driverInfo[0]) {
            snprintf(drv_buf,
                     sizeof(drv_buf),
                     "%s | %s",
                     drv.driverName[0] ? drv.driverName : "(unknown)",
                     drv.driverInfo[0] ? drv.driverInfo : "(no info)");
        }
    } else {
        dt->vkGetPhysicalDeviceProperties(phys, &props);
    }

    char api_buf[32];
    snprintf(api_buf,
             sizeof(api_buf),
             "%u.%u.%u",
             VK_API_VERSION_MAJOR(props.apiVersion),
             VK_API_VERSION_MINOR(props.apiVersion),
             VK_API_VERSION_PATCH(props.apiVersion));

    /* Render the device name through a stack copy so the helper can
     * emit a stable pointer (deviceName is a fixed-size char[]). */
    char dev_buf[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 1];
    snprintf(dev_buf, sizeof(dev_buf), "%s", props.deviceName);

    const ww_gpu_info_field_t fields[] = {
        { "device", dev_buf },
        { "api ver", api_buf },
        { "driver", drv_buf[0] ? drv_buf : NULL },
    };
    ww_bridge_log_gpu_info(prefix, fields, sizeof(fields) / sizeof(fields[0]));
}

int ww_bridge_vk_resolve_render_node(const ww_bridge_vk_dt_t* dt, VkInstance instance,
                                     const char* render_node_path, uint8_t out_uuid[16]) {
    if (! dt || ! render_node_path || ! out_uuid) return -EINVAL;
    if (instance == VK_NULL_HANDLE) return -EINVAL;
    if (! dt->vkEnumeratePhysicalDevices || ! dt->vkEnumerateDeviceExtensionProperties ||
        ! dt->vkGetPhysicalDeviceProperties2)
        return -ENOTSUP;

    struct stat st;
    if (stat(render_node_path, &st) != 0) return -errno;
    const dev_t want_rdev = st.st_rdev;

    uint32_t pd_count = 0;
    if (dt->vkEnumeratePhysicalDevices(instance, &pd_count, NULL) != VK_SUCCESS || pd_count == 0)
        return -ENOENT;

    VkPhysicalDevice* pds = (VkPhysicalDevice*)calloc(pd_count, sizeof(*pds));
    if (! pds) return -ENOMEM;
    if (dt->vkEnumeratePhysicalDevices(instance, &pd_count, pds) != VK_SUCCESS) {
        free(pds);
        return -ENOENT;
    }

    int rc = -ENOENT;
    for (uint32_t i = 0; i < pd_count; ++i) {
        VkPhysicalDevice pd = pds[i];

        /* Need VK_EXT_physical_device_drm to map (renderMajor, renderMinor)
         * to a /dev/dri path. Skip devices without it. */
        uint32_t ec = 0;
        if (dt->vkEnumerateDeviceExtensionProperties(pd, NULL, &ec, NULL) != VK_SUCCESS || ec == 0)
            continue;
        VkExtensionProperties* exts = (VkExtensionProperties*)calloc(ec, sizeof(*exts));
        if (! exts) continue;
        if (dt->vkEnumerateDeviceExtensionProperties(pd, NULL, &ec, exts) != VK_SUCCESS) {
            free(exts);
            continue;
        }
        int has_drm = 0;
        for (uint32_t j = 0; j < ec; ++j) {
            if (strcmp(exts[j].extensionName, "VK_EXT_physical_device_drm") == 0) {
                has_drm = 1;
                break;
            }
        }
        free(exts);
        if (! has_drm) continue;

        VkPhysicalDeviceDrmPropertiesEXT drm;
        memset(&drm, 0, sizeof(drm));
        drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;

        VkPhysicalDeviceIDProperties id;
        memset(&id, 0, sizeof(id));
        id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        id.pNext = &drm;

        VkPhysicalDeviceProperties2 p2;
        memset(&p2, 0, sizeof(p2));
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &id;
        dt->vkGetPhysicalDeviceProperties2(pd, &p2);

        if (! drm.hasRender) continue;
        const dev_t pd_rdev = makedev((unsigned)drm.renderMajor, (unsigned)drm.renderMinor);
        if (pd_rdev != want_rdev) continue;

        memcpy(out_uuid, id.deviceUUID, VK_UUID_SIZE);
        rc = 0;
        break;
    }

    free(pds);
    return rc;
}

int ww_bridge_vk_query_render_node(const ww_bridge_vk_dt_t* dt, VkPhysicalDevice phys,
                                   uint32_t* out_major, uint32_t* out_minor) {
    if (out_major) *out_major = 0;
    if (out_minor) *out_minor = 0;
    if (! dt || phys == VK_NULL_HANDLE || ! out_major || ! out_minor) return -EINVAL;
    if (! dt->vkGetPhysicalDeviceProperties2) return -ENOTSUP;

    VkPhysicalDeviceDrmPropertiesEXT drm;
    memset(&drm, 0, sizeof(drm));
    drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;

    VkPhysicalDeviceProperties2 p2;
    memset(&p2, 0, sizeof(p2));
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &drm;
    dt->vkGetPhysicalDeviceProperties2(phys, &p2);

    if (! drm.hasRender) return -ENOENT;
    if (drm.renderMajor < 0 || drm.renderMinor < 0) return -ERANGE;
    if ((uint64_t)drm.renderMajor > UINT32_MAX || (uint64_t)drm.renderMinor > UINT32_MAX)
        return -ERANGE;

    *out_major = (uint32_t)drm.renderMajor;
    *out_minor = (uint32_t)drm.renderMinor;
    return 0;
}
