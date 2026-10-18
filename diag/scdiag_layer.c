// VK_LAYER_SCDIAG_diagnose: records every vkCreateDevice request (extensions, queues, raw bytes
// of each pNext struct) to $SCDIAG_LOG (default /tmp/scdiag.log) in a format scdiag_replay can
// rebuild, then passes the call through unchanged and records the driver's result.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "scdiag_structs.h"

#define EXPORT __attribute__((visibility("default")))

static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static VkInstance last_inst;
static FILE *logf_;

static FILE *lg(void) {
    if (!logf_) {
        const char *p = getenv("SCDIAG_LOG");
        logf_ = fopen(p ? p : "/tmp/scdiag.log", "a");
        if (!logf_) logf_ = stderr;
        setvbuf(logf_, NULL, _IONBF, 0);
    }
    return logf_;
}

typedef struct { VkStructureType sType; const void *pNext; } Hdr;

static void proc_name(char *buf, size_t n) {
    FILE *c = fopen("/proc/self/cmdline", "r");
    size_t k = c ? fread(buf, 1, n - 1, c) : 0;
    if (c) fclose(c);
    buf[k] = 0;
}

// With SCDIAG_PAUSE=<dir>, block until <dir>/go.<pid> exists (max 120 s) so a tracer can attach.
static void maybe_pause(FILE *f) {
    const char *dir = getenv("SCDIAG_PAUSE");
    if (!dir) return;
    char go[512];
    snprintf(go, sizeof go, "%s/go.%d", dir, (int)getpid());
    fprintf(f, "PAUSE %d\n", (int)getpid());
    for (int i = 0; i < 1200 && access(go, F_OK); i++) usleep(100000);
    fprintf(f, "RESUME %d\n", (int)getpid());
}

static void hexdump(FILE *f, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) fprintf(f, "%02x", ((const unsigned char *)p)[i]);
}

static VKAPI_ATTR VkResult VKAPI_CALL diag_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                        const VkAllocationCallbacks *alloc, VkDevice *out) {
    VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerDeviceCreateInfo *)chain->pNext;
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    PFN_vkCreateDevice fn = (PFN_vkCreateDevice)gipa(last_inst, "vkCreateDevice");

    FILE *f = lg();
    VkPhysicalDeviceProperties props;
    ((PFN_vkGetPhysicalDeviceProperties)gipa(last_inst, "vkGetPhysicalDeviceProperties"))(pd, &props);
    char cmd[256];
    proc_name(cmd, sizeof cmd);
    fprintf(f, "BEGIN %s\n", props.deviceName);
    fprintf(f, "PROC %d %s\n", (int)getpid(), cmd);
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++)
        fprintf(f, "QUEUE %u %u %u\n", ci->pQueueCreateInfos[i].queueFamilyIndex,
                ci->pQueueCreateInfos[i].queueCount, ci->pQueueCreateInfos[i].flags);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) fprintf(f, "EXT %s\n", ci->ppEnabledExtensionNames[i]);
    if (ci->pEnabledFeatures) {
        fprintf(f, "FEATURES ");
        hexdump(f, ci->pEnabledFeatures, sizeof(VkPhysicalDeviceFeatures));
        fprintf(f, "\n");
    }
    for (const Hdr *h = ci->pNext; h; h = h->pNext) {
        if (h->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) continue;
        const ScStructInfo *si = sc_find(h->sType);
        if (!si) { fprintf(f, "UNKNOWN %d\n", h->sType); continue; }
        fprintf(f, "STRUCT %d %s ", h->sType, si->name);
        hexdump(f, h, si->size);
        fprintf(f, "\n");
    }
    if (props.vendorID == 0x10de) maybe_pause(f);
    VkResult r = fn(pd, ci, alloc, out);
    fprintf(f, "END %d\n", r);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL diag_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
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
    uint32_t api = ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0;
    fprintf(lg(), "INSTANCE %u.%u.%u %d\n", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api), r);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) fprintf(lg(), "INSTEXT %s\n", ci->ppEnabledExtensionNames[i]);
    return r;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL scdiag_GetDeviceProcAddr(VkDevice d, const char *name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)scdiag_GetDeviceProcAddr;
    return next_gdpa(d, name);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL scdiag_GetInstanceProcAddr(VkInstance inst, const char *name) {
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)scdiag_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)scdiag_GetDeviceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)diag_CreateInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)diag_CreateDevice;
    return next_gipa ? next_gipa(inst, name) : NULL;
}
