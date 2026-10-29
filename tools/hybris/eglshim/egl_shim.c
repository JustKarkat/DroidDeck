/* egl_shim.c — libEGL.so.1 window-surface shim for CEF-on-Tegra (ShieldDeck).
 *
 * Why: the Tegra EGL (via hybris) is built with egl platform=null — it cannot
 * accept an X11 native display/window. ANGLE's gles-egl backend (which the CEF
 * GPU process uses after droiddeck-cef-patch) dlopens "libEGL.so.1" and drives
 * it with X11 objects, so it fails and silently falls back to SwiftShader.
 * This shim sits where ANGLE looks for the system EGL, forwards everything to
 * the real hybris EGL by absolute path, and emulates window surfaces:
 *   eglGetDisplay / eglGetPlatformDisplay[EXT]  -> the real default display
 *   eglCreateWindowSurface(X11 XID)             -> pbuffer at the window size
 *   eglSwapBuffers                              -> glReadPixels + XPutImage blit
 *   eglQuerySurface                             -> the X window's current size
 * Rendering is 100% on the Tegra GPU; only the final readback + X11 put-image
 * (960x540x4 ≈ 2 MB/frame) run on the CPU.
 *
 * Design notes:
 * - Our own X connection (XOpenDisplay(NULL)) — never touch Chromium's Xlib
 *   state; window IDs are server-global.
 * - The real lib is dlopened by absolute path (soname dedup would hand us
 *   glvnd or ourselves).
 * - Swap variants (SwapBuffersWithDamage*) are intercepted at
 *   eglGetProcAddress time — a real swap on a pbuffer is an EGL error.
 * - DDEGL_DEBUG=1 enables per-call logging.
 */
#define _GNU_SOURCE
#include "egl_abi.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define REAL_LIB "/usr/local/hybris/usr/lib/libEGL.so.1"
#define MAX_WND 16
#define GL_BACK 0x0402
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401

static int g_debug;
/* Diagnostic build: log to a file, not stderr — the CEF GPU child's stderr
 * is suppressed (no --enable-logging reaches it), so file logging is the
 * only way to see the shim's side of the story. */
static void logline(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[512];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    int fd = open("/tmp/ddcef-egl.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    write(fd, "[ddcef-egl] ", 12);
    write(fd, buf, (size_t)n);
    write(fd, "\n", 1);
    close(fd);
}

/* ---- real library binding ---- */
#define DECLARE(ret, name, ...) \
    typedef ret (*pfn_##name##_t)(__VA_ARGS__); \
    static pfn_##name##_t p_##name;

DECLARE(EGLBoolean, eglChooseConfig, EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *)
DECLARE(EGLContext, eglCreateContext, EGLDisplay, EGLConfig, EGLContext, const EGLint *)
DECLARE(EGLBoolean, eglDestroyContext, EGLDisplay, EGLContext)
DECLARE(EGLSurface, eglCreatePbufferSurface, EGLDisplay, EGLConfig, const EGLint *)
DECLARE(EGLSurface, eglCreatePixmapSurface, EGLDisplay, EGLConfig, void *, const EGLint *)
DECLARE(EGLBoolean, eglDestroySurface, EGLDisplay, EGLSurface)
DECLARE(EGLBoolean, eglGetConfigAttrib, EGLDisplay, EGLConfig, EGLint, EGLint *)
DECLARE(EGLBoolean, eglGetConfigs, EGLDisplay, EGLConfig *, EGLint, EGLint *)
DECLARE(EGLDisplay, eglGetCurrentDisplay, void)
DECLARE(EGLSurface, eglGetCurrentSurface, EGLint)
DECLARE(EGLDisplay, eglGetDisplay, void *)
DECLARE(EGLint, eglGetError, void)
DECLARE(void *, eglGetProcAddressReal, const char *)
DECLARE(EGLBoolean, eglInitialize, EGLDisplay, EGLint *, EGLint *)
DECLARE(EGLBoolean, eglMakeCurrent, EGLDisplay, EGLSurface, EGLSurface, EGLContext)
DECLARE(EGLBoolean, eglQueryContext, EGLDisplay, EGLContext, EGLint, EGLint *)
DECLARE(const char *, eglQueryString, EGLDisplay, EGLint)
DECLARE(EGLBoolean, eglQuerySurface, EGLDisplay, EGLSurface, EGLint, EGLint *)
DECLARE(EGLBoolean, eglSwapInterval, EGLDisplay, EGLint)
DECLARE(EGLBoolean, eglTerminate, EGLDisplay)
DECLARE(EGLBoolean, eglBindAPI, EGLenum)
DECLARE(EGLenum, eglQueryAPI, void)
DECLARE(EGLBoolean, eglReleaseThread, void)
DECLARE(EGLContext, eglGetCurrentContext, void)
DECLARE(EGLSurface, eglCreatePbufferFromClientBuffer, EGLDisplay, EGLenum, void *, EGLConfig, const EGLint *)
DECLARE(EGLBoolean, eglWaitClient, void)
DECLARE(EGLBoolean, eglWaitGL, void)
DECLARE(EGLBoolean, eglWaitNative, EGLint)
DECLARE(EGLSync, eglCreateSync, EGLDisplay, EGLenum, const EGLAttrib *)
DECLARE(EGLBoolean, eglDestroySync, EGLDisplay, EGLSync)
DECLARE(EGLint, eglClientWaitSync, EGLDisplay, EGLSync, EGLint, EGLTime)
DECLARE(EGLBoolean, eglGetSyncAttrib, EGLDisplay, EGLSync, EGLint, EGLAttrib *)
DECLARE(EGLBoolean, eglWaitSync, EGLDisplay, EGLSync, EGLint)
DECLARE(EGLImage, eglCreateImage, EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *)
DECLARE(EGLBoolean, eglDestroyImage, EGLDisplay, EGLImage)
DECLARE(EGLSurface, eglCreatePlatformPixmapSurface, EGLDisplay, EGLConfig, void *, const EGLint *)
DECLARE(EGLBoolean, eglCopyBuffers, EGLDisplay, EGLSurface, void *)

#undef DECLARE

static void *real_lib;
static pthread_once_t real_once = PTHREAD_ONCE_INIT;

static void load_real(void) {
    const char *env = getenv("DDEGL_DEBUG");
    g_debug = env && *env && *env != '0';
    /* DDEGL_DLOPEN_LOCAL=1: load the hybris EGL with RTLD_LOCAL instead of
     * RTLD_GLOBAL. The GPU child dies pre-EGL with a signature that could be
     * the bionic-stack load itself; GLOBAL vs LOCAL is the A/B that splits
     * "the load's global namespace insertion breaks the child" from other
     * causes. */
    const char *localenv = getenv("DDEGL_DLOPEN_LOCAL");
    int use_local = localenv && *localenv && *localenv != '0';
    logline("load_real: dlopen %s (%s) starting", REAL_LIB,
            use_local ? "RTLD_NOW|LOCAL" : "RTLD_NOW|GLOBAL");
    real_lib = dlopen(REAL_LIB, RTLD_NOW | (use_local ? RTLD_LOCAL : RTLD_GLOBAL));
    logline("load_real: dlopen returned %p", real_lib);
    if (!real_lib) {
        const char *err = dlerror();
        logline("load_real: dlopen FAILED: %s", err ?: "(no dlerror)");
        fprintf(stderr, "[ddcef-egl] FATAL: dlopen %s: %s\n", REAL_LIB, err ?: "?");
        abort();
    }
    struct { const char *n; void **p; } t[] = {
#define ENTRY(name) { #name, (void **)&p_##name }
        ENTRY(eglChooseConfig), ENTRY(eglCreateContext), ENTRY(eglDestroyContext),
        ENTRY(eglCreatePbufferSurface), ENTRY(eglCreatePixmapSurface), ENTRY(eglDestroySurface),
        ENTRY(eglGetConfigAttrib), ENTRY(eglGetConfigs), ENTRY(eglGetCurrentDisplay),
        ENTRY(eglGetCurrentSurface), ENTRY(eglGetDisplay), ENTRY(eglGetError),
        { "eglGetProcAddress", (void **)&p_eglGetProcAddressReal },
        ENTRY(eglInitialize), ENTRY(eglMakeCurrent),
        ENTRY(eglQueryContext), ENTRY(eglQueryString), ENTRY(eglQuerySurface),
        ENTRY(eglSwapInterval), ENTRY(eglTerminate), ENTRY(eglBindAPI), ENTRY(eglQueryAPI),
        ENTRY(eglReleaseThread), ENTRY(eglGetCurrentContext),
        ENTRY(eglCreatePbufferFromClientBuffer), ENTRY(eglWaitClient), ENTRY(eglWaitGL),
        ENTRY(eglWaitNative), ENTRY(eglCreateSync), ENTRY(eglDestroySync),
        ENTRY(eglClientWaitSync), ENTRY(eglGetSyncAttrib), ENTRY(eglWaitSync),
        ENTRY(eglCreateImage), ENTRY(eglDestroyImage), ENTRY(eglCreatePlatformPixmapSurface),
        ENTRY(eglCopyBuffers),
#undef ENTRY
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        if (!(*t[i].p = dlsym(real_lib, t[i].n)))
            logline("WARN: real lib lacks %s", t[i].n);
    }
    logline("real lib loaded");
}

/* ---- window table ---- */
struct Wnd {
    EGLSurface surf;      /* the real pbuffer we hand out as a "window surface" */
    EGLConfig cfg;
    Window xid;
    Visual *visual;
    int depth;
    int w, h;
    GC gc;
    XImage *img;
    unsigned char *buf;
    size_t bufsz;
};
static struct Wnd g_wnds[MAX_WND];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Display *g_xdpy;

typedef void (*pfn_glReadBuffer_t)(unsigned);
typedef void (*pfn_glReadPixels_t)(int, int, int, int, unsigned, unsigned, void *);
static pfn_glReadBuffer_t p_glReadBuffer;
static pfn_glReadPixels_t p_glReadPixels;

static struct Wnd *wnd_find(EGLSurface s) {
    for (int i = 0; i < MAX_WND; i++)
        if (g_wnds[i].surf == s && s != EGL_NO_SURFACE) return &g_wnds[i];
    return NULL;
}
static struct Wnd *wnd_slot(void) {
    for (int i = 0; i < MAX_WND; i++)
        if (!g_wnds[i].surf) return &g_wnds[i];
    return NULL;
}

static Display *xconn(void) {
    if (!g_xdpy) {
        g_xdpy = XOpenDisplay(NULL);
        if (!g_xdpy) logline("FATAL: XOpenDisplay(NULL) failed");
    }
    return g_xdpy;
}

static void load_glprocs(void) {
    if (p_glReadPixels || !p_eglGetProcAddressReal) return;
    p_glReadPixels = (pfn_glReadPixels_t)p_eglGetProcAddressReal("glReadPixels");
    p_glReadBuffer = (pfn_glReadBuffer_t)p_eglGetProcAddressReal("glReadBuffer");
    logline("gl procs: read=%p buf=%p", (void *)p_glReadPixels, (void *)p_glReadBuffer);
}

static void wnd_wipe(struct Wnd *wn) {
    if (wn->img) { wn->img->data = NULL; XDestroyImage(wn->img); }
    if (wn->gc && g_xdpy) XFreeGC(g_xdpy, wn->gc);
    free(wn->buf);
    memset(wn, 0, sizeof *wn);
}

/* ---- shared window-surface creation (core 1.5 and EXT paths) ---- */
static EGLSurface wnd_create(EGLDisplay dpy, EGLConfig config, void *native_window) {
    pthread_once(&real_once, load_real);
    Window xid = (Window)(uintptr_t)native_window;
    Display *xd = xconn();
    if (!xd) return EGL_NO_SURFACE;
    Window root; int x, y; unsigned w = 0, h = 0, bw, depth = 24;
    if (!XGetGeometry(xd, xid, &root, &x, &y, &w, &h, &bw, &depth)) {
        logline("CreateWindowSurface: XGetGeometry(%#lx) failed", (unsigned long)xid);
        return EGL_NO_SURFACE;
    }
    if (!w || !h) { w = 1; h = 1; }
    XWindowAttributes wa; Visual *visual = NULL;
    if (XGetWindowAttributes(xd, xid, &wa)) visual = wa.visual;
    EGLint pb[] = { EGL_WIDTH, (EGLint)w, EGL_HEIGHT, (EGLint)h,
                    EGL_TEXTURE_FORMAT, EGL_NO_TEXTURE, EGL_NONE };
    EGLSurface s = p_eglCreatePbufferSurface(dpy, config, pb);
    logline("CreateWindowSurface xid=%#lx %ux%u depth=%u -> pbuffer %p", (unsigned long)xid, w, h, depth, s);
    if (s == EGL_NO_SURFACE) return s;
    pthread_mutex_lock(&g_lock);
    struct Wnd *wn = wnd_slot();
    if (!wn) { pthread_mutex_unlock(&g_lock); p_eglDestroySurface(dpy, s); return EGL_NO_SURFACE; }
    memset(wn, 0, sizeof *wn);
    wn->surf = s; wn->cfg = config; wn->xid = xid; wn->visual = visual;
    wn->depth = (int)depth; wn->w = (int)w; wn->h = (int)h;
    wn->gc = XCreateGC(xd, xid, 0, NULL);
    pthread_mutex_unlock(&g_lock);
    return s;
}

/* re-make the current context onto a fresh pbuffer of the new size */
static int wnd_resize(struct Wnd *wn, EGLDisplay dpy) {
    EGLContext ctx = p_eglGetCurrentContext();
    p_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    p_eglDestroySurface(dpy, wn->surf);
    EGLint pb[] = { EGL_WIDTH, wn->w, EGL_HEIGHT, wn->h, EGL_NONE };
    EGLSurface s = p_eglCreatePbufferSurface(dpy, wn->cfg, pb);
    if (s == EGL_NO_SURFACE) { logline("resize: pbuffer create failed"); wn->surf = EGL_NO_SURFACE; return 0; }
    wn->surf = s;
    if (ctx) p_eglMakeCurrent(dpy, s, s, ctx);
    logline("resized pbuffer -> %dx%d", wn->w, wn->h);
    return 1;
}

static void blit_to_x(struct Wnd *wn, int w, int h) {
    Display *xd = xconn();
    if (!xd || !wn->xid || !wn->visual || !p_glReadPixels || !wn->buf) return;
    size_t need = (size_t)w * h * 4;
    if (need > wn->bufsz) { free(wn->buf); wn->buf = malloc(need); wn->bufsz = need; }
    if (!wn->buf) return;
    if (p_glReadBuffer) p_glReadBuffer(GL_BACK);
    p_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, wn->buf);
    /* RGBA bottom-up -> BGRA top-down for X ZPixmap (LSBFirst, 24/32bpp) */
    for (int y = 0; y < h; y++) {
        const unsigned char *src = wn->buf + (size_t)(h - 1 - y) * w * 4;
        unsigned char *d = wn->buf + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            unsigned char r = src[0], g = src[1], b = src[2];
            d[0] = b; d[1] = g; d[2] = r; d[3] = 0xff;
            src += 4; d += 4;
        }
    }
    if (!wn->img || wn->img->width != w || wn->img->height != h) {
        if (wn->img) { wn->img->data = NULL; XDestroyImage(wn->img); wn->img = NULL; }
        wn->img = XCreateImage(xd, wn->visual, wn->depth, ZPixmap, 0, (char *)wn->buf, w, h, 32, 0);
        if (!wn->img) { logline("XCreateImage failed (depth %d)", wn->depth); return; }
        wn->img->byte_order = LSBFirst;
    } else {
        wn->img->data = (char *)wn->buf;
    }
    if (wn->gc)
        XPutImage(xd, wn->xid, wn->gc, wn->img, 0, 0, 0, 0, w, h);
    XFlush(xd);
}

/* ---- exported EGL ---- */

EGLDisplay eglGetDisplay(void *native_display) {
    /* Log BEFORE load_real: a death inside the hybris dlopen must be
     * distinguishable from "ANGLE never drove the shim at all". */
    logline("eglGetDisplay(%p) entry", native_display);
    pthread_once(&real_once, load_real);
    (void)native_display; /* X11 Display* or anything else: always the default */
    return p_eglGetDisplay((void *)EGL_DEFAULT_DISPLAY);
}

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native_display, const EGLAttrib *attribs) {
    logline("eglGetPlatformDisplay(%#x, %p) entry", platform, native_display);
    pthread_once(&real_once, load_real);
    return p_eglGetDisplay((void *)EGL_DEFAULT_DISPLAY);
}

EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *native_display, const EGLint *attribs) {
    (void)attribs; /* ANGLE-specific attribs mean nothing to the real driver */
    return eglGetPlatformDisplay(platform, native_display, NULL);
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, void *native_window, const EGLint *attribs) {
    (void)attribs; /* render-buffer etc. hints don't apply to the pbuffer */
    return wnd_create(dpy, config, (void *)(uintptr_t)native_window);
}

EGLSurface eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void *native_window, const EGLint *attribs) {
    (void)attribs;
    return wnd_create(dpy, config, native_window);
}

EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy, EGLConfig config, void *native_window, const EGLint *attribs) {
    return eglCreatePlatformWindowSurface(dpy, config, native_window, attribs);
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    pthread_once(&real_once, load_real);
    pthread_mutex_lock(&g_lock);
    struct Wnd *wn = wnd_find(surface);
    if (wn) wnd_wipe(wn);
    pthread_mutex_unlock(&g_lock);
    return p_eglDestroySurface(dpy, surface);
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    pthread_once(&real_once, load_real);
    struct Wnd *wn = wnd_find(surface);
    if (!wn) { logline("SwapBuffers on unmapped surface %p", surface); return EGL_FALSE; }
    pthread_mutex_lock(&g_lock);
    Display *xd = xconn();
    if (xd) {
        Window root; int x, y; unsigned w, h, bw, depth;
        if (XGetGeometry(xd, wn->xid, &root, &x, &y, &w, &h, &bw, &depth) && w && h) {
            if ((int)w == wn->w && (int)h == wn->h) {
                blit_to_x(wn, wn->w, wn->h);
            } else {
                /* present the last frame at the old size, then resize */
                blit_to_x(wn, wn->w, wn->h);
                wn->w = (int)w; wn->h = (int)h;
                wnd_resize(wn, dpy);
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return EGL_TRUE;
}

static EGLBoolean swap_with_damage(EGLDisplay dpy, EGLSurface surface, const EGLint *rects, EGLint n) {
    (void)rects; (void)n;
    return eglSwapBuffers(dpy, surface);
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attr, EGLint *value) {
    pthread_once(&real_once, load_real);
    struct Wnd *wn = wnd_find(surface);
    if (!wn) return p_eglQuerySurface(dpy, surface, attr, value);
    EGLBoolean r = p_eglQuerySurface(dpy, surface, attr, value);
    if (r == EGL_TRUE && (attr == EGL_WIDTH || attr == EGL_HEIGHT)) {
        Display *xd = xconn();
        Window root; int x, y; unsigned w, h, bw, depth;
        if (xd && XGetGeometry(xd, wn->xid, &root, &x, &y, &w, &h, &bw, &depth) && w && h) {
            if (attr == EGL_WIDTH) *value = (EGLint)w;
            else *value = (EGLint)h;
        }
    }
    return r;
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) {
    static int first_call = 1;
    if (first_call) {
        first_call = 0;
        logline("eglGetProcAddress first call (%s)", name ?: "(null)");
    }
    pthread_once(&real_once, load_real);
    if (!name) return NULL;
    if (!strcmp(name, "eglGetPlatformDisplayEXT")) return (__eglMustCastToProperFunctionPointerType)eglGetPlatformDisplayEXT;
    if (!strcmp(name, "eglGetPlatformDisplay")) return (__eglMustCastToProperFunctionPointerType)eglGetPlatformDisplay;
    if (!strcmp(name, "eglCreatePlatformWindowSurfaceEXT")) return (__eglMustCastToProperFunctionPointerType)eglCreatePlatformWindowSurfaceEXT;
    if (!strcmp(name, "eglSwapBuffersWithDamageKHR")) return (__eglMustCastToProperFunctionPointerType)swap_with_damage;
    if (!strcmp(name, "eglSwapBuffersWithDamageEXT")) return (__eglMustCastToProperFunctionPointerType)swap_with_damage;
    if (!strcmp(name, "eglSwapBuffersWithDamageClientRectKHR")) return (__eglMustCastToProperFunctionPointerType)swap_with_damage;
    void *p = dlsym(real_lib, name);
    if (!p && p_eglGetProcAddressReal) p = (void *)p_eglGetProcAddressReal(name);
    return (__eglMustCastToProperFunctionPointerType)p;
}

EGLBoolean eglTerminate(EGLDisplay dpy) {
    pthread_once(&real_once, load_real);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_WND; i++)
        if (g_wnds[i].surf) wnd_wipe(&g_wnds[i]);
    pthread_mutex_unlock(&g_lock);
    return p_eglTerminate(dpy);
}

/* ---- pure forwarders ---- */
EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *maj, EGLint *min) { pthread_once(&real_once, load_real); EGLBoolean r = p_eglInitialize(dpy, maj, min); logline("eglInitialize(%p) -> %d (%d.%d)", dpy, r, maj ? *maj : 0, min ? *min : 0); return r; }
EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *a, EGLConfig *c, EGLint n, EGLint *m) { pthread_once(&real_once, load_real); EGLBoolean r = p_eglChooseConfig(dpy, a, c, n, m); logline("eglChooseConfig -> %d n=%d", r, m ? *m : -1); return r; }
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig c, EGLContext sh, const EGLint *a) { pthread_once(&real_once, load_real); return p_eglCreateContext(dpy, c, sh, a); }
EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext c) { pthread_once(&real_once, load_real); return p_eglDestroyContext(dpy, c); }
EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface rd, EGLSurface dr, EGLContext c) { pthread_once(&real_once, load_real); EGLBoolean r = p_eglMakeCurrent(dpy, rd, dr, c); logline("MakeCurrent rd=%p dr=%p -> %d", rd, dr, r); return r; }
EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig c, EGLint a, EGLint *v) { pthread_once(&real_once, load_real); return p_eglGetConfigAttrib(dpy, c, a, v); }
EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig *c, EGLint n, EGLint *m) { pthread_once(&real_once, load_real); return p_eglGetConfigs(dpy, c, n, m); }
EGLDisplay eglGetCurrentDisplay(void) { pthread_once(&real_once, load_real); return p_eglGetCurrentDisplay(); }
EGLSurface eglGetCurrentSurface(EGLint t) { pthread_once(&real_once, load_real); return p_eglGetCurrentSurface(t); }
EGLint eglGetError(void) { pthread_once(&real_once, load_real); return p_eglGetError(); }
EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext c, EGLint a, EGLint *v) { pthread_once(&real_once, load_real); return p_eglQueryContext(dpy, c, a, v); }
const char *eglQueryString(EGLDisplay dpy, EGLint n) { pthread_once(&real_once, load_real); return p_eglQueryString(dpy, n); }
EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint i) { pthread_once(&real_once, load_real); return p_eglSwapInterval(dpy, i); }
EGLBoolean eglBindAPI(EGLenum a) { pthread_once(&real_once, load_real); return p_eglBindAPI(a); }
EGLenum eglQueryAPI(void) { pthread_once(&real_once, load_real); return p_eglQueryAPI(); }
EGLBoolean eglReleaseThread(void) { pthread_once(&real_once, load_real); return p_eglReleaseThread(); }
EGLContext eglGetCurrentContext(void) { pthread_once(&real_once, load_real); return p_eglGetCurrentContext(); }
EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig c, const EGLint *a) { pthread_once(&real_once, load_real); return p_eglCreatePbufferSurface(dpy, c, a); }
EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum t, void *b, EGLConfig c, const EGLint *a) { pthread_once(&real_once, load_real); return p_eglCreatePbufferFromClientBuffer(dpy, t, b, c, a); }
EGLSurface eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig c, void *pm, const EGLint *a) { pthread_once(&real_once, load_real); return p_eglCreatePixmapSurface(dpy, c, pm, a); }
EGLSurface eglCreatePlatformPixmapSurface(EGLDisplay dpy, EGLConfig c, void *pm, const EGLint *a) { pthread_once(&real_once, load_real); return p_eglCreatePlatformPixmapSurface ? p_eglCreatePlatformPixmapSurface(dpy, c, pm, a) : EGL_NO_SURFACE; }
EGLBoolean eglWaitClient(void) { pthread_once(&real_once, load_real); return p_eglWaitClient(); }
EGLBoolean eglWaitGL(void) { pthread_once(&real_once, load_real); return p_eglWaitGL(); }
EGLBoolean eglWaitNative(EGLint e) { pthread_once(&real_once, load_real); return p_eglWaitNative(e); }
EGLSync eglCreateSync(EGLDisplay dpy, EGLenum t, const EGLAttrib *a) { pthread_once(&real_once, load_real); return p_eglCreateSync ? p_eglCreateSync(dpy, t, a) : EGL_NO_SYNC; }
EGLBoolean eglDestroySync(EGLDisplay dpy, EGLSync s) { pthread_once(&real_once, load_real); return p_eglDestroySync ? p_eglDestroySync(dpy, s) : EGL_FALSE; }
EGLint eglClientWaitSync(EGLDisplay dpy, EGLSync s, EGLint f, EGLTime t) { pthread_once(&real_once, load_real); return p_eglClientWaitSync ? p_eglClientWaitSync(dpy, s, f, t) : 0; }
EGLBoolean eglGetSyncAttrib(EGLDisplay dpy, EGLSync s, EGLint a, EGLAttrib *v) { pthread_once(&real_once, load_real); return p_eglGetSyncAttrib ? p_eglGetSyncAttrib(dpy, s, a, v) : EGL_FALSE; }
EGLBoolean eglWaitSync(EGLDisplay dpy, EGLSync s, EGLint f) { pthread_once(&real_once, load_real); return p_eglWaitSync ? p_eglWaitSync(dpy, s, f) : EGL_FALSE; }
EGLImage eglCreateImage(EGLDisplay dpy, EGLContext c, EGLenum t, EGLClientBuffer b, const EGLint *a) { pthread_once(&real_once, load_real); return p_eglCreateImage ? p_eglCreateImage(dpy, c, t, b, a) : EGL_NO_IMAGE; }
EGLBoolean eglDestroyImage(EGLDisplay dpy, EGLImage i) { pthread_once(&real_once, load_real); return p_eglDestroyImage ? p_eglDestroyImage(dpy, i) : EGL_FALSE; }
EGLBoolean eglCopyBuffers(EGLDisplay dpy, EGLSurface s, void *t) { pthread_once(&real_once, load_real); return p_eglCopyBuffers(dpy, s, t); }

__attribute__((constructor))
static void shim_init(void) {
    const char *env = getenv("DDEGL_DEBUG");
    g_debug = env && *env && *env != '0';
    char cmd[512] = {0};
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, cmd, sizeof cmd - 2);
        if (r > 0) for (ssize_t i = 0; i < r - 1; i++) if (cmd[i] == '\0') cmd[i] = ' ';
        close(fd);
    }
    logline("shim loaded (pid %d) cmd=%.380s LD_LIBRARY_PATH=%s",
            (int)getpid(), cmd, getenv("LD_LIBRARY_PATH") ?: "(unset)");
}

/* Diagnostic: the GPU process dies pre-EGL (before any shim call) and its
 * stderr is suppressed, so the fault logger rides in the shim — the one lib
 * every steamwebhelper child loads eagerly via SDL3's DT_NEEDED. It only
 * arms itself in --type=gpu-process processes. See crashlog_embed.c. */
#include "crashlog_embed.c"
