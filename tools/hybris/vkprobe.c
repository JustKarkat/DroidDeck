// Headless Vulkan probe over libhybris (pairs with eglprobe.c): proves the
// Shield's bionic Vulkan stack (Android loader + vulkan.tegra.so ICD) works
// from the glibc guest with the Halium-patched bionic. Links the hybris
// wrapper's loader API and fetches device functions dynamically, so only the
// wrapper's exported entrypoints matter. Ends with a GPU fill-buffer submit.
#define _GNU_SOURCE
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <hybris/common/binding.h>

/* The wrapper is opened with dlopen in main (see below), so rebind the
 * entrypoints we use to dlsym'd pointers while keeping the call sites
 * reading like ordinary Vulkan code. */
#define vkCreateInstance p_vkCreateInstance
#define vkGetInstanceProcAddr p_vkGetInstanceProcAddr
#define vkEnumerateInstanceExtensionProperties p_vkEnumerateInstanceExtensionProperties
#define vkEnumeratePhysicalDevices p_vkEnumeratePhysicalDevices
#define vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties
#define vkGetPhysicalDeviceQueueFamilyProperties p_vkGetPhysicalDeviceQueueFamilyProperties
#define vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties
#define vkEnumerateDeviceExtensionProperties p_vkEnumerateDeviceExtensionProperties
#define vkCreateDevice p_vkCreateDevice
#define vkGetDeviceProcAddr p_vkGetDeviceProcAddr
#define vkDestroyInstance p_vkDestroyInstance

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
            if (strstr(line, "r-xp") || strstr(line, "/usr/local/hybris") ||
                strstr(line, "vulkan"))
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

static const char *wanted_extensions[] = {
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
    "VK_KHR_external_semaphore_fd",
    "VK_EXT_external_memory_host",
    "VK_ANDROID_external_memory_android_hardware_buffer",
    "VK_KHR_timeline_semaphore",
    "VK_KHR_push_descriptor",
    "VK_EXT_descriptor_indexing",
    "VK_KHR_dedicated_allocation",
};

/* Stable since Android HAL v0; avoid hunting for the exact android-headers
 * layout in the build. */
struct hw_module_methods_t;
struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version, hal_api_version;
    const char *id, *name, *author;
    struct hw_module_methods_t *methods;
    void *dso;
};
struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *module, const char *id, void **device);
};

/* hardware/hwvulkan.h — the device carries all three entrypoints; the
 * tegra HAL fills them via packed (APS2) relocations at load. */
struct hwvulkan_device_t {
    struct hw_device_t {
        uint32_t tag;
        uint32_t version;
        const struct hw_module_t *module;
        int (*close)(void *);
    } common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
};

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    install();
    say("vkstage: start\n");

    /* The hybris wrapper resolves its entrypoints via IFUNC, whose resolvers
     * run inside ld.so's relocation phase when linked at startup — that
     * crashed before main. Open it explicitly instead and bind by dlsym. */
    static PFN_vkCreateInstance vkCreateInstance;
    static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    static PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties;
    static PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
    static PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
    static PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
    static PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
    static PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
    static PFN_vkCreateDevice vkCreateDevice;
    static PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
    static PFN_vkDestroyInstance vkDestroyInstance;
    void *libvulkan = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    printf("vkstage: dlopen libvulkan.so.1 -> %p (%s)\n", libvulkan,
           libvulkan ? "ok (hybris wrapper)" : (dlerror() ? dlerror() : "failed"));
    if (!libvulkan) return 1;
#define BIND(sym) sym = (PFN_##sym)dlsym(libvulkan, #sym); \
    if (!sym) { printf("vkstage: dlsym %s failed\n", #sym); return 1; }
    BIND(vkCreateInstance); BIND(vkGetInstanceProcAddr);
    BIND(vkEnumerateInstanceExtensionProperties); BIND(vkEnumeratePhysicalDevices);
    BIND(vkGetPhysicalDeviceProperties); BIND(vkGetPhysicalDeviceQueueFamilyProperties);
    BIND(vkGetPhysicalDeviceMemoryProperties); BIND(vkEnumerateDeviceExtensionProperties);
    BIND(vkCreateDevice); BIND(vkGetDeviceProcAddr); BIND(vkDestroyInstance);
#undef BIND

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "droiddeck-vkprobe",
                              .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app };

    /* Separate "the loader's file discovery failed" from "the ICD itself
     * cannot load": try the tegra ICD directly through the hybris linker. */
    {
        void *icd = android_dlopen("/vendor/lib64/hw/vulkan.tegra.so", RTLD_NOW | RTLD_LOCAL);
        printf("vkstage: direct ICD dlopen -> %p (%s)\n", icd,
               icd ? "ok" : (android_dlerror() ? android_dlerror() : "failed"));
        if (icd) {
            void *hmi = android_dlsym(icd, "HMI");
            printf("vkstage: ICD HMI symbol -> %p\n", hmi);
        }
        /* And test the loader's own discovery pieces: bionic access() on the
         * ICD path, then the exact hw_get_module the loader calls. */
        void *libc = android_dlopen("libc.so", RTLD_NOW);
        int (*baccess)(const char *, int) =
            libc ? (int (*)(const char *, int))android_dlsym(libc, "access") : NULL;
        if (baccess)
            printf("vkstage: bionic access(vendor ICD, R_OK) -> %d\n",
                   baccess("/vendor/lib64/hw/vulkan.tegra.so", 4));
        void *lhw = android_dlopen("libhardware.so", RTLD_NOW);
        int (*hgm)(const char *, void **) =
            lhw ? (int (*)(const char *, void **))android_dlsym(lhw, "hw_get_module") : NULL;
        const struct hw_module_t *hmod = NULL;
        if (hgm) {
            void *mod = NULL;
            int hrc = hgm("vulkan", &mod);
            hmod = (const struct hw_module_t *)mod;
            printf("vkstage: hw_get_module(vulkan) -> %d module=%p %s\n",
                   hrc, mod, hrc == 0 ? "ok" : "FAILED (this is the loader's discovery)");
        } else {
            printf("vkstage: hw_get_module unavailable (%s)\n",
                   android_dlerror() ? android_dlerror() : "?");
        }
        /* The loader's next step after hw_get_module: open the HAL device.
         * The device id is "vk0" (HWVULKAN_DEVICE_0); open() strcmps it and
         * returns -ENOENT for anything else. NVRM init happens in there. */
        if (hmod && hmod->methods && hmod->methods->open) {
            void *hdev = NULL;
            int orc = hmod->methods->open(hmod, "vk0", &hdev);
            printf("vkstage: HAL open(vk0) -> %d device=%p\n", orc, hdev);
            if (orc != 0)
                printf("vkstage: HAL open dlerror: %s\n",
                       android_dlerror() ? android_dlerror() : "(none)");
            /* Drive the driver bare — this is what sysvk hands to the loader. */
            if (orc == 0 && hdev) {
                struct hwvulkan_device_t *vdev = (struct hwvulkan_device_t *)hdev;
                printf("vkstage: device procs: EnumerateInstanceExt=%p CreateInstance=%p "
                       "GetInstanceProcAddr=%p\n",
                       (void *)vdev->EnumerateInstanceExtensionProperties,
                       (void *)vdev->CreateInstance, (void *)vdev->GetInstanceProcAddr);
                PFN_vkGetInstanceProcAddr dev_gipa = vdev->GetInstanceProcAddr;
                if (dev_gipa) {
                    PFN_vkEnumerateInstanceExtensionProperties dep =
                        (PFN_vkEnumerateInstanceExtensionProperties)dev_gipa(
                            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
                    uint32_t nx = 0;
                    VkResult dr =
                        dep ? dep(VK_NULL_HANDLE, &nx, NULL) : VK_ERROR_INITIALIZATION_FAILED;
                    printf("vkstage: driver vkEnumerateInstanceExtensionProperties -> %d count=%u\n",
                           dr, nx);
                    if (dr == VK_SUCCESS && nx > 0) {
                        VkExtensionProperties ex[48] = { 0 };
                        if (nx > 48) nx = 48;
                        dep(VK_NULL_HANDLE, &nx, ex);
                        printf("vkstage: driver instance extensions:");
                        for (uint32_t i = 0; i < nx; i++) printf(" %s", ex[i].extensionName);
                        printf("\n");
                    }
                    PFN_vkCreateInstance dci =
                        (PFN_vkCreateInstance)dev_gipa(VK_NULL_HANDLE, "vkCreateInstance");
                    VkInstance inst2 = VK_NULL_HANDLE;
                    if (dci && dci(&ici, NULL, &inst2) == VK_SUCCESS) {
                        PFN_vkEnumeratePhysicalDevices depd = (PFN_vkEnumeratePhysicalDevices)dev_gipa(
                            inst2, "vkEnumeratePhysicalDevices");
                        PFN_vkGetPhysicalDeviceProperties dpp = (PFN_vkGetPhysicalDeviceProperties)dev_gipa(
                            inst2, "vkGetPhysicalDeviceProperties");
                        uint32_t np = 0;
                        VkResult pr = depd ? depd(inst2, &np, NULL) : VK_ERROR_INITIALIZATION_FAILED;
                        printf("vkstage: driver vkEnumeratePhysicalDevices -> %d count=%u\n", pr, np);
                        if (pr == VK_SUCCESS && np > 0) {
                            VkPhysicalDevice pd = VK_NULL_HANDLE;
                            depd(inst2, &np, &pd);
                            VkPhysicalDeviceProperties pp = { 0 };
                            dpp(pd, &pp);
                            printf("RESULT VK_DIRECT_DEVICE=%s api=%u.%u.%u driver=0x%x\n",
                                   pp.deviceName, VK_VERSION_MAJOR(pp.apiVersion),
                                   VK_VERSION_MINOR(pp.apiVersion), VK_VERSION_PATCH(pp.apiVersion),
                                   pp.driverVersion);
                        }
                    }
                }
            }
        }
        /* The decisive bypass test: drive the ICD like a bare ICD (no Android
         * loader). If this enumerates, a direct-ICD shim can feed gamescope. */
        PFN_vkGetInstanceProcAddr icd_gipa =
            icd ? (PFN_vkGetInstanceProcAddr)android_dlsym(icd, "vkGetInstanceProcAddr") : NULL;
        if (icd_gipa) {
            PFN_vkCreateInstance icd_ci =
                (PFN_vkCreateInstance)icd_gipa(VK_NULL_HANDLE, "vkCreateInstance");
            printf("vkstage: ICD vkGetInstanceProcAddr -> %p\n", (void *)icd_ci);
            VkInstance inst = VK_NULL_HANDLE;
            VkResult dir = icd_ci ? icd_ci(&ici, NULL, &inst) : VK_ERROR_INITIALIZATION_FAILED;
            printf("vkstage: direct ICD vkCreateInstance -> %d\n", dir);
            if (dir == VK_SUCCESS) {
                PFN_vkEnumeratePhysicalDevices icd_epd =
                    (PFN_vkEnumeratePhysicalDevices)icd_gipa(inst, "vkEnumeratePhysicalDevices");
                PFN_vkGetPhysicalDeviceProperties icd_gpp =
                    (PFN_vkGetPhysicalDeviceProperties)icd_gipa(inst, "vkGetPhysicalDeviceProperties");
                PFN_vkEnumerateDeviceExtensionProperties icd_edx =
                    (PFN_vkEnumerateDeviceExtensionProperties)icd_gipa(
                        inst, "vkEnumerateDeviceExtensionProperties");
                uint32_t n = 0;
                dir = icd_epd(inst, &n, NULL);
                printf("vkstage: direct vkEnumeratePhysicalDevices -> %d count=%u\n", dir, n);
                if (dir == VK_SUCCESS && n > 0) {
                    VkPhysicalDevice pd = VK_NULL_HANDLE;
                    icd_epd(inst, &n, &pd);
                    VkPhysicalDeviceProperties p = { 0 };
                    icd_gpp(pd, &p);
                    printf("RESULT VK_DIRECT_DEVICE=%s api=%u.%u.%u driver=0x%x\n", p.deviceName,
                           VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
                           VK_VERSION_PATCH(p.apiVersion), p.driverVersion);
                    if (icd_edx) {
                        uint32_t ndx = 0;
                        icd_edx(pd, NULL, &ndx, NULL);
                        VkExtensionProperties *dx = calloc(ndx ? ndx : 1, sizeof *dx);
                        if (dx && icd_edx(pd, NULL, &ndx, dx) == VK_SUCCESS) {
                            for (size_t w = 0;
                                 w < sizeof wanted_extensions / sizeof wanted_extensions[0];
                                 w++) {
                                int found = 0;
                                for (uint32_t i = 0; i < ndx; i++)
                                    if (!strcmp(dx[i].extensionName, wanted_extensions[w]))
                                        found = 1;
                                printf("RESULT VK_DIRECT_EXT %s = %s\n", wanted_extensions[w],
                                       found ? "yes" : "NO");
                            }
                        }
                        free(dx);
                    }
                }
            }
        } else {
            printf("vkstage: ICD does not export vkGetInstanceProcAddr\n");
        }
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, NULL, &instance);
    printf("vkstage: vkCreateInstance -> %d\n", r);
    if (r != VK_SUCCESS) return 1;

    PFN_vkGetInstanceProcAddr gipa = vkGetInstanceProcAddr;

    uint32_t nix = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &nix, NULL);
    VkExtensionProperties ix[64] = { 0 };
    if (nix > 64) nix = 64;
    vkEnumerateInstanceExtensionProperties(NULL, &nix, ix);
    printf("vkstage: instance extensions=%u:", nix);
    for (uint32_t i = 0; i < nix; i++) printf(" %s", ix[i].extensionName);
    printf("\n");

    uint32_t ndev = 0;
    r = vkEnumeratePhysicalDevices(instance, &ndev, NULL);
    printf("vkstage: vkEnumeratePhysicalDevices -> %d count=%u\n", r, ndev);
    if (r != VK_SUCCESS || ndev == 0) return 1;
    VkPhysicalDevice devs[8] = { 0 };
    if (ndev > 8) ndev = 8;
    vkEnumeratePhysicalDevices(instance, &ndev, devs);

    VkPhysicalDeviceProperties props = { 0 };
    vkGetPhysicalDeviceProperties(devs[0], &props);
    printf("RESULT VK_DEVICE=%s\n", props.deviceName);
    printf("RESULT VK_API=%u.%u.%u driver=0x%x (%u.%u.%u nvidia-style %u.%u.%u)\n",
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion), props.driverVersion,
           VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion),
           VK_VERSION_PATCH(props.driverVersion),
           props.driverVersion >> 22, (props.driverVersion >> 14) & 0xff,
           props.driverVersion & 0x3fff);
    printf("vkstage: vendorID=0x%x deviceID=0x%x type=%d\n",
           props.vendorID, props.deviceID, props.deviceType);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devs[0], &nq, NULL);
    printf("vkstage: queue families=%u\n", nq);

    VkPhysicalDeviceMemoryProperties mem = { 0 };
    vkGetPhysicalDeviceMemoryProperties(devs[0], &mem);
    printf("vkstage: memory types=%u heaps=%u:", mem.memoryTypeCount, mem.memoryHeapCount);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
        printf(" %d:0x%x", i, mem.memoryTypes[i].propertyFlags);
    printf("\n");

    uint32_t ndx = 0;
    vkEnumerateDeviceExtensionProperties(devs[0], NULL, &ndx, NULL);
    VkExtensionProperties dx[256] = { 0 };
    if (ndx > 256) ndx = 256;
    vkEnumerateDeviceExtensionProperties(devs[0], NULL, &ndx, dx);
    printf("vkstage: device extensions=%u\n", ndx);
    for (size_t w = 0; w < sizeof wanted_extensions / sizeof wanted_extensions[0]; w++) {
        int found = 0;
        for (uint32_t i = 0; i < ndx; i++)
            if (!strcmp(dx[i].extensionName, wanted_extensions[w])) found = 1;
        printf("RESULT VK_EXT %s = %s\n", wanted_extensions[w], found ? "yes" : "NO");
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo q = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &q };
    VkDevice device = VK_NULL_HANDLE;
    r = vkCreateDevice(devs[0], &dci, NULL, &device);
    printf("vkstage: vkCreateDevice -> %d\n", r);
    if (r != VK_SUCCESS) return 1;

    PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
#define GET(name) PFN_##name name = (PFN_##name)gdpa(device, #name)
    GET(vkGetDeviceQueue); GET(vkCreateCommandPool); GET(vkAllocateCommandBuffers);
    GET(vkBeginCommandBuffer); GET(vkCmdFillBuffer); GET(vkEndCommandBuffer);
    GET(vkQueueSubmit); GET(vkQueueWaitIdle); GET(vkCreateBuffer);
    GET(vkGetBufferMemoryRequirements); GET(vkAllocateMemory); GET(vkBindBufferMemory);
    GET(vkMapMemory); GET(vkDestroyBuffer); GET(vkFreeMemory);
    GET(vkDestroyCommandPool); GET(vkDestroyDevice);
#undef GET

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buf = VK_NULL_HANDLE;
    r = vkCreateBuffer(device, &bci, NULL, &buf);
    printf("vkstage: vkCreateBuffer -> %d\n", r);
    if (r != VK_SUCCESS) return 1;

    VkMemoryRequirements req = { 0 };
    vkGetBufferMemoryRequirements(device, buf, &req);
    int mem_type = -1;
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mem_type = (int)i;
            break;
        }
    }
    printf("vkstage: buffer req size=%lu align=%lu types=0x%x host-visible type=%d\n",
           (unsigned long)req.size, (unsigned long)req.alignment, req.memoryTypeBits, mem_type);
    if (mem_type < 0) return 1;

    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mem_type };
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    r = vkAllocateMemory(device, &mai, NULL, &vmem);
    printf("vkstage: vkAllocateMemory -> %d\n", r);
    if (r != VK_SUCCESS) return 1;
    if ((r = vkBindBufferMemory(device, buf, vmem, 0)) != VK_SUCCESS) {
        printf("vkstage: vkBindBufferMemory -> %d\n", r);
        return 1;
    }

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .queueFamilyIndex = 0 };
    VkCommandPool pool = VK_NULL_HANDLE;
    if ((r = vkCreateCommandPool(device, &pci, NULL, &pool)) != VK_SUCCESS) {
        printf("vkstage: vkCreateCommandPool -> %d\n", r);
        return 1;
    }
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if ((r = vkAllocateCommandBuffers(device, &cai, &cmd)) != VK_SUCCESS) {
        printf("vkstage: vkAllocateCommandBuffers -> %d\n", r);
        return 1;
    }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cmd, &bi);
    vkCmdFillBuffer(cmd, buf, 0, VK_WHOLE_SIZE, 0x5a5aa5a5u);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                        .pCommandBuffers = &cmd };
    r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    printf("vkstage: vkQueueSubmit -> %d\n", r);
    if (r != VK_SUCCESS) return 1;
    vkQueueWaitIdle(queue);

    void *mapped = NULL;
    if ((r = vkMapMemory(device, vmem, 0, VK_WHOLE_SIZE, 0, &mapped)) != VK_SUCCESS) {
        printf("vkstage: vkMapMemory -> %d\n", r);
        return 1;
    }
    uint32_t *words = (uint32_t *)mapped;
    int pass = 1;
    for (int i = 0; i < 16; i++)
        if (words[i] != 0x5a5aa5a5u) pass = 0;
    printf("RESULT VK_FILLTEST=%s first=%08x\n", pass ? "PASS" : "FAIL", words[0]);
    printf("RESULT VK_STACK=hybris-bionic (no FAULT above = loader+ICD survived TLS)\n");

    vkDestroyBuffer(device, buf, NULL);
    vkFreeMemory(device, vmem, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("vkstage: teardown clean\n");
    return 0;
}
