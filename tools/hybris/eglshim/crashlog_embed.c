/* crashlog.c — fault logger for the CEF GPU process (ShieldDeck).
 *
 * Interposes sigaction(): whenever anyone (Chromium/crashpad included) installs
 * a handler for a fatal signal in a process whose argv has --type=gpu-process,
 * we keep their handler and put ours in front. Our handler dumps the fault
 * address, PC/LR/SP, /proc/self/maps and the first stack words to
 * /tmp/crashlog.txt, then calls the saved handler so crashpad still works.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <link.h>
#include <dirent.h>

#define NSIGS 8
static int g_sig[NSIGS];                 /* the signals we watch */
static struct sigaction g_prev[NSIGS];   /* handler that was installed under us */
static int g_chain[NSIGS];               /* 1 = g_prev valid */
static int g_busy;
static int g_gpu = -1;

/* The shim can be the LAST object in a process's link map (it is, in the CEF
 * GPU process — SDL3 pulls it in as its libEGL.so.1 dependency), where
 * dlsym(RTLD_NEXT, ...) finds nothing. Resolve libc symbols through the
 * already-loaded libc object instead; dlsym on that handle cannot return our
 * own interposers. */
static void *real_libc_sym(const char *name) {
    static void *h;
    if (!h) h = dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
    return h ? dlsym(h, name) : NULL;
}

/* Crashpad's dump has no module list, so keep a rolling /proc/self/maps
 * snapshot on disk; the minidump's PC can then be resolved to a module
 * after the fact. */
#include <pthread.h>
static void *maps_thread(void *arg) {
    (void)arg;
    char path[64];
    snprintf(path, sizeof path, "/tmp/ddmaps-%d.txt", (int)getpid());
    char spath[64];
    snprintf(spath, sizeof spath, "/tmp/ddstat-%d.txt", (int)getpid());
    for (;;) {
        int in = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
        int out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (in >= 0 && out >= 0) {
            char b[8192]; ssize_t r;
            while ((r = read(in, b, sizeof b)) > 0)
                if (write(out, b, (size_t)r) < 0) break;
        }
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        /* per-thread state: wchan + pending signals, ~20 ms before death */
        int sin = open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        int sout = open(spath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (sin >= 0 && sout >= 0) {
            /* per-thread state: name + blocked signals for EVERY thread —
             * a thread that blocks SIGSEGV dies with the default action on a
             * synchronous fault, bypassing every handler (the exit_code=11
             * no-crashlog signature). */
            DIR *tasks = fdopendir(sin);
            char eb[256];
            struct dirent *de;
            while (tasks && (de = readdir(tasks))) {
                if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
                char ps[96];
                snprintf(ps, sizeof ps, "/proc/self/task/%s/status", de->d_name);
                int pin = open(ps, O_RDONLY | O_CLOEXEC);
                if (pin < 0) continue;
                char pb[2048]; ssize_t pr = read(pin, pb, sizeof pb - 1);
                close(pin);
                if (pr <= 0) continue;
                pb[pr] = 0;
                char name[64] = "?", blk[64] = "?";
                for (char *line = pb, *end = pb + pr; line < end; ) {
                    char *nl = memchr(line, '\n', (size_t)(end - line));
                    if (!nl) nl = end;
                    if (!strncmp(line, "Name:", 5))
                        snprintf(name, sizeof name, "%.*s", (int)(nl - line - 6), line + 6);
                    else if (!strncmp(line, "SigBlk:", 7))
                        snprintf(blk, sizeof blk, "%.*s", (int)(nl - line - 8), line + 8);
                    line = nl + 1;
                }
                int n = snprintf(eb, sizeof eb, "tid=%s name=%s SigBlk=%s\n", de->d_name, name, blk);
                if (write(sout, eb, (size_t)n) < 0) break;
            }
            if (tasks) closedir(tasks);
            else close(sin);
        } else if (sin >= 0) {
            close(sin);
        }
        if (sout >= 0) close(sout);
        usleep(20000);
    }
    return NULL;
}

/* ---- signal-send interposition ----
 * The GPU process dies from an asynchronously delivered SIGSEGV while its main
 * thread sits in a raw rt_sigtimedwait loop. If the sender uses libc from any
 * steamwebhelper process (all of them load this shim), we see it here. */
static void log_send(const char *fn, long a, long b, int sig) {
    if (g_gpu != 1) return;   /* preloaded everywhere: only log GPU children */
    if (sig < 1 || sig > 64) return;
    char buf[256];
    int n = snprintf(buf, sizeof buf, "SEND %s(pid=%ld targ=%ld sig=%d) from pc=%p",
                     fn, a, b, sig, __builtin_return_address(0));
    int fd = open("/tmp/ddcef-egl.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (fd >= 0) { write(fd, buf, n); write(fd, "\n", 1); close(fd); }
}

int kill(pid_t pid, int sig) {
    log_send("kill", (long)pid, 0, sig);
    static int (*real)(pid_t, int);
    if (!real) real = real_libc_sym("kill");
    return real ? real(pid, sig) : -1;
}

int tgkill(int tgid, int tid, int sig) {
    log_send("tgkill", (long)tgid, (long)tid, sig);
    static int (*real)(int, int, int);
    if (!real) real = real_libc_sym("tgkill");
    return real ? real(tgid, tid, sig) : -1;
}

int pthread_kill(pthread_t t, int sig) {
    log_send("pthread_kill", (long)(uintptr_t)t, 0, sig);
    static int (*real)(pthread_t, int);
    if (!real) real = real_libc_sym("pthread_kill");
    return real ? real(t, sig) : -1;
}

int raise(int sig) {
    log_send("raise", 0, 0, sig);
    static int (*real)(int);
    if (!real) real = real_libc_sym("raise");
    return real ? real(sig) : -1;
}

static int is_watched(int s) {
    for (int i = 0; i < NSIGS; i++) if (g_sig[i] == s) return i;
    return -1;
}

static void logger(int sig, siginfo_t *si, void *uc) {
    if (g_busy) {                       /* fault inside the logger: bail out */
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_busy = 1;
    ucontext_t *u = (ucontext_t *)uc;
    mcontext_t *m = &u->uc_mcontext;
    uint64_t pc = m->pc, lr = m->regs[30], sp = m->sp;
    int out = open("/tmp/crashlog.txt", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (out < 0) out = 2;
    char buf[256];
    int n = snprintf(buf, sizeof buf,
        "\n=== crash sig=%d addr=%p pc=%p lr=%p sp=%p\n",
        sig, si->si_addr, (void *)pc, (void *)lr, (void *)sp);
    if (write(out, buf, n) > 0) {
        int mp = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
        if (mp >= 0) {
            char b[4096]; ssize_t r;
            while ((r = read(mp, b, sizeof b)) > 0)
                if (write(out, b, r) < 0) break;
            close(mp);
        }
        n = snprintf(buf, sizeof buf, "--- stack words @ sp:\n");
        (void)write(out, buf, n);
        int mf = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
        for (int i = 0; mf >= 0 && i < 96; i++) {
            uint64_t v = 0;
            if (pread(mf, &v, 8, (off_t)(sp + (uint64_t)i * 8)) == 8) {
                int k = snprintf(buf, sizeof buf, "  sp+%03d: %016llx\n",
                                 i * 8, (unsigned long long)v);
                (void)write(out, buf, k);
            }
        }
        if (mf >= 0) close(mf);
    }
    close(out);
    g_busy = 0;
    /* hand over to the handler we displaced (crashpad etc.) */
    int idx = is_watched(sig);
    if (idx >= 0 && g_chain[idx]) {
        struct sigaction *p = &g_prev[idx];
        if (p->sa_flags & SA_SIGINFO) {
            if (p->sa_sigaction) p->sa_sigaction(sig, si, uc);
            return;
        }
        if (p->sa_handler == SIG_IGN) return;
        if (p->sa_handler == SIG_DFL || p->sa_handler == NULL) {
            signal(sig, SIG_DFL);
            raise(sig);
            return;
        }
        void (*h)(int) = p->sa_handler;
        signal(sig, SIG_DFL);
        h(sig);
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    static int (*real)(int, const struct sigaction *, struct sigaction *);
    if (!real) real = real_libc_sym("sigaction");
    if (!real) return -1;
    if (g_gpu && act) {
        int idx = is_watched(signum);
        if (idx >= 0) {
            logline("sigaction(%d) intercepted in gpu process", signum);
            struct sigaction mine;
            memcpy(&mine, act, sizeof mine);
            if (g_chain[idx]) { /* keep the earlier saved handler, do not stack */
                memcpy(&g_prev[idx], act, sizeof mine);
            } else {
                memcpy(&g_prev[idx], act, sizeof mine);
                g_chain[idx] = 1;
            }
            mine.sa_sigaction = logger;
            mine.sa_flags |= SA_SIGINFO;
            mine.sa_flags &= ~(SA_RESETHAND);
            return real(signum, &mine, oldact);
        }
    }
    return real(signum, act, oldact);
}

/* Runtime ground truth for the libcef DT16 patch: read the actual
 * instruction word at the display-list builder's string-match site
 * (vaddr 0x7872bd0) in whatever libcef THIS process mapped. Catches both
 * "patch not applied on disk" and "child mapped a different libcef than
 * the one patched". mov w10,#14 = 0x528001ca, mov w10,#16 = 0x5280020a. */
#define CEF_DT_SITE_VADDR 0x7872bd0
static int cef_dt_checked;
static int cef_dt_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    if (!strstr(info->dlpi_name, "libcef.so")) return 0;
    cef_dt_checked = 1;
    uint32_t w = *(uint32_t *)(info->dlpi_addr + CEF_DT_SITE_VADDR);
    if (w == 0x5280020a)
        logline("cef DT insn=%08x (#16 PATCHED ok) base=%p", w, (void *)(uintptr_t)info->dlpi_addr);
    else if (w == 0x528001ca)
        logline("cef DT insn=%08x (#14 UNPATCHED) base=%p", w, (void *)(uintptr_t)info->dlpi_addr);
    else
        logline("cef DT insn=%08x (SITE MOVED?) base=%p", w, (void *)(uintptr_t)info->dlpi_addr);
    return 1;
}

__attribute__((constructor)) static void init(void) {
    g_sig[0] = SIGSEGV; g_sig[1] = SIGBUS; g_sig[2] = SIGILL;
    g_sig[3] = SIGTRAP; g_sig[4] = SIGABRT;
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char b[4096]; ssize_t r = read(fd, b, sizeof b - 1);
        close(fd);
        if (r > 0) g_gpu = memmem(b, (size_t)r, "--type=gpu-process", 18) != NULL;
    }
    if (g_gpu) {
        dl_iterate_phdr(cef_dt_cb, NULL);
        if (!cef_dt_checked) logline("cef DT check: libcef.so not in link map");
        /* nobody installed anything yet: put our logger up front.
         * RTLD_NEXT returns NULL when the shim is the LAST object in the
         * link map (the GPU process loads it via SDL3's dependency chain) —
         * calling through that NULL was itself a SIGSEGV. */
        static int (*probe)(int, const struct sigaction *, struct sigaction *);
        if (!probe) probe = real_libc_sym("sigaction");
        if (probe) {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = logger;
            sa.sa_flags = SA_SIGINFO;
            sigemptyset(&sa.sa_mask);
            for (int i = 0; i < NSIGS && g_sig[i]; i++) {
                int idx = is_watched(g_sig[i]);
                struct sigaction old;
                if (probe(g_sig[i], NULL, &old) == 0) {
                    memcpy(&g_prev[idx], &old, sizeof old);
                    g_chain[idx] = 1;
                }
                sigaction(g_sig[i], &sa, NULL);
            }
            logline("crashlog ARMED in gpu process (pid %d)", (int)getpid());
            pthread_t mt;
            if (pthread_create(&mt, NULL, maps_thread, NULL) == 0) pthread_detach(mt);
        } else {
            logline("crashlog ARM FAILED: libc sigaction unresolved");
        }
    } else {
        logline("crashlog not armed (gpu=%d)", g_gpu);
    }
}

/* ---- exit-path interposition ----
 * The GPU process dies with exit_code=11 and no signal handler ever fires,
 * which points at a plain exit(11)/_exit(11) rather than a fatal signal.
 * Log every exit call from a GPU process with the caller's addresses so the
 * module can be named from /proc/self/maps. */
static void dump_exit(const char *kind, int code, void *ra) {
    if (!g_gpu) return;
    int out = open("/tmp/crashlog.txt", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (out < 0) out = 2;
    char buf[256];
    int n = snprintf(buf, sizeof buf, "\n=== %s(%d) ra=%p\n", kind, code, ra);
    (void)write(out, buf, n);
    int mp = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (mp >= 0) {
        char b[4096]; ssize_t r;
        while ((r = read(mp, b, sizeof b)) > 0)
            if (write(out, b, r) < 0) break;
        close(mp);
    }
    if (out != 2) close(out);
}

void exit(int code) {
    dump_exit("exit", code, __builtin_return_address(0));
    static void (*real)(int);
    if (!real) real = real_libc_sym("exit");
    real(code);
    for (;;) ;
}

void _exit(int code) {
    dump_exit("_exit", code, __builtin_return_address(0));
    static void (*real)(int);
    if (!real) real = real_libc_sym("_exit");
    real(code);
    for (;;) ;
}

void _Exit(int code) {
    dump_exit("_Exit", code, __builtin_return_address(0));
    static void (*real)(int);
    if (!real) real = real_libc_sym("_Exit");
    real(code);
    for (;;) ;
}

void abort(void) {
    dump_exit("abort", -1, __builtin_return_address(0));
    static void (*real)(void);
    if (!real) real = real_libc_sym("abort");
    real();
    for (;;) ;
}
