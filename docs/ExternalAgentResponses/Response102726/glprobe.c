// glprobe.c — does the Tegra EGL (via hybris) give us desktop-GL contexts, and does
// it accept the exact context-creation sequence ANGLE's DisplayEGL uses (gles-egl)?
// Build like eglprobe.c:  gcc -O1 -o glprobe glprobe.c -lEGL -lhybris-common -lhardware
// (no -lGLESv2: every GL call goes through eglGetProcAddress, as ANGLE does).
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef EGL_CONTEXT_MAJOR_VERSION
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x1
#define EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT 0x2
#endif
typedef const unsigned char *(*PFNGETSTRING)(unsigned);
typedef void (*PFNREAD)(int, int, int, int, unsigned, unsigned, void *);
typedef void (*PFNCLEAR)(float, float, float, float);
typedef void (*PFNCLR)(unsigned);

static void die(int s) { (void)s; dprintf(1, "FAULT (signal) — last stage above is the culprit\n"); _exit(99); }
static EGLDisplay dpy;

static int has(const char *list, const char *w) { return list && strstr(list, w) != NULL; }

// One attempt: bind api, pick config with renderable bit, create ctx with attribs, make current, print GL strings.
static void attempt(const char *tag, EGLenum api, EGLint rbit, const EGLint *ctxattr) {
    printf("--- %s\n", tag);
    if (!eglBindAPI(api)) { printf("  eglBindAPI FAILED 0x%x\n", eglGetError()); return; }
    EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, rbit,
                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, ca, &cfg, 1, &n) || n < 1) { printf("  eglChooseConfig n=%d err=0x%x (ANGLE would stop here)\n", n, eglGetError()); return; }
    EGLint pb[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(dpy, cfg, pb);
    EGLContext c = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
    printf("  pbuffer=%p ctx=%p err=0x%x\n", s, c, eglGetError());
    if (s == EGL_NO_SURFACE || c == EGL_NO_CONTEXT) return;
    if (!eglMakeCurrent(dpy, s, s, c)) { printf("  MakeCurrent FAILED 0x%x\n", eglGetError()); return; }
    PFNGETSTRING gs = (PFNGETSTRING)eglGetProcAddress("glGetString");
    PFNCLEAR cc = (PFNCLEAR)eglGetProcAddress("glClearColor");
    PFNCLR cl = (PFNCLR)eglGetProcAddress("glClear");
    PFNREAD rp = (PFNREAD)eglGetProcAddress("glReadPixels");
    printf("RESULT[%s] GL_VERSION=%s | GL_RENDERER=%s | GLSL=%s\n", tag, gs ? (const char *)gs(0x1F02) : "(null pfn)",
           gs ? (const char *)gs(0x1F01) : "?", gs ? (const char *)gs(0x8B8C) : "?");
    if (cc && cl && rp) { unsigned char px[4] = {0}; cc(0.2f, 0.4f, 0.8f, 1.f); cl(0x4000); rp(0, 0, 1, 1, 0x1908, 0x1401, px);
        printf("  readback RGBA=%u,%u,%u,%u (expect ~51,102,204,255)\n", px[0], px[1], px[2], px[3]); }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, c); eglDestroySurface(dpy, s);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGSEGV, die); signal(SIGBUS, die); signal(SIGABRT, die);
    const char *cext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("EGL client exts: %s\n", cext ? cext : "(none)");
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint M = 0, m = 0;
    if (!eglInitialize(dpy, &M, &m)) { printf("eglInitialize FAILED 0x%x\n", eglGetError()); return 1; }
    const char *dext = eglQueryString(dpy, EGL_EXTENSIONS);
    printf("EGL %d.%d vendor=%s client_apis=%s\n", M, m, eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_CLIENT_APIS));
    printf("ANGLE gates: KHR_create_context=%d no_config_context=%d surfaceless=%d robustness=%d platform_base(client)=%d\n",
           has(dext, "EGL_KHR_create_context"), has(dext, "EGL_KHR_no_config_context"), has(dext, "EGL_KHR_surfaceless_context"),
           has(dext, "EGL_EXT_create_context_robustness"), has(cext, "EGL_EXT_platform_base"));
    // Config census: ANGLE gl-egl filters on EGL_OPENGL_BIT, gles-egl on ES3/ES2 bits.
    EGLConfig all[512]; EGLint nc = 0, bits[4] = {0}; eglGetConfigs(dpy, all, 512, &nc);
    for (int i = 0; i < nc; i++) { EGLint r = 0; eglGetConfigAttrib(dpy, all[i], EGL_RENDERABLE_TYPE, &r);
        bits[0] += !!(r & EGL_OPENGL_BIT); bits[1] += !!(r & EGL_OPENGL_ES2_BIT); bits[2] += !!(r & EGL_OPENGL_ES3_BIT); }
    printf("configs=%d  OPENGL_BIT=%d ES2_BIT=%d ES3_BIT=%d\n", nc, bits[0], bits[1], bits[2]);
    // ES3.x proc coverage (ANGLE loads these through eglGetProcAddress).
    const char *fn[] = { "glGetStringi", "glTexStorage2D", "glBindVertexArray", "glDispatchCompute", "glBlendBarrier", "glPatchParameteri", "glFenceSync" };
    int nul = 0; for (unsigned i = 0; i < sizeof fn / sizeof *fn; i++) if (!eglGetProcAddress(fn[i])) { printf("  NULL proc: %s\n", fn[i]); nul++; }
    printf("ES3.x procs missing: %d\n", nul);

    // 1) The decisive one: desktop GL.
    EGLint d45[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT, EGL_NONE };
    EGLint d33[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    EGLint dleg[] = { EGL_NONE };
    attempt("GL desktop 4.5 compat", EGL_OPENGL_API, EGL_OPENGL_BIT, d45);
    attempt("GL desktop 3.3 core", EGL_OPENGL_API, EGL_OPENGL_BIT, d33);
    attempt("GL desktop default (2.1-ish)", EGL_OPENGL_API, EGL_OPENGL_BIT, dleg);
    // 2) Control = what ANGLE gles-egl will do: ES 3.2 via MAJOR/MINOR, with and without the robustness attrib it tries first.
    EGLint es32r[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT, EGL_LOSE_CONTEXT_ON_RESET_EXT, EGL_NONE };
    EGLint es32[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE };
    attempt("ES 3.2 + robustness (ANGLE 1st try)", EGL_OPENGL_ES_API, EGL_OPENGL_ES3_BIT, es32r);
    attempt("ES 3.2 plain (ANGLE 2nd try)", EGL_OPENGL_ES_API, EGL_OPENGL_ES3_BIT, es32);
    return 0;
}
