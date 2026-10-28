/* eglfront.c — ANGLE front-lib wrapper for the CEF GPU process (ShieldDeck).
 *
 * Why: Chromium selects the ANGLE display type from the CLIENT extension
 * string, queried as eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS). ANGLE
 * 6478 rejects that query with EGL_BAD_DISPLAY (verified on device), so
 * Chromium sees no EGL_ANGLE_platform_angle* extensions, every explicit
 * --use-angle request adds nothing to the display-init list, and the GPU
 * process falls back to the DEFAULT display (GLX -> Mesa llvmpipe).
 * This wrapper is loaded by Chromium as the ANGLE front lib, forwards every
 * call to the real ANGLE front by absolute path, and answers the client
 * extension query itself with the string ANGLE 6478 would generate for this
 * build configuration (GL backend, null backend, no Vulkan/D3D/Metal).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define REAL_LIB "/usr/local/hybris/usr/lib/angle/libEGL.so"

static void logpid(char *buf, size_t n) {
    char cmd[300] = {0};
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, cmd, sizeof cmd - 2);
        if (r > 0) for (ssize_t i = 0; i < r - 1; i++) if (cmd[i] == '\0') cmd[i] = ' ';
        close(fd);
    }
    snprintf(buf, n, "[pid %d %.200s] ", (int)getpid(), cmd);
}

static void logline(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[256];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    int fd = open("/tmp/ddcef-egl.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    char pbuf[340];
    logpid(pbuf, sizeof pbuf);
    write(fd, "[eglfront] ", 11);
    write(fd, pbuf, strlen(pbuf));
    write(fd, buf, (size_t)n);
    write(fd, "\n", 1);
    close(fd);
}

static const char kClientExtensions[] =
    "EGL_EXT_client_extensions "
    "EGL_EXT_platform_base "
    "EGL_ANGLE_platform_angle "
    "EGL_ANGLE_platform_angle_opengl "
    "EGL_ANGLE_platform_angle_null "
    "EGL_ANGLE_platform_angle_device_type_egl_angle "
    "EGL_ANGLE_x11_visual "
    "EGL_MESA_platform_surfaceless "
    "EGL_ANGLE_client_get_all_proc_addresses "
    "EGL_ANGLE_debug "
    "EGL_ANGLE_feature_control "
    "EGL_ANGLE_device_query_ext "
    "EGL_ANGLE_no_error";

static void *real_handle;

__attribute__((constructor))
static void eglfront_init(void) {
    real_handle = dlopen(REAL_LIB, RTLD_NOW | RTLD_GLOBAL);
    if (!real_handle) {
        logline("FATAL: dlopen %s: %s", REAL_LIB, dlerror());
        return;
    }
    logline("front wrapper loaded, real ANGLE at %p", real_handle);
}

const char *eglQueryString(void *dpy, int name) {
    logline("eglQueryString(dpy=%p name=%#x)", dpy, name);
    if (!real_handle) return NULL;
    static const char *(*real)(void *, int);
    if (!real) real = dlsym(real_handle, "eglQueryString");
    if (dpy == 0 && name == 0x3055) {  /* EGL_EXTENSIONS */
        logline("client extension query served");
        return kClientExtensions;
    }
    return real ? real(dpy, name) : NULL;
}

static __attribute__((used)) void *p_eglAcquireExternalContextANGLE;
__asm__(".text\n.globl eglAcquireExternalContextANGLE\n.type eglAcquireExternalContextANGLE,%function\neglAcquireExternalContextANGLE:\n\tadrp x16, p_eglAcquireExternalContextANGLE\n\tldr x16, [x16, :lo12:p_eglAcquireExternalContextANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglBindAPI;
__asm__(".text\n.globl eglBindAPI\n.type eglBindAPI,%function\neglBindAPI:\n\tadrp x16, p_eglBindAPI\n\tldr x16, [x16, :lo12:p_eglBindAPI]\n\tbr x16\n");
static __attribute__((used)) void *p_eglBindTexImage;
__asm__(".text\n.globl eglBindTexImage\n.type eglBindTexImage,%function\neglBindTexImage:\n\tadrp x16, p_eglBindTexImage\n\tldr x16, [x16, :lo12:p_eglBindTexImage]\n\tbr x16\n");
static __attribute__((used)) void *p_eglChooseConfig;
__asm__(".text\n.globl eglChooseConfig\n.type eglChooseConfig,%function\neglChooseConfig:\n\tadrp x16, p_eglChooseConfig\n\tldr x16, [x16, :lo12:p_eglChooseConfig]\n\tbr x16\n");
static __attribute__((used)) void *p_eglClientWaitSync;
__asm__(".text\n.globl eglClientWaitSync\n.type eglClientWaitSync,%function\neglClientWaitSync:\n\tadrp x16, p_eglClientWaitSync\n\tldr x16, [x16, :lo12:p_eglClientWaitSync]\n\tbr x16\n");
static __attribute__((used)) void *p_eglClientWaitSyncKHR;
__asm__(".text\n.globl eglClientWaitSyncKHR\n.type eglClientWaitSyncKHR,%function\neglClientWaitSyncKHR:\n\tadrp x16, p_eglClientWaitSyncKHR\n\tldr x16, [x16, :lo12:p_eglClientWaitSyncKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCopyBuffers;
__asm__(".text\n.globl eglCopyBuffers\n.type eglCopyBuffers,%function\neglCopyBuffers:\n\tadrp x16, p_eglCopyBuffers\n\tldr x16, [x16, :lo12:p_eglCopyBuffers]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCopyMetalSharedEventANGLE;
__asm__(".text\n.globl eglCopyMetalSharedEventANGLE\n.type eglCopyMetalSharedEventANGLE,%function\neglCopyMetalSharedEventANGLE:\n\tadrp x16, p_eglCopyMetalSharedEventANGLE\n\tldr x16, [x16, :lo12:p_eglCopyMetalSharedEventANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreateContext;
__asm__(".text\n.globl eglCreateContext\n.type eglCreateContext,%function\neglCreateContext:\n\tadrp x16, p_eglCreateContext\n\tldr x16, [x16, :lo12:p_eglCreateContext]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreateDeviceANGLE;
__asm__(".text\n.globl eglCreateDeviceANGLE\n.type eglCreateDeviceANGLE,%function\neglCreateDeviceANGLE:\n\tadrp x16, p_eglCreateDeviceANGLE\n\tldr x16, [x16, :lo12:p_eglCreateDeviceANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreateImage;
__asm__(".text\n.globl eglCreateImage\n.type eglCreateImage,%function\neglCreateImage:\n\tadrp x16, p_eglCreateImage\n\tldr x16, [x16, :lo12:p_eglCreateImage]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreateImageKHR;
__asm__(".text\n.globl eglCreateImageKHR\n.type eglCreateImageKHR,%function\neglCreateImageKHR:\n\tadrp x16, p_eglCreateImageKHR\n\tldr x16, [x16, :lo12:p_eglCreateImageKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreateNativeClientBufferANDROID;
__asm__(".text\n.globl eglCreateNativeClientBufferANDROID\n.type eglCreateNativeClientBufferANDROID,%function\neglCreateNativeClientBufferANDROID:\n\tadrp x16, p_eglCreateNativeClientBufferANDROID\n\tldr x16, [x16, :lo12:p_eglCreateNativeClientBufferANDROID]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreatePbufferFromClientBuffer;
__asm__(".text\n.globl eglCreatePbufferFromClientBuffer\n.type eglCreatePbufferFromClientBuffer,%function\neglCreatePbufferFromClientBuffer:\n\tadrp x16, p_eglCreatePbufferFromClientBuffer\n\tldr x16, [x16, :lo12:p_eglCreatePbufferFromClientBuffer]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreatePbufferSurface;
__asm__(".text\n.globl eglCreatePbufferSurface\n.type eglCreatePbufferSurface,%function\neglCreatePbufferSurface:\n\tadrp x16, p_eglCreatePbufferSurface\n\tldr x16, [x16, :lo12:p_eglCreatePbufferSurface]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreatePixmapSurface;
__asm__(".text\n.globl eglCreatePixmapSurface\n.type eglCreatePixmapSurface,%function\neglCreatePixmapSurface:\n\tadrp x16, p_eglCreatePixmapSurface\n\tldr x16, [x16, :lo12:p_eglCreatePixmapSurface]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreatePlatformPixmapSurface;
__asm__(".text\n.globl eglCreatePlatformPixmapSurface\n.type eglCreatePlatformPixmapSurface,%function\neglCreatePlatformPixmapSurface:\n\tadrp x16, p_eglCreatePlatformPixmapSurface\n\tldr x16, [x16, :lo12:p_eglCreatePlatformPixmapSurface]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreatePlatformPixmapSurfaceEXT;
__asm__(".text\n.globl eglCreatePlatformPixmapSurfaceEXT\n.type eglCreatePlatformPixmapSurfaceEXT,%function\neglCreatePlatformPixmapSurfaceEXT:\n\tadrp x16, p_eglCreatePlatformPixmapSurfaceEXT\n\tldr x16, [x16, :lo12:p_eglCreatePlatformPixmapSurfaceEXT]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreatePlatformWindowSurface;
__asm__(".text\n.globl eglCreatePlatformWindowSurface\n.type eglCreatePlatformWindowSurface,%function\neglCreatePlatformWindowSurface:\n\tadrp x16, p_eglCreatePlatformWindowSurface\n\tldr x16, [x16, :lo12:p_eglCreatePlatformWindowSurface]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreatePlatformWindowSurfaceEXT;
__asm__(".text\n.globl eglCreatePlatformWindowSurfaceEXT\n.type eglCreatePlatformWindowSurfaceEXT,%function\neglCreatePlatformWindowSurfaceEXT:\n\tadrp x16, p_eglCreatePlatformWindowSurfaceEXT\n\tldr x16, [x16, :lo12:p_eglCreatePlatformWindowSurfaceEXT]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreateStreamKHR;
__asm__(".text\n.globl eglCreateStreamKHR\n.type eglCreateStreamKHR,%function\neglCreateStreamKHR:\n\tadrp x16, p_eglCreateStreamKHR\n\tldr x16, [x16, :lo12:p_eglCreateStreamKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreateStreamProducerD3DTextureANGLE;
__asm__(".text\n.globl eglCreateStreamProducerD3DTextureANGLE\n.type eglCreateStreamProducerD3DTextureANGLE,%function\neglCreateStreamProducerD3DTextureANGLE:\n\tadrp x16, p_eglCreateStreamProducerD3DTextureANGLE\n\tldr x16, [x16, :lo12:p_eglCreateStreamProducerD3DTextureANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreateSync;
__asm__(".text\n.globl eglCreateSync\n.type eglCreateSync,%function\neglCreateSync:\n\tadrp x16, p_eglCreateSync\n\tldr x16, [x16, :lo12:p_eglCreateSync]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreateSyncKHR;
__asm__(".text\n.globl eglCreateSyncKHR\n.type eglCreateSyncKHR,%function\neglCreateSyncKHR:\n\tadrp x16, p_eglCreateSyncKHR\n\tldr x16, [x16, :lo12:p_eglCreateSyncKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglCreateWindowSurface;
__asm__(".text\n.globl eglCreateWindowSurface\n.type eglCreateWindowSurface,%function\neglCreateWindowSurface:\n\tadrp x16, p_eglCreateWindowSurface\n\tldr x16, [x16, :lo12:p_eglCreateWindowSurface]\n\tbr x16\n");
static __attribute__((used)) void *p_eglDebugMessageControlKHR;
__asm__(".text\n.globl eglDebugMessageControlKHR\n.type eglDebugMessageControlKHR,%function\neglDebugMessageControlKHR:\n\tadrp x16, p_eglDebugMessageControlKHR\n\tldr x16, [x16, :lo12:p_eglDebugMessageControlKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglDestroyContext;
__asm__(".text\n.globl eglDestroyContext\n.type eglDestroyContext,%function\neglDestroyContext:\n\tadrp x16, p_eglDestroyContext\n\tldr x16, [x16, :lo12:p_eglDestroyContext]\n\tbr x16\n");
static __attribute__((used)) void *p_eglDestroyImage;
__asm__(".text\n.globl eglDestroyImage\n.type eglDestroyImage,%function\neglDestroyImage:\n\tadrp x16, p_eglDestroyImage\n\tldr x16, [x16, :lo12:p_eglDestroyImage]\n\tbr x16\n");
static __attribute__((used)) void *p_eglDestroyImageKHR;
__asm__(".text\n.globl eglDestroyImageKHR\n.type eglDestroyImageKHR,%function\neglDestroyImageKHR:\n\tadrp x16, p_eglDestroyImageKHR\n\tldr x16, [x16, :lo12:p_eglDestroyImageKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglDestroyStreamKHR;
__asm__(".text\n.globl eglDestroyStreamKHR\n.type eglDestroyStreamKHR,%function\neglDestroyStreamKHR:\n\tadrp x16, p_eglDestroyStreamKHR\n\tldr x16, [x16, :lo12:p_eglDestroyStreamKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglDestroySurface;
__asm__(".text\n.globl eglDestroySurface\n.type eglDestroySurface,%function\neglDestroySurface:\n\tadrp x16, p_eglDestroySurface\n\tldr x16, [x16, :lo12:p_eglDestroySurface]\n\tbr x16\n");
static __attribute__((used)) void *p_eglDestroySync;
__asm__(".text\n.globl eglDestroySync\n.type eglDestroySync,%function\neglDestroySync:\n\tadrp x16, p_eglDestroySync\n\tldr x16, [x16, :lo12:p_eglDestroySync]\n\tbr x16\n");
static __attribute__((used)) void *p_eglDestroySyncKHR;
__asm__(".text\n.globl eglDestroySyncKHR\n.type eglDestroySyncKHR,%function\neglDestroySyncKHR:\n\tadrp x16, p_eglDestroySyncKHR\n\tldr x16, [x16, :lo12:p_eglDestroySyncKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglDupNativeFenceFDANDROID;
__asm__(".text\n.globl eglDupNativeFenceFDANDROID\n.type eglDupNativeFenceFDANDROID,%function\neglDupNativeFenceFDANDROID:\n\tadrp x16, p_eglDupNativeFenceFDANDROID\n\tldr x16, [x16, :lo12:p_eglDupNativeFenceFDANDROID]\n\tbr x16\n");
static __attribute__((used)) void *p_eglExportVkImageANGLE;
__asm__(".text\n.globl eglExportVkImageANGLE\n.type eglExportVkImageANGLE,%function\neglExportVkImageANGLE:\n\tadrp x16, p_eglExportVkImageANGLE\n\tldr x16, [x16, :lo12:p_eglExportVkImageANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglForceGPUSwitchANGLE;
__asm__(".text\n.globl eglForceGPUSwitchANGLE\n.type eglForceGPUSwitchANGLE,%function\neglForceGPUSwitchANGLE:\n\tadrp x16, p_eglForceGPUSwitchANGLE\n\tldr x16, [x16, :lo12:p_eglForceGPUSwitchANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetCompositorTimingANDROID;
__asm__(".text\n.globl eglGetCompositorTimingANDROID\n.type eglGetCompositorTimingANDROID,%function\neglGetCompositorTimingANDROID:\n\tadrp x16, p_eglGetCompositorTimingANDROID\n\tldr x16, [x16, :lo12:p_eglGetCompositorTimingANDROID]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetCompositorTimingSupportedANDROID;
__asm__(".text\n.globl eglGetCompositorTimingSupportedANDROID\n.type eglGetCompositorTimingSupportedANDROID,%function\neglGetCompositorTimingSupportedANDROID:\n\tadrp x16, p_eglGetCompositorTimingSupportedANDROID\n\tldr x16, [x16, :lo12:p_eglGetCompositorTimingSupportedANDROID]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetConfigAttrib;
__asm__(".text\n.globl eglGetConfigAttrib\n.type eglGetConfigAttrib,%function\neglGetConfigAttrib:\n\tadrp x16, p_eglGetConfigAttrib\n\tldr x16, [x16, :lo12:p_eglGetConfigAttrib]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetConfigs;
__asm__(".text\n.globl eglGetConfigs\n.type eglGetConfigs,%function\neglGetConfigs:\n\tadrp x16, p_eglGetConfigs\n\tldr x16, [x16, :lo12:p_eglGetConfigs]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetCurrentContext;
__asm__(".text\n.globl eglGetCurrentContext\n.type eglGetCurrentContext,%function\neglGetCurrentContext:\n\tadrp x16, p_eglGetCurrentContext\n\tldr x16, [x16, :lo12:p_eglGetCurrentContext]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetCurrentDisplay;
__asm__(".text\n.globl eglGetCurrentDisplay\n.type eglGetCurrentDisplay,%function\neglGetCurrentDisplay:\n\tadrp x16, p_eglGetCurrentDisplay\n\tldr x16, [x16, :lo12:p_eglGetCurrentDisplay]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetCurrentSurface;
__asm__(".text\n.globl eglGetCurrentSurface\n.type eglGetCurrentSurface,%function\neglGetCurrentSurface:\n\tadrp x16, p_eglGetCurrentSurface\n\tldr x16, [x16, :lo12:p_eglGetCurrentSurface]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetError;
__asm__(".text\n.globl eglGetError\n.type eglGetError,%function\neglGetError:\n\tadrp x16, p_eglGetError\n\tldr x16, [x16, :lo12:p_eglGetError]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetFrameTimestampsANDROID;
__asm__(".text\n.globl eglGetFrameTimestampsANDROID\n.type eglGetFrameTimestampsANDROID,%function\neglGetFrameTimestampsANDROID:\n\tadrp x16, p_eglGetFrameTimestampsANDROID\n\tldr x16, [x16, :lo12:p_eglGetFrameTimestampsANDROID]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetFrameTimestampSupportedANDROID;
__asm__(".text\n.globl eglGetFrameTimestampSupportedANDROID\n.type eglGetFrameTimestampSupportedANDROID,%function\neglGetFrameTimestampSupportedANDROID:\n\tadrp x16, p_eglGetFrameTimestampSupportedANDROID\n\tldr x16, [x16, :lo12:p_eglGetFrameTimestampSupportedANDROID]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetMscRateANGLE;
__asm__(".text\n.globl eglGetMscRateANGLE\n.type eglGetMscRateANGLE,%function\neglGetMscRateANGLE:\n\tadrp x16, p_eglGetMscRateANGLE\n\tldr x16, [x16, :lo12:p_eglGetMscRateANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetNativeClientBufferANDROID;
__asm__(".text\n.globl eglGetNativeClientBufferANDROID\n.type eglGetNativeClientBufferANDROID,%function\neglGetNativeClientBufferANDROID:\n\tadrp x16, p_eglGetNativeClientBufferANDROID\n\tldr x16, [x16, :lo12:p_eglGetNativeClientBufferANDROID]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetNextFrameIdANDROID;
__asm__(".text\n.globl eglGetNextFrameIdANDROID\n.type eglGetNextFrameIdANDROID,%function\neglGetNextFrameIdANDROID:\n\tadrp x16, p_eglGetNextFrameIdANDROID\n\tldr x16, [x16, :lo12:p_eglGetNextFrameIdANDROID]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetSyncAttrib;
__asm__(".text\n.globl eglGetSyncAttrib\n.type eglGetSyncAttrib,%function\neglGetSyncAttrib:\n\tadrp x16, p_eglGetSyncAttrib\n\tldr x16, [x16, :lo12:p_eglGetSyncAttrib]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetSyncAttribKHR;
__asm__(".text\n.globl eglGetSyncAttribKHR\n.type eglGetSyncAttribKHR,%function\neglGetSyncAttribKHR:\n\tadrp x16, p_eglGetSyncAttribKHR\n\tldr x16, [x16, :lo12:p_eglGetSyncAttribKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglGetSyncValuesCHROMIUM;
__asm__(".text\n.globl eglGetSyncValuesCHROMIUM\n.type eglGetSyncValuesCHROMIUM,%function\neglGetSyncValuesCHROMIUM:\n\tadrp x16, p_eglGetSyncValuesCHROMIUM\n\tldr x16, [x16, :lo12:p_eglGetSyncValuesCHROMIUM]\n\tbr x16\n");
static __attribute__((used)) void *p_eglHandleGPUSwitchANGLE;
__asm__(".text\n.globl eglHandleGPUSwitchANGLE\n.type eglHandleGPUSwitchANGLE,%function\neglHandleGPUSwitchANGLE:\n\tadrp x16, p_eglHandleGPUSwitchANGLE\n\tldr x16, [x16, :lo12:p_eglHandleGPUSwitchANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglInitialize;
__asm__(".text\n.globl eglInitialize\n.type eglInitialize,%function\neglInitialize:\n\tadrp x16, p_eglInitialize\n\tldr x16, [x16, :lo12:p_eglInitialize]\n\tbr x16\n");
static __attribute__((used)) void *p_eglLabelObjectKHR;
__asm__(".text\n.globl eglLabelObjectKHR\n.type eglLabelObjectKHR,%function\neglLabelObjectKHR:\n\tadrp x16, p_eglLabelObjectKHR\n\tldr x16, [x16, :lo12:p_eglLabelObjectKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglLockSurfaceKHR;
__asm__(".text\n.globl eglLockSurfaceKHR\n.type eglLockSurfaceKHR,%function\neglLockSurfaceKHR:\n\tadrp x16, p_eglLockSurfaceKHR\n\tldr x16, [x16, :lo12:p_eglLockSurfaceKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglMakeCurrent;
__asm__(".text\n.globl eglMakeCurrent\n.type eglMakeCurrent,%function\neglMakeCurrent:\n\tadrp x16, p_eglMakeCurrent\n\tldr x16, [x16, :lo12:p_eglMakeCurrent]\n\tbr x16\n");
static __attribute__((used)) void *p_eglPostSubBufferNV;
__asm__(".text\n.globl eglPostSubBufferNV\n.type eglPostSubBufferNV,%function\neglPostSubBufferNV:\n\tadrp x16, p_eglPostSubBufferNV\n\tldr x16, [x16, :lo12:p_eglPostSubBufferNV]\n\tbr x16\n");
static __attribute__((used)) void *p_eglPrepareSwapBuffersANGLE;
__asm__(".text\n.globl eglPrepareSwapBuffersANGLE\n.type eglPrepareSwapBuffersANGLE,%function\neglPrepareSwapBuffersANGLE:\n\tadrp x16, p_eglPrepareSwapBuffersANGLE\n\tldr x16, [x16, :lo12:p_eglPrepareSwapBuffersANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglPresentationTimeANDROID;
__asm__(".text\n.globl eglPresentationTimeANDROID\n.type eglPresentationTimeANDROID,%function\neglPresentationTimeANDROID:\n\tadrp x16, p_eglPresentationTimeANDROID\n\tldr x16, [x16, :lo12:p_eglPresentationTimeANDROID]\n\tbr x16\n");
static __attribute__((used)) void *p_eglProgramCacheGetAttribANGLE;
__asm__(".text\n.globl eglProgramCacheGetAttribANGLE\n.type eglProgramCacheGetAttribANGLE,%function\neglProgramCacheGetAttribANGLE:\n\tadrp x16, p_eglProgramCacheGetAttribANGLE\n\tldr x16, [x16, :lo12:p_eglProgramCacheGetAttribANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglProgramCachePopulateANGLE;
__asm__(".text\n.globl eglProgramCachePopulateANGLE\n.type eglProgramCachePopulateANGLE,%function\neglProgramCachePopulateANGLE:\n\tadrp x16, p_eglProgramCachePopulateANGLE\n\tldr x16, [x16, :lo12:p_eglProgramCachePopulateANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglProgramCacheQueryANGLE;
__asm__(".text\n.globl eglProgramCacheQueryANGLE\n.type eglProgramCacheQueryANGLE,%function\neglProgramCacheQueryANGLE:\n\tadrp x16, p_eglProgramCacheQueryANGLE\n\tldr x16, [x16, :lo12:p_eglProgramCacheQueryANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglProgramCacheResizeANGLE;
__asm__(".text\n.globl eglProgramCacheResizeANGLE\n.type eglProgramCacheResizeANGLE,%function\neglProgramCacheResizeANGLE:\n\tadrp x16, p_eglProgramCacheResizeANGLE\n\tldr x16, [x16, :lo12:p_eglProgramCacheResizeANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQueryAPI;
__asm__(".text\n.globl eglQueryAPI\n.type eglQueryAPI,%function\neglQueryAPI:\n\tadrp x16, p_eglQueryAPI\n\tldr x16, [x16, :lo12:p_eglQueryAPI]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQueryContext;
__asm__(".text\n.globl eglQueryContext\n.type eglQueryContext,%function\neglQueryContext:\n\tadrp x16, p_eglQueryContext\n\tldr x16, [x16, :lo12:p_eglQueryContext]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQueryDebugKHR;
__asm__(".text\n.globl eglQueryDebugKHR\n.type eglQueryDebugKHR,%function\neglQueryDebugKHR:\n\tadrp x16, p_eglQueryDebugKHR\n\tldr x16, [x16, :lo12:p_eglQueryDebugKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQueryDeviceAttribEXT;
__asm__(".text\n.globl eglQueryDeviceAttribEXT\n.type eglQueryDeviceAttribEXT,%function\neglQueryDeviceAttribEXT:\n\tadrp x16, p_eglQueryDeviceAttribEXT\n\tldr x16, [x16, :lo12:p_eglQueryDeviceAttribEXT]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQueryDeviceStringEXT;
__asm__(".text\n.globl eglQueryDeviceStringEXT\n.type eglQueryDeviceStringEXT,%function\neglQueryDeviceStringEXT:\n\tadrp x16, p_eglQueryDeviceStringEXT\n\tldr x16, [x16, :lo12:p_eglQueryDeviceStringEXT]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQueryDisplayAttribANGLE;
__asm__(".text\n.globl eglQueryDisplayAttribANGLE\n.type eglQueryDisplayAttribANGLE,%function\neglQueryDisplayAttribANGLE:\n\tadrp x16, p_eglQueryDisplayAttribANGLE\n\tldr x16, [x16, :lo12:p_eglQueryDisplayAttribANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQueryDisplayAttribEXT;
__asm__(".text\n.globl eglQueryDisplayAttribEXT\n.type eglQueryDisplayAttribEXT,%function\neglQueryDisplayAttribEXT:\n\tadrp x16, p_eglQueryDisplayAttribEXT\n\tldr x16, [x16, :lo12:p_eglQueryDisplayAttribEXT]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQueryDmaBufFormatsEXT;
__asm__(".text\n.globl eglQueryDmaBufFormatsEXT\n.type eglQueryDmaBufFormatsEXT,%function\neglQueryDmaBufFormatsEXT:\n\tadrp x16, p_eglQueryDmaBufFormatsEXT\n\tldr x16, [x16, :lo12:p_eglQueryDmaBufFormatsEXT]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQueryDmaBufModifiersEXT;
__asm__(".text\n.globl eglQueryDmaBufModifiersEXT\n.type eglQueryDmaBufModifiersEXT,%function\neglQueryDmaBufModifiersEXT:\n\tadrp x16, p_eglQueryDmaBufModifiersEXT\n\tldr x16, [x16, :lo12:p_eglQueryDmaBufModifiersEXT]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQueryStreamKHR;
__asm__(".text\n.globl eglQueryStreamKHR\n.type eglQueryStreamKHR,%function\neglQueryStreamKHR:\n\tadrp x16, p_eglQueryStreamKHR\n\tldr x16, [x16, :lo12:p_eglQueryStreamKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQueryStreamu64KHR;
__asm__(".text\n.globl eglQueryStreamu64KHR\n.type eglQueryStreamu64KHR,%function\neglQueryStreamu64KHR:\n\tadrp x16, p_eglQueryStreamu64KHR\n\tldr x16, [x16, :lo12:p_eglQueryStreamu64KHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQueryStringiANGLE;
__asm__(".text\n.globl eglQueryStringiANGLE\n.type eglQueryStringiANGLE,%function\neglQueryStringiANGLE:\n\tadrp x16, p_eglQueryStringiANGLE\n\tldr x16, [x16, :lo12:p_eglQueryStringiANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQuerySurface;
__asm__(".text\n.globl eglQuerySurface\n.type eglQuerySurface,%function\neglQuerySurface:\n\tadrp x16, p_eglQuerySurface\n\tldr x16, [x16, :lo12:p_eglQuerySurface]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQuerySurface64KHR;
__asm__(".text\n.globl eglQuerySurface64KHR\n.type eglQuerySurface64KHR,%function\neglQuerySurface64KHR:\n\tadrp x16, p_eglQuerySurface64KHR\n\tldr x16, [x16, :lo12:p_eglQuerySurface64KHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglQuerySurfacePointerANGLE;
__asm__(".text\n.globl eglQuerySurfacePointerANGLE\n.type eglQuerySurfacePointerANGLE,%function\neglQuerySurfacePointerANGLE:\n\tadrp x16, p_eglQuerySurfacePointerANGLE\n\tldr x16, [x16, :lo12:p_eglQuerySurfacePointerANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglReacquireHighPowerGPUANGLE;
__asm__(".text\n.globl eglReacquireHighPowerGPUANGLE\n.type eglReacquireHighPowerGPUANGLE,%function\neglReacquireHighPowerGPUANGLE:\n\tadrp x16, p_eglReacquireHighPowerGPUANGLE\n\tldr x16, [x16, :lo12:p_eglReacquireHighPowerGPUANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglReleaseDeviceANGLE;
__asm__(".text\n.globl eglReleaseDeviceANGLE\n.type eglReleaseDeviceANGLE,%function\neglReleaseDeviceANGLE:\n\tadrp x16, p_eglReleaseDeviceANGLE\n\tldr x16, [x16, :lo12:p_eglReleaseDeviceANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglReleaseExternalContextANGLE;
__asm__(".text\n.globl eglReleaseExternalContextANGLE\n.type eglReleaseExternalContextANGLE,%function\neglReleaseExternalContextANGLE:\n\tadrp x16, p_eglReleaseExternalContextANGLE\n\tldr x16, [x16, :lo12:p_eglReleaseExternalContextANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglReleaseHighPowerGPUANGLE;
__asm__(".text\n.globl eglReleaseHighPowerGPUANGLE\n.type eglReleaseHighPowerGPUANGLE,%function\neglReleaseHighPowerGPUANGLE:\n\tadrp x16, p_eglReleaseHighPowerGPUANGLE\n\tldr x16, [x16, :lo12:p_eglReleaseHighPowerGPUANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglReleaseTexImage;
__asm__(".text\n.globl eglReleaseTexImage\n.type eglReleaseTexImage,%function\neglReleaseTexImage:\n\tadrp x16, p_eglReleaseTexImage\n\tldr x16, [x16, :lo12:p_eglReleaseTexImage]\n\tbr x16\n");
static __attribute__((used)) void *p_eglReleaseThread;
__asm__(".text\n.globl eglReleaseThread\n.type eglReleaseThread,%function\neglReleaseThread:\n\tadrp x16, p_eglReleaseThread\n\tldr x16, [x16, :lo12:p_eglReleaseThread]\n\tbr x16\n");
static __attribute__((used)) void *p_eglSetBlobCacheFuncsANDROID;
__asm__(".text\n.globl eglSetBlobCacheFuncsANDROID\n.type eglSetBlobCacheFuncsANDROID,%function\neglSetBlobCacheFuncsANDROID:\n\tadrp x16, p_eglSetBlobCacheFuncsANDROID\n\tldr x16, [x16, :lo12:p_eglSetBlobCacheFuncsANDROID]\n\tbr x16\n");
static __attribute__((used)) void *p_eglSetDamageRegionKHR;
__asm__(".text\n.globl eglSetDamageRegionKHR\n.type eglSetDamageRegionKHR,%function\neglSetDamageRegionKHR:\n\tadrp x16, p_eglSetDamageRegionKHR\n\tldr x16, [x16, :lo12:p_eglSetDamageRegionKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglSetValidationEnabledANGLE;
__asm__(".text\n.globl eglSetValidationEnabledANGLE\n.type eglSetValidationEnabledANGLE,%function\neglSetValidationEnabledANGLE:\n\tadrp x16, p_eglSetValidationEnabledANGLE\n\tldr x16, [x16, :lo12:p_eglSetValidationEnabledANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglSignalSyncKHR;
__asm__(".text\n.globl eglSignalSyncKHR\n.type eglSignalSyncKHR,%function\neglSignalSyncKHR:\n\tadrp x16, p_eglSignalSyncKHR\n\tldr x16, [x16, :lo12:p_eglSignalSyncKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglStreamAttribKHR;
__asm__(".text\n.globl eglStreamAttribKHR\n.type eglStreamAttribKHR,%function\neglStreamAttribKHR:\n\tadrp x16, p_eglStreamAttribKHR\n\tldr x16, [x16, :lo12:p_eglStreamAttribKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglStreamConsumerAcquireKHR;
__asm__(".text\n.globl eglStreamConsumerAcquireKHR\n.type eglStreamConsumerAcquireKHR,%function\neglStreamConsumerAcquireKHR:\n\tadrp x16, p_eglStreamConsumerAcquireKHR\n\tldr x16, [x16, :lo12:p_eglStreamConsumerAcquireKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglStreamConsumerGLTextureExternalAttribsNV;
__asm__(".text\n.globl eglStreamConsumerGLTextureExternalAttribsNV\n.type eglStreamConsumerGLTextureExternalAttribsNV,%function\neglStreamConsumerGLTextureExternalAttribsNV:\n\tadrp x16, p_eglStreamConsumerGLTextureExternalAttribsNV\n\tldr x16, [x16, :lo12:p_eglStreamConsumerGLTextureExternalAttribsNV]\n\tbr x16\n");
static __attribute__((used)) void *p_eglStreamConsumerGLTextureExternalKHR;
__asm__(".text\n.globl eglStreamConsumerGLTextureExternalKHR\n.type eglStreamConsumerGLTextureExternalKHR,%function\neglStreamConsumerGLTextureExternalKHR:\n\tadrp x16, p_eglStreamConsumerGLTextureExternalKHR\n\tldr x16, [x16, :lo12:p_eglStreamConsumerGLTextureExternalKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglStreamConsumerReleaseKHR;
__asm__(".text\n.globl eglStreamConsumerReleaseKHR\n.type eglStreamConsumerReleaseKHR,%function\neglStreamConsumerReleaseKHR:\n\tadrp x16, p_eglStreamConsumerReleaseKHR\n\tldr x16, [x16, :lo12:p_eglStreamConsumerReleaseKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglStreamPostD3DTextureANGLE;
__asm__(".text\n.globl eglStreamPostD3DTextureANGLE\n.type eglStreamPostD3DTextureANGLE,%function\neglStreamPostD3DTextureANGLE:\n\tadrp x16, p_eglStreamPostD3DTextureANGLE\n\tldr x16, [x16, :lo12:p_eglStreamPostD3DTextureANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglSurfaceAttrib;
__asm__(".text\n.globl eglSurfaceAttrib\n.type eglSurfaceAttrib,%function\neglSurfaceAttrib:\n\tadrp x16, p_eglSurfaceAttrib\n\tldr x16, [x16, :lo12:p_eglSurfaceAttrib]\n\tbr x16\n");
static __attribute__((used)) void *p_eglSwapBuffers;
__asm__(".text\n.globl eglSwapBuffers\n.type eglSwapBuffers,%function\neglSwapBuffers:\n\tadrp x16, p_eglSwapBuffers\n\tldr x16, [x16, :lo12:p_eglSwapBuffers]\n\tbr x16\n");
static __attribute__((used)) void *p_eglSwapBuffersWithDamageKHR;
__asm__(".text\n.globl eglSwapBuffersWithDamageKHR\n.type eglSwapBuffersWithDamageKHR,%function\neglSwapBuffersWithDamageKHR:\n\tadrp x16, p_eglSwapBuffersWithDamageKHR\n\tldr x16, [x16, :lo12:p_eglSwapBuffersWithDamageKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglSwapBuffersWithFrameTokenANGLE;
__asm__(".text\n.globl eglSwapBuffersWithFrameTokenANGLE\n.type eglSwapBuffersWithFrameTokenANGLE,%function\neglSwapBuffersWithFrameTokenANGLE:\n\tadrp x16, p_eglSwapBuffersWithFrameTokenANGLE\n\tldr x16, [x16, :lo12:p_eglSwapBuffersWithFrameTokenANGLE]\n\tbr x16\n");
static __attribute__((used)) void *p_eglSwapInterval;
__asm__(".text\n.globl eglSwapInterval\n.type eglSwapInterval,%function\neglSwapInterval:\n\tadrp x16, p_eglSwapInterval\n\tldr x16, [x16, :lo12:p_eglSwapInterval]\n\tbr x16\n");
static __attribute__((used)) void *p_eglTerminate;
__asm__(".text\n.globl eglTerminate\n.type eglTerminate,%function\neglTerminate:\n\tadrp x16, p_eglTerminate\n\tldr x16, [x16, :lo12:p_eglTerminate]\n\tbr x16\n");
static __attribute__((used)) void *p_eglUnlockSurfaceKHR;
__asm__(".text\n.globl eglUnlockSurfaceKHR\n.type eglUnlockSurfaceKHR,%function\neglUnlockSurfaceKHR:\n\tadrp x16, p_eglUnlockSurfaceKHR\n\tldr x16, [x16, :lo12:p_eglUnlockSurfaceKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglWaitClient;
__asm__(".text\n.globl eglWaitClient\n.type eglWaitClient,%function\neglWaitClient:\n\tadrp x16, p_eglWaitClient\n\tldr x16, [x16, :lo12:p_eglWaitClient]\n\tbr x16\n");
static __attribute__((used)) void *p_eglWaitGL;
__asm__(".text\n.globl eglWaitGL\n.type eglWaitGL,%function\neglWaitGL:\n\tadrp x16, p_eglWaitGL\n\tldr x16, [x16, :lo12:p_eglWaitGL]\n\tbr x16\n");
static __attribute__((used)) void *p_eglWaitNative;
__asm__(".text\n.globl eglWaitNative\n.type eglWaitNative,%function\neglWaitNative:\n\tadrp x16, p_eglWaitNative\n\tldr x16, [x16, :lo12:p_eglWaitNative]\n\tbr x16\n");
static __attribute__((used)) void *p_eglWaitSync;
__asm__(".text\n.globl eglWaitSync\n.type eglWaitSync,%function\neglWaitSync:\n\tadrp x16, p_eglWaitSync\n\tldr x16, [x16, :lo12:p_eglWaitSync]\n\tbr x16\n");
static __attribute__((used)) void *p_eglWaitSyncKHR;
__asm__(".text\n.globl eglWaitSyncKHR\n.type eglWaitSyncKHR,%function\neglWaitSyncKHR:\n\tadrp x16, p_eglWaitSyncKHR\n\tldr x16, [x16, :lo12:p_eglWaitSyncKHR]\n\tbr x16\n");
static __attribute__((used)) void *p_eglWaitUntilWorkScheduledANGLE;
__asm__(".text\n.globl eglWaitUntilWorkScheduledANGLE\n.type eglWaitUntilWorkScheduledANGLE,%function\neglWaitUntilWorkScheduledANGLE:\n\tadrp x16, p_eglWaitUntilWorkScheduledANGLE\n\tldr x16, [x16, :lo12:p_eglWaitUntilWorkScheduledANGLE]\n\tbr x16\n");
static struct { const char *name; void **slot; } kForwards[] = {
    {"eglAcquireExternalContextANGLE", &p_eglAcquireExternalContextANGLE},
    {"eglBindAPI", &p_eglBindAPI},
    {"eglBindTexImage", &p_eglBindTexImage},
    {"eglChooseConfig", &p_eglChooseConfig},
    {"eglClientWaitSync", &p_eglClientWaitSync},
    {"eglClientWaitSyncKHR", &p_eglClientWaitSyncKHR},
    {"eglCopyBuffers", &p_eglCopyBuffers},
    {"eglCopyMetalSharedEventANGLE", &p_eglCopyMetalSharedEventANGLE},
    {"eglCreateContext", &p_eglCreateContext},
    {"eglCreateDeviceANGLE", &p_eglCreateDeviceANGLE},
    {"eglCreateImage", &p_eglCreateImage},
    {"eglCreateImageKHR", &p_eglCreateImageKHR},
    {"eglCreateNativeClientBufferANDROID", &p_eglCreateNativeClientBufferANDROID},
    {"eglCreatePbufferFromClientBuffer", &p_eglCreatePbufferFromClientBuffer},
    {"eglCreatePbufferSurface", &p_eglCreatePbufferSurface},
    {"eglCreatePixmapSurface", &p_eglCreatePixmapSurface},
    {"eglCreatePlatformPixmapSurface", &p_eglCreatePlatformPixmapSurface},
    {"eglCreatePlatformPixmapSurfaceEXT", &p_eglCreatePlatformPixmapSurfaceEXT},
    {"eglCreatePlatformWindowSurface", &p_eglCreatePlatformWindowSurface},
    {"eglCreatePlatformWindowSurfaceEXT", &p_eglCreatePlatformWindowSurfaceEXT},
    {"eglCreateStreamKHR", &p_eglCreateStreamKHR},
    {"eglCreateStreamProducerD3DTextureANGLE", &p_eglCreateStreamProducerD3DTextureANGLE},
    {"eglCreateSync", &p_eglCreateSync},
    {"eglCreateSyncKHR", &p_eglCreateSyncKHR},
    {"eglCreateWindowSurface", &p_eglCreateWindowSurface},
    {"eglDebugMessageControlKHR", &p_eglDebugMessageControlKHR},
    {"eglDestroyContext", &p_eglDestroyContext},
    {"eglDestroyImage", &p_eglDestroyImage},
    {"eglDestroyImageKHR", &p_eglDestroyImageKHR},
    {"eglDestroyStreamKHR", &p_eglDestroyStreamKHR},
    {"eglDestroySurface", &p_eglDestroySurface},
    {"eglDestroySync", &p_eglDestroySync},
    {"eglDestroySyncKHR", &p_eglDestroySyncKHR},
    {"eglDupNativeFenceFDANDROID", &p_eglDupNativeFenceFDANDROID},
    {"eglExportVkImageANGLE", &p_eglExportVkImageANGLE},
    {"eglForceGPUSwitchANGLE", &p_eglForceGPUSwitchANGLE},
    {"eglGetCompositorTimingANDROID", &p_eglGetCompositorTimingANDROID},
    {"eglGetCompositorTimingSupportedANDROID", &p_eglGetCompositorTimingSupportedANDROID},
    {"eglGetConfigAttrib", &p_eglGetConfigAttrib},
    {"eglGetConfigs", &p_eglGetConfigs},
    {"eglGetCurrentContext", &p_eglGetCurrentContext},
    {"eglGetCurrentDisplay", &p_eglGetCurrentDisplay},
    {"eglGetCurrentSurface", &p_eglGetCurrentSurface},
    {"eglGetError", &p_eglGetError},
    {"eglGetFrameTimestampsANDROID", &p_eglGetFrameTimestampsANDROID},
    {"eglGetFrameTimestampSupportedANDROID", &p_eglGetFrameTimestampSupportedANDROID},
    {"eglGetMscRateANGLE", &p_eglGetMscRateANGLE},
    {"eglGetNativeClientBufferANDROID", &p_eglGetNativeClientBufferANDROID},
    {"eglGetNextFrameIdANDROID", &p_eglGetNextFrameIdANDROID},
    {"eglGetSyncAttrib", &p_eglGetSyncAttrib},
    {"eglGetSyncAttribKHR", &p_eglGetSyncAttribKHR},
    {"eglGetSyncValuesCHROMIUM", &p_eglGetSyncValuesCHROMIUM},
    {"eglHandleGPUSwitchANGLE", &p_eglHandleGPUSwitchANGLE},
    {"eglInitialize", &p_eglInitialize},
    {"eglLabelObjectKHR", &p_eglLabelObjectKHR},
    {"eglLockSurfaceKHR", &p_eglLockSurfaceKHR},
    {"eglMakeCurrent", &p_eglMakeCurrent},
    {"eglPostSubBufferNV", &p_eglPostSubBufferNV},
    {"eglPrepareSwapBuffersANGLE", &p_eglPrepareSwapBuffersANGLE},
    {"eglPresentationTimeANDROID", &p_eglPresentationTimeANDROID},
    {"eglProgramCacheGetAttribANGLE", &p_eglProgramCacheGetAttribANGLE},
    {"eglProgramCachePopulateANGLE", &p_eglProgramCachePopulateANGLE},
    {"eglProgramCacheQueryANGLE", &p_eglProgramCacheQueryANGLE},
    {"eglProgramCacheResizeANGLE", &p_eglProgramCacheResizeANGLE},
    {"eglQueryAPI", &p_eglQueryAPI},
    {"eglQueryContext", &p_eglQueryContext},
    {"eglQueryDebugKHR", &p_eglQueryDebugKHR},
    {"eglQueryDeviceAttribEXT", &p_eglQueryDeviceAttribEXT},
    {"eglQueryDeviceStringEXT", &p_eglQueryDeviceStringEXT},
    {"eglQueryDisplayAttribANGLE", &p_eglQueryDisplayAttribANGLE},
    {"eglQueryDisplayAttribEXT", &p_eglQueryDisplayAttribEXT},
    {"eglQueryDmaBufFormatsEXT", &p_eglQueryDmaBufFormatsEXT},
    {"eglQueryDmaBufModifiersEXT", &p_eglQueryDmaBufModifiersEXT},
    {"eglQueryStreamKHR", &p_eglQueryStreamKHR},
    {"eglQueryStreamu64KHR", &p_eglQueryStreamu64KHR},
    {"eglQueryStringiANGLE", &p_eglQueryStringiANGLE},
    {"eglQuerySurface", &p_eglQuerySurface},
    {"eglQuerySurface64KHR", &p_eglQuerySurface64KHR},
    {"eglQuerySurfacePointerANGLE", &p_eglQuerySurfacePointerANGLE},
    {"eglReacquireHighPowerGPUANGLE", &p_eglReacquireHighPowerGPUANGLE},
    {"eglReleaseDeviceANGLE", &p_eglReleaseDeviceANGLE},
    {"eglReleaseExternalContextANGLE", &p_eglReleaseExternalContextANGLE},
    {"eglReleaseHighPowerGPUANGLE", &p_eglReleaseHighPowerGPUANGLE},
    {"eglReleaseTexImage", &p_eglReleaseTexImage},
    {"eglReleaseThread", &p_eglReleaseThread},
    {"eglSetBlobCacheFuncsANDROID", &p_eglSetBlobCacheFuncsANDROID},
    {"eglSetDamageRegionKHR", &p_eglSetDamageRegionKHR},
    {"eglSetValidationEnabledANGLE", &p_eglSetValidationEnabledANGLE},
    {"eglSignalSyncKHR", &p_eglSignalSyncKHR},
    {"eglStreamAttribKHR", &p_eglStreamAttribKHR},
    {"eglStreamConsumerAcquireKHR", &p_eglStreamConsumerAcquireKHR},
    {"eglStreamConsumerGLTextureExternalAttribsNV", &p_eglStreamConsumerGLTextureExternalAttribsNV},
    {"eglStreamConsumerGLTextureExternalKHR", &p_eglStreamConsumerGLTextureExternalKHR},
    {"eglStreamConsumerReleaseKHR", &p_eglStreamConsumerReleaseKHR},
    {"eglStreamPostD3DTextureANGLE", &p_eglStreamPostD3DTextureANGLE},
    {"eglSurfaceAttrib", &p_eglSurfaceAttrib},
    {"eglSwapBuffers", &p_eglSwapBuffers},
    {"eglSwapBuffersWithDamageKHR", &p_eglSwapBuffersWithDamageKHR},
    {"eglSwapBuffersWithFrameTokenANGLE", &p_eglSwapBuffersWithFrameTokenANGLE},
    {"eglSwapInterval", &p_eglSwapInterval},
    {"eglTerminate", &p_eglTerminate},
    {"eglUnlockSurfaceKHR", &p_eglUnlockSurfaceKHR},
    {"eglWaitClient", &p_eglWaitClient},
    {"eglWaitGL", &p_eglWaitGL},
    {"eglWaitNative", &p_eglWaitNative},
    {"eglWaitSync", &p_eglWaitSync},
    {"eglWaitSyncKHR", &p_eglWaitSyncKHR},
    {"eglWaitUntilWorkScheduledANGLE", &p_eglWaitUntilWorkScheduledANGLE},
};



static void log_attribs(const char *fn, unsigned platform, void *native, void *attribs, int attrib_is_int) {
    char buf[400];
    int n = snprintf(buf, sizeof buf, "%s(platform=%#x native=%p attribs=%p:", fn, platform, native, attribs);
    int fd = open("/tmp/ddcef-egl.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) { write(fd, "[eglfront] ", 11); write(fd, buf, n); }
    if (attribs) {
        char v[200];
        int m = 0;
        for (int i = 0; i < 8; i++) {
            long long val = attrib_is_int
                ? (long long)((int *)attribs)[i]
                : ((long long *)attribs)[i];
            if (attrib_is_int ? val == 0x3038 : val == 0x3038) { m += snprintf(v+m, sizeof v-m, " NONE"); break; }
            m += snprintf(v+m, sizeof v-m, " %#llx", val);
        }
        if (fd >= 0) write(fd, v, m);
    }
    if (fd >= 0) { write(fd, ")\n", 2); close(fd); }
}

void *eglGetDisplay(void *native_display) {
    log_attribs("eglGetDisplay", 0, native_display, NULL, 0);
    static void *(*real)(void *);
    if (!real) real = dlsym(real_handle, "eglGetDisplay");
    return real ? real(native_display) : NULL;
}

void *eglGetPlatformDisplay(unsigned platform, void *native_display, long long *attribs) {
    log_attribs("eglGetPlatformDisplay", platform, native_display, attribs, 0);
    static void *(*real)(unsigned, void *, long long *);
    if (!real) real = dlsym(real_handle, "eglGetPlatformDisplay");
    return real ? real(platform, native_display, attribs) : NULL;
}

void *eglGetPlatformDisplayEXT(unsigned platform, void *native_display, int *attribs) {
    log_attribs("eglGetPlatformDisplayEXT", platform, native_display, attribs, 1);
    static void *(*real)(unsigned, void *, int *);
    if (!real) real = dlsym(real_handle, "eglGetPlatformDisplayEXT");
    return real ? real(platform, native_display, attribs) : NULL;
}

void *eglGetProcAddress(const char *name) {
    if (name && !strcmp(name, "eglQueryString")) return (void *)&eglQueryString;
    if (name && !strcmp(name, "eglGetDisplay")) return (void *)&eglGetDisplay;
    if (name && !strcmp(name, "eglGetPlatformDisplay")) return (void *)&eglGetPlatformDisplay;
    if (name && !strcmp(name, "eglGetPlatformDisplayEXT")) return (void *)&eglGetPlatformDisplayEXT;
    if (!real_handle) return NULL;
    static void *(*real)(const char *);
    if (!real) real = dlsym(real_handle, "eglGetProcAddress");
    return real ? real(name) : NULL;
}

__attribute__((constructor))
static void eglfront_bind(void) {
    if (!real_handle) return;
    for (unsigned i = 0; i < sizeof kForwards / sizeof kForwards[0]; i++) {
        *kForwards[i].slot = dlsym(real_handle, kForwards[i].name);
        if (!*kForwards[i].slot)
            logline("WARN: real lib lacks %s", kForwards[i].name);
    }
}
