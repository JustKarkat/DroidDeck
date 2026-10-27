/* Shared EGL warm-up (vkprobe + sysvk): the one proven path that initializes
 * the NVIDIA driver process-wide. Disassembly (libglcore 33.01/495.00):
 * NvGlEglGetFunctions' internal ensure block calls singleton(0x19c5a70) then
 * master(0x19c5640)->fn_0x160 — both NULL unless the NvGlEglApiInit chain ran
 * (EGL init runs it; the master is a 41 KB dispatch object allocated by
 * libglcore+0xf60200). Proven on device: after this sequence CreateInstance +
 * vkEnumeratePhysicalDevices return the Tegra X1 (session 2026-10-26-12). */
#ifndef EGL_WARMUP_H
#define EGL_WARMUP_H

#include <EGL/egl.h>
#include <stdio.h>

static void egl_warmup(const char *tag) {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    printf("%s: egl warmup: GetDisplay=%p\n", tag, (void *)dpy);
    if (!dpy) return;
    EGLint maj = 0, mi = 0;
    if (!eglInitialize(dpy, &maj, &mi)) {
        printf("%s: egl warmup: Initialize FAILED err 0x%x\n", tag, eglGetError());
        return;
    }
    printf("%s: egl warmup: EGL %d.%d vendor=%s\n", tag, maj, mi,
           eglQueryString(dpy, EGL_VENDOR));
    EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                          EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                          EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                          EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        printf("%s: egl warmup: ChooseConfig FAILED\n", tag);
        return;
    }
    EGLint pb[] = { EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
    printf("%s: egl warmup: surf=%p ctx=%p\n", tag, (void *)surf, (void *)ctx);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) return;
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        printf("%s: egl warmup: MakeCurrent FAILED err 0x%x\n", tag, eglGetError());
        return;
    }
    printf("%s: egl warmup: current\n", tag);
}

#endif
