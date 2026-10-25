// Headless EGL probe with an in-process fault handler (proot forbids nested ptrace).
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static void say(const char *s) { write(1, s, strlen(s)); }

static void on_fault(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    char buf[512];
    void *pc = (void *)uc->uc_mcontext.pc;
    void *lr = (void *)uc->uc_mcontext.regs[30];
    int n = snprintf(buf, sizeof buf,
                     "\nFAULT sig=%d addr=%p pc=%p lr=%p\n", sig, si->si_addr, pc, lr);
    if (n > 0) write(1, buf, (size_t)n);
    void *bt[32];
    int c = backtrace(bt, 32);
    backtrace_symbols_fd(bt, c, 1);
    {
        FILE *m = fopen("/proc/self/maps", "r");
        char line[512];
        if (m) {
            while (fgets(line, sizeof line, m)) {
                unsigned long s = 0, e = 0;
                if (sscanf(line, "%lx-%lx", &s, &e) != 2) continue;
                unsigned long a = (unsigned long)si->si_addr;
                unsigned long p = (unsigned long)pc;
                unsigned long l = (unsigned long)lr;
                if ((p >= s && p < e) || (l >= s && l < e) || (a >= s && a < e))
                    write(1, line, strlen(line));
            }
            fclose(m);
        }
    }
    say("--- /proc/self/maps (executable) ---\n");
    FILE *f = fopen("/proc/self/maps", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "r-xp") || strstr(line, "/usr/local/hybris") || strstr(line, "libEGL"))
                write(1, line, strlen(line));
        }
        fclose(f);
    }
    _exit(99);
}

static void install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    install();
    say("stage: start\n");

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    printf("stage: eglGetDisplay -> %p err=0x%x\n", (void *)dpy, eglGetError());
    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        printf("stage: eglInitialize FAILED err 0x%x\n", eglGetError());
        return 1;
    }
    printf("stage: EGL %d.%d vendor=%s version=%s apis=%s\n",
           major, minor, eglQueryString(dpy, EGL_VENDOR),
           eglQueryString(dpy, EGL_VERSION), eglQueryString(dpy, EGL_CLIENT_APIS));

    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        printf("stage: eglChooseConfig FAILED n=%d err 0x%x\n", n, eglGetError());
        return 1;
    }
    EGLint pb[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    printf("stage: pbuffer=%p err 0x%x\n", (void *)surf, eglGetError());
    if (surf == EGL_NO_SURFACE) return 1;
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    printf("stage: context=%p err 0x%x\n", (void *)ctx, eglGetError());
    if (ctx == EGL_NO_CONTEXT) return 1;
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        printf("stage: eglMakeCurrent FAILED err 0x%x\n", eglGetError());
        return 1;
    }
    printf("RESULT GL_RENDERER=%s\nRESULT GL_VENDOR=%s\nRESULT GL_VERSION=%s\n",
           glGetString(GL_RENDERER), glGetString(GL_VENDOR), glGetString(GL_VERSION));
    glClearColor(0.2f, 0.4f, 0.8f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    printf("stage: glGetError=0x%x\n", glGetError());
    return 0;
}
