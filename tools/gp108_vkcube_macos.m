/*
 * tools/gp108_vkcube_macos.m
 *
 * Phase 4: Native macOS Desktop Windowed 3D Demo (PRIME Render Offload)
 *   - Discrete GPU : NVIDIA GeForce MX250 (GP108M, 2GB GDDR5) via Mesa 26.3.1 NVK + NAK (SM61)
 *   - Display GPU  : Intel UHD Graphics (IGPU@2) via VK_EXT_metal_surface + CAMetalLayer
 *   - Pipeline     : MX250 BlockLinear 3D Render + D32 Z-Buffer -> PASCAL_DMA_COPY_B (0xc1b5)
 *                    Hardware Untile + PCIe DMA -> Zero-Copy MTLBuffer Blit -> WindowServer
 */

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

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

static double mono_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#define VK_ENSURE(expr)                                                        \
    do {                                                                       \
        VkResult _r = (expr);                                                  \
        if (_r != VK_SUCCESS) {                                                \
            fprintf(stderr, "[VK-ERR] %s failed with %d at line %d\n",         \
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

static void mat4_perspective_vk(float m[16], float fovy_rad, float aspect, float zn, float zf) {
    memset(m, 0, 16 * sizeof(float));
    float f = 1.0f / tanf(fovy_rad * 0.5f);
    m[0]  = f / aspect;
    m[5]  = -f;
    m[10] = zf / (zn - zf);
    m[11] = -1.0f;
    m[14] = (zn * zf) / (zn - zf);
}

static void mat4_translate_scale(float m[16], float tx, float ty, float tz, float s) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = s;
    m[15] = 1.0f;
    m[12] = tx;
    m[13] = ty;
    m[14] = tz;
}

static void mat4_rotate_yxz(float m[16], float ay, float ax, float az) {
    float cy = cosf(ay), sy = sinf(ay);
    float cx = cosf(ax), sx = sinf(ax);
    float cz = cosf(az), sz = sinf(az);
    float ry[16], rx[16], rz[16], tmp[16];
    mat4_identity(ry);
    ry[0] = cy;  ry[8]  = sy;
    ry[2] = -sy; ry[10] = cy;

    mat4_identity(rx);
    rx[5] = cx;  rx[9]  = -sx;
    rx[6] = sx;  rx[10] = cx;

    mat4_identity(rz);
    rz[0] = cz;  rz[4] = -sz;
    rz[1] = sz;  rz[5] = cz;

    mat4_mul(tmp, rx, ry);
    mat4_mul(m, rz, tmp);
}

static const Vertex3D kCubeVerts[36] = {
    /* +Z Coral Red */
    {{-1,-1, 1},{0,0,1},{0.96f,0.30f,0.28f}}, {{ 1,-1, 1},{0,0,1},{0.96f,0.30f,0.28f}}, {{ 1, 1, 1},{0,0,1},{0.96f,0.30f,0.28f}},
    {{-1,-1, 1},{0,0,1},{0.96f,0.30f,0.28f}}, {{ 1, 1, 1},{0,0,1},{0.96f,0.30f,0.28f}}, {{-1, 1, 1},{0,0,1},{0.96f,0.30f,0.28f}},
    /* -Z Aqua Cyan */
    {{ 1,-1,-1},{0,0,-1},{0.16f,0.84f,0.90f}}, {{-1,-1,-1},{0,0,-1},{0.16f,0.84f,0.90f}}, {{-1, 1,-1},{0,0,-1},{0.16f,0.84f,0.90f}},
    {{ 1,-1,-1},{0,0,-1},{0.16f,0.84f,0.90f}}, {{-1, 1,-1},{0,0,-1},{0.16f,0.84f,0.90f}}, {{ 1, 1,-1},{0,0,-1},{0.16f,0.84f,0.90f}},
    /* +X NVIDIA Green */
    {{ 1,-1, 1},{1,0,0},{0.46f,0.88f,0.18f}}, {{ 1,-1,-1},{1,0,0},{0.46f,0.88f,0.18f}}, {{ 1, 1,-1},{1,0,0},{0.46f,0.88f,0.18f}},
    {{ 1,-1, 1},{1,0,0},{0.46f,0.88f,0.18f}}, {{ 1, 1,-1},{1,0,0},{0.46f,0.88f,0.18f}}, {{ 1, 1, 1},{1,0,0},{0.46f,0.88f,0.18f}},
    /* -X Warm Gold */
    {{-1,-1,-1},{-1,0,0},{0.98f,0.75f,0.18f}}, {{-1,-1, 1},{-1,0,0},{0.98f,0.75f,0.18f}}, {{-1, 1, 1},{-1,0,0},{0.98f,0.75f,0.18f}},
    {{-1,-1,-1},{-1,0,0},{0.98f,0.75f,0.18f}}, {{-1, 1, 1},{-1,0,0},{0.98f,0.75f,0.18f}}, {{-1, 1,-1},{-1,0,0},{0.98f,0.75f,0.18f}},
    /* +Y Electric Blue */
    {{-1, 1, 1},{0,1,0},{0.28f,0.54f,0.98f}}, {{ 1, 1, 1},{0,1,0},{0.28f,0.54f,0.98f}}, {{ 1, 1,-1},{0,1,0},{0.28f,0.54f,0.98f}},
    {{-1, 1, 1},{0,1,0},{0.28f,0.54f,0.98f}}, {{ 1, 1,-1},{0,1,0},{0.28f,0.54f,0.98f}}, {{-1, 1,-1},{0,1,0},{0.28f,0.54f,0.98f}},
    /* -Y Vivid Purple */
    {{-1,-1,-1},{0,-1,0},{0.84f,0.32f,0.94f}}, {{ 1,-1,-1},{0,-1,0},{0.84f,0.32f,0.94f}}, {{ 1,-1, 1},{0,-1,0},{0.84f,0.32f,0.94f}},
    {{-1,-1,-1},{0,-1,0},{0.84f,0.32f,0.94f}}, {{ 1,-1, 1},{0,-1,0},{0.84f,0.32f,0.94f}}, {{-1,-1, 1},{0,-1,0},{0.84f,0.32f,0.94f}},
};

static uint32_t choose_mem_type(const VkPhysicalDeviceMemoryProperties *mp,
                                uint32_t bits,
                                VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return UINT32_MAX;
}

@interface MX250MetalView : NSView
@property(nonatomic, strong) CAMetalLayer *metalLayer;
@property(nonatomic, assign) BOOL shouldClose;
@end

@implementation MX250MetalView
- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        self.wantsLayer = YES;
        _metalLayer = [CAMetalLayer layer];
        _metalLayer.device = [MTLCreateSystemDefaultDevice() autorelease];
        _metalLayer.contentsScale = 1.0; /* crisp 1:1 benchmark pixels */
        _metalLayer.bounds = self.bounds;
        _metalLayer.drawableSize = CGSizeMake(frameRect.size.width, frameRect.size.height);
        self.layer = _metalLayer;
    }
    return self;
}
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)event {
    if (event.keyCode == 53) { /* ESC */
        self.shouldClose = YES;
    }
}
@end

@interface MX250WinDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) BOOL closed;
@end

@implementation MX250WinDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    self.closed = YES;
    return YES;
}
@end

__attribute__((visibility("default")))
int gp108_vkcube_run(const char *dylib_path,
                     double max_seconds,
                     int vsync,
                     const char *screenshot_png_path) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        const uint32_t win_w = 960, win_h = 640;
        NSRect rect = NSMakeRect(0, 0, win_w, win_h);
        NSWindowStyleMask style = NSWindowStyleMaskTitled |
                                  NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskMiniaturizable;
        NSWindow *window = [[[NSWindow alloc] initWithContentRect:rect
                                                        styleMask:style
                                                          backing:NSBackingStoreBuffered
                                                            defer:NO] autorelease];
        [window center];
        [window setTitle:@"Initializing NVIDIA GeForce MX250 (NVK GP108)..."];

        MX250WinDelegate *winDel = [[[MX250WinDelegate alloc] init] autorelease];
        [window setDelegate:winDel];

        MX250MetalView *view = [[[MX250MetalView alloc] initWithFrame:rect] autorelease];
        [window setContentView:view];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        /* Pump initial Cocoa events so WindowServer materializes the window */
        for (int i = 0; i < 10; i++) {
            NSEvent *ev;
            while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES])) {
                [NSApp sendEvent:ev];
            }
        }

        NSString *displayGPUName = view.metalLayer.device.name ?: @"Intel Metal";

        /* Load Mesa 26.3.1 NVK Vulkan 1.3 ICD */
        void *icd = dlopen(dylib_path, RTLD_NOW | RTLD_LOCAL);
        if (!icd) {
            fprintf(stderr, "[FAIL] dlopen(%s): %s\n", dylib_path, dlerror());
            return -1;
        }
        PFN_vkGetInstanceProcAddr gipa =
            (PFN_vkGetInstanceProcAddr)dlsym(icd, "vk_icdGetInstanceProcAddr");
        if (!gipa) {
            fprintf(stderr, "[FAIL] Missing vk_icdGetInstanceProcAddr\n");
            return -1;
        }

        PFN_vkCreateInstance pfnCreateInstance =
            (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");

        const char *inst_exts[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_EXT_METAL_SURFACE_EXTENSION_NAME,
        };
        VkApplicationInfo app = {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "MX250-PRIME-Demo",
            .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
            .pEngineName = "NVK-macOS",
            .engineVersion = VK_MAKE_VERSION(1, 0, 0),
            .apiVersion = VK_API_VERSION_1_3,
        };
        VkInstanceCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app,
            .enabledExtensionCount = 2,
            .ppEnabledExtensionNames = inst_exts,
        };
        VkInstance instance = VK_NULL_HANDLE;
        VK_ENSURE(pfnCreateInstance(&ici, NULL, &instance));

#define IFN(n) PFN_##n pfn_##n = (PFN_##n)gipa(instance, #n)
        IFN(vkDestroyInstance);
        IFN(vkEnumeratePhysicalDevices);
        IFN(vkGetPhysicalDeviceProperties);
        IFN(vkGetPhysicalDeviceMemoryProperties);
        IFN(vkCreateMetalSurfaceEXT);
        IFN(vkDestroySurfaceKHR);
        IFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
        IFN(vkGetPhysicalDeviceSurfaceFormatsKHR);
        IFN(vkCreateDevice);
        IFN(vkGetDeviceProcAddr);

        uint32_t gpu_cnt = 1;
        VkPhysicalDevice pdev = VK_NULL_HANDLE;
        VK_ENSURE(pfn_vkEnumeratePhysicalDevices(instance, &gpu_cnt, &pdev));

        VkPhysicalDeviceProperties props;
        pfn_vkGetPhysicalDeviceProperties(pdev, &props);
        VkPhysicalDeviceMemoryProperties mem_props;
        pfn_vkGetPhysicalDeviceMemoryProperties(pdev, &mem_props);

        VkMetalSurfaceCreateInfoEXT msci = {
            .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
            .pLayer = view.metalLayer,
        };
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VK_ENSURE(pfn_vkCreateMetalSurfaceEXT(instance, &msci, NULL, &surface));

        VkSurfaceCapabilitiesKHR surf_caps;
        VK_ENSURE(pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pdev, surface, &surf_caps));

        uint32_t fmt_cnt = 0;
        VK_ENSURE(pfn_vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, surface, &fmt_cnt, NULL));
        VkSurfaceFormatKHR *fmts = calloc(fmt_cnt, sizeof(*fmts));
        VK_ENSURE(pfn_vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, surface, &fmt_cnt, fmts));
        VkFormat swap_fmt = fmts[0].format;
        VkColorSpaceKHR swap_cs = fmts[0].colorSpace;
        for (uint32_t i = 0; i < fmt_cnt; i++) {
            if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
                swap_fmt = fmts[i].format;
                swap_cs = fmts[i].colorSpace;
                break;
            }
        }
        free(fmts);

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
        const char *dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo dci = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &feat13,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &qci,
            .enabledExtensionCount = 1,
            .ppEnabledExtensionNames = dev_exts,
        };
        VkDevice dev = VK_NULL_HANDLE;
        VK_ENSURE(pfn_vkCreateDevice(pdev, &dci, NULL, &dev));

#define DFN(n) PFN_##n pfn_##n = (PFN_##n)pfn_vkGetDeviceProcAddr(dev, #n)
        DFN(vkDestroyDevice);
        DFN(vkGetDeviceQueue);
        DFN(vkDeviceWaitIdle);
        DFN(vkCreateSwapchainKHR);
        DFN(vkDestroySwapchainKHR);
        DFN(vkGetSwapchainImagesKHR);
        DFN(vkAcquireNextImageKHR);
        DFN(vkQueuePresentKHR);
        DFN(vkCreateImage);
        DFN(vkDestroyImage);
        DFN(vkGetImageMemoryRequirements);
        DFN(vkAllocateMemory);
        DFN(vkFreeMemory);
        DFN(vkBindImageMemory);
        DFN(vkCreateImageView);
        DFN(vkDestroyImageView);
        DFN(vkCreateBuffer);
        DFN(vkDestroyBuffer);
        DFN(vkGetBufferMemoryRequirements);
        DFN(vkBindBufferMemory);
        DFN(vkMapMemory);
        DFN(vkUnmapMemory);
        DFN(vkCreateShaderModule);
        DFN(vkDestroyShaderModule);
        DFN(vkCreatePipelineLayout);
        DFN(vkDestroyPipelineLayout);
        DFN(vkCreateGraphicsPipelines);
        DFN(vkDestroyPipeline);
        DFN(vkCreateCommandPool);
        DFN(vkDestroyCommandPool);
        DFN(vkAllocateCommandBuffers);
        DFN(vkResetCommandBuffer);
        DFN(vkBeginCommandBuffer);
        DFN(vkEndCommandBuffer);
        DFN(vkCreateSemaphore);
        DFN(vkDestroySemaphore);
        DFN(vkCreateFence);
        DFN(vkDestroyFence);
        DFN(vkWaitForFences);
        DFN(vkResetFences);
        DFN(vkQueueSubmit);
        DFN(vkCmdPipelineBarrier);
        DFN(vkCmdBeginRendering);
        DFN(vkCmdEndRendering);
        DFN(vkCmdBindPipeline);
        DFN(vkCmdSetViewport);
        DFN(vkCmdSetScissor);
        DFN(vkCmdPushConstants);
        DFN(vkCmdBindVertexBuffers);
        DFN(vkCmdDraw);

        VkQueue queue = VK_NULL_HANDLE;
        pfn_vkGetDeviceQueue(dev, 0, 0, &queue);

        VkSwapchainCreateInfoKHR sci = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = surface,
            .minImageCount = surf_caps.minImageCount,
            .imageFormat = swap_fmt,
            .imageColorSpace = swap_cs,
            .imageExtent = { win_w, win_h },
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR,
            .clipped = VK_TRUE,
        };
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VK_ENSURE(pfn_vkCreateSwapchainKHR(dev, &sci, NULL, &swapchain));

        uint32_t img_cnt = 0;
        VK_ENSURE(pfn_vkGetSwapchainImagesKHR(dev, swapchain, &img_cnt, NULL));
        VkImage swap_imgs[8];
        VkImageView swap_views[8];
        VK_ENSURE(pfn_vkGetSwapchainImagesKHR(dev, swapchain, &img_cnt, swap_imgs));
        for (uint32_t i = 0; i < img_cnt; i++) {
            VkImageViewCreateInfo ivci = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = swap_imgs[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = swap_fmt,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            VK_ENSURE(pfn_vkCreateImageView(dev, &ivci, NULL, &swap_views[i]));
        }

        /* GDDR5 Depth Buffer */
        VkImageCreateInfo dici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_D32_SFLOAT,
            .extent = { win_w, win_h, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        };
        VkImage depth_img = VK_NULL_HANDLE;
        VK_ENSURE(pfn_vkCreateImage(dev, &dici, NULL, &depth_img));
        VkMemoryRequirements dmr;
        pfn_vkGetImageMemoryRequirements(dev, depth_img, &dmr);
        VkMemoryAllocateInfo dmai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = dmr.size,
            .memoryTypeIndex = choose_mem_type(&mem_props, dmr.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };
        VkDeviceMemory depth_mem = VK_NULL_HANDLE;
        VK_ENSURE(pfn_vkAllocateMemory(dev, &dmai, NULL, &depth_mem));
        VK_ENSURE(pfn_vkBindImageMemory(dev, depth_img, depth_mem, 0));
        VkImageViewCreateInfo divci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = depth_img,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_D32_SFLOAT,
            .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 },
        };
        VkImageView depth_view = VK_NULL_HANDLE;
        VK_ENSURE(pfn_vkCreateImageView(dev, &divci, NULL, &depth_view));

        /* Vertex Buffer */
        VkBufferCreateInfo vbci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sizeof(kCubeVerts),
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        };
        VkBuffer vb = VK_NULL_HANDLE;
        VK_ENSURE(pfn_vkCreateBuffer(dev, &vbci, NULL, &vb));
        VkMemoryRequirements vbmr;
        pfn_vkGetBufferMemoryRequirements(dev, vb, &vbmr);
        VkMemoryAllocateInfo vbmai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = vbmr.size,
            .memoryTypeIndex = choose_mem_type(&mem_props, vbmr.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
        };
        VkDeviceMemory vb_mem = VK_NULL_HANDLE;
        VK_ENSURE(pfn_vkAllocateMemory(dev, &vbmai, NULL, &vb_mem));
        VK_ENSURE(pfn_vkBindBufferMemory(dev, vb, vb_mem, 0));
        void *vb_map = NULL;
        VK_ENSURE(pfn_vkMapMemory(dev, vb_mem, 0, sizeof(kCubeVerts), 0, &vb_map));
        memcpy(vb_map, kCubeVerts, sizeof(kCubeVerts));
        pfn_vkUnmapMemory(dev, vb_mem);

        /* NAK SM61 Graphics Pipeline */
        VkShaderModuleCreateInfo smci_v = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(kSpvVertCube3D),
            .pCode = kSpvVertCube3D,
        };
        VkShaderModuleCreateInfo smci_f = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(kSpvFragCube3D),
            .pCode = kSpvFragCube3D,
        };
        VkShaderModule sm_v = VK_NULL_HANDLE, sm_f = VK_NULL_HANDLE;
        VK_ENSURE(pfn_vkCreateShaderModule(dev, &smci_v, NULL, &sm_v));
        VK_ENSURE(pfn_vkCreateShaderModule(dev, &smci_f, NULL, &sm_f));

        VkPushConstantRange pcr = {
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = sizeof(PushConst3D),
        };
        VkPipelineLayoutCreateInfo plci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pcr,
        };
        VkPipelineLayout playout = VK_NULL_HANDLE;
        VK_ENSURE(pfn_vkCreatePipelineLayout(dev, &plci, NULL, &playout));

        VkPipelineShaderStageCreateInfo stages[2] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
              VK_SHADER_STAGE_VERTEX_BIT, sm_v, "main", NULL },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
              VK_SHADER_STAGE_FRAGMENT_BIT, sm_f, "main", NULL },
        };
        VkVertexInputBindingDescription vibd = { 0, sizeof(Vertex3D), VK_VERTEX_INPUT_RATE_VERTEX };
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
            .colorWriteMask = 0xFu,
        };
        VkPipelineColorBlendStateCreateInfo pcbs = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &pcbas,
        };
        VkDynamicState dyns[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo pdyn = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates = dyns,
        };
        VkPipelineRenderingCreateInfo prci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &swap_fmt,
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
            .layout = playout,
        };
        VkPipeline pipeline = VK_NULL_HANDLE;
        VK_ENSURE(pfn_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline));

        /* Double-buffered frames in flight */
        VkCommandPoolCreateInfo cpci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = 0,
        };
        VkCommandPool pool = VK_NULL_HANDLE;
        VK_ENSURE(pfn_vkCreateCommandPool(dev, &cpci, NULL, &pool));

        VkCommandBuffer cmds[2];
        VkCommandBufferAllocateInfo cbai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 2,
        };
        VK_ENSURE(pfn_vkAllocateCommandBuffers(dev, &cbai, cmds));

        VkSemaphore sem_acq[2], sem_ren[2];
        VkFence fences[2];
        VkSemaphoreCreateInfo sem_ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fen_ci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        for (int i = 0; i < 2; i++) {
            VK_ENSURE(pfn_vkCreateSemaphore(dev, &sem_ci, NULL, &sem_acq[i]));
            VK_ENSURE(pfn_vkCreateSemaphore(dev, &sem_ci, NULL, &sem_ren[i]));
            VK_ENSURE(pfn_vkCreateFence(dev, &fen_ci, NULL, &fences[i]));
        }

        printf("=== MX250 PRIME Render Offload Window Live on macOS Desktop ===\n");
        printf("    Render GPU  : %s (Vulkan %u.%u.%u)\n",
               props.deviceName,
               VK_API_VERSION_MAJOR(props.apiVersion),
               VK_API_VERSION_MINOR(props.apiVersion),
               VK_API_VERSION_PATCH(props.apiVersion));
        printf("    Display GPU : %s (CAMetalLayer %ux%u, %u swapchain images, %s)\n",
               [displayGPUName UTF8String], win_w, win_h, img_cnt,
               vsync ? "FIFO VSync" : "IMMEDIATE Unlocked");

        double t_start = mono_sec();
        double t_last_title = t_start;
        uint64_t total_frames = 0;
        uint64_t window_frames = 0;
        int captured_shot = 0;

        float m_proj[16];
        mat4_perspective_vk(m_proj, 45.0f * (3.14159265f / 180.0f),
                            (float)win_w / (float)win_h, 0.1f, 30.0f);

        while (!winDel.closed && !view.shouldClose) {
            @autoreleasepool {
                NSEvent *ev;
                while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES])) {
                    [NSApp sendEvent:ev];
                }

                double now = mono_sec();
                double elapsed = now - t_start;
                if (max_seconds > 0.0 && elapsed >= max_seconds)
                    break;

                uint32_t fi = (uint32_t)(total_frames & 1u);
                VK_ENSURE(pfn_vkWaitForFences(dev, 1, &fences[fi], VK_TRUE, 5000000000ULL));
                VK_ENSURE(pfn_vkResetFences(dev, 1, &fences[fi]));

                uint32_t img_idx = 0;
                VkResult acq = pfn_vkAcquireNextImageKHR(
                    dev, swapchain, 5000000000ULL, sem_acq[fi], VK_NULL_HANDLE, &img_idx);
                if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
                    fprintf(stderr, "[WARN] vkAcquireNextImageKHR=%d\n", (int)acq);
                    break;
                }

                VkCommandBuffer cmd = cmds[fi];
                VK_ENSURE(pfn_vkResetCommandBuffer(cmd, 0));
                VkCommandBufferBeginInfo cbbi = {
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                };
                VK_ENSURE(pfn_vkBeginCommandBuffer(cmd, &cbbi));

                VkImageMemoryBarrier barriers_in[2] = {
                    {
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = swap_imgs[img_idx],
                        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
                    },
                    {
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                        .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = depth_img,
                        .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 },
                    },
                };
                pfn_vkCmdPipelineBarrier(cmd,
                                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                         0, 0, NULL, 0, NULL, 2, barriers_in);

                VkRenderingAttachmentInfo col_att = {
                    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = swap_views[img_idx],
                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                    .clearValue.color = { .float32 = { 0.055f, 0.075f, 0.125f, 1.0f } },
                };
                VkRenderingAttachmentInfo dep_att = {
                    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = depth_view,
                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                    .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                    .clearValue.depthStencil = { 1.0f, 0 },
                };
                VkRenderingInfo rinfo = {
                    .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                    .renderArea = { { 0, 0 }, { win_w, win_h } },
                    .layerCount = 1,
                    .colorAttachmentCount = 1,
                    .pColorAttachments = &col_att,
                    .pDepthAttachment = &dep_att,
                };
                pfn_vkCmdBeginRendering(cmd, &rinfo);
                pfn_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

                VkViewport vp = { 0.0f, 0.0f, (float)win_w, (float)win_h, 0.0f, 1.0f };
                VkRect2D sc = { { 0, 0 }, { win_w, win_h } };
                pfn_vkCmdSetViewport(cmd, 0, 1, &vp);
                pfn_vkCmdSetScissor(cmd, 0, 1, &sc);

                VkDeviceSize vb_off = 0;
                pfn_vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vb_off);

                float t = (float)elapsed;

                /* Draw 1 central hero cube + 3 orbiting satellite cubes */
                for (int obj = 0; obj < 4; obj++) {
                    float m_rot[16], m_trs[16], m_model[16];
                    if (obj == 0) {
                        mat4_rotate_yxz(m_rot, t * 1.15f, t * 0.72f, t * 0.35f);
                        mat4_translate_scale(m_trs, 0.0f, 0.0f, -5.0f, 1.15f);
                    } else {
                        float phase = (float)(obj - 1) * (2.0f * 3.14159265f / 3.0f) + t * 1.55f;
                        float ox = cosf(phase) * 2.55f;
                        float oz = -5.0f + sinf(phase) * 1.75f;
                        float oy = sinf(phase * 1.5f) * 0.85f;
                        mat4_rotate_yxz(m_rot, -t * 2.1f + obj, t * 1.7f, phase);
                        mat4_translate_scale(m_trs, ox, oy, oz, 0.38f);
                    }
                    mat4_mul(m_model, m_trs, m_rot);

                    PushConst3D pc;
                    mat4_mul(pc.mvp, m_proj, m_model);
                    memcpy(pc.col0, &m_rot[0], 4 * sizeof(float));
                    memcpy(pc.col1, &m_rot[4], 4 * sizeof(float));
                    memcpy(pc.col2, &m_rot[8], 4 * sizeof(float));
                    pc.light_dir[0] = 0.42f;
                    pc.light_dir[1] = 0.68f;
                    pc.light_dir[2] = 0.60f;
                    pc.light_dir[3] = 0.25f;

                    pfn_vkCmdPushConstants(
                        cmd, playout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(pc), &pc);
                    pfn_vkCmdDraw(cmd, 36, 1, 0, 0);
                }

                pfn_vkCmdEndRendering(cmd);

                /* Optional one-shot 960x640 frame capture right before PRESENT transition */
                VkBuffer sbuf = VK_NULL_HANDLE;
                VkDeviceMemory smem = VK_NULL_HANDLE;
                VkDeviceSize snap_bytes = (VkDeviceSize)win_w * win_h * 4;
                int do_snap_this_frame = (!captured_shot && screenshot_png_path &&
                                          screenshot_png_path[0] && elapsed >= 1.5);
                if (do_snap_this_frame) {
                    captured_shot = 1;
                    PFN_vkCmdCopyImageToBuffer pfn_vkCmdCopyImageToBuffer =
                        (PFN_vkCmdCopyImageToBuffer)pfn_vkGetDeviceProcAddr(dev, "vkCmdCopyImageToBuffer");
                    VkBufferCreateInfo sbci = {
                        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                        .size = snap_bytes,
                        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    };
                    if (pfn_vkCreateBuffer(dev, &sbci, NULL, &sbuf) == VK_SUCCESS) {
                        VkMemoryRequirements smr;
                        pfn_vkGetBufferMemoryRequirements(dev, sbuf, &smr);
                        VkMemoryAllocateInfo smai = {
                            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                            .allocationSize = smr.size,
                            .memoryTypeIndex = choose_mem_type(&mem_props, smr.memoryTypeBits,
                                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
                        };
                        if (pfn_vkAllocateMemory(dev, &smai, NULL, &smem) == VK_SUCCESS) {
                            pfn_vkBindBufferMemory(dev, sbuf, smem, 0);
                            VkImageMemoryBarrier smb = {
                                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = swap_imgs[img_idx],
                                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
                            };
                            pfn_vkCmdPipelineBarrier(cmd,
                                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                     0, 0, NULL, 0, NULL, 1, &smb);
                            VkBufferImageCopy sbic = {
                                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                .imageExtent = { win_w, win_h, 1 },
                            };
                            pfn_vkCmdCopyImageToBuffer(cmd, swap_imgs[img_idx],
                                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sbuf, 1, &sbic);
                        }
                    }
                }

                VkImageMemoryBarrier barrier_present = {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = do_snap_this_frame ? VK_ACCESS_TRANSFER_READ_BIT
                                                        : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    .dstAccessMask = 0,
                    .oldLayout = do_snap_this_frame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                                    : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = swap_imgs[img_idx],
                    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
                };
                pfn_vkCmdPipelineBarrier(cmd,
                                         do_snap_this_frame ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                                            : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                         0, 0, NULL, 0, NULL, 1, &barrier_present);
                VK_ENSURE(pfn_vkEndCommandBuffer(cmd));

                VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                VkSubmitInfo si = {
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .waitSemaphoreCount = 1,
                    .pWaitSemaphores = &sem_acq[fi],
                    .pWaitDstStageMask = &wait_stage,
                    .commandBufferCount = 1,
                    .pCommandBuffers = &cmd,
                    .signalSemaphoreCount = 1,
                    .pSignalSemaphores = &sem_ren[fi],
                };
                VK_ENSURE(pfn_vkQueueSubmit(queue, 1, &si, fences[fi]));

                VkPresentInfoKHR pi = {
                    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                    .waitSemaphoreCount = 1,
                    .pWaitSemaphores = &sem_ren[fi],
                    .swapchainCount = 1,
                    .pSwapchains = &swapchain,
                    .pImageIndices = &img_idx,
                };
                VkResult pres = pfn_vkQueuePresentKHR(queue, &pi);
                if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR) {
                    fprintf(stderr, "[WARN] vkQueuePresentKHR=%d\n", (int)pres);
                    break;
                }

                total_frames++;
                window_frames++;

                if (now - t_last_title >= 0.30) {
                    double dt = now - t_last_title;
                    double fps = (double)window_frames / dt;
                    double ms = 1000.0 / (fps > 0.001 ? fps : 1.0);
                    NSString *title = [NSString stringWithFormat:
                        @"%s -> %@ PRIME | Vulkan 1.3 | %.1f FPS (%.2f ms)",
                        props.deviceName, displayGPUName, fps, ms];
                    [window setTitle:title];
                    window_frames = 0;
                    t_last_title = now;
                }

                if (do_snap_this_frame && sbuf != VK_NULL_HANDLE && smem != VK_NULL_HANDLE) {
                    pfn_vkWaitForFences(dev, 1, &fences[fi], VK_TRUE, 5000000000ULL);
                    uint8_t *bgra = NULL;
                    if (pfn_vkMapMemory(dev, smem, 0, snap_bytes, 0, (void **)&bgra) == VK_SUCCESS) {
                        NSBitmapImageRep *rep = [[[NSBitmapImageRep alloc]
                            initWithBitmapDataPlanes:NULL
                                          pixelsWide:win_w
                                          pixelsHigh:win_h
                                       bitsPerSample:8
                                     samplesPerPixel:4
                                            hasAlpha:YES
                                            isPlanar:NO
                                      colorSpaceName:NSDeviceRGBColorSpace
                                         bytesPerRow:win_w * 4
                                        bitsPerPixel:32] autorelease];
                        uint8_t *dst = [rep bitmapData];
                        for (uint32_t p = 0; p < win_w * win_h; p++) {
                            dst[p * 4 + 0] = bgra[p * 4 + 2];
                            dst[p * 4 + 1] = bgra[p * 4 + 1];
                            dst[p * 4 + 2] = bgra[p * 4 + 0];
                            dst[p * 4 + 3] = 255;
                        }
                        NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                        [png writeToFile:[NSString stringWithUTF8String:screenshot_png_path] atomically:YES];
                        pfn_vkUnmapMemory(dev, smem);
                    }
                    pfn_vkFreeMemory(dev, smem, NULL);
                    pfn_vkDestroyBuffer(dev, sbuf, NULL);
                }
            }
        }

        double total_sec = mono_sec() - t_start;
        double avg_fps = total_sec > 0.0 ? (double)total_frames / total_sec : 0.0;
        double avg_ms = avg_fps > 0.0 ? 1000.0 / avg_fps : 0.0;
        printf("=== MX250 PRIME Desktop Benchmark Complete ===\n");
        printf("    Frames Presented : %llu frames in %.2f s\n",
               (unsigned long long)total_frames, total_sec);
        printf("    Average FPS      : %.2f FPS (%.2f ms / frame)\n", avg_fps, avg_ms);

        pfn_vkDeviceWaitIdle(dev);
        for (int i = 0; i < 2; i++) {
            pfn_vkDestroySemaphore(dev, sem_acq[i], NULL);
            pfn_vkDestroySemaphore(dev, sem_ren[i], NULL);
            pfn_vkDestroyFence(dev, fences[i], NULL);
        }
        pfn_vkDestroyCommandPool(dev, pool, NULL);
        pfn_vkDestroyPipeline(dev, pipeline, NULL);
        pfn_vkDestroyPipelineLayout(dev, playout, NULL);
        pfn_vkDestroyShaderModule(dev, sm_v, NULL);
        pfn_vkDestroyShaderModule(dev, sm_f, NULL);
        pfn_vkDestroyBuffer(dev, vb, NULL);
        pfn_vkFreeMemory(dev, vb_mem, NULL);
        pfn_vkDestroyImageView(dev, depth_view, NULL);
        pfn_vkDestroyImage(dev, depth_img, NULL);
        pfn_vkFreeMemory(dev, depth_mem, NULL);
        for (uint32_t i = 0; i < img_cnt; i++)
            pfn_vkDestroyImageView(dev, swap_views[i], NULL);
        pfn_vkDestroySwapchainKHR(dev, swapchain, NULL);
        pfn_vkDestroyDevice(dev, NULL);
        pfn_vkDestroySurfaceKHR(instance, surface, NULL);
        pfn_vkDestroyInstance(instance, NULL);
        [window orderOut:nil];
        dlclose(icd);
        return (total_frames >= 30) ? 0 : -1;
    }
}
