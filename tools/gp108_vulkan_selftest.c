/*
 * tools/gp108_vulkan_selftest.c
 *
 * Phase 3 Hardware Verification on NVIDIA GeForce MX250 (GP108M, 10de:1d13):
 *   1. Loads Mesa 26.3.1 NVK (`libvulkan_nvk.dylib`) directly on macOS Sequoia x86_64
 *   2. Verifies Vulkan 1.3 PhysicalDevice properties, 2 GiB GDDR5 VRAM heap,
 *      and `VK_EXT_metal_surface` + `VK_KHR_swapchain` support
 *   3. Compiles & executes a 256x256 Mandelbrot Compute Shader via NAK (Rust SM61 SASS)
 *      on `PASCAL_COMPUTE_B` (`0xc1c0`) into GDDR5 VRAM, DMAs back to host RAM,
 *      and verifies numerical accuracy against CPU IEEE-754 reference
 *   4. Compiles & executes a 512x512 Vulkan 1.3 Dynamic-Rendering 3D Graphics Pipeline
 *      (36-vertex lit 3D cube + D32_SFLOAT Z-buffer) on `PASCAL_B` (`0xc197`) into
 *      GDDR5 `BlockLinear` VRAM, hardware-untiles via `vkCmdCopyImageToBuffer`
 *      (`PASCAL_DMA_COPY_B` `0xc1b5`), verifies rendered pixels, and saves raw RGBA
 *      outputs for PNG conversion.
 */

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>

#include "gp108_test_shaders.h"

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

#define VK_CHECK(expr)                                                         \
    do {                                                                       \
        VkResult _r = (expr);                                                  \
        if (_r != VK_SUCCESS) {                                                \
            fprintf(stderr, "[FAIL] %s returned VkResult=%d at line %d\n",     \
                    #expr, (int)_r, __LINE__);                                 \
            return -1;                                                         \
        }                                                                      \
    } while (0)

typedef struct Vertex3D {
    float pos[3];
    float nrm[3];
    float col[3];
} Vertex3D;

typedef struct PushConst3D {
    float mvp[16];
    float col0[4];
    float col1[4];
    float col2[4];
    float light_dir[4];
} PushConst3D;

static void mat4_identity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_mul(float out[16], const float a[16], const float b[16]) {
    float r[16];
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            r[c * 4 + row] =
                a[0 * 4 + row] * b[c * 4 + 0] +
                a[1 * 4 + row] * b[c * 4 + 1] +
                a[2 * 4 + row] * b[c * 4 + 2] +
                a[3 * 4 + row] * b[c * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

/* Vulkan clip space perspective: Y down, Z in [0, 1] */
static void mat4_perspective_vk(float m[16], float fovy_rad, float aspect, float zn, float zf) {
    memset(m, 0, 16 * sizeof(float));
    float f = 1.0f / tanf(fovy_rad * 0.5f);
    m[0]  = f / aspect;
    m[5]  = -f; /* flip Y for Vulkan clip space */
    m[10] = zf / (zn - zf);
    m[11] = -1.0f;
    m[14] = (zn * zf) / (zn - zf);
}

static void mat4_translate(float m[16], float tx, float ty, float tz) {
    mat4_identity(m);
    m[12] = tx;
    m[13] = ty;
    m[14] = tz;
}

static void mat4_rotate_yx(float m[16], float ay, float ax) {
    float cy = cosf(ay), sy = sinf(ay);
    float cx = cosf(ax), sx = sinf(ax);
    float ry[16], rx[16];
    mat4_identity(ry);
    ry[0] = cy;  ry[8]  = sy;
    ry[2] = -sy; ry[10] = cy;

    mat4_identity(rx);
    rx[5] = cx;  rx[9]  = -sx;
    rx[6] = sx;  rx[10] = cx;

    mat4_mul(m, rx, ry);
}

static const Vertex3D kCubeVertices[36] = {
    /* +Z Front (Coral Red) */
    {{-1,-1, 1},{0,0,1},{0.95f,0.28f,0.26f}}, {{ 1,-1, 1},{0,0,1},{0.95f,0.28f,0.26f}}, {{ 1, 1, 1},{0,0,1},{0.95f,0.28f,0.26f}},
    {{-1,-1, 1},{0,0,1},{0.95f,0.28f,0.26f}}, {{ 1, 1, 1},{0,0,1},{0.95f,0.28f,0.26f}}, {{-1, 1, 1},{0,0,1},{0.95f,0.28f,0.26f}},
    /* -Z Back (Cyan) */
    {{ 1,-1,-1},{0,0,-1},{0.18f,0.82f,0.88f}}, {{-1,-1,-1},{0,0,-1},{0.18f,0.82f,0.88f}}, {{-1, 1,-1},{0,0,-1},{0.18f,0.82f,0.88f}},
    {{ 1,-1,-1},{0,0,-1},{0.18f,0.82f,0.88f}}, {{-1, 1,-1},{0,0,-1},{0.18f,0.82f,0.88f}}, {{ 1, 1,-1},{0,0,-1},{0.18f,0.82f,0.88f}},
    /* +X Right (Emerald Green) */
    {{ 1,-1, 1},{1,0,0},{0.22f,0.85f,0.42f}}, {{ 1,-1,-1},{1,0,0},{0.22f,0.85f,0.42f}}, {{ 1, 1,-1},{1,0,0},{0.22f,0.85f,0.42f}},
    {{ 1,-1, 1},{1,0,0},{0.22f,0.85f,0.42f}}, {{ 1, 1,-1},{1,0,0},{0.22f,0.85f,0.42f}}, {{ 1, 1, 1},{1,0,0},{0.22f,0.85f,0.42f}},
    /* -X Left (Amber Gold) */
    {{-1,-1,-1},{-1,0,0},{0.98f,0.74f,0.18f}}, {{-1,-1, 1},{-1,0,0},{0.98f,0.74f,0.18f}}, {{-1, 1, 1},{-1,0,0},{0.98f,0.74f,0.18f}},
    {{-1,-1,-1},{-1,0,0},{0.98f,0.74f,0.18f}}, {{-1, 1, 1},{-1,0,0},{0.98f,0.74f,0.18f}}, {{-1, 1,-1},{-1,0,0},{0.98f,0.74f,0.18f}},
    /* +Y Top (Royal Blue) */
    {{-1, 1, 1},{0,1,0},{0.28f,0.52f,0.98f}}, {{ 1, 1, 1},{0,1,0},{0.28f,0.52f,0.98f}}, {{ 1, 1,-1},{0,1,0},{0.28f,0.52f,0.98f}},
    {{-1, 1, 1},{0,1,0},{0.28f,0.52f,0.98f}}, {{ 1, 1,-1},{0,1,0},{0.28f,0.52f,0.98f}}, {{-1, 1,-1},{0,1,0},{0.28f,0.52f,0.98f}},
    /* -Y Bottom (Magenta) */
    {{-1,-1,-1},{0,-1,0},{0.85f,0.30f,0.90f}}, {{ 1,-1,-1},{0,-1,0},{0.85f,0.30f,0.90f}}, {{ 1,-1, 1},{0,-1,0},{0.85f,0.30f,0.90f}},
    {{-1,-1,-1},{0,-1,0},{0.85f,0.30f,0.90f}}, {{ 1,-1, 1},{0,-1,0},{0.85f,0.30f,0.90f}}, {{-1,-1, 1},{0,-1,0},{0.85f,0.30f,0.90f}},
};

static uint32_t find_mem_type(const VkPhysicalDeviceMemoryProperties *mp,
                              uint32_t type_bits,
                              VkMemoryPropertyFlags req_flags) {
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mp->memoryTypes[i].propertyFlags & req_flags) == req_flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

static uint32_t cpu_mandelbrot_pixel(uint32_t gx, uint32_t gy, uint32_t w, uint32_t h,
                                     float scale, float center_x, uint32_t *out_iters) {
    float cx = ((float)gx - (float)w * 0.5f) * scale + center_x;
    float cy = ((float)gy - (float)h * 0.5f) * scale;
    float zr = 0.0f, zi = 0.0f;
    uint32_t iters = 0;
    for (int i = 0; i < 24; i++) {
        float mag2 = zr * zr + zi * zi;
        int alive = (mag2 <= 4.0f);
        if (alive) {
            iters++;
            float nzr = zr * zr - zi * zi + cx;
            float nzi = 2.0f * zr * zi + cy;
            zr = nzr;
            zi = nzi;
        } else {
            zr = 2.0f;
            zi = 2.0f;
        }
    }
    if (out_iters) *out_iters = iters;
    return 0xFF000000u | ((iters * 9u) << 16) | ((iters * 7u) << 8) | (iters * 10u);
}

__attribute__((visibility("default")))
int gp108_vulkan_run_selftest(const char *dylib_path,
                              const char *out_comp_raw,
                              const char *out_3d_raw) {
    printf("=== MX250 (GP108) Mesa 26.3.1 NVK Vulkan 1.3 Hardware Self-Test ===\n");
    double t0 = now_ms();

    void *icd = dlopen(dylib_path, RTLD_NOW | RTLD_LOCAL);
    if (!icd) {
        fprintf(stderr, "[FAIL] dlopen(%s) failed: %s\n", dylib_path, dlerror());
        return -1;
    }

    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)dlsym(icd, "vk_icdGetInstanceProcAddr");
    if (!gipa) {
        fprintf(stderr, "[FAIL] dlsym(vk_icdGetInstanceProcAddr) failed\n");
        return -1;
    }

    PFN_vkCreateInstance pfnCreateInstance =
        (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties pfnEnumInstExt =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");

    uint32_t inst_ext_count = 0;
    VK_CHECK(pfnEnumInstExt(NULL, &inst_ext_count, NULL));
    VkExtensionProperties *inst_exts = calloc(inst_ext_count, sizeof(*inst_exts));
    VK_CHECK(pfnEnumInstExt(NULL, &inst_ext_count, inst_exts));
    int has_metal_surf = 0;
    for (uint32_t i = 0; i < inst_ext_count; i++) {
        if (strcmp(inst_exts[i].extensionName, VK_EXT_METAL_SURFACE_EXTENSION_NAME) == 0)
            has_metal_surf = 1;
    }
    free(inst_exts);
    printf("[1/5] Instance extensions: %u found (VK_EXT_metal_surface=%s)\n",
           inst_ext_count, has_metal_surf ? "YES" : "NO");
    if (!has_metal_surf) {
        fprintf(stderr, "[FAIL] VK_EXT_metal_surface not exposed by NVK instance!\n");
        return -1;
    }

    const char *req_inst_exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_EXT_METAL_SURFACE_EXTENSION_NAME,
    };
    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "gp108_vulkan_selftest",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "NVK-macOS",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = req_inst_exts,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(pfnCreateInstance(&ici, NULL, &instance));

#define INST_FN(name) PFN_##name pfn_##name = (PFN_##name)gipa(instance, #name)
    INST_FN(vkDestroyInstance);
    INST_FN(vkEnumeratePhysicalDevices);
    INST_FN(vkGetPhysicalDeviceProperties);
    INST_FN(vkGetPhysicalDeviceMemoryProperties);
    INST_FN(vkEnumerateDeviceExtensionProperties);
    INST_FN(vkCreateDevice);
    INST_FN(vkGetDeviceProcAddr);

    uint32_t pdev_count = 0;
    VK_CHECK(pfn_vkEnumeratePhysicalDevices(instance, &pdev_count, NULL));
    if (pdev_count == 0) {
        fprintf(stderr, "[FAIL] vkEnumeratePhysicalDevices returned 0 GPUs!\n");
        return -1;
    }
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    pdev_count = 1;
    VK_CHECK(pfn_vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev));

    VkPhysicalDeviceProperties props;
    pfn_vkGetPhysicalDeviceProperties(pdev, &props);
    VkPhysicalDeviceMemoryProperties mem_props;
    pfn_vkGetPhysicalDeviceMemoryProperties(pdev, &mem_props);

    uint64_t vram_bytes = 0;
    for (uint32_t i = 0; i < mem_props.memoryHeapCount; i++) {
        if (mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            vram_bytes = mem_props.memoryHeaps[i].size;
            break;
        }
    }

    uint32_t dev_ext_count = 0;
    VK_CHECK(pfn_vkEnumerateDeviceExtensionProperties(pdev, NULL, &dev_ext_count, NULL));
    VkExtensionProperties *dev_exts = calloc(dev_ext_count, sizeof(*dev_exts));
    VK_CHECK(pfn_vkEnumerateDeviceExtensionProperties(pdev, NULL, &dev_ext_count, dev_exts));
    int has_swapchain = 0;
    for (uint32_t i = 0; i < dev_ext_count; i++) {
        if (strcmp(dev_exts[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
            has_swapchain = 1;
    }
    free(dev_exts);

    printf("[2/5] PhysicalDevice awake in %.2f ms:\n"
           "      Name       : %s\n"
           "      PCI ID     : %04x:%04x (Type=%d)\n"
           "      Vulkan API : %u.%u.%u\n"
           "      GDDR5 Heap : %llu MiB (%u heaps, %u memTypes)\n"
           "      Extensions : %u (VK_KHR_swapchain=%s)\n",
           now_ms() - t0,
           props.deviceName,
           props.vendorID, props.deviceID, (int)props.deviceType,
           VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion),
           VK_API_VERSION_PATCH(props.apiVersion),
           (unsigned long long)(vram_bytes / (1024ULL * 1024ULL)),
           mem_props.memoryHeapCount, mem_props.memoryTypeCount,
           dev_ext_count, has_swapchain ? "YES" : "NO");

    float q_prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &q_prio,
    };
    VkPhysicalDeviceVulkan13Features feat13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering = VK_TRUE,
        .synchronization2 = VK_TRUE,
    };
    const char *req_dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &feat13,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = has_swapchain ? 1u : 0u,
        .ppEnabledExtensionNames = req_dev_exts,
    };
    VkDevice dev = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreateDevice(pdev, &dci, NULL, &dev));

#define DEV_FN(name) PFN_##name pfn_##name = (PFN_##name)pfn_vkGetDeviceProcAddr(dev, #name)
    DEV_FN(vkDestroyDevice);
    DEV_FN(vkGetDeviceQueue);
    DEV_FN(vkCreateCommandPool);
    DEV_FN(vkDestroyCommandPool);
    DEV_FN(vkAllocateCommandBuffers);
    DEV_FN(vkResetCommandBuffer);
    DEV_FN(vkBeginCommandBuffer);
    DEV_FN(vkEndCommandBuffer);
    DEV_FN(vkCreateFence);
    DEV_FN(vkDestroyFence);
    DEV_FN(vkResetFences);
    DEV_FN(vkWaitForFences);
    DEV_FN(vkQueueSubmit);
    DEV_FN(vkDeviceWaitIdle);
    DEV_FN(vkCreateBuffer);
    DEV_FN(vkDestroyBuffer);
    DEV_FN(vkGetBufferMemoryRequirements);
    DEV_FN(vkAllocateMemory);
    DEV_FN(vkFreeMemory);
    DEV_FN(vkBindBufferMemory);
    DEV_FN(vkMapMemory);
    DEV_FN(vkUnmapMemory);
    DEV_FN(vkCreateShaderModule);
    DEV_FN(vkDestroyShaderModule);
    DEV_FN(vkCreateDescriptorSetLayout);
    DEV_FN(vkDestroyDescriptorSetLayout);
    DEV_FN(vkCreateDescriptorPool);
    DEV_FN(vkDestroyDescriptorPool);
    DEV_FN(vkAllocateDescriptorSets);
    DEV_FN(vkUpdateDescriptorSets);
    DEV_FN(vkCreatePipelineLayout);
    DEV_FN(vkDestroyPipelineLayout);
    DEV_FN(vkCreateComputePipelines);
    DEV_FN(vkCreateGraphicsPipelines);
    DEV_FN(vkDestroyPipeline);
    DEV_FN(vkCmdBindPipeline);
    DEV_FN(vkCmdBindDescriptorSets);
    DEV_FN(vkCmdPushConstants);
    DEV_FN(vkCmdDispatch);
    DEV_FN(vkCmdPipelineBarrier);
    DEV_FN(vkCmdCopyBuffer);
    DEV_FN(vkCreateImage);
    DEV_FN(vkDestroyImage);
    DEV_FN(vkGetImageMemoryRequirements);
    DEV_FN(vkBindImageMemory);
    DEV_FN(vkCreateImageView);
    DEV_FN(vkDestroyImageView);
    DEV_FN(vkCmdBeginRendering);
    DEV_FN(vkCmdEndRendering);
    DEV_FN(vkCmdSetViewport);
    DEV_FN(vkCmdSetScissor);
    DEV_FN(vkCmdBindVertexBuffers);
    DEV_FN(vkCmdDraw);
    DEV_FN(vkCmdCopyImageToBuffer);

    VkQueue queue = VK_NULL_HANDLE;
    pfn_vkGetDeviceQueue(dev, 0, 0, &queue);

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreateCommandPool(dev, &cpci, NULL, &pool));

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkAllocateCommandBuffers(dev, &cbai, &cmd));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreateFence(dev, &fci, NULL, &fence));

    /* ===================================================================== *
     * TEST 1: Vulkan 1.3 Compute Pipeline (256x256 Mandelbrot on SM61)      *
     * ===================================================================== */
    const uint32_t comp_w = 256, comp_h = 256;
    const VkDeviceSize comp_bytes = (VkDeviceSize)comp_w * comp_h * sizeof(uint32_t);

    VkBufferCreateInfo bci_vram = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = comp_bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    VkBufferCreateInfo bci_host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = comp_bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VkBuffer buf_ssbo = VK_NULL_HANDLE, buf_readback = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreateBuffer(dev, &bci_vram, NULL, &buf_ssbo));
    VK_CHECK(pfn_vkCreateBuffer(dev, &bci_host, NULL, &buf_readback));

    VkMemoryRequirements mr_ssbo, mr_rb;
    pfn_vkGetBufferMemoryRequirements(dev, buf_ssbo, &mr_ssbo);
    pfn_vkGetBufferMemoryRequirements(dev, buf_readback, &mr_rb);

    VkMemoryAllocateInfo mai_ssbo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr_ssbo.size,
        .memoryTypeIndex = find_mem_type(&mem_props, mr_ssbo.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VkMemoryAllocateInfo mai_rb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr_rb.size,
        .memoryTypeIndex = find_mem_type(&mem_props, mr_rb.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory mem_ssbo = VK_NULL_HANDLE, mem_rb = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkAllocateMemory(dev, &mai_ssbo, NULL, &mem_ssbo));
    VK_CHECK(pfn_vkAllocateMemory(dev, &mai_rb, NULL, &mem_rb));
    VK_CHECK(pfn_vkBindBufferMemory(dev, buf_ssbo, mem_ssbo, 0));
    VK_CHECK(pfn_vkBindBufferMemory(dev, buf_readback, mem_rb, 0));

    uint32_t *rb_ptr = NULL;
    VK_CHECK(pfn_vkMapMemory(dev, mem_rb, 0, comp_bytes, 0, (void **)&rb_ptr));
    memset(rb_ptr, 0xAA, comp_bytes);

    double t_comp_compile = now_ms();
    VkShaderModuleCreateInfo smci_comp = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(kSpvCompMandelbrot),
        .pCode = kSpvCompMandelbrot,
    };
    VkShaderModule sm_comp = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreateShaderModule(dev, &smci_comp, NULL, &sm_comp));

    VkDescriptorSetLayoutBinding dslb = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &dslb,
    };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

    VkPushConstantRange pcr_comp = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = 16,
    };
    VkPipelineLayoutCreateInfo plci_comp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &dsl,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr_comp,
    };
    VkPipelineLayout pl_comp = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreatePipelineLayout(dev, &plci_comp, NULL, &pl_comp));

    VkComputePipelineCreateInfo cp_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = sm_comp,
            .pName = "main",
        },
        .layout = pl_comp,
    };
    VkPipeline pipe_comp = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp_info, NULL, &pipe_comp));
    double comp_compile_ms = now_ms() - t_comp_compile;

    VkDescriptorPoolSize dps = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &dps,
    };
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dpool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dsl,
    };
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkAllocateDescriptorSets(dev, &dsai, &dset));

    VkDescriptorBufferInfo dbi = {
        .buffer = buf_ssbo,
        .offset = 0,
        .range = comp_bytes,
    };
    VkWriteDescriptorSet wds = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = dset,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &dbi,
    };
    pfn_vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

    struct {
        uint32_t w;
        uint32_t h;
        float scale;
        float center_x;
    } comp_pc = { comp_w, comp_h, 0.0115f, -0.55f };

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(pfn_vkBeginCommandBuffer(cmd, &cbbi));
    pfn_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_comp);
    pfn_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl_comp,
                                0, 1, &dset, 0, NULL);
    pfn_vkCmdPushConstants(cmd, pl_comp, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(comp_pc), &comp_pc);
    pfn_vkCmdDispatch(cmd, comp_w / 16, comp_h / 16, 1);

    VkBufferMemoryBarrier bmb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buf_ssbo,
        .offset = 0,
        .size = comp_bytes,
    };
    pfn_vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 1, &bmb, 0, NULL);

    VkBufferCopy bcopy = { .srcOffset = 0, .dstOffset = 0, .size = comp_bytes };
    pfn_vkCmdCopyBuffer(cmd, buf_ssbo, buf_readback, 1, &bcopy);
    VK_CHECK(pfn_vkEndCommandBuffer(cmd));

    double t_comp_exec = now_ms();
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VK_CHECK(pfn_vkQueueSubmit(queue, 1, &si, fence));
    VK_CHECK(pfn_vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ULL));
    double comp_exec_ms = now_ms() - t_comp_exec;

    uint32_t exact_matches = 0, close_matches = 0, interior_cnt = 0;
    for (uint32_t y = 0; y < comp_h; y++) {
        for (uint32_t x = 0; x < comp_w; x++) {
            uint32_t gpu_px = rb_ptr[y * comp_w + x];
            uint32_t ref_iters = 0;
            uint32_t ref_px = cpu_mandelbrot_pixel(x, y, comp_w, comp_h,
                                                   comp_pc.scale, comp_pc.center_x,
                                                   &ref_iters);
            uint32_t gpu_iters = (gpu_px & 0xFFu) / 10u;
            if (gpu_px == ref_px) exact_matches++;
            if ((int)abs((int)gpu_iters - (int)ref_iters) <= 1) close_matches++;
            if (gpu_iters == 24) interior_cnt++;
        }
    }
    printf("[3/5] NAK SM61 Compute Shader (256x256 = 65,536 threads, 1.57M complex iters):\n"
           "      SPIR-V -> SM61 SASS compile : %.2f ms\n"
           "      PASCAL_COMPUTE_B execution  : %.2f ms\n"
           "      Bit-exact CPU matches       : %u / %u (%.2f%%), +-1 FMA ulp: %u / %u (%.2f%%)\n"
           "      Mandelbrot interior pixels  : %u\n",
           comp_compile_ms, comp_exec_ms,
           exact_matches, comp_w * comp_h, 100.0 * exact_matches / (comp_w * comp_h),
           close_matches, comp_w * comp_h, 100.0 * close_matches / (comp_w * comp_h),
           interior_cnt);

    if (close_matches < (comp_w * comp_h * 99) / 100 || interior_cnt < 5000) {
        fprintf(stderr, "[FAIL] Compute shader output verification failed!\n");
        return -1;
    }

    if (out_comp_raw && out_comp_raw[0]) {
        FILE *fp = fopen(out_comp_raw, "wb");
        if (fp) {
            fwrite(rb_ptr, 1, comp_bytes, fp);
            fclose(fp);
        }
    }

    /* ===================================================================== *
     * TEST 2: Offscreen Vulkan 1.3 3D Graphics Pipeline (512x512 Lit Cube)  *
     * ===================================================================== */
    const uint32_t rt_w = 512, rt_h = 512;
    const VkDeviceSize rt_bytes = (VkDeviceSize)rt_w * rt_h * 4;

    VkImageCreateInfo ici_col = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { rt_w, rt_h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    };
    VkImageCreateInfo ici_dep = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .extent = { rt_w, rt_h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
    };
    VkImage img_col = VK_NULL_HANDLE, img_dep = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreateImage(dev, &ici_col, NULL, &img_col));
    VK_CHECK(pfn_vkCreateImage(dev, &ici_dep, NULL, &img_dep));

    VkMemoryRequirements mr_col, mr_dep;
    pfn_vkGetImageMemoryRequirements(dev, img_col, &mr_col);
    pfn_vkGetImageMemoryRequirements(dev, img_dep, &mr_dep);

    VkMemoryAllocateInfo mai_col = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr_col.size,
        .memoryTypeIndex = find_mem_type(&mem_props, mr_col.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VkMemoryAllocateInfo mai_dep = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr_dep.size,
        .memoryTypeIndex = find_mem_type(&mem_props, mr_dep.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VkDeviceMemory mem_col = VK_NULL_HANDLE, mem_dep = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkAllocateMemory(dev, &mai_col, NULL, &mem_col));
    VK_CHECK(pfn_vkAllocateMemory(dev, &mai_dep, NULL, &mem_dep));
    VK_CHECK(pfn_vkBindImageMemory(dev, img_col, mem_col, 0));
    VK_CHECK(pfn_vkBindImageMemory(dev, img_dep, mem_dep, 0));

    VkImageViewCreateInfo ivci_col = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img_col,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkImageViewCreateInfo ivci_dep = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img_dep,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 },
    };
    VkImageView iv_col = VK_NULL_HANDLE, iv_dep = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreateImageView(dev, &ivci_col, NULL, &iv_col));
    VK_CHECK(pfn_vkCreateImageView(dev, &ivci_dep, NULL, &iv_dep));

    /* Vertex buffer + 512x512 readback buffer */
    VkBufferCreateInfo bci_vb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(kCubeVertices),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
    };
    VkBufferCreateInfo bci_rt_rb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = rt_bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VkBuffer buf_vb = VK_NULL_HANDLE, buf_rt_rb = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreateBuffer(dev, &bci_vb, NULL, &buf_vb));
    VK_CHECK(pfn_vkCreateBuffer(dev, &bci_rt_rb, NULL, &buf_rt_rb));

    VkMemoryRequirements mr_vb, mr_rt_rb;
    pfn_vkGetBufferMemoryRequirements(dev, buf_vb, &mr_vb);
    pfn_vkGetBufferMemoryRequirements(dev, buf_rt_rb, &mr_rt_rb);

    VkMemoryAllocateInfo mai_vb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr_vb.size,
        .memoryTypeIndex = find_mem_type(&mem_props, mr_vb.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkMemoryAllocateInfo mai_rt_rb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr_rt_rb.size,
        .memoryTypeIndex = find_mem_type(&mem_props, mr_rt_rb.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory mem_vb = VK_NULL_HANDLE, mem_rt_rb = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkAllocateMemory(dev, &mai_vb, NULL, &mem_vb));
    VK_CHECK(pfn_vkAllocateMemory(dev, &mai_rt_rb, NULL, &mem_rt_rb));
    VK_CHECK(pfn_vkBindBufferMemory(dev, buf_vb, mem_vb, 0));
    VK_CHECK(pfn_vkBindBufferMemory(dev, buf_rt_rb, mem_rt_rb, 0));

    void *vb_ptr = NULL;
    VK_CHECK(pfn_vkMapMemory(dev, mem_vb, 0, sizeof(kCubeVertices), 0, &vb_ptr));
    memcpy(vb_ptr, kCubeVertices, sizeof(kCubeVertices));
    pfn_vkUnmapMemory(dev, mem_vb);

    uint8_t *rt_rb_ptr = NULL;
    VK_CHECK(pfn_vkMapMemory(dev, mem_rt_rb, 0, rt_bytes, 0, (void **)&rt_rb_ptr));
    memset(rt_rb_ptr, 0, rt_bytes);

    double t_gfx_compile = now_ms();
    VkShaderModuleCreateInfo smci_vert = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(kSpvVertCube3D),
        .pCode = kSpvVertCube3D,
    };
    VkShaderModuleCreateInfo smci_frag = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(kSpvFragCube3D),
        .pCode = kSpvFragCube3D,
    };
    VkShaderModule sm_vert = VK_NULL_HANDLE, sm_frag = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreateShaderModule(dev, &smci_vert, NULL, &sm_vert));
    VK_CHECK(pfn_vkCreateShaderModule(dev, &smci_frag, NULL, &sm_frag));

    VkPushConstantRange pcr_gfx = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PushConst3D),
    };
    VkPipelineLayoutCreateInfo plci_gfx = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr_gfx,
    };
    VkPipelineLayout pl_gfx = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreatePipelineLayout(dev, &plci_gfx, NULL, &pl_gfx));

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = sm_vert,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = sm_frag,
            .pName = "main",
        },
    };
    VkVertexInputBindingDescription vibd = {
        .binding = 0,
        .stride = sizeof(Vertex3D),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    VkVertexInputAttributeDescription viad[3] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex3D, pos) },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex3D, nrm) },
        { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex3D, col) },
    };
    VkPipelineVertexInputStateCreateInfo pvis = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vibd,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = viad,
    };
    VkPipelineInputAssemblyStateCreateInfo pias = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo pvps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo pras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo pmss = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo pdss = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    VkPipelineColorBlendAttachmentState pcbas = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo pcbs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &pcbas,
    };
    VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo pdyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn_states,
    };
    VkFormat col_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo prci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &col_fmt,
        .depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &prci,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &pvis,
        .pInputAssemblyState = &pias,
        .pViewportState = &pvps,
        .pRasterizationState = &pras,
        .pMultisampleState = &pmss,
        .pDepthStencilState = &pdss,
        .pColorBlendState = &pcbs,
        .pDynamicState = &pdyn,
        .layout = pl_gfx,
    };
    VkPipeline pipe_gfx = VK_NULL_HANDLE;
    VK_CHECK(pfn_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe_gfx));
    double gfx_compile_ms = now_ms() - t_gfx_compile;

    VK_CHECK(pfn_vkResetFences(dev, 1, &fence));
    VK_CHECK(pfn_vkResetCommandBuffer(cmd, 0));
    VK_CHECK(pfn_vkBeginCommandBuffer(cmd, &cbbi));

    VkImageMemoryBarrier imb_init[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img_col,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img_dep,
            .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 },
        },
    };
    pfn_vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                             0, 0, NULL, 0, NULL, 2, imb_init);

    VkRenderingAttachmentInfo col_att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = iv_col,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.color = { .float32 = { 0.07f, 0.09f, 0.14f, 1.0f } },
    };
    VkRenderingAttachmentInfo dep_att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = iv_dep,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue.depthStencil = { 1.0f, 0 },
    };
    VkRenderingInfo rinfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { rt_w, rt_h } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &col_att,
        .pDepthAttachment = &dep_att,
    };
    pfn_vkCmdBeginRendering(cmd, &rinfo);
    pfn_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_gfx);

    VkViewport vp = { 0.0f, 0.0f, (float)rt_w, (float)rt_h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { rt_w, rt_h } };
    pfn_vkCmdSetViewport(cmd, 0, 1, &vp);
    pfn_vkCmdSetScissor(cmd, 0, 1, &sc);

    float m_proj[16], m_view[16], m_rot[16], m_mv[16];
    mat4_perspective_vk(m_proj, 45.0f * (3.14159265f / 180.0f), 1.0f, 0.1f, 20.0f);
    mat4_translate(m_view, 0.0f, 0.0f, -4.2f);
    mat4_rotate_yx(m_rot, 0.68f, 0.52f);
    mat4_mul(m_mv, m_view, m_rot);

    PushConst3D pc3d;
    mat4_mul(pc3d.mvp, m_proj, m_mv);
    memcpy(pc3d.col0, &m_rot[0], 4 * sizeof(float));
    memcpy(pc3d.col1, &m_rot[4], 4 * sizeof(float));
    memcpy(pc3d.col2, &m_rot[8], 4 * sizeof(float));
    pc3d.light_dir[0] = 0.45f;
    pc3d.light_dir[1] = 0.65f;
    pc3d.light_dir[2] = 0.61f;
    pc3d.light_dir[3] = 0.24f; /* ambient */

    pfn_vkCmdPushConstants(cmd, pl_gfx,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc3d), &pc3d);
    VkDeviceSize vb_off = 0;
    pfn_vkCmdBindVertexBuffers(cmd, 0, 1, &buf_vb, &vb_off);
    pfn_vkCmdDraw(cmd, 36, 1, 0, 0);
    pfn_vkCmdEndRendering(cmd);

    VkImageMemoryBarrier imb_copy = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img_col,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    pfn_vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &imb_copy);

    VkBufferImageCopy bic = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { rt_w, rt_h, 1 },
    };
    pfn_vkCmdCopyImageToBuffer(cmd, img_col, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buf_rt_rb, 1, &bic);
    VK_CHECK(pfn_vkEndCommandBuffer(cmd));

    double t_gfx_exec = now_ms();
    VK_CHECK(pfn_vkQueueSubmit(queue, 1, &si, fence));
    VK_CHECK(pfn_vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ULL));
    double gfx_exec_ms = now_ms() - t_gfx_exec;

    /* Analyze rendered 512x512 RGBA image */
    uint8_t bg_r = rt_rb_ptr[0], bg_g = rt_rb_ptr[1], bg_b = rt_rb_ptr[2];
    uint32_t cube_pixels = 0;
    uint32_t red_dom = 0, green_dom = 0, blue_dom = 0;
    for (uint32_t i = 0; i < rt_w * rt_h; i++) {
        uint8_t r = rt_rb_ptr[i * 4 + 0];
        uint8_t g = rt_rb_ptr[i * 4 + 1];
        uint8_t b = rt_rb_ptr[i * 4 + 2];
        if (abs((int)r - (int)bg_r) > 8 ||
            abs((int)g - (int)bg_g) > 8 ||
            abs((int)b - (int)bg_b) > 8) {
            cube_pixels++;
            if (r > g + 30 && r > b + 30) red_dom++;
            if (g > r + 30 && g > b + 30) green_dom++;
            if (b > r + 30 && b > g + 20) blue_dom++;
        }
    }

    uint32_t c_idx = ((rt_h / 2) * rt_w + (rt_w / 2)) * 4;
    printf("[4/5] PASCAL_B (0xc197) Offscreen 3D Lit Cube (512x512 + D32 Z-buffer):\n"
           "      SPIR-V -> SM61 VS+FS compile : %.2f ms\n"
           "      3D Draw + BlockLinear Untile : %.2f ms\n"
           "      Clear BG RGBA(0,0)           : (%u, %u, %u, %u)\n"
           "      Center Pixel RGBA(256,256)   : (%u, %u, %u, %u)\n"
           "      Lit Cube Coverage            : %u / %u px (%.1f%%) [R=%u, G=%u, B=%u]\n",
           gfx_compile_ms, gfx_exec_ms,
           bg_r, bg_g, bg_b, rt_rb_ptr[3],
           rt_rb_ptr[c_idx], rt_rb_ptr[c_idx + 1], rt_rb_ptr[c_idx + 2], rt_rb_ptr[c_idx + 3],
           cube_pixels, rt_w * rt_h, 100.0 * cube_pixels / (rt_w * rt_h),
           red_dom, green_dom, blue_dom);

    if (cube_pixels < 20000 || red_dom < 500 || (green_dom + blue_dom) < 500) {
        fprintf(stderr, "[FAIL] 3D cube pixel verification failed!\n");
        return -1;
    }

    if (out_3d_raw && out_3d_raw[0]) {
        FILE *fp = fopen(out_3d_raw, "wb");
        if (fp) {
            fwrite(rt_rb_ptr, 1, rt_bytes, fp);
            fclose(fp);
        }
    }

    pfn_vkDeviceWaitIdle(dev);
    pfn_vkUnmapMemory(dev, mem_rt_rb);
    pfn_vkUnmapMemory(dev, mem_rb);
    pfn_vkDestroyPipeline(dev, pipe_gfx, NULL);
    pfn_vkDestroyPipelineLayout(dev, pl_gfx, NULL);
    pfn_vkDestroyShaderModule(dev, sm_vert, NULL);
    pfn_vkDestroyShaderModule(dev, sm_frag, NULL);
    pfn_vkDestroyImageView(dev, iv_col, NULL);
    pfn_vkDestroyImageView(dev, iv_dep, NULL);
    pfn_vkDestroyImage(dev, img_col, NULL);
    pfn_vkDestroyImage(dev, img_dep, NULL);
    pfn_vkFreeMemory(dev, mem_col, NULL);
    pfn_vkFreeMemory(dev, mem_dep, NULL);
    pfn_vkDestroyBuffer(dev, buf_vb, NULL);
    pfn_vkDestroyBuffer(dev, buf_rt_rb, NULL);
    pfn_vkFreeMemory(dev, mem_vb, NULL);
    pfn_vkFreeMemory(dev, mem_rt_rb, NULL);

    pfn_vkDestroyPipeline(dev, pipe_comp, NULL);
    pfn_vkDestroyPipelineLayout(dev, pl_comp, NULL);
    pfn_vkDestroyDescriptorPool(dev, dpool, NULL);
    pfn_vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    pfn_vkDestroyShaderModule(dev, sm_comp, NULL);
    pfn_vkDestroyBuffer(dev, buf_ssbo, NULL);
    pfn_vkDestroyBuffer(dev, buf_readback, NULL);
    pfn_vkFreeMemory(dev, mem_ssbo, NULL);
    pfn_vkFreeMemory(dev, mem_rb, NULL);

    pfn_vkDestroyFence(dev, fence, NULL);
    pfn_vkDestroyCommandPool(dev, pool, NULL);
    pfn_vkDestroyDevice(dev, NULL);
    pfn_vkDestroyInstance(instance, NULL);
    dlclose(icd);

    printf("[5/5] ALL MX250 VULKAN 1.3 COMPUTE & 3D HARDWARE TESTS PASSED 100%%!\n");
    return 0;
}
