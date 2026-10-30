/* ddcefmem.c — LD_PRELOAD mapping tracer for the gamescope memory question.
 *
 * gamescope (lavapipe/SwiftShader-class software Vulkan) settles at ~830 MB
 * resident on the Shield, in guard-paged arenas (46 MiB `---p` + 46 MiB
 * `rw-p` pairs) whose contents are heap data (float constants and pointers),
 * not framebuffers. We need to know whether those mappings are made by
 * gamescope itself or by lavapipe/LLVM inside it: that decides whether the
 * fix is a gamescope buffer/pool patch or a Mesa allocation-policy knob.
 *
 * Preloaded first (via /etc/ld.so.preload) so interposition is real. Silent
 * in every process except gamescope. Logs mappings >= 8 MiB with prot, flags
 * and the caller resolved to module+offset through dladdr().
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define BIG (8u << 20)

static void logline(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[512];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    int fd = open("/tmp/ddcef-egl.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    write(fd, "[ddcef-mem] ", 13);
    write(fd, buf, (size_t)n);
    write(fd, "\n", 1);
    close(fd);
}

static int trace = -1;
static int tracing(void) {
    if (trace < 0) {
        trace = 0;
        int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char b[2048]; ssize_t r = read(fd, b, sizeof b - 1);
            close(fd);
            if (r > 0) trace = memmem(b, (size_t)r, "gamescope", 9) != NULL;
        }
    }
    return trace;
}

static void say(const char *what, size_t len, int prot, int flags, void *ra) {
    char mod[192] = "?";
    Dl_info di;
    if (ra && dladdr(ra, &di) && di.dli_fname) {
        const char *base = strrchr(di.dli_fname, '/');
        snprintf(mod, sizeof mod, "%s+%#lx", base ? base + 1 : di.dli_fname,
                 (unsigned long)((const char *)ra - (const char *)di.dli_fbase));
    }
    logline("%s len=%zuMB prot=%d flags=%#x from %s", what, len >> 20, prot, flags, mod);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    static void *(*real)(void *, size_t, int, int, int, off_t);
    if (!real) real = (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
    void *r = real(addr, len, prot, flags, fd, off);
    if (len >= BIG && tracing() && !(flags & MAP_FIXED))
        say(r == MAP_FAILED ? "mmap FAILED" : "mmap", len, prot, flags, __builtin_return_address(0));
    return r;
}

void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off64_t off) {
    static void *(*real)(void *, size_t, int, int, int, off64_t);
    if (!real) real = (void *(*)(void *, size_t, int, int, int, off64_t))dlsym(RTLD_NEXT, "mmap64");
    void *r = real(addr, len, prot, flags, fd, off);
    if (len >= BIG && tracing() && !(flags & MAP_FIXED))
        say(r == MAP_FAILED ? "mmap64 FAILED" : "mmap64", len, prot, flags, __builtin_return_address(0));
    return r;
}

void *mremap(void *old, size_t oldlen, size_t newlen, int flags, ...) {
    static void *(*real)(void *, size_t, size_t, int, ...);
    if (!real) real = (void *(*)(void *, size_t, size_t, int, ...))dlsym(RTLD_NEXT, "mremap");
    va_list ap; va_start(ap, flags);
    void *newaddr = va_arg(ap, void *);
    va_end(ap);
    void *r = real(old, oldlen, newlen, flags, newaddr);
    if (newlen >= BIG && tracing())
        say("mremap", newlen, 0, flags, __builtin_return_address(0));
    return r;
}

/* --- large CPU-side allocations ---
 * gamescope holds ~830 MB of committed, fully-resident anonymous memory in
 * 46/18 MiB blocks that no Vulkan API call in its source accounts for and
 * that the mmap tracer never sees. glibc maps big malloc requests through an
 * internal __mmap, invisible to symbol interposition at the mmap level — but
 * the malloc call itself comes through the PLT, so it can be traced here.
 * ddcefmem.dladdr() may itself allocate: `busy` keeps that from recursing. */
static __thread int busy;

static void trace_alloc(const char *what, size_t n, void *ra) {
    if (n < BIG || busy || !tracing()) return;
    busy = 1;
    say(what, n, 0, 0, ra);
    busy = 0;
}

void *malloc(size_t n) {
    static void *(*real)(size_t);
    if (!real) real = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
    void *p = real(n);
    trace_alloc("malloc", n, __builtin_return_address(0));
    return p;
}

void *calloc(size_t a, size_t b) {
    static void *(*real)(size_t, size_t);
    if (!real) real = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    void *p = real(a, b);
    trace_alloc("calloc", a * b, __builtin_return_address(0));
    return p;
}

void *realloc(void *old, size_t n) {
    static void *(*real)(void *, size_t);
    if (!real) real = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    void *p = real(old, n);
    trace_alloc("realloc", n, __builtin_return_address(0));
    return p;
}
