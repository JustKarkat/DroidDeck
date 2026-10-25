// Headless EGL probe: can the vendor driver initialise and render to a pbuffer?
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    printf("eglGetDisplay: %p (err 0x%x)\n", (void *)dpy, eglGetError());
    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        printf("eglInitialize FAILED err 0x%x\n", eglGetError());
        return 1;
    }
    printf("EGL %d.%d\nvendor=%s\nversion=%s\napis=%s\n",
           major, minor,
           eglQueryString(dpy, EGL_VENDOR),
           eglQueryString(dpy, EGL_VERSION),
           eglQueryString(dpy, EGL_CLIENT_APIS));

    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        printf("eglChooseConfig FAILED n=%d err 0x%x\n", n, eglGetError());
        // try without surface type
        EGLint any[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
        if (!eglChooseConfig(dpy, any, &cfg, 1, &n) || n < 1) {
            printf("eglChooseConfig(any) FAILED n=%d err 0x%x\n", n, eglGetError());
            return 1;
        }
    }
    EGLint pb[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    printf("pbuffer=%p err 0x%x\n", (void *)surf, eglGetError());
    if (surf == EGL_NO_SURFACE) return 1;
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    printf("context=%p err 0x%x\n", (void *)ctx, eglGetError());
    if (ctx == EGL_NO_CONTEXT) return 1;
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        printf("eglMakeCurrent FAILED err 0x%x\n", eglGetError());
        return 1;
    }
    printf("GL_RENDERER=%s\nGL_VENDOR=%s\nGL_VERSION=%s\n",
           glGetString(GL_RENDERER), glGetString(GL_VENDOR), glGetString(GL_VERSION));
    glClearColor(0.2f, 0.4f, 0.8f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    printf("glGetError after clear: 0x%x\n", glGetError());
    return 0;
}
