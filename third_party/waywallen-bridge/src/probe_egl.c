/* waywallen-bridge — EGL dispatch loader + GPU info logger.
 *
 * Implements probe_egl.h. No libEGL/libGLES linkage; the dispatch
 * table is populated by the plugin-supplied get_proc_addr.
 */
#include <waywallen-bridge/probe_egl.h>
#include <waywallen-bridge/bridge.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ww_bridge_egl_dt_load(ww_bridge_egl_dt_t* dt, ww_bridge_egl_get_proc_addr_fn get_proc_addr) {
    if (! dt || ! get_proc_addr) return -EINVAL;
    memset(dt, 0, sizeof(*dt));

    /* Each cast goes via (void *) to silence -Wcast-function-type:
     * get_proc_addr returns the generic
     * __eglMustCastToProperFunctionPointerType, which we re-cast to
     * the per-entry PFN type. Explicit per-field expansion (rather
     * than a `typeof` macro) keeps the unit free of GNU extensions
     * under -Wpedantic. */
    dt->eglQueryString = (PFNEGLQUERYSTRINGPROC)(void*)get_proc_addr("eglQueryString");
    dt->eglGetError    = (PFNEGLGETERRORPROC)(void*)get_proc_addr("eglGetError");
    dt->eglQueryDmaBufFormatsEXT =
        (PFNEGLQUERYDMABUFFORMATSEXTPROC)(void*)get_proc_addr("eglQueryDmaBufFormatsEXT");
    dt->eglQueryDmaBufModifiersEXT =
        (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)(void*)get_proc_addr("eglQueryDmaBufModifiersEXT");
    dt->eglQueryDisplayAttribEXT =
        (PFNEGLQUERYDISPLAYATTRIBEXTPROC)(void*)get_proc_addr("eglQueryDisplayAttribEXT");
    dt->eglQueryDeviceStringEXT =
        (PFNEGLQUERYDEVICESTRINGEXTPROC)(void*)get_proc_addr("eglQueryDeviceStringEXT");
    dt->eglCreateImageKHR  = (PFNEGLCREATEIMAGEKHRPROC)(void*)get_proc_addr("eglCreateImageKHR");
    dt->eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)(void*)get_proc_addr("eglDestroyImageKHR");
    dt->eglCreateSyncKHR   = (PFNEGLCREATESYNCKHRPROC)(void*)get_proc_addr("eglCreateSyncKHR");
    dt->eglDestroySyncKHR  = (PFNEGLDESTROYSYNCKHRPROC)(void*)get_proc_addr("eglDestroySyncKHR");
    dt->eglDupNativeFenceFDANDROID =
        (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)(void*)get_proc_addr("eglDupNativeFenceFDANDROID");
    /* glGetString uses our local inline signature; cast to it. */
    dt->glGetString = (const unsigned char* (*)(unsigned int))(void*)get_proc_addr("glGetString");
    return 0;
}

int ww_bridge_egl_query_modifiers_for_fourcc(const ww_bridge_egl_dt_t* dt, EGLDisplay display,
                                             uint32_t fourcc, ww_bridge_egl_modifier_emit_fn emit,
                                             void* user) {
    if (! dt || ! emit || ! dt->eglQueryDmaBufModifiersEXT) return -EINVAL;

    /* First call: count only. NULL out-arrays are explicitly allowed
     * by EGL_EXT_image_dma_buf_import_modifiers. */
    EGLint count = 0;
    if (! dt->eglQueryDmaBufModifiersEXT(display, (EGLint)fourcc, 0, NULL, NULL, &count)) {
        return -EIO;
    }
    /* Implicit-modifier-only fourcc — emission is the caller's call. */
    if (count <= 0) return 0;

    EGLuint64KHR* mods     = calloc((size_t)count, sizeof(*mods));
    EGLBoolean*   ext_only = calloc((size_t)count, sizeof(*ext_only));
    if (! mods || ! ext_only) {
        free(mods);
        free(ext_only);
        return -ENOMEM;
    }
    int rc = 0;
    if (! dt->eglQueryDmaBufModifiersEXT(display, (EGLint)fourcc, count, mods, ext_only, &count)) {
        rc = -EIO;
    } else {
        for (EGLint i = 0; i < count; ++i) {
            emit((uint64_t)mods[i], ext_only[i] ? 1 : 0, user);
        }
    }
    free(mods);
    free(ext_only);
    return rc;
}

void ww_bridge_egl_log_gpu_info(const char* prefix, const ww_bridge_egl_dt_t* dt,
                                EGLDisplay display, int egl_major, int egl_minor) {
    if (! dt || ! dt->eglQueryString || ! dt->glGetString) return;

    /* Compose "EGL_VERSION (major.minor)" so the line matches the
     * old hand-written fprintf layout in mpv's renderer. */
    const char* egl_ver_str = dt->eglQueryString(display, EGL_VERSION);
    char        egl_ver_buf[64];
    snprintf(egl_ver_buf,
             sizeof(egl_ver_buf),
             "%s (%d.%d)",
             egl_ver_str ? egl_ver_str : "(null)",
             egl_major,
             egl_minor);

    const ww_gpu_info_field_t fields[] = {
        { "egl vendor", dt->eglQueryString(display, EGL_VENDOR) },
        { "egl version", egl_ver_buf },
        { "egl client", dt->eglQueryString(display, EGL_CLIENT_APIS) },
        { "gl vendor", (const char*)dt->glGetString(WW_GL_VENDOR) },
        { "gl renderer", (const char*)dt->glGetString(WW_GL_RENDERER) },
        { "gl version", (const char*)dt->glGetString(WW_GL_VERSION) },
        { "glsl ver", (const char*)dt->glGetString(WW_GL_SHADING_LANGUAGE_VERSION) },
    };
    ww_bridge_log_gpu_info(prefix, fields, sizeof(fields) / sizeof(fields[0]));
}
