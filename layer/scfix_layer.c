// VK_LAYER_SCFIX_hide_nvx: hides VK_NVX_binary_import and VK_NVX_image_view_handle from the
// application and strips them from vkCreateDevice.
//
// Why: with these extensions the NVIDIA driver initialises CUDA (libcuda) for interop, and libcuda
// needs ~4 GiB of free address space inside a window starting at 8 GiB. Under Wine, Star Citizen's
// process has that whole window reserved, so vkCreateDevice fails with VK_ERROR_INITIALIZATION_FAILED.
// The game does not retry without the extensions (DXVK does), so it dies. Cost: DLSS is unavailable.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static VkInstance last_inst;

// Extra extensions to hide can be listed in $SCFIX_HIDE (comma-separated).
static int hidden(const char *name) {
    if (!strcmp(name, "VK_NVX_binary_import") || !strcmp(name, "VK_NVX_image_view_handle")) return 1;
    const char *extra = getenv("SCFIX_HIDE");
    size_t n = strlen(name);
    for (const char *p = extra; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL)
        if (!strncmp(p, name, n) && (p[n] == ',' || p[n] == 0)) return 1;
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL fix_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer,
                                                                             uint32_t *count, VkExtensionProperties *props) {
    PFN_vkEnumerateDeviceExtensionProperties next =
        (PFN_vkEnumerateDeviceExtensionProperties)next_gipa(last_inst, "vkEnumerateDeviceExtensionProperties");
    if (layer) return next(pd, layer, count, props);
    uint32_t n = 0;
    VkResult r = next(pd, NULL, &n, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *all = malloc(sizeof *all * (n ? n : 1));
    r = next(pd, NULL, &n, all);
    if (r < 0) { free(all); return r; }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) if (!hidden(all[i].extensionName)) all[k++] = all[i];
    if (!props) { *count = k; free(all); return VK_SUCCESS; }
    uint32_t c = *count < k ? *count : k;
    memcpy(props, all, sizeof *all * c);
    *count = c;
    free(all);
    return c < k ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL fix_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                       const VkAllocationCallbacks *alloc, VkDevice *out) {
    VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerDeviceCreateInfo *)chain->pNext;
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    PFN_vkCreateDevice fn = (PFN_vkCreateDevice)gipa(last_inst, "vkCreateDevice");

    VkDeviceCreateInfo m = *ci;
    const char **ext = malloc(sizeof(char *) * (ci->enabledExtensionCount + 1));
    m.enabledExtensionCount = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        if (!hidden(ci->ppEnabledExtensionNames[i])) ext[m.enabledExtensionCount++] = ci->ppEnabledExtensionNames[i];
    m.ppEnabledExtensionNames = ext;
    VkResult r = fn(pd, &m, alloc, out);
    free(ext);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL fix_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                                         VkInstance *out) {
    VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerInstanceCreateInfo *)chain->pNext;
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    PFN_vkCreateInstance fn = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
    VkResult r = fn(ci, alloc, out);
    if (r == VK_SUCCESS) last_inst = *out;
    return r;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL scfix_GetDeviceProcAddr(VkDevice d, const char *name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)scfix_GetDeviceProcAddr;
    return next_gdpa(d, name);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL scfix_GetInstanceProcAddr(VkInstance inst, const char *name) {
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)scfix_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)scfix_GetDeviceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)fix_CreateInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)fix_CreateDevice;
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) return (PFN_vkVoidFunction)fix_EnumerateDeviceExtensionProperties;
    return next_gipa ? next_gipa(inst, name) : NULL;
}
