// scdiag_replay: rebuild a vkCreateDevice request recorded by the SCDIAG layer and replay it on
// the NVIDIA GPU. With --bisect, a failing request is retried with one extension removed, one
// pNext struct removed, or one feature bit cleared at a time, reporting which change makes it work.
// usage: build/scdiag_replay LOGFILE [--bisect] [--block N]   (default: last recorded block)
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scdiag_structs.h"

#define MAXN 512
typedef struct { VkStructureType sType; void *pNext; } Hdr;

static char inst_ext[MAXN][256]; static int n_inst_ext; static uint32_t api_version = VK_API_VERSION_1_3;
static char ext[MAXN][256]; static int n_ext;
static VkDeviceQueueCreateInfo queues[16]; static int n_queues;
static VkPhysicalDeviceFeatures features; static int has_features;
static void *structs[MAXN]; static const ScStructInfo *sinfo[MAXN]; static int n_structs;
static int recorded_result;

static void unhex(const char *s, void *dst, size_t n) {
    for (size_t i = 0; i < n; i++) sscanf(s + 2 * i, "%2hhx", (unsigned char *)dst + i);
}

static int load(const char *path, int want_block) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    static char line[1 << 16];
    int block = -1, in = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "INSTANCE ", 9)) {
            unsigned a, b, c; sscanf(line + 9, "%u.%u.%u", &a, &b, &c);
            api_version = VK_MAKE_API_VERSION(0, a, b, c); n_inst_ext = 0;
        } else if (!strncmp(line, "INSTEXT ", 8)) {
            strcpy(inst_ext[n_inst_ext++], line + 8);
        } else if (!strncmp(line, "BEGIN ", 6)) {
            block++;
            in = (want_block < 0 || block == want_block) && strstr(line, "NVIDIA");
            if (in) { n_ext = n_queues = n_structs = has_features = 0; }
        } else if (!in) {
            continue;
        } else if (!strncmp(line, "QUEUE ", 6)) {
            VkDeviceQueueCreateInfo *q = &queues[n_queues++];
            memset(q, 0, sizeof *q); q->sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            sscanf(line + 6, "%u %u %u", &q->queueFamilyIndex, &q->queueCount, &q->flags);
        } else if (!strncmp(line, "EXT ", 4)) {
            strcpy(ext[n_ext++], line + 4);
        } else if (!strncmp(line, "FEATURES ", 9)) {
            unhex(line + 9, &features, sizeof features); has_features = 1;
        } else if (!strncmp(line, "STRUCT ", 7)) {
            int st; char name[256], hex[1 << 15];
            sscanf(line + 7, "%d %255s %32767s", &st, name, hex);
            const ScStructInfo *si = sc_find(st);
            if (!si) { fprintf(stderr, "unknown struct %d\n", st); continue; }
            structs[n_structs] = calloc(1, si->size);
            unhex(hex, structs[n_structs], si->size);
            sinfo[n_structs++] = si;
        } else if (!strncmp(line, "UNKNOWN ", 8)) {
            fprintf(stderr, "warning: recorded request had unrecognised struct %s (not replayed)\n", line + 8);
        } else if (!strncmp(line, "END ", 4)) {
            recorded_result = atoi(line + 4);
            if (want_block >= 0) break;
        }
    }
    fclose(f);
    return n_queues ? 0 : -1;
}

static VkInstance inst;
static VkPhysicalDevice pd;

// skip_ext / skip_struct: index to leave out, or -1
static VkResult attempt(int skip_ext, int skip_struct) {
    const char *names[MAXN]; uint32_t n = 0;
    for (int i = 0; i < n_ext; i++) if (i != skip_ext) names[n++] = ext[i];
    Hdr *head = NULL, **tail = &head;
    for (int i = 0; i < n_structs; i++) {
        if (i == skip_struct) continue;
        Hdr *h = structs[i]; *tail = h; tail = (Hdr **)&h->pNext;
    }
    *tail = NULL;
    static float prio[64];
    for (int i = 0; i < 64; i++) prio[i] = 1.0f;
    for (int i = 0; i < n_queues; i++) queues[i].pQueuePriorities = prio;
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = head, .queueCreateInfoCount = n_queues,
        .pQueueCreateInfos = queues, .enabledExtensionCount = n, .ppEnabledExtensionNames = names,
        .pEnabledFeatures = has_features ? &features : NULL};
    VkDevice d;
    VkResult r = vkCreateDevice(pd, &ci, NULL, &d);
    if (r == VK_SUCCESS) vkDestroyDevice(d, NULL);
    return r;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s LOGFILE [--bisect] [--block N]\n", argv[0]); return 2; }
    int bisect = 0, block = -1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--bisect")) bisect = 1;
        else if (!strcmp(argv[i], "--block") && i + 1 < argc) block = atoi(argv[++i]);
    }
    if (load(argv[1], block)) { fprintf(stderr, "no NVIDIA vkCreateDevice block found\n"); return 2; }
    printf("loaded: %d extensions, %d pNext structs, %d queues, recorded result %d\n", n_ext, n_structs, n_queues, recorded_result);

    const char *ie[MAXN];
    for (int i = 0; i < n_inst_ext; i++) ie[i] = inst_ext[i];
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = api_version};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .enabledExtensionCount = n_inst_ext, .ppEnabledExtensionNames = ie};
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r) { printf("vkCreateInstance with recorded extensions failed (%d)\n", r); return 2; }
    uint32_t n = 8; VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(inst, &n, pds);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.vendorID == 0x10de) pd = pds[i];
    }
    if (!pd) { puts("no NVIDIA GPU"); return 2; }

    // Layers below the loader's validation may add extensions the device doesn't expose (the driver
    // ignores those); drop them and duplicates so the replay goes through the loader cleanly.
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    int kept = 0;
    for (int i = 0; i < n_ext; i++) {
        int ok = 0, dup = 0;
        for (uint32_t j = 0; j < ne; j++) if (!strcmp(ext[i], ep[j].extensionName)) ok = 1;
        for (int j = 0; j < kept; j++) if (!strcmp(ext[i], ext[j])) dup = 1;
        if (!ok) printf("note: dropping %s (not a device extension of this GPU)\n", ext[i]);
        if (ok && !dup) memmove(ext[kept++], ext[i], sizeof ext[i]);
    }
    n_ext = kept;
    free(ep);

    r = attempt(-1, -1);
    printf("replay of full request -> %d\n", r);
    if (r == VK_SUCCESS || !bisect) return r ? 1 : 0;

    for (int i = 0; i < n_ext; i++)
        if (attempt(i, -1) == VK_SUCCESS) printf("FIX: works without extension %s\n", ext[i]);
    for (int i = 0; i < n_structs; i++) {
        if (attempt(-1, i) == VK_SUCCESS) printf("FIX: works without struct %s\n", sinfo[i]->name);
        for (int b = 0; b < sinfo[i]->nbools; b++) {
            VkBool32 *v = (VkBool32 *)((char *)structs[i] + sinfo[i]->bool_off[b]);
            if (!*v) continue;
            *v = VK_FALSE;
            if (attempt(-1, -1) == VK_SUCCESS) printf("FIX: works with %s.%s = VK_FALSE\n", sinfo[i]->name, sinfo[i]->bool_name[b]);
            *v = VK_TRUE;
        }
    }
    if (has_features) {
        VkBool32 *v = (VkBool32 *)&features;
        for (size_t b = 0; b < sizeof features / sizeof *v; b++) {
            if (!v[b]) continue;
            v[b] = VK_FALSE;
            if (attempt(-1, -1) == VK_SUCCESS) printf("FIX: works with pEnabledFeatures bool #%zu = VK_FALSE\n", b);
            v[b] = VK_TRUE;
        }
    }
    puts("bisect done");
    return 1;
}
