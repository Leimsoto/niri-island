/* waywallen-bridge — EGL/GL dispatch table and probe helpers.
 *
 * Bridge does NOT link libEGL or libGLES. Function-pointer types are
 * pulled from the system <EGL/egl.h> + <EGL/eglext.h> headers; the
 * plugin resolves entry points via its own `eglGetProcAddress`
 * (mandatory for core EGL on EGL 1.5+) and hands the table to bridge
 * helpers. Bridge then invokes through the table without needing the
 * libs in its DT_NEEDED.
 *
 * Pattern mirrors `probe_vk.h` (Vulkan side). Plugins pick exactly
 * one backend at init time.
 */
#ifndef WAYWALLEN_BRIDGE_PROBE_EGL_H
#define WAYWALLEN_BRIDGE_PROBE_EGL_H

#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GLenum constants the bridge needs for `glGetString`. Hardcoded so
 * we don't pull <GLES2/gl2.h> into the bridge compile surface; the
 * GL spec freezes these values. */
#ifndef WW_GL_VENDOR
#    define WW_GL_VENDOR                   0x1F00
#    define WW_GL_RENDERER                 0x1F01
#    define WW_GL_VERSION                  0x1F02
#    define WW_GL_SHADING_LANGUAGE_VERSION 0x8B8C
#endif

/* `eglGetProcAddress` signature, exposed under a friendlier name so
 * callers can pass the function directly without casting. */
typedef __eglMustCastToProperFunctionPointerType (*ww_bridge_egl_get_proc_addr_fn)(
    const char* name);

/* Dispatch table populated by ww_bridge_egl_dt_load. NULL members
 * mean the entry point wasn't resolvable; helper functions check
 * before invoking. Add new fields here as more EGL probe code moves
 * into the bridge — the layout is stable across additions because
 * loaders use named lookups. */
typedef struct ww_bridge_egl_dt {
    /* Core EGL — re-resolvable via get_proc_addr on EGL 1.5+. */
    PFNEGLQUERYSTRINGPROC eglQueryString;
    PFNEGLGETERRORPROC    eglGetError;
    /* DMA-BUF / device extensions. */
    PFNEGLQUERYDMABUFFORMATSEXTPROC   eglQueryDmaBufFormatsEXT;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT;
    PFNEGLQUERYDISPLAYATTRIBEXTPROC   eglQueryDisplayAttribEXT;
    PFNEGLQUERYDEVICESTRINGEXTPROC    eglQueryDeviceStringEXT;
    /* Image / sync extensions used by DMA-BUF producers. */
    PFNEGLCREATEIMAGEKHRPROC          eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC         eglDestroyImageKHR;
    PFNEGLCREATESYNCKHRPROC           eglCreateSyncKHR;
    PFNEGLDESTROYSYNCKHRPROC          eglDestroySyncKHR;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID;
    /* GL via the EGL loader. Signature mirrors `glGetString` —
     * declared inline so the bridge stays free of <GLES2/gl2.h>. */
    const unsigned char* (*glGetString)(unsigned int /* GLenum */);
} ww_bridge_egl_dt_t;

/* Resolve every dispatch entry by calling `get_proc_addr` for its
 * canonical name. Members that don't resolve stay NULL. Returns 0
 * on success, -EINVAL if either argument is NULL. */
int ww_bridge_egl_dt_load(ww_bridge_egl_dt_t* dt, ww_bridge_egl_get_proc_addr_fn get_proc_addr);

/* Per-modifier callback for `ww_bridge_egl_query_modifiers_for_fourcc`.
 * Invoked once per modifier the EGL driver advertises for the given
 * fourcc. `external_only` is the third out-array of
 * eglQueryDmaBufModifiersEXT — non-zero means the modifier is only
 * importable as `GL_TEXTURE_EXTERNAL_OES`, which non-YUV
 * GL_TEXTURE_2D consumers typically reject. The callback decides
 * whether to keep, filter, or transform the entry. */
typedef void (*ww_bridge_egl_modifier_emit_fn)(uint64_t modifier, int external_only, void* user);

/* Run the two-call eglQueryDmaBufModifiersEXT idiom for one fourcc
 * and emit one (modifier, external_only) tuple per result.
 *
 * Implicit-modifier-only fourccs (driver returns 0 modifiers) yield
 * zero emissions and a return of 0; the caller decides whether to
 * synthesize a LINEAR fallback or skip the fourcc. The helper does
 * NOT filter external_only — callers that want only GL_TEXTURE_2D-
 * importable modifiers should drop entries with external_only != 0
 * inside the callback.
 *
 * Returns:
 *   0        on success (including the zero-modifier case)
 *   -EINVAL  if `dt`, `emit`, or `dt->eglQueryDmaBufModifiersEXT` is NULL
 *   -EIO     if either eglQueryDmaBufModifiersEXT call returns EGL_FALSE
 *   -ENOMEM  on allocation failure
 */
int ww_bridge_egl_query_modifiers_for_fourcc(const ww_bridge_egl_dt_t* dt, EGLDisplay display,
                                             uint32_t fourcc, ww_bridge_egl_modifier_emit_fn emit,
                                             void* user);

/* Print a "GPU info" diagnostic block to stderr:
 *
 *     {prefix}: GPU info
 *       egl vendor:  ...
 *       egl version: ... (major.minor)
 *       egl client:  ...
 *       gl vendor:   ...
 *       gl renderer: ...
 *       gl version:  ...
 *       glsl ver:    ...
 *
 * Both `dt->eglQueryString` and `dt->glGetString` must be populated;
 * the call is a no-op otherwise. NULL strings render as "(null)".
 * `egl_major`/`egl_minor` are typically what `eglInitialize` wrote. */
void ww_bridge_egl_log_gpu_info(const char* prefix, const ww_bridge_egl_dt_t* dt,
                                EGLDisplay display, int egl_major, int egl_minor);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WAYWALLEN_BRIDGE_PROBE_EGL_H */
