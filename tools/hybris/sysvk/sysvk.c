/* sysvk — a Vulkan ICD that drives the NVIDIA Tegra driver over the
 * "driver table" protocol (https://github.com/xMeM/sysvk, unlicensed;
 * ShieldDeck rewrite, session 2026-10-26-12).
 *
 * The tegra driver (libglcore, Shield firmware 33.01 / 495.00) is not
 * reachable through the hwvulkan HAL: the device struct ships all-NULL
 * entrypoints and the HAL's CreateInstance wrapper depends on driver-internal
 * state only the consumer init installs. What works (proven by tools/hybris/
 * vkprobe.c on device): after the EGL init chain has run in-process,
 *   1. NvGlEglGetFunctions(1, &table[1]) with table[0] = the dlopen handle
 *      fills table[1] = vkGetProcAddressNV(name) (48-byte table),
 *   2. the consumer stores table[4] = GetProcAddressNV("vkGetInstanceProcAddr")
 *      — the REAL driver GIPA, conformant with NULL instances (the HAL's
 *      wrapper is not),
 *   3. global queries go through that GIPA with instance == VK_NULL_HANDLE,
 *   4. vkCreateInstance REQUIRES a non-NULL VkAllocationCallbacks
 *      (pfnAllocation; NULL falls back to an unset process-global in
 *      libglcore) — we substitute glibc-backed callbacks when the loader
 *      passes NULL, and keep them for the matching destroy calls so
 *      allocation/free pairs stay on one heap.
 * The EGL init chain (the one libEGL_tegra runs via NvGlEglApiInit) is what
 * installs libglcore's dispatch master object + singleton allocators that
 * every CreateInstance call needs; see tools/hybris/egl_warmup.h.
 *
 * Build (tools/hybris/build-in-arch.sh):
 *   gcc -shared -fPIC sysvk.c -I<xMeM sysvk includes> -I<hybris root include> \
 *     -lhardware -lhybris-common -lEGL
 */
#include <dlfcn.h>
#include <hardware/hardware.h>
#include <hardware/hwvulkan.h>
#include <hybris/common/binding.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "../egl_warmup.h"

/* Driver table: [0] = dlopen handle (consumer-stored), [1] = GetProcAddressNV,
 * [4] = vkGetInstanceProcAddr (consumer-stored; the driver calls through it
 * internally, so leaving it NULL faults — see vkprobe.c). */
static void *table[6];
static int inited;

/* glibc-backed callbacks, substituted when the caller passes no allocator.
 * The driver dereferences pAllocator unconditionally, so NULL is fatal. */
static void *sk_alloc(void *ud, size_t size, size_t align, VkSystemAllocationScope scope) {
    (void)ud; (void)scope;
    void *p = NULL;
    if (posix_memalign(&p, align < 8 ? 8 : align, size) != 0) return NULL;
    return p;
}
/* shortcut: copies `size` bytes from the original (real size unknown),
 * only if a realloc-happy driver ever requests one; upgrade to a size map
 * if data corruption shows up */
static void *sk_realloc(void *ud, void *orig, size_t size, size_t align,
                        VkSystemAllocationScope scope) {
    void *p = sk_alloc(ud, size, align, scope);
    if (p && orig) memcpy(p, orig, size);
    free(orig);
    return p;
}
static void sk_free(void *ud, void *p) { (void)ud; free(p); }
static void sk_notify(void *ud, size_t size, VkInternalAllocationType t,
                      VkSystemAllocationScope s) {
    (void)ud; (void)size; (void)t; (void)s;
}

static const VkAllocationCallbacks sk_cb = {
    .pUserData = NULL,
    .pfnAllocation = sk_alloc,
    .pfnReallocation = sk_realloc,
    .pfnFree = sk_free,
    .pfnInternalAllocation = sk_notify,
    .pfnInternalFree = sk_notify,
};
#define ALLOC_OR(a) ((const VkAllocationCallbacks *)(a) ? (a) : &sk_cb)

static PFN_vkCreateInstance real_ci;
static PFN_vkGetInstanceProcAddr real_gipa;

/* The hybris EGL wrapper dlopens its platform backend (…/libhybris/
 * eglplatform_null.so) by absolute pkglibdir path, but that plugin's soname
 * deps (libhybris-eglplatformcommon.so.1 -> libhybris-common.so.1) only
 * resolve via LD_LIBRARY_PATH — which vulkaninfo/gamescope must NOT have (it
 * would shadow the guest's Mesa loader). Preload the chain by absolute path:
 * an already-loaded soname satisfies later loads. The pkglibdir symlink is
 * normally created by the probe wrapper; recreate it on fresh installs. */
static void preload_hybris_egl_chain(void) {
    const char *lib = "/usr/local/hybris/usr/lib/";
    if (access("/usr/lib/libhybris", F_OK) != 0)
        symlink(lib, "/usr/lib/libhybris");
    dlopen("/usr/local/hybris/usr/lib/libhybris-common.so.1", RTLD_NOW | RTLD_GLOBAL);
    dlopen("/usr/local/hybris/usr/lib/libhybris-eglplatformcommon.so.1",
           RTLD_NOW | RTLD_GLOBAL);
}

/* The full consumer init, in the order vkprobe proved on device: EGL chain
 * (installs the driver singletons) -> HAL device open -> driver table. */
static int sysvk_init_once(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (!inited) {
        preload_hybris_egl_chain();
        egl_warmup("sysvk");
        void *lhw = android_dlopen("libhardware.so", RTLD_NOW);
        int (*hgm)(const char *, void **) =
            lhw ? (int (*)(const char *, void **))android_dlsym(lhw, "hw_get_module") : NULL;
        const struct hw_module_t *mod = NULL;
        if (hgm && hgm(HWVULKAN_HARDWARE_MODULE_ID, (void **)&mod) == 0 && mod &&
            mod->methods && mod->methods->open) {
            struct hw_device_t *dev = NULL;
            int rc = mod->methods->open(mod, HWVULKAN_DEVICE_0, &dev);
            fprintf(stderr, "sysvk: HAL open(vk0) -> %d\n", rc);
        } else {
            fprintf(stderr, "sysvk: hw_get_module/open(vk0) unavailable\n");
        }
        void *glc = android_dlopen("libglcore.so", RTLD_NOW | RTLD_LOCAL);
        int (*nvgl)(int, void *) =
            glc ? (int (*)(int, void *))android_dlsym(glc, "NvGlEglGetFunctions") : NULL;
        if (glc && nvgl) {
            memset(table, 0, sizeof table);
            table[0] = glc;
            nvgl(1, &table[1]);
            if (table[1])
                table[4] = ((void *(*)(const char *))table[1])("vkGetInstanceProcAddr");
        }
        real_gipa = (PFN_vkGetInstanceProcAddr)table[4];
        if (real_gipa) {
            real_ci = (PFN_vkCreateInstance)real_gipa(VK_NULL_HANDLE, "vkCreateInstance");
            fprintf(stderr, "sysvk: table ready gipa=%p ci=%p\n", (void *)real_gipa,
                    (void *)real_ci);
        } else {
            fprintf(stderr, "sysvk: driver table init FAILED\n");
        }
        inited = real_gipa != NULL;
    }
    pthread_mutex_unlock(&mu);
    return inited;
}

/* The loader passes no allocator; the driver needs one. */
static VKAPI_ATTR VkResult VKAPI_CALL
sk_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *ac,
                  VkInstance *inst) {
    if (!sysvk_init_once() || !real_ci) return VK_ERROR_INITIALIZATION_FAILED;
    return real_ci(ci, ALLOC_OR(ac), inst);
}
static VKAPI_ATTR void VKAPI_CALL
sk_DestroyInstance(VkInstance inst, const VkAllocationCallbacks *ac) {
    PFN_vkDestroyInstance f =
        (PFN_vkDestroyInstance)(real_gipa ? real_gipa(inst, "vkDestroyInstance") : NULL);
    if (f) f(inst, ALLOC_OR(ac));
}
/* shortcut: vkCreateDevice/vkDestroyDevice pass the caller's allocator through
 * unwrapped — the allocator requirement is only proven for CreateInstance.
 * If device creation faults on a NULL allocator in gamescope, wrap them here
 * (needs a pd→instance registry, since gipa needs the owning instance). */

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (!sysvk_init_once() || !real_gipa)
        return NULL;
    /* Create/destroy pairs get the allocator-substituting wrappers; the
     * driver must alloc and free through the same callbacks. */
    if (!strcmp(pName, "vkCreateInstance"))
        return (PFN_vkVoidFunction)sk_CreateInstance;
    if (instance && !strcmp(pName, "vkDestroyInstance"))
        return (PFN_vkVoidFunction)sk_DestroyInstance;
    return real_gipa(instance, pName);
}

PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(VkInstance _instance, const char *pName) {
    (void)_instance;
    (void)pName;
    return NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion) {
#define MIN(i, j) (((i) < (j)) ? (i) : (j))
    *pSupportedVersion = MIN(*pSupportedVersion, 5u);
    return VK_SUCCESS;
}
