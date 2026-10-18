// vkdevtest: try vkCreateDevice on the NVIDIA GPU with different extension/feature sets.
// usage: vkdevtest [--coopmat-feature] [--all-features] [--all-ext] [--skip EXT]... [EXT]...
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    int coop_feat = 0, all_feat = 0, all_ext = 0;
    const char *want[256]; int nwant = 0;
    const char *skip[64]; int nskip = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--coopmat-feature")) coop_feat = 1;
        else if (!strcmp(argv[i], "--all-features")) all_feat = 1;
        else if (!strcmp(argv[i], "--all-ext")) all_ext = 1;
        else if (!strcmp(argv[i], "--skip") && i + 1 < argc) skip[nskip++] = argv[++i];
        else want[nwant++] = argv[i];
    }

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) { puts("instance failed"); return 2; }

    uint32_t n = 8; VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.vendorID == 0x10de) { pd = pds[i]; printf("GPU: %s\n", p.deviceName); }
    }
    if (!pd) { puts("no NVIDIA GPU"); return 2; }

    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    const char **exts = calloc(ne + nwant, sizeof *exts); uint32_t nexts = 0;
    if (all_ext) {
        for (uint32_t i = 0; i < ne; i++) {
            int s = 0;
            for (int j = 0; j < nskip; j++) if (!strcmp(ep[i].extensionName, skip[j])) s = 1;
            if (!s) exts[nexts++] = ep[i].extensionName;
        }
    }
    for (int i = 0; i < nwant; i++) exts[nexts++] = want[i];

    // Feature chain: core 1.1-1.3 + cooperative matrix (KHR/NV/NV2)
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &cm};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    printf("driver reports cooperativeMatrix=%u robustBufferAccess=%u\n",
           cm.cooperativeMatrix, cm.cooperativeMatrixRobustBufferAccess);
    if (!all_feat) {
        // start from nothing, optionally enable just the coop matrix feature
        VkBool32 c = cm.cooperativeMatrix;
        memset(&f2.features, 0, sizeof f2.features);
        memset((char *)&f11 + 16, 0, sizeof f11 - 16);
        memset((char *)&f12 + 16, 0, sizeof f12 - 16);
        memset((char *)&f13 + 16, 0, sizeof f13 - 16);
        memset((char *)&cm + 16, 0, sizeof cm - 16);
        if (coop_feat) cm.cooperativeMatrix = c;
    } else if (!coop_feat) {
        cm.cooperativeMatrix = cm.cooperativeMatrixRobustBufferAccess = VK_FALSE;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci, .enabledExtensionCount = nexts, .ppEnabledExtensionNames = exts};
    VkDevice dev;
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    printf("exts=%u coop_feat=%d all_feat=%d -> vkCreateDevice = %d (%s)\n",
           nexts, coop_feat, all_feat, r, r == VK_SUCCESS ? "OK" : "FAILED");
    if (r == VK_SUCCESS) vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return r == VK_SUCCESS ? 0 : 1;
}
