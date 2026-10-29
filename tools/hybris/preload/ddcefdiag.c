/* ddcefdiag.c — LD_PRELOAD diagnostic for the CEF GPU process (ShieldDeck).
 *
 * The shim cannot interpose anything in the GPU child (it is LAST in the
 * link map — SDL3's DT_NEEDED — so every symbol binds to libc first). This
 * library goes into /etc/ld.so.preload, i.e. FIRST in every guest process,
 * which makes interposition real. It is silent everywhere except in
 * --type=gpu-process children, where it:
 *   - traces every dlopen: name, result, dlerror — the failing seam is
 *     ANGLE's "Could not dlopen native EGL: libddcef.so" inside the GPU
 *     sandbox (probe-proven good outside the sandbox, 2026-10-28);
 *   - runs the fault logger from crashlog_embed.c (sigaction keep-in-front,
 *     exit/abort interposers, 20 ms maps+status poller).
 * Logs to /tmp/ddcef-egl.log with the [ddcef-diag] prefix.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void logline(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[512];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    int fd = open("/tmp/ddcef-egl.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    write(fd, "[ddcef-diag] ", 13);
    write(fd, buf, (size_t)n);
    write(fd, "\n", 1);
    close(fd);
}

static int diag_gpu = -1;
static int diag_init(void) {
    if (diag_gpu < 0) {
        int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char b[4096]; ssize_t r = read(fd, b, sizeof b - 1);
            close(fd);
            if (r > 0) diag_gpu = memmem(b, (size_t)r, "--type=gpu-process", 18) != NULL;
            else diag_gpu = 0;
        } else diag_gpu = 0;
    }
    return diag_gpu;
}

/* --- dlopen tracer: the whole point is to catch ANGLE's dlopen("libddcef.so")
 * failing INSIDE the GPU sandbox, with the real dlerror. --- */
void *dlopen(const char *filename, int flags) {
    static void *(*real)(const char *, int);
    if (!real) real = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
    void *h = real(filename, flags);
    if (diag_init() && filename && *filename) {
        if (h)
            logline("dlopen(%s, %#x) -> %p", filename, flags, h);
        else {
            const char *err = dlerror();
            logline("dlopen(%s, %#x) -> NULL: %s", filename, flags, err ?: "?");
        }
    }
    return h;
}

/* --- SIGSEGV-mask tracer ---
 * ddstat-15887 (session 62) showed SigBlk=0x20400: SIGSEGV *blocked* on the
 * reading thread. A synchronous SIGSEGV on a thread that blocked it kills the
 * process with the default action — handlers never run, which is exactly the
 * exit_code=11-with-no-crashlog signature. Log whoever blocks it. */
static void log_segblock(const char *fn, int how, const void *set) {
    if (!diag_init() || !set || (how != SIG_BLOCK && how != SIG_SETMASK)) return;
    if (sigismember((const sigset_t *)set, SIGSEGV) == 1)
        logline("%s(how=%d) BLOCKS SIGSEGV from pc=%p", fn, how, __builtin_return_address(1));
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    static int (*real)(int, const sigset_t *, sigset_t *);
    if (!real) real = (int (*)(int, const sigset_t *, sigset_t *))dlsym(RTLD_NEXT, "sigprocmask");
    log_segblock("sigprocmask", how, set);
    return real ? real(how, set, oldset) : -1;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
    static int (*real)(int, const sigset_t *, sigset_t *);
    if (!real) real = (int (*)(int, const sigset_t *, sigset_t *))dlsym(RTLD_NEXT, "pthread_sigmask");
    log_segblock("pthread_sigmask", how, set);
    return real ? real(how, set, oldset) : -1;
}

#include "crashlog_embed.c"
