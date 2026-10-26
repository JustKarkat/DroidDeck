/* sysvk — a Vulkan ICD that drives the Android hwvulkan HAL directly
 * (https://github.com/xMeM/sysvk, unlicensed). ShieldDeck fork: the NVIDIA
 * Tegra HAL on Shield Android 11 (495.00) is not loader-conformant —
 * GetInstanceProcAddr crashes on a NULL instance, and the hwvulkan_device_t
 * ships with all three entrypoints NULL (empty ANDROID_RELA, atexit-only
 * constructor; NVIDIA's own loader uses the device-struct fields for
 * global-level queries instead). So: fill the fields if the HAL left them
 * NULL (offsets are for vulkan.tegra.so from Shield firmware 33.01, stable
 * there), and answer NULL-instance queries from the struct rather than
 * forwarding into the HAL. For conformant HALs (fields pre-filled) nothing
 * changes: the fill is skipped and NULL-instance queries go through
 * GetInstanceProcAddr as upstream does.
 *
 * Build (see tools/hybris/build-in-arch.sh):
 *   gcc -shared -fPIC sysvk.c -I<sysvk bundled includes> -lhardware
 */
#include <hardware/hardware.h>
#include <hardware/hwvulkan.h>
#include <string.h>

/* The device struct the tegra HAL hands back lives at this vaddr inside the
 * module; everything else is placed relative to it. */
#define TEGRA_DEVICE_VADDR 0x4448
#define TEGRA_ENUMERATE_INSTANCE_EXTENSION_PROPERTIES 0x2060
#define TEGRA_CREATE_INSTANCE 0x242c
#define TEGRA_GET_INSTANCE_PROC_ADDR 0x22f0
#define TEGRA_ENUMERATE_INSTANCE_VERSION 0x2954
#define TEGRA_DESTROY_INSTANCE 0x28e8

static const struct hwvulkan_device_t *dev;

static void fill_device(struct hwvulkan_device_t *device) {
    if (device->EnumerateInstanceExtensionProperties || device->CreateInstance ||
        device->GetInstanceProcAddr)
        return; /* conformant HAL — leave it alone */
    char *base = (char *)device - TEGRA_DEVICE_VADDR;
    device->EnumerateInstanceExtensionProperties =
        (PFN_vkEnumerateInstanceExtensionProperties)(base +
                                                     TEGRA_ENUMERATE_INSTANCE_EXTENSION_PROPERTIES);
    device->CreateInstance = (PFN_vkCreateInstance)(base + TEGRA_CREATE_INSTANCE);
    device->GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(base + TEGRA_GET_INSTANCE_PROC_ADDR);
}

static void *tegra_global_proc(const struct hwvulkan_device_t *d, const char *name) {
    char *base = (char *)d - TEGRA_DEVICE_VADDR;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))
        return (void *)(base + TEGRA_ENUMERATE_INSTANCE_EXTENSION_PROPERTIES);
    if (!strcmp(name, "vkCreateInstance"))
        return (void *)(base + TEGRA_CREATE_INSTANCE);
    if (!strcmp(name, "vkEnumerateInstanceVersion"))
        return (void *)(base + TEGRA_ENUMERATE_INSTANCE_VERSION);
    if (!strcmp(name, "vkDestroyInstance"))
        return (void *)(base + TEGRA_DESTROY_INSTANCE);
    return NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName) {
    hw_module_t *module;
    int ret = hw_get_module(HWVULKAN_HARDWARE_MODULE_ID, (const hw_module_t **)&module);
    if (ret)
        return NULL;
    /* open() is cheap after the first call (HAL caches its device); the
     * loader may ask for global procs before anything else. */
    struct hwvulkan_device_t *hal_dev = NULL;
    ret = module->methods->open(module, HWVULKAN_DEVICE_0, (hw_device_t **)&hal_dev);
    if (ret || !hal_dev)
        return NULL;
    dev = hal_dev;
    fill_device(hal_dev);

    /* Global-level (instance == NULL): the tegra HAL's GetInstanceProcAddr
     * dereferences the instance, so answer from the device struct. */
    if (!instance) {
        if (!strcmp(pName, "vkEnumerateInstanceExtensionProperties"))
            return (PFN_vkVoidFunction)dev->EnumerateInstanceExtensionProperties;
        if (!strcmp(pName, "vkCreateInstance"))
            return (PFN_vkVoidFunction)dev->CreateInstance;
        if (!dev->GetInstanceProcAddr)
            return (PFN_vkVoidFunction)tegra_global_proc(dev, pName);
        return NULL;
    }
    return dev->GetInstanceProcAddr(instance, pName);
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
