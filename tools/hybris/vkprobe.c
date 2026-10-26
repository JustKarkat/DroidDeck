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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    install();
    say("vkstage: start\n");

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "droiddeck-vkprobe",
                              .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app };
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
