/* SPDX-License-Identifier: MIT
 *
 * tools/mx250_metal_interpose.m -> build/libmx250_metal.dylib
 *
 * SIP-Safe Userspace Metal-to-Vulkan Translation & Zero-Copy IOSurface Layer
 * for NVIDIA GeForce MX250 (GP108M, Full-Blood P0) on macOS Sequoia.
 *
 * Architecture:
 *   Native macOS Metal App (SIP & AMFI 100% Enabled)
 *     -> Mach-O __DATA,__interpose (MTLCreateSystemDefaultDevice / MTLCopyAllDevices)
 *        + CAMetalLayer Zero-Copy IOSurface Drawable Engine
 *     -> In-process MSL / metallib -> AIR Bitcode -> GPUCompiler libLLVM.dylib (.ll)
 *     -> Dynamic dlopen(libnvmtl_translate.dylib) [AIR -> Vulkan 1.3 SPIR-V + JSON Reflect]
 *     -> Dynamic dlopen(libvulkan_nvk.dylib) [Mesa 26.3.1 NVK + NAK SM61 Compiler]
 *     -> gp108-rmd (NVIDIA 470.256.02 RM P0 1296 MHz / 7008 Mbps GDDR5 + 0xc1b5 Copy Engine)
 *     -> Zero-Copy IOSurface Physical DDR4 Pages -> WindowServer (Intel UHD 620 1600x900 HiDPI)
 */

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <objc/message.h>
#import <objc/runtime.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private bitcode accessor on Apple's _MTLFunction (used strictly offline for AIR extraction) */
@interface NSObject (MX250AppleMTLBitcode)
- (NSData *)bitcodeData;
@end

/* LLVM C API types (resolved dynamically from macOS GPUCompiler.framework/libLLVM.dylib) */
typedef void *LLVMContextRef;
typedef void *LLVMModuleRef;
typedef void *LLVMMemoryBufferRef;

/* Dynamic Translator C ABI (libnvmtl_translate.dylib) */
typedef int  (*PFN_nvmtl_reflect)(const char *ll, const char *stage, uint8_t **out, size_t *out_len, char *err, size_t err_len);
typedef int  (*PFN_nvmtl_translate)(const char *ll, const char *stage, uint8_t **out, size_t *out_len, char *err, size_t err_len);
typedef void (*PFN_nvmtl_translate_free)(uint8_t *ptr, size_t len);
typedef void (*PFN_nvmtl_set_tg_lens)(const uint32_t *lens, uint32_t count);
typedef void (*PFN_nvmtl_set_rsc)(uint32_t samples);

/* Zero-Copy IOSurface TLS hook exported by our libvulkan_nouveau.dylib */
typedef void (*PFN_gp108_vk_set_next_alloc_iosurface_id)(uint32_t iosurface_id);
typedef bool (*PFN_gp108_vk_was_last_iosurface_imported)(void);

/* Forward declarations of Objective-C classes */
@class MX250MTLDevice;
@class MX250MTLBuffer;
@class MX250MTLTexture;
@class MX250MTLSamplerState;
@class MX250MTLDepthStencilState;
@class MX250MTLFunction;
@class MX250MTLLibrary;
@class MX250MTLComputePipelineState;
@class MX250MTLRenderPipelineState;
@class MX250MTLCommandQueue;
@class MX250MTLCommandBuffer;
@class MX250MTLDrawable;

#define MX250_MAX_BUFFERS   32
#define MX250_MAX_TEXTURES  32
#define MX250_MAX_SAMPLERS  16

typedef struct MX250ShaderBindingInfo {
    uint32_t metal_index;
    uint32_t vk_binding;
    uint32_t kind; /* 0=Buffer, 1=SampledImage, 2=StorageImage, 3=Sampler, 4=StaticSampler */
    VkSampler static_vk_sampler;
} MX250ShaderBindingInfo;

/* ========================================================================= *
 * 1. Global Runtime Table (Vulkan + LLVM + AIR->SPIR-V Translator)          *
 * ========================================================================= */
typedef struct MX250Runtime {
    pthread_mutex_t lock;
    bool initialized;
    bool log_enabled;

    id<MTLDevice> (*real_MTLCreateSystemDefaultDevice)(void);
    NSArray<id<MTLDevice>> *(*real_MTLCopyAllDevices)(void);
    id<MTLDevice> compiler_device;

    /* LLVM Bitcode Disassembler */
    void *llvm_lib;
    LLVMContextRef (*LLVMContextCreate)(void);
    void (*LLVMContextDispose)(LLVMContextRef);
    LLVMMemoryBufferRef (*LLVMCreateMemoryBufferWithMemoryRange)(const char *, size_t, const char *, int);
    void (*LLVMDisposeMemoryBuffer)(LLVMMemoryBufferRef);
    int (*LLVMParseBitcodeInContext2)(LLVMContextRef, LLVMMemoryBufferRef, LLVMModuleRef *);
    char *(*LLVMPrintModuleToString)(LLVMModuleRef);
    void (*LLVMDisposeMessage)(char *);
    void (*LLVMDisposeModule)(LLVMModuleRef);

    /* AIR -> SPIR-V Translator */
    void *tr_lib;
    PFN_nvmtl_reflect        nvmtl_reflect;
    PFN_nvmtl_translate      nvmtl_translate;
    PFN_nvmtl_translate_free nvmtl_free;
    PFN_nvmtl_set_tg_lens    nvmtl_set_tg_lens;
    PFN_nvmtl_set_rsc        nvmtl_set_rsc;

    /* Mesa NVK Vulkan 1.3 ICD */
    void *vk_lib;
    PFN_gp108_vk_set_next_alloc_iosurface_id set_next_iosurface_id;
    PFN_gp108_vk_was_last_iosurface_imported was_last_iosurface_ok;

    VkInstance                       vk_inst;
    VkPhysicalDevice                 vk_pdev;
    VkPhysicalDeviceProperties       vk_props;
    VkPhysicalDeviceMemoryProperties vk_mem_props;
    VkDevice                         vk_dev;
    VkQueue                          vk_queue;
    pthread_mutex_t                  queue_lock;
    VkCommandPool                    vk_cmd_pool;
    VkDescriptorPool                 vk_desc_pool;
    VkSampler                        default_sampler;
    VkBuffer                         dummy_buf;
    VkDeviceMemory                   dummy_mem;

    /* Vulkan Function Pointers */
    PFN_vkGetInstanceProcAddr                vkGetInstanceProcAddr;
    PFN_vkCreateInstance                     vkCreateInstance;
    PFN_vkEnumeratePhysicalDevices           vkEnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties        vkGetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties  vkGetPhysicalDeviceMemoryProperties;
    PFN_vkCreateDevice                       vkCreateDevice;
    PFN_vkGetDeviceProcAddr                  vkGetDeviceProcAddr;
    PFN_vkGetDeviceQueue                     vkGetDeviceQueue;
    PFN_vkDeviceWaitIdle                     vkDeviceWaitIdle;
    PFN_vkCreateCommandPool                  vkCreateCommandPool;
    PFN_vkAllocateCommandBuffers             vkAllocateCommandBuffers;
    PFN_vkFreeCommandBuffers                 vkFreeCommandBuffers;
    PFN_vkBeginCommandBuffer                 vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer                   vkEndCommandBuffer;
    PFN_vkResetCommandBuffer                 vkResetCommandBuffer;
    PFN_vkQueueSubmit                        vkQueueSubmit;
    PFN_vkQueueWaitIdle                      vkQueueWaitIdle;
    PFN_vkCreateFence                        vkCreateFence;
    PFN_vkDestroyFence                       vkDestroyFence;
    PFN_vkWaitForFences                      vkWaitForFences;
    PFN_vkResetFences                        vkResetFences;
    PFN_vkCreateBuffer                       vkCreateBuffer;
    PFN_vkDestroyBuffer                      vkDestroyBuffer;
    PFN_vkGetBufferMemoryRequirements        vkGetBufferMemoryRequirements;
    PFN_vkBindBufferMemory                   vkBindBufferMemory;
    PFN_vkCreateImage                        vkCreateImage;
    PFN_vkDestroyImage                       vkDestroyImage;
    PFN_vkGetImageMemoryRequirements         vkGetImageMemoryRequirements;
    PFN_vkBindImageMemory                    vkBindImageMemory;
    PFN_vkCreateImageView                    vkCreateImageView;
    PFN_vkDestroyImageView                   vkDestroyImageView;
    PFN_vkCreateSampler                      vkCreateSampler;
    PFN_vkDestroySampler                     vkDestroySampler;
    PFN_vkAllocateMemory                     vkAllocateMemory;
    PFN_vkFreeMemory                         vkFreeMemory;
    PFN_vkMapMemory                          vkMapMemory;
    PFN_vkUnmapMemory                        vkUnmapMemory;
    PFN_vkCreateShaderModule                 vkCreateShaderModule;
    PFN_vkDestroyShaderModule                vkDestroyShaderModule;
    PFN_vkCreateDescriptorSetLayout          vkCreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout         vkDestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool               vkCreateDescriptorPool;
    PFN_vkResetDescriptorPool                vkResetDescriptorPool;
    PFN_vkAllocateDescriptorSets             vkAllocateDescriptorSets;
    PFN_vkFreeDescriptorSets                 vkFreeDescriptorSets;
    PFN_vkUpdateDescriptorSets               vkUpdateDescriptorSets;
    PFN_vkCreatePipelineLayout               vkCreatePipelineLayout;
    PFN_vkDestroyPipelineLayout              vkDestroyPipelineLayout;
    PFN_vkCreateComputePipelines             vkCreateComputePipelines;
    PFN_vkCreateGraphicsPipelines            vkCreateGraphicsPipelines;
    PFN_vkDestroyPipeline                    vkDestroyPipeline;
    PFN_vkCmdBindPipeline                    vkCmdBindPipeline;
    PFN_vkCmdBindDescriptorSets              vkCmdBindDescriptorSets;
    PFN_vkCmdPushConstants                   vkCmdPushConstants;
    PFN_vkCmdDispatch                        vkCmdDispatch;
    PFN_vkCmdBeginRendering                  vkCmdBeginRendering;
    PFN_vkCmdEndRendering                    vkCmdEndRendering;
    PFN_vkCmdSetViewport                     vkCmdSetViewport;
    PFN_vkCmdSetScissor                      vkCmdSetScissor;
    PFN_vkCmdBindVertexBuffers               vkCmdBindVertexBuffers;
    PFN_vkCmdBindIndexBuffer                 vkCmdBindIndexBuffer;
    PFN_vkCmdDraw                            vkCmdDraw;
    PFN_vkCmdDrawIndexed                     vkCmdDrawIndexed;
    PFN_vkCmdPipelineBarrier                 vkCmdPipelineBarrier;
    PFN_vkCmdCopyBuffer                      vkCmdCopyBuffer;
    PFN_vkCmdCopyBufferToImage               vkCmdCopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer               vkCmdCopyImageToBuffer;
    PFN_vkCmdFillBuffer                      vkCmdFillBuffer;
} MX250Runtime;

static MX250Runtime g_rt = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .queue_lock = PTHREAD_MUTEX_INITIALIZER,
};

static NSString *mx250_dylib_dir(void) {
    Dl_info info;
    if (dladdr((const void *)&mx250_dylib_dir, &info) && info.dli_fname) {
        NSString *p = [NSString stringWithUTF8String:info.dli_fname];
        return [p stringByDeletingLastPathComponent];
    }
    return @"/Users/maple/projects/temp/mx250-macos/build";
}

static uint32_t mx250_find_mem_type(uint32_t type_bits, VkMemoryPropertyFlags req_flags, VkMemoryPropertyFlags prefer_no_flags) {
    for (uint32_t i = 0; i < g_rt.vk_mem_props.memoryTypeCount; i++) {
        VkMemoryPropertyFlags pf = g_rt.vk_mem_props.memoryTypes[i].propertyFlags;
        if ((type_bits & (1u << i)) && (pf & req_flags) == req_flags && (pf & prefer_no_flags) == 0)
            return i;
    }
    for (uint32_t i = 0; i < g_rt.vk_mem_props.memoryTypeCount; i++) {
        VkMemoryPropertyFlags pf = g_rt.vk_mem_props.memoryTypes[i].propertyFlags;
        if ((type_bits & (1u << i)) && (pf & req_flags) == req_flags)
            return i;
    }
    for (uint32_t i = 0; i < g_rt.vk_mem_props.memoryTypeCount; i++) {
        if (type_bits & (1u << i))
            return i;
    }
    return 0;
}

NS_RETURNS_RETAINED id<MTLDevice> mx250_MTLCreateSystemDefaultDevice(void);
NS_RETURNS_RETAINED NSArray<id<MTLDevice>> *mx250_MTLCopyAllDevices(void);

static bool mx250_runtime_init(void) {
    pthread_mutex_lock(&g_rt.lock);
    if (g_rt.initialized) {
        pthread_mutex_unlock(&g_rt.lock);
        return true;
    }

    g_rt.log_enabled = (getenv("MX250_METAL_QUIET") == NULL);

    id<NSObject> dev_obs = nil;
    NSArray<id<MTLDevice>> *sys_devs = MTLCopyAllDevicesWithObserver(&dev_obs, ^(id<MTLDevice> d, MTLDeviceNotificationName n) {
        (void)d; (void)n;
    });
    g_rt.compiler_device = sys_devs.firstObject;
    if (dev_obs) MTLRemoveDeviceObserver(dev_obs);

    /* 1. Load macOS built-in GPUCompiler libLLVM.dylib */
    const char *llvm_paths[] = {
        "/System/Library/PrivateFrameworks/GPUCompiler.framework/Versions/32023/Libraries/libLLVM.dylib",
        "/System/Library/PrivateFrameworks/GPUCompiler.framework/Versions/Current/Libraries/libLLVM.dylib",
        NULL
    };
    for (int i = 0; llvm_paths[i]; i++) {
        g_rt.llvm_lib = dlopen(llvm_paths[i], RTLD_LAZY | RTLD_LOCAL);
        if (g_rt.llvm_lib) break;
    }
    if (!g_rt.llvm_lib) {
        fprintf(stderr, "[MX250-Metal] Failed to dlopen GPUCompiler libLLVM.dylib: %s\n", dlerror());
        pthread_mutex_unlock(&g_rt.lock);
        return false;
    }
    g_rt.LLVMContextCreate = dlsym(g_rt.llvm_lib, "LLVMContextCreate");
    g_rt.LLVMContextDispose = dlsym(g_rt.llvm_lib, "LLVMContextDispose");
    g_rt.LLVMCreateMemoryBufferWithMemoryRange = dlsym(g_rt.llvm_lib, "LLVMCreateMemoryBufferWithMemoryRange");
    g_rt.LLVMDisposeMemoryBuffer = dlsym(g_rt.llvm_lib, "LLVMDisposeMemoryBuffer");
    g_rt.LLVMParseBitcodeInContext2 = dlsym(g_rt.llvm_lib, "LLVMParseBitcodeInContext2");
    g_rt.LLVMPrintModuleToString = dlsym(g_rt.llvm_lib, "LLVMPrintModuleToString");
    g_rt.LLVMDisposeMessage = dlsym(g_rt.llvm_lib, "LLVMDisposeMessage");
    g_rt.LLVMDisposeModule = dlsym(g_rt.llvm_lib, "LLVMDisposeModule");

    /* 2. Load LGPL-3.0 AIR->SPIR-V Translator dynamically via dlopen */
    NSString *dir = mx250_dylib_dir();
    const char *env_tr = getenv("MX250_TRANSLATOR_DYLIB");
    NSString *tr_path = env_tr ? [NSString stringWithUTF8String:env_tr]
                               : [dir stringByAppendingPathComponent:@"libnvmtl_translate.dylib"];
    g_rt.tr_lib = dlopen([tr_path UTF8String], RTLD_NOW | RTLD_LOCAL);
    if (!g_rt.tr_lib) {
        fprintf(stderr, "[MX250-Metal] Failed to dlopen(%s): %s\n", [tr_path UTF8String], dlerror());
        pthread_mutex_unlock(&g_rt.lock);
        return false;
    }
    g_rt.nvmtl_reflect      = (PFN_nvmtl_reflect)dlsym(g_rt.tr_lib, "nvmtl_reflect");
    g_rt.nvmtl_translate    = (PFN_nvmtl_translate)dlsym(g_rt.tr_lib, "nvmtl_translate");
    g_rt.nvmtl_free         = (PFN_nvmtl_translate_free)dlsym(g_rt.tr_lib, "nvmtl_translate_free");
    g_rt.nvmtl_set_tg_lens  = (PFN_nvmtl_set_tg_lens)dlsym(g_rt.tr_lib, "nvmtl_translate_set_threadgroup_lengths");
    g_rt.nvmtl_set_rsc      = (PFN_nvmtl_set_rsc)dlsym(g_rt.tr_lib, "nvmtl_translate_set_raster_sample_count");

    /* 3. Load Mesa 26.3.1 NVK Vulkan 1.3 ICD */
    const char *env_vk = getenv("MX250_VK_ICD_DYLIB");
    NSString *vk_path = env_vk ? [NSString stringWithUTF8String:env_vk]
                               : [dir stringByAppendingPathComponent:@"libvulkan_nvk.dylib"];
    g_rt.vk_lib = dlopen([vk_path UTF8String], RTLD_NOW | RTLD_LOCAL);
    if (!g_rt.vk_lib) {
        fprintf(stderr, "[MX250-Metal] Failed to dlopen(%s): %s\n", [vk_path UTF8String], dlerror());
        pthread_mutex_unlock(&g_rt.lock);
        return false;
    }
    g_rt.set_next_iosurface_id = (PFN_gp108_vk_set_next_alloc_iosurface_id)dlsym(g_rt.vk_lib, "gp108_vk_set_next_alloc_iosurface_id");
    g_rt.was_last_iosurface_ok = (PFN_gp108_vk_was_last_iosurface_imported)dlsym(g_rt.vk_lib, "gp108_vk_was_last_iosurface_imported");

    g_rt.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(g_rt.vk_lib, "vk_icdGetInstanceProcAddr");
    if (!g_rt.vkGetInstanceProcAddr)
        g_rt.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(g_rt.vk_lib, "vkGetInstanceProcAddr");

    g_rt.vkCreateInstance = (PFN_vkCreateInstance)g_rt.vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "MX250-Metal-Interposer",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "MX250MetalOnVulkan",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    if (g_rt.vkCreateInstance(&ici, NULL, &g_rt.vk_inst) != VK_SUCCESS) {
        fprintf(stderr, "[MX250-Metal] vkCreateInstance failed\n");
        pthread_mutex_unlock(&g_rt.lock);
        return false;
    }

#define LOAD_IFN(fn) g_rt.fn = (PFN_##fn)g_rt.vkGetInstanceProcAddr(g_rt.vk_inst, #fn)
    LOAD_IFN(vkEnumeratePhysicalDevices);
    LOAD_IFN(vkGetPhysicalDeviceProperties);
    LOAD_IFN(vkGetPhysicalDeviceMemoryProperties);
    LOAD_IFN(vkCreateDevice);
    LOAD_IFN(vkGetDeviceProcAddr);
#undef LOAD_IFN

    uint32_t pd_cnt = 1;
    if (g_rt.vkEnumeratePhysicalDevices(g_rt.vk_inst, &pd_cnt, &g_rt.vk_pdev) != VK_SUCCESS || pd_cnt == 0) {
        fprintf(stderr, "[MX250-Metal] No MX250 Vulkan physical device found (is gp108-rmd running?)\n");
        pthread_mutex_unlock(&g_rt.lock);
        return false;
    }
    g_rt.vkGetPhysicalDeviceProperties(g_rt.vk_pdev, &g_rt.vk_props);
    g_rt.vkGetPhysicalDeviceMemoryProperties(g_rt.vk_pdev, &g_rt.vk_mem_props);

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &qprio,
    };
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
        .maintenance4 = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features f12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &f13,
        .scalarBlockLayout = VK_TRUE,
        .uniformAndStorageBuffer8BitAccess = VK_TRUE,
        .storageBuffer8BitAccess = VK_TRUE,
        .shaderFloat16 = VK_FALSE,
    };
    VkPhysicalDeviceVulkan11Features f11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &f12,
        .storageBuffer16BitAccess = VK_TRUE,
        .uniformAndStorageBuffer16BitAccess = VK_TRUE,
    };
    VkPhysicalDeviceFeatures f10 = {
        .shaderInt64 = VK_TRUE,
        .shaderInt16 = VK_TRUE,
        .fragmentStoresAndAtomics = VK_TRUE,
        .vertexPipelineStoresAndAtomics = VK_TRUE,
        .shaderStorageImageExtendedFormats = VK_TRUE,
        .shaderStorageImageReadWithoutFormat = VK_TRUE,
        .shaderStorageImageWriteWithoutFormat = VK_TRUE,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &f11,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .pEnabledFeatures = &f10,
    };
    if (g_rt.vkCreateDevice(g_rt.vk_pdev, &dci, NULL, &g_rt.vk_dev) != VK_SUCCESS) {
        fprintf(stderr, "[MX250-Metal] vkCreateDevice failed\n");
        pthread_mutex_unlock(&g_rt.lock);
        return false;
    }

#define LOAD_DFN(fn) g_rt.fn = (PFN_##fn)g_rt.vkGetDeviceProcAddr(g_rt.vk_dev, #fn)
    LOAD_DFN(vkGetDeviceQueue);
    LOAD_DFN(vkDeviceWaitIdle);
    LOAD_DFN(vkCreateCommandPool);
    LOAD_DFN(vkAllocateCommandBuffers);
    LOAD_DFN(vkFreeCommandBuffers);
    LOAD_DFN(vkBeginCommandBuffer);
    LOAD_DFN(vkEndCommandBuffer);
    LOAD_DFN(vkResetCommandBuffer);
    LOAD_DFN(vkQueueSubmit);
    LOAD_DFN(vkQueueWaitIdle);
    LOAD_DFN(vkCreateFence);
    LOAD_DFN(vkDestroyFence);
    LOAD_DFN(vkWaitForFences);
    LOAD_DFN(vkResetFences);
    LOAD_DFN(vkCreateBuffer);
    LOAD_DFN(vkDestroyBuffer);
    LOAD_DFN(vkGetBufferMemoryRequirements);
    LOAD_DFN(vkBindBufferMemory);
    LOAD_DFN(vkCreateImage);
    LOAD_DFN(vkDestroyImage);
    LOAD_DFN(vkGetImageMemoryRequirements);
    LOAD_DFN(vkBindImageMemory);
    LOAD_DFN(vkCreateImageView);
    LOAD_DFN(vkDestroyImageView);
    LOAD_DFN(vkCreateSampler);
    LOAD_DFN(vkDestroySampler);
    LOAD_DFN(vkAllocateMemory);
    LOAD_DFN(vkFreeMemory);
    LOAD_DFN(vkMapMemory);
    LOAD_DFN(vkUnmapMemory);
    LOAD_DFN(vkCreateShaderModule);
    LOAD_DFN(vkDestroyShaderModule);
    LOAD_DFN(vkCreateDescriptorSetLayout);
    LOAD_DFN(vkDestroyDescriptorSetLayout);
    LOAD_DFN(vkCreateDescriptorPool);
    LOAD_DFN(vkResetDescriptorPool);
    LOAD_DFN(vkAllocateDescriptorSets);
    LOAD_DFN(vkFreeDescriptorSets);
    LOAD_DFN(vkUpdateDescriptorSets);
    LOAD_DFN(vkCreatePipelineLayout);
    LOAD_DFN(vkDestroyPipelineLayout);
    LOAD_DFN(vkCreateComputePipelines);
    LOAD_DFN(vkCreateGraphicsPipelines);
    LOAD_DFN(vkDestroyPipeline);
    LOAD_DFN(vkCmdBindPipeline);
    LOAD_DFN(vkCmdBindDescriptorSets);
    LOAD_DFN(vkCmdPushConstants);
    LOAD_DFN(vkCmdDispatch);
    LOAD_DFN(vkCmdBeginRendering);
    LOAD_DFN(vkCmdEndRendering);
    LOAD_DFN(vkCmdSetViewport);
    LOAD_DFN(vkCmdSetScissor);
    LOAD_DFN(vkCmdBindVertexBuffers);
    LOAD_DFN(vkCmdBindIndexBuffer);
    LOAD_DFN(vkCmdDraw);
    LOAD_DFN(vkCmdDrawIndexed);
    LOAD_DFN(vkCmdPipelineBarrier);
    LOAD_DFN(vkCmdCopyBuffer);
    LOAD_DFN(vkCmdCopyBufferToImage);
    LOAD_DFN(vkCmdCopyImageToBuffer);
    LOAD_DFN(vkCmdFillBuffer);
#undef LOAD_DFN

    g_rt.vkGetDeviceQueue(g_rt.vk_dev, 0, 0, &g_rt.vk_queue);

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0,
    };
    g_rt.vkCreateCommandPool(g_rt.vk_dev, &cpci, NULL, &g_rt.vk_cmd_pool);

    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1024 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  512  },
        { VK_DESCRIPTOR_TYPE_SAMPLER,        512  },
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 2048,
        .poolSizeCount = 4,
        .pPoolSizes = pool_sizes,
    };
    g_rt.vkCreateDescriptorPool(g_rt.vk_dev, &dpci, NULL, &g_rt.vk_desc_pool);

    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxAnisotropy = 1.0f,
        .maxLod = 1000.0f,
    };
    g_rt.vkCreateSampler(g_rt.vk_dev, &sci, NULL, &g_rt.default_sampler);

    /* 4 KB dummy storage buffer for any unbound shader slots */
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    };
    g_rt.vkCreateBuffer(g_rt.vk_dev, &bci, NULL, &g_rt.dummy_buf);
    VkMemoryRequirements mr;
    g_rt.vkGetBufferMemoryRequirements(g_rt.vk_dev, g_rt.dummy_buf, &mr);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mx250_find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    g_rt.vkAllocateMemory(g_rt.vk_dev, &mai, NULL, &g_rt.dummy_mem);
    g_rt.vkBindBufferMemory(g_rt.vk_dev, g_rt.dummy_buf, g_rt.dummy_mem, 0);

    g_rt.initialized = true;
    if (g_rt.log_enabled) {
        fprintf(stderr,
                "[MX250-Metal] SIP-Safe Userspace Metal-on-Vulkan Active -> %s (Vulkan %u.%u.%u)\n",
                g_rt.vk_props.deviceName,
                VK_VERSION_MAJOR(g_rt.vk_props.apiVersion),
                VK_VERSION_MINOR(g_rt.vk_props.apiVersion),
                VK_VERSION_PATCH(g_rt.vk_props.apiVersion));
    }
    pthread_mutex_unlock(&g_rt.lock);
    return true;
}

/* ========================================================================= *
 * 2. Format & Bitcode -> SPIR-V Helpers                                     *
 * ========================================================================= */
static VkFormat mx250_mtl_to_vk_format(MTLPixelFormat fmt) {
    switch (fmt) {
    case MTLPixelFormatBGRA8Unorm:      return VK_FORMAT_B8G8R8A8_UNORM;
    case MTLPixelFormatBGRA8Unorm_sRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case MTLPixelFormatRGBA8Unorm:      return VK_FORMAT_R8G8B8A8_UNORM;
    case MTLPixelFormatRGBA8Unorm_sRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case MTLPixelFormatRGBA16Float:     return VK_FORMAT_R16G16B16A16_SFLOAT;
    case MTLPixelFormatRGBA32Float:     return VK_FORMAT_R32G32B32A32_SFLOAT;
    case MTLPixelFormatR32Float:        return VK_FORMAT_R32_SFLOAT;
    case MTLPixelFormatR32Uint:         return VK_FORMAT_R32_UINT;
    case MTLPixelFormatR8Unorm:         return VK_FORMAT_R8_UNORM;
    case MTLPixelFormatRGB10A2Unorm:    return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case MTLPixelFormatBGR10A2Unorm:    return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    case MTLPixelFormatDepth32Float:    return VK_FORMAT_D32_SFLOAT;
    default:                            return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

static uint32_t mx250_mtl_bpp(MTLPixelFormat fmt) {
    switch (fmt) {
    case MTLPixelFormatR8Unorm:      return 1;
    case MTLPixelFormatRGBA16Float:  return 8;
    case MTLPixelFormatRGBA32Float:  return 16;
    default:                         return 4;
    }
}

static VkFormat mx250_mtl_vertex_to_vk_format(MTLVertexFormat vf) {
    switch (vf) {
    case MTLVertexFormatFloat:            return VK_FORMAT_R32_SFLOAT;
    case MTLVertexFormatFloat2:           return VK_FORMAT_R32G32_SFLOAT;
    case MTLVertexFormatFloat3:           return VK_FORMAT_R32G32B32_SFLOAT;
    case MTLVertexFormatFloat4:           return VK_FORMAT_R32G32B32A32_SFLOAT;
    case MTLVertexFormatHalf2:            return VK_FORMAT_R16G16_SFLOAT;
    case MTLVertexFormatHalf4:            return VK_FORMAT_R16G16B16A16_SFLOAT;
    case MTLVertexFormatUChar4Normalized: return VK_FORMAT_R8G8B8A8_UNORM;
    case MTLVertexFormatInt:              return VK_FORMAT_R32_SINT;
    case MTLVertexFormatInt2:             return VK_FORMAT_R32G32_SINT;
    case MTLVertexFormatInt3:             return VK_FORMAT_R32G32B32_SINT;
    case MTLVertexFormatInt4:             return VK_FORMAT_R32G32B32A32_SINT;
    case MTLVertexFormatUInt:             return VK_FORMAT_R32_UINT;
    case MTLVertexFormatUInt2:            return VK_FORMAT_R32G32_UINT;
    case MTLVertexFormatUInt3:            return VK_FORMAT_R32G32B32_UINT;
    case MTLVertexFormatUInt4:            return VK_FORMAT_R32G32B32A32_UINT;
    default:                              return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkCompareOp mx250_mtl_compare_to_vk(MTLCompareFunction fn) {
    switch (fn) {
    case MTLCompareFunctionNever:        return VK_COMPARE_OP_NEVER;
    case MTLCompareFunctionLess:         return VK_COMPARE_OP_LESS;
    case MTLCompareFunctionEqual:        return VK_COMPARE_OP_EQUAL;
    case MTLCompareFunctionLessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
    case MTLCompareFunctionGreater:      return VK_COMPARE_OP_GREATER;
    case MTLCompareFunctionNotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
    case MTLCompareFunctionGreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case MTLCompareFunctionAlways:       return VK_COMPARE_OP_ALWAYS;
    default:                             return VK_COMPARE_OP_ALWAYS;
    }
}

static NSString *mx250_disassemble_air_bitcode(NSData *bc) {
    if (!bc || bc.length < 4) return nil;
    LLVMContextRef ctx = g_rt.LLVMContextCreate();
    LLVMMemoryBufferRef mb = g_rt.LLVMCreateMemoryBufferWithMemoryRange((const char *)bc.bytes, bc.length, "air", 0);
    LLVMModuleRef mod = NULL;
    if (g_rt.LLVMParseBitcodeInContext2(ctx, mb, &mod) != 0 || !mod) {
        g_rt.LLVMDisposeMemoryBuffer(mb);
        g_rt.LLVMContextDispose(ctx);
        return nil;
    }
    char *raw = g_rt.LLVMPrintModuleToString(mod);
    NSMutableString *san = [NSMutableString stringWithCapacity:strlen(raw)];
    NSString *rawStr = [NSString stringWithUTF8String:raw];
    for (NSString *line in [rawStr componentsSeparatedByString:@"\n"]) {
        NSString *t = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([t hasPrefix:@"target triple"]) {
            [san appendString:@"target triple = \"spirv-unknown-vulkan1.2\"\n"];
        } else if ([t hasPrefix:@"; ModuleID ="] ||
                   [t hasPrefix:@"@llvm.global_ctors"] ||
                   [t hasPrefix:@"@llvm.global_dtors"] ||
                   [t hasPrefix:@"@llvm.used"] ||
                   [t hasPrefix:@"@llvm.compiler.used"]) {
            continue;
        } else {
            [san appendString:line];
            [san appendString:@"\n"];
        }
    }
    g_rt.LLVMDisposeMessage(raw);
    g_rt.LLVMDisposeModule(mod);
    g_rt.LLVMDisposeMemoryBuffer(mb);
    g_rt.LLVMContextDispose(ctx);
    return san;
}

/* Rewrites DescriptorSet 0 -> 1 in fragment SPIR-V so Vertex (set 0) & Fragment (set 1) never collide */
static void mx250_patch_spirv_descriptor_set(uint32_t *words, size_t word_count, uint32_t new_set) {
    size_t i = 5;
    while (i < word_count) {
        uint16_t wc = (uint16_t)(words[i] >> 16);
        uint16_t op = (uint16_t)(words[i] & 0xffffu);
        if (wc == 0 || op == 54 /* OpFunction */) break;
        if (op == 71 /* OpDecorate */ && wc >= 4 && words[i + 2] == 34 /* DescriptorSet */ && words[i + 3] == 0) {
            words[i + 3] = new_set;
        }
        i += wc;
    }
}

static VkSampler mx250_create_static_sampler_from_dict(NSDictionary *ss) {
    if (!ss) return g_rt.default_sampler;
    VkFilter magF = [[ss[@"mag_filter"] description] isEqualToString:@"Nearest"] ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    VkFilter minF = [[ss[@"min_filter"] description] isEqualToString:@"Nearest"] ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    VkSamplerMipmapMode mipM = [[ss[@"mip_filter"] description] isEqualToString:@"Linear"]
                                   ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    auto VkSamplerAddressMode (^mapAddr)(NSString *) = ^VkSamplerAddressMode(NSString *s) {
        if ([s isEqualToString:@"Repeat"]) return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if ([s isEqualToString:@"MirroredRepeat"]) return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        if ([s isEqualToString:@"ClampToZero"] || [s isEqualToString:@"ClampToBorder"])
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    };
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = magF,
        .minFilter = minF,
        .mipmapMode = mipM,
        .addressModeU = mapAddr([ss[@"address_mode_s"] description]),
        .addressModeV = mapAddr([ss[@"address_mode_t"] description]),
        .addressModeW = mapAddr([ss[@"address_mode_r"] description]),
        .maxAnisotropy = 1.0f,
        .minLod = ss[@"lod_min_clamp"] ? [ss[@"lod_min_clamp"] floatValue] : 0.0f,
        .maxLod = ss[@"lod_max_clamp"] ? [ss[@"lod_max_clamp"] floatValue] : 1000.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    VkSampler out = VK_NULL_HANDLE;
    if (g_rt.vkCreateSampler(g_rt.vk_dev, &sci, NULL, &out) != VK_SUCCESS)
        return g_rt.default_sampler;
    return out;
}

/* ========================================================================= *
 * 3. MX250 Metal Resource Classes (Buffer, Texture, Sampler, DepthStencil)  *
 * ========================================================================= */
@interface MX250MTLBuffer : NSObject <MTLBuffer> {
@public
    MX250MTLDevice *_dev;
    NSUInteger      _length;
    MTLResourceOptions _options;
    VkBuffer        _vkBuf;
    VkDeviceMemory  _vkMem;
    void           *_mapped;
}
@property(nullable, copy, atomic) NSString *label;
@end

@implementation MX250MTLBuffer
- (id<MTLDevice>)device { return (id<MTLDevice>)_dev; }
- (NSUInteger)length { return _length; }
- (void *)contents { return _mapped; }
- (MTLResourceOptions)resourceOptions { return _options; }
- (MTLStorageMode)storageMode {
    return ((_options & MTLResourceStorageModeMask) == MTLResourceStorageModePrivate)
               ? MTLStorageModePrivate : MTLStorageModeShared;
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { (void)state; return MTLPurgeableStateKeepCurrent; }
- (void)makeAliasable {}
- (BOOL)isAliasable { return NO; }
- (NSUInteger)allocatedSize { return _length; }
- (void)didModifyRange:(NSRange)range { (void)range; }
- (void)addDebugMarker:(NSString *)marker range:(NSRange)range { (void)marker; (void)range; }
- (void)removeAllDebugMarkers {}
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options deallocator:(void (^)(void *, NSUInteger))deallocator {
    (void)bytes; (void)length; (void)options; (void)deallocator; return nil;
}
- (nullable id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bytesPerRow {
    (void)descriptor; (void)offset; (void)bytesPerRow; return nil;
}
- (uint64_t)gpuAddress { return 0; }
- (void)dealloc {
    if (_vkBuf) g_rt.vkDestroyBuffer(g_rt.vk_dev, _vkBuf, NULL);
    if (_vkMem) g_rt.vkFreeMemory(g_rt.vk_dev, _vkMem, NULL);
}
@end

@interface MX250MTLTexture : NSObject <MTLTexture> {
@public
    MX250MTLDevice  *_dev;
    MTLPixelFormat   _pixelFormat;
    MTLTextureType   _textureType;
    MTLTextureUsage  _usage;
    NSUInteger       _width;
    NSUInteger       _height;
    NSUInteger       _depth;
    NSUInteger       _mipmapLevelCount;
    NSUInteger       _sampleCount;
    VkFormat         _vkFormat;
    VkImage          _vkImage;
    VkDeviceMemory   _vkMem;
    VkImageView      _vkView;
    VkImageLayout    _currentLayout;
    /* Optional Zero-Copy IOSurface backing (for CAMetalLayer drawables / IOSurface textures) */
    IOSurfaceRef     _iosurface;
    uint32_t         _iosurfacePitch;
    uint64_t         _iosurfaceAllocSize;
    VkBuffer         _iosurfaceVkBuf;
    VkDeviceMemory   _iosurfaceVkMem;
}
@property(nullable, copy, atomic) NSString *label;
@end

@implementation MX250MTLTexture
- (id<MTLDevice>)device { return (id<MTLDevice>)_dev; }
- (MTLPixelFormat)pixelFormat { return _pixelFormat; }
- (MTLTextureType)textureType { return _textureType; }
- (MTLTextureUsage)usage { return _usage; }
- (NSUInteger)width { return _width; }
- (NSUInteger)height { return _height; }
- (NSUInteger)depth { return _depth; }
- (NSUInteger)mipmapLevelCount { return _mipmapLevelCount; }
- (NSUInteger)sampleCount { return _sampleCount; }
- (NSUInteger)arrayLength { return 1; }
- (BOOL)isFramebufferOnly { return NO; }
- (IOSurfaceRef)iosurface { return _iosurface; }
- (NSUInteger)iosurfacePlane { return 0; }
- (MTLStorageMode)storageMode { return MTLStorageModePrivate; }
- (MTLResourceOptions)resourceOptions { return MTLResourceStorageModePrivate; }
- (NSUInteger)allocatedSize { return _width * _height * mx250_mtl_bpp(_pixelFormat); }

- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level withBytes:(const void *)pixelBytes bytesPerRow:(NSUInteger)bytesPerRow {
    if (!pixelBytes || region.size.width == 0 || region.size.height == 0) return;
    uint32_t bpp = mx250_mtl_bpp(_pixelFormat);
    uint32_t tight_row = (uint32_t)region.size.width * bpp;
    uint64_t total = (uint64_t)tight_row * region.size.height;

    VkBuffer stg_buf = VK_NULL_HANDLE;
    VkDeviceMemory stg_mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = total, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    g_rt.vkCreateBuffer(g_rt.vk_dev, &bci, NULL, &stg_buf);
    VkMemoryRequirements mr;
    g_rt.vkGetBufferMemoryRequirements(g_rt.vk_dev, stg_buf, &mr);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mx250_find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    g_rt.vkAllocateMemory(g_rt.vk_dev, &mai, NULL, &stg_mem);
    g_rt.vkBindBufferMemory(g_rt.vk_dev, stg_buf, stg_mem, 0);
    uint8_t *dst = NULL;
    g_rt.vkMapMemory(g_rt.vk_dev, stg_mem, 0, total, 0, (void **)&dst);
    for (NSUInteger y = 0; y < region.size.height; y++) {
        memcpy(dst + y * tight_row, (const uint8_t *)pixelBytes + y * bytesPerRow, tight_row);
    }
    g_rt.vkUnmapMemory(g_rt.vk_dev, stg_mem);

    pthread_mutex_lock(&g_rt.queue_lock);
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_rt.vk_cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    g_rt.vkAllocateCommandBuffers(g_rt.vk_dev, &cbai, &cmd);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    g_rt.vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier imb = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = _currentLayout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = _vkImage,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 1, 0, 1 },
    };
    g_rt.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &imb);

    VkBufferImageCopy bic = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 0, 1 },
        .imageOffset = { (int32_t)region.origin.x, (int32_t)region.origin.y, 0 },
        .imageExtent = { (uint32_t)region.size.width, (uint32_t)region.size.height, 1 },
    };
    g_rt.vkCmdCopyBufferToImage(cmd, stg_buf, _vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

    imb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_rt.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &imb);
    _currentLayout = VK_IMAGE_LAYOUT_GENERAL;

    g_rt.vkEndCommandBuffer(cmd);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    g_rt.vkQueueSubmit(g_rt.vk_queue, 1, &si, VK_NULL_HANDLE);
    g_rt.vkQueueWaitIdle(g_rt.vk_queue);
    g_rt.vkFreeCommandBuffers(g_rt.vk_dev, g_rt.vk_cmd_pool, 1, &cmd);
    pthread_mutex_unlock(&g_rt.queue_lock);

    g_rt.vkDestroyBuffer(g_rt.vk_dev, stg_buf, NULL);
    g_rt.vkFreeMemory(g_rt.vk_dev, stg_mem, NULL);
}

- (void)getBytes:(void *)pixelBytes bytesPerRow:(NSUInteger)bytesPerRow fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level {
    if (!pixelBytes || region.size.width == 0 || region.size.height == 0) return;
    uint32_t bpp = mx250_mtl_bpp(_pixelFormat);
    uint32_t tight_row = (uint32_t)region.size.width * bpp;
    uint64_t total = (uint64_t)tight_row * region.size.height;

    VkBuffer stg_buf = VK_NULL_HANDLE;
    VkDeviceMemory stg_mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = total, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    g_rt.vkCreateBuffer(g_rt.vk_dev, &bci, NULL, &stg_buf);
    VkMemoryRequirements mr;
    g_rt.vkGetBufferMemoryRequirements(g_rt.vk_dev, stg_buf, &mr);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mx250_find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    g_rt.vkAllocateMemory(g_rt.vk_dev, &mai, NULL, &stg_mem);
    g_rt.vkBindBufferMemory(g_rt.vk_dev, stg_buf, stg_mem, 0);

    pthread_mutex_lock(&g_rt.queue_lock);
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_rt.vk_cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    g_rt.vkAllocateCommandBuffers(g_rt.vk_dev, &cbai, &cmd);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    g_rt.vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier imb = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = (_currentLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_IMAGE_LAYOUT_GENERAL : _currentLayout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = _vkImage,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 1, 0, 1 },
    };
    g_rt.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &imb);

    VkBufferImageCopy bic = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 0, 1 },
        .imageOffset = { (int32_t)region.origin.x, (int32_t)region.origin.y, 0 },
        .imageExtent = { (uint32_t)region.size.width, (uint32_t)region.size.height, 1 },
    };
    g_rt.vkCmdCopyImageToBuffer(cmd, _vkImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stg_buf, 1, &bic);

    imb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    imb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_rt.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &imb);
    _currentLayout = VK_IMAGE_LAYOUT_GENERAL;

    g_rt.vkEndCommandBuffer(cmd);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    g_rt.vkQueueSubmit(g_rt.vk_queue, 1, &si, VK_NULL_HANDLE);
    g_rt.vkQueueWaitIdle(g_rt.vk_queue);
    g_rt.vkFreeCommandBuffers(g_rt.vk_dev, g_rt.vk_cmd_pool, 1, &cmd);
    pthread_mutex_unlock(&g_rt.queue_lock);

    const uint8_t *src = NULL;
    g_rt.vkMapMemory(g_rt.vk_dev, stg_mem, 0, total, 0, (void **)&src);
    for (NSUInteger y = 0; y < region.size.height; y++) {
        memcpy((uint8_t *)pixelBytes + y * bytesPerRow, src + y * tight_row, tight_row);
    }
    g_rt.vkUnmapMemory(g_rt.vk_dev, stg_mem);
    g_rt.vkDestroyBuffer(g_rt.vk_dev, stg_buf, NULL);
    g_rt.vkFreeMemory(g_rt.vk_dev, stg_mem, NULL);
}

- (void)dealloc {
    if (_iosurfaceVkBuf) g_rt.vkDestroyBuffer(g_rt.vk_dev, _iosurfaceVkBuf, NULL);
    if (_iosurfaceVkMem) g_rt.vkFreeMemory(g_rt.vk_dev, _iosurfaceVkMem, NULL);
    if (_iosurface) CFRelease(_iosurface);
    if (_vkView) g_rt.vkDestroyImageView(g_rt.vk_dev, _vkView, NULL);
    if (_vkImage) g_rt.vkDestroyImage(g_rt.vk_dev, _vkImage, NULL);
    if (_vkMem) g_rt.vkFreeMemory(g_rt.vk_dev, _vkMem, NULL);
}
@end

@interface MX250MTLSamplerState : NSObject <MTLSamplerState> {
@public
    MX250MTLDevice *_dev;
    VkSampler       _vkSampler;
}
@property(nullable, copy, atomic) NSString *label;
@end

@implementation MX250MTLSamplerState
- (id<MTLDevice>)device { return (id<MTLDevice>)_dev; }
- (void)dealloc {
    if (_vkSampler && _vkSampler != g_rt.default_sampler)
        g_rt.vkDestroySampler(g_rt.vk_dev, _vkSampler, NULL);
}
@end

@interface MX250MTLDepthStencilState : NSObject <MTLDepthStencilState> {
@public
    MX250MTLDevice *_dev;
    BOOL            _depthWriteEnabled;
    VkCompareOp     _depthCompareOp;
}
@property(nullable, copy, atomic) NSString *label;
@end

@implementation MX250MTLDepthStencilState
- (id<MTLDevice>)device { return (id<MTLDevice>)_dev; }
@end

/* ========================================================================= *
 * 4. Shaders & Pipelines (MX250MTLFunction / Library / Compute / Render)    *
 * ========================================================================= */
@interface MX250MTLFunction : NSObject <MTLFunction> {
@public
    MX250MTLDevice        *_dev;
    id<MTLFunction>        _appleFn;
    NSString              *_name;
    MTLFunctionType        _functionType;
    NSString              *_sanitizedLL;
    NSDictionary          *_reflectDict;
    NSData                *_spirvData;
    uint32_t               _localSize[3];
    bool                   _usesDynamicThreadsPC;
    uint32_t               _bindingCount;
    MX250ShaderBindingInfo _bindings[64];
}
@property(nullable, copy, atomic) NSString *label;
@end

@implementation MX250MTLFunction
- (id<MTLDevice>)device { return (id<MTLDevice>)_dev; }
- (NSString *)name { return _name; }
- (MTLFunctionType)functionType { return _functionType; }
- (MTLPatchType)patchType { return MTLPatchTypeNone; }
- (NSInteger)patchControlPointCount { return 0; }
- (nullable NSArray<MTLVertexAttribute *> *)vertexAttributes { return [_appleFn vertexAttributes]; }
- (nullable NSArray<MTLAttribute *> *)stageInputAttributes { return [_appleFn stageInputAttributes]; }
- (NSDictionary<NSString *, MTLFunctionConstant *> *)functionConstantsDictionary { return @{}; }
- (MTLFunctionOptions)options { return MTLFunctionOptionNone; }

- (bool)ensureTranslatedWithError:(NSError **)error {
    if (_spirvData != nil) return true;
    NSData *bc = [(id)_appleFn bitcodeData];
    _sanitizedLL = mx250_disassemble_air_bitcode(bc);
    if (!_sanitizedLL) {
        if (error) *error = [NSError errorWithDomain:@"MX250Metal" code:-10 userInfo:@{NSLocalizedDescriptionKey: @"Failed to disassemble AIR bitcode"}];
        return false;
    }
    const char *stage = "kernel";
    if (_functionType == MTLFunctionTypeVertex) stage = "vertex";
    else if (_functionType == MTLFunctionTypeFragment) stage = "fragment";

    uint8_t *rjson = NULL, *spv = NULL;
    size_t rlen = 0, slen = 0;
    char ebuf[1024] = {0};
    if (g_rt.nvmtl_reflect([_sanitizedLL UTF8String], stage, &rjson, &rlen, ebuf, sizeof(ebuf)) != 0) {
        NSString *msg = [NSString stringWithFormat:@"nvmtl_reflect(%s) failed: %s", stage, ebuf];
        if (error) *error = [NSError errorWithDomain:@"MX250Metal" code:-11 userInfo:@{NSLocalizedDescriptionKey: msg}];
        return false;
    }
    NSData *jdata = [NSData dataWithBytes:rjson length:rlen];
    g_rt.nvmtl_free(rjson, rlen);
    _reflectDict = [NSJSONSerialization JSONObjectWithData:jdata options:0 error:nil];

    if (g_rt.nvmtl_set_rsc) g_rt.nvmtl_set_rsc(1);
    if (g_rt.nvmtl_translate([_sanitizedLL UTF8String], stage, &spv, &slen, ebuf, sizeof(ebuf)) != 0) {
        NSString *msg = [NSString stringWithFormat:@"nvmtl_translate(%s) failed: %s", stage, ebuf];
        if (error) *error = [NSError errorWithDomain:@"MX250Metal" code:-12 userInfo:@{NSLocalizedDescriptionKey: msg}];
        return false;
    }

    NSMutableData *spvCopy = [NSMutableData dataWithBytes:spv length:slen];
    g_rt.nvmtl_free(spv, slen);

    if (_functionType == MTLFunctionTypeFragment) {
        mx250_patch_spirv_descriptor_set((uint32_t *)spvCopy.mutableBytes, spvCopy.length / 4, 1);
    }
    _spirvData = spvCopy;

    _localSize[0] = 64; _localSize[1] = 1; _localSize[2] = 1;
    NSArray *ls = _reflectDict[@"local_size"];
    if ([ls isKindOfClass:[NSArray class]] && ls.count == 3) {
        _localSize[0] = MAX(1u, [ls[0] unsignedIntValue]);
        _localSize[1] = MAX(1u, [ls[1] unsignedIntValue]);
        _localSize[2] = MAX(1u, [ls[2] unsignedIntValue]);
    }
    id kd = _reflectDict[@"kernel_dispatch"];
    _usesDynamicThreadsPC = (kd != nil && kd != [NSNull null] && ![kd isEqual:@"Workgroups"]);

    _bindingCount = 0;
    NSArray *binds = _reflectDict[@"bindings"];
    if ([binds isKindOfClass:[NSArray class]]) {
        for (NSDictionary *b in binds) {
            if (_bindingCount >= 64) break;
            NSDictionary *desc = b[@"descriptor"];
            if (![desc isKindOfClass:[NSDictionary class]]) continue;
            NSString *kind = [b[@"kind"] description];
            MX250ShaderBindingInfo *bi = &_bindings[_bindingCount];
            bi->metal_index = [b[@"metal_index"] unsignedIntValue];
            bi->vk_binding  = [desc[@"binding"] unsignedIntValue];
            bi->static_vk_sampler = VK_NULL_HANDLE;
            if ([kind isEqualToString:@"Buffer"] || [kind isEqualToString:@"BufferAddressTable"]) {
                bi->kind = 0;
            } else if ([kind isEqualToString:@"Texture"] || [kind isEqualToString:@"TextureArray"] || [kind isEqualToString:@"SynthesizedNullTexture"]) {
                bi->kind = 1;
            } else if ([kind isEqualToString:@"StorageImage"]) {
                bi->kind = 2;
            } else if ([kind isEqualToString:@"Sampler"] || [kind isEqualToString:@"SynthesizedReadSampler"]) {
                bi->kind = 3;
            } else if ([kind isEqualToString:@"StaticSampler"]) {
                bi->kind = 4;
                bi->static_vk_sampler = mx250_create_static_sampler_from_dict(b[@"static_sampler"]);
            } else {
                continue;
            }
            _bindingCount++;
        }
    }
    return true;
}

- (void)dealloc {
    for (uint32_t i = 0; i < _bindingCount; i++) {
        if (_bindings[i].static_vk_sampler && _bindings[i].static_vk_sampler != g_rt.default_sampler)
            g_rt.vkDestroySampler(g_rt.vk_dev, _bindings[i].static_vk_sampler, NULL);
    }
}
@end

@interface MX250MTLLibrary : NSObject <MTLLibrary> {
@public
    MX250MTLDevice *_dev;
    id<MTLLibrary>  _appleLib;
}
@property(nullable, copy, atomic) NSString *label;
@end

@implementation MX250MTLLibrary
- (id<MTLDevice>)device { return (id<MTLDevice>)_dev; }
- (NSArray<NSString *> *)functionNames { return [_appleLib functionNames]; }
- (MTLLibraryType)type { return [_appleLib type]; }
- (nullable NSString *)installName { return [_appleLib installName]; }

- (nullable id<MTLFunction>)newFunctionWithName:(NSString *)functionName {
    id<MTLFunction> afn = [_appleLib newFunctionWithName:functionName];
    if (!afn) return nil;
    MX250MTLFunction *fn = [[MX250MTLFunction alloc] init];
    fn->_dev = _dev;
    fn->_appleFn = afn;
    fn->_name = [functionName copy];
    fn->_functionType = [afn functionType];
    return fn;
}

- (nullable id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)constantValues error:(NSError **)error {
    id<MTLFunction> afn = [_appleLib newFunctionWithName:name constantValues:constantValues error:error];
    if (!afn) return nil;
    MX250MTLFunction *fn = [[MX250MTLFunction alloc] init];
    fn->_dev = _dev;
    fn->_appleFn = afn;
    fn->_name = [name copy];
    fn->_functionType = [afn functionType];
    return fn;
}
@end

typedef struct MX250ComputeSpecEntry {
    uint32_t   ls[3];
    VkPipeline pipeline;
} MX250ComputeSpecEntry;

@interface MX250MTLComputePipelineState : NSObject <MTLComputePipelineState> {
@public
    MX250MTLDevice       *_dev;
    MX250MTLFunction     *_fn;
    VkShaderModule        _vkShader;
    VkDescriptorSetLayout _vkSetLayout;
    VkPipelineLayout      _vkPipeLayout;
    uint32_t              _specCount;
    MX250ComputeSpecEntry _specs[16];
}
@property(nullable, readonly) NSString *label;
@end

@implementation MX250MTLComputePipelineState
- (id<MTLDevice>)device { return (id<MTLDevice>)_dev; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1024; }
- (NSUInteger)threadExecutionWidth { return 32; /* NVIDIA Warp Size */ }
- (NSUInteger)staticThreadgroupMemoryLength { return 0; }
- (BOOL)supportIndirectCommandBuffers { return NO; }

- (VkPipeline)pipelineForThreadgroupSize:(MTLSize)tgSize {
    uint32_t lx = (uint32_t)MAX(1u, tgSize.width);
    uint32_t ly = (uint32_t)MAX(1u, tgSize.height);
    uint32_t lz = (uint32_t)MAX(1u, tgSize.depth);
    for (uint32_t i = 0; i < _specCount; i++) {
        if (_specs[i].ls[0] == lx && _specs[i].ls[1] == ly && _specs[i].ls[2] == lz)
            return _specs[i].pipeline;
    }
    uint32_t spec_data[3] = { lx, ly, lz };
    VkSpecializationMapEntry map[3] = {
        { 0, 0, sizeof(uint32_t) },
        { 1, 4, sizeof(uint32_t) },
        { 2, 8, sizeof(uint32_t) },
    };
    VkSpecializationInfo sinfo = {
        .mapEntryCount = 3,
        .pMapEntries = map,
        .dataSize = sizeof(spec_data),
        .pData = spec_data,
    };
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = _vkShader,
            .pName = "main",
            .pSpecializationInfo = &sinfo,
        },
        .layout = _vkPipeLayout,
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    if (g_rt.vkCreateComputePipelines(g_rt.vk_dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    if (_specCount < 16) {
        _specs[_specCount].ls[0] = lx;
        _specs[_specCount].ls[1] = ly;
        _specs[_specCount].ls[2] = lz;
        _specs[_specCount].pipeline = pipe;
        _specCount++;
    }
    return pipe;
}

- (void)dealloc {
    for (uint32_t i = 0; i < _specCount; i++)
        if (_specs[i].pipeline) g_rt.vkDestroyPipeline(g_rt.vk_dev, _specs[i].pipeline, NULL);
    if (_vkPipeLayout) g_rt.vkDestroyPipelineLayout(g_rt.vk_dev, _vkPipeLayout, NULL);
    if (_vkSetLayout)  g_rt.vkDestroyDescriptorSetLayout(g_rt.vk_dev, _vkSetLayout, NULL);
    if (_vkShader)     g_rt.vkDestroyShaderModule(g_rt.vk_dev, _vkShader, NULL);
}
@end

@interface MX250MTLRenderPipelineState : NSObject <MTLRenderPipelineState> {
@public
    MX250MTLDevice       *_dev;
    MX250MTLFunction     *_vsFn;
    MX250MTLFunction     *_fsFn;
    VkShaderModule        _vsMod;
    VkShaderModule        _fsMod;
    VkDescriptorSetLayout _setLayouts[2]; /* 0 = VS, 1 = FS */
    VkPipelineLayout      _pipeLayout;
    VkFormat              _colorFormats[8];
    uint32_t              _colorCount;
    VkFormat              _depthFormat;
    BOOL                  _blendEnabled;
    uint32_t              _vbBindCount;
    VkVertexInputBindingDescription   _vbBinds[16];
    uint32_t              _vaAttrCount;
    VkVertexInputAttributeDescription _vaAttrs[32];
    bool                  _isStageInVB[MX250_MAX_BUFFERS];
    /* Cached pipelines keyed by (cullMode, frontFace, depthWrite, depthCompare, topology) */
    uint32_t              _varCount;
    uint64_t              _varKeys[16];
    VkPipeline            _varPipes[16];
}
@property(nullable, readonly) NSString *label;
@end

@implementation MX250MTLRenderPipelineState
- (id<MTLDevice>)device { return (id<MTLDevice>)_dev; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1024; }
- (BOOL)threadgroupSizeMatchesTileSize { return NO; }
- (NSUInteger)imageblockSampleLength { return 0; }

- (VkPipeline)pipelineForTopology:(VkPrimitiveTopology)topo
                         cullMode:(VkCullModeFlags)cull
                        frontFace:(VkFrontFace)ff
                        depthTest:(BOOL)dTest
                       depthWrite:(BOOL)dWrite
                          depthOp:(VkCompareOp)dOp {
    uint64_t key = ((uint64_t)topo) | ((uint64_t)cull << 8) | ((uint64_t)ff << 12) |
                   ((uint64_t)(dTest ? 1 : 0) << 16) | ((uint64_t)(dWrite ? 1 : 0) << 17) |
                   ((uint64_t)dOp << 20);
    for (uint32_t i = 0; i < _varCount; i++) {
        if (_varKeys[i] == key) return _varPipes[i];
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = _vsMod,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = _fsMod,
            .pName = "main",
        },
    };
    uint32_t stageCount = _fsMod ? 2 : 1;

    VkPipelineVertexInputStateCreateInfo vis = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = _vbBindCount,
        .pVertexBindingDescriptions = _vbBinds,
        .vertexAttributeDescriptionCount = _vaAttrCount,
        .pVertexAttributeDescriptions = _vaAttrs,
    };
    VkPipelineInputAssemblyStateCreateInfo ias = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topo,
    };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = cull,
        .frontFace = ff,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = (_depthFormat != VK_FORMAT_UNDEFINED && dTest) ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = (_depthFormat != VK_FORMAT_UNDEFINED && dWrite) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = dOp,
    };
    VkPipelineColorBlendAttachmentState cbas[8];
    for (uint32_t i = 0; i < MAX(1u, _colorCount); i++) {
        cbas[i] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = _blendEnabled ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = 0xFu,
        };
    }
    VkPipelineColorBlendStateCreateInfo cbs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = _colorCount,
        .pAttachments = cbas,
    };
    VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn_states,
    };
    VkPipelineRenderingCreateInfo prci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = _colorCount,
        .pColorAttachmentFormats = _colorFormats,
        .depthAttachmentFormat = _depthFormat,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &prci,
        .stageCount = stageCount,
        .pStages = stages,
        .pVertexInputState = &vis,
        .pInputAssemblyState = &ias,
        .pViewportState = &vps,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = &ds,
        .pColorBlendState = &cbs,
        .pDynamicState = &dsi,
        .layout = _pipeLayout,
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    if (g_rt.vkCreateGraphicsPipelines(g_rt.vk_dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    if (_varCount < 16) {
        _varKeys[_varCount] = key;
        _varPipes[_varCount] = pipe;
        _varCount++;
    }
    return pipe;
}

- (void)dealloc {
    for (uint32_t i = 0; i < _varCount; i++)
        if (_varPipes[i]) g_rt.vkDestroyPipeline(g_rt.vk_dev, _varPipes[i], NULL);
    if (_pipeLayout) g_rt.vkDestroyPipelineLayout(g_rt.vk_dev, _pipeLayout, NULL);
    if (_setLayouts[0]) g_rt.vkDestroyDescriptorSetLayout(g_rt.vk_dev, _setLayouts[0], NULL);
    if (_setLayouts[1]) g_rt.vkDestroyDescriptorSetLayout(g_rt.vk_dev, _setLayouts[1], NULL);
    if (_vsMod) g_rt.vkDestroyShaderModule(g_rt.vk_dev, _vsMod, NULL);
    if (_fsMod) g_rt.vkDestroyShaderModule(g_rt.vk_dev, _fsMod, NULL);
}
@end

/* ========================================================================= *
 * 5. Zero-Copy IOSurface CAMetalDrawable                                    *
 * ========================================================================= */
static char kMX250LayerDeviceKey   = 1;
static char kMX250LayerStateKey    = 2;
static char kMX250LayerSublayerKey = 3;

@interface MX250MTLDrawable : NSObject <CAMetalDrawable> {
@public
    CAMetalLayer          *_layer;
    MX250MTLTexture       *_texture;
    NSUInteger             _drawableID;
    NSMutableArray        *_presentedHandlers;
}
@end

@implementation MX250MTLDrawable
- (id<MTLTexture>)texture { return _texture; }
- (CAMetalLayer *)layer { return _layer; }
- (CFTimeInterval)presentedTime { return CACurrentMediaTime(); }
- (NSUInteger)drawableID { return _drawableID; }

- (void)addPresentedHandler:(MTLDrawablePresentedHandler)block {
    if (!block) return;
    if (!_presentedHandlers) _presentedHandlers = [NSMutableArray array];
    [_presentedHandlers addObject:[block copy]];
}

- (void)present {
    if (!_layer || !_texture || !_texture->_iosurface) return;
    @autoreleasepool {
        IOSurfaceRef surf = _texture->_iosurface;
        uint32_t seed = 0;
        IOSurfaceLock(surf, 0, &seed);
        IOSurfaceUnlock(surf, 0, &seed);

        CALayer *root = (CALayer *)_layer;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        CALayer *ioLayer = objc_getAssociatedObject(root, &kMX250LayerSublayerKey);
        if (!ioLayer) {
            ioLayer = [CALayer layer];
            ioLayer.anchorPoint = CGPointMake(0.0, 0.0);
            ioLayer.position = CGPointMake(0.0, 0.0);
            ioLayer.bounds = root.bounds;
            ioLayer.contentsScale = root.contentsScale;
            ioLayer.contentsGravity = kCAGravityResize;
            ioLayer.opaque = root.opaque;
            ioLayer.actions = @{
                @"contents": [NSNull null],
                @"bounds": [NSNull null],
                @"position": [NSNull null],
                @"sublayers": [NSNull null],
                @"onOrderIn": [NSNull null],
            };
            [root addSublayer:ioLayer];
            objc_setAssociatedObject(root, &kMX250LayerSublayerKey, ioLayer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        } else if (!CGRectEqualToRect(ioLayer.bounds, root.bounds)) {
            ioLayer.bounds = root.bounds;
            ioLayer.contentsScale = root.contentsScale;
        }
        ioLayer.contents = (__bridge id)surf;
        [CATransaction commit];
        [CATransaction flush];

        if (_presentedHandlers.count > 0) {
            NSArray *handlers = [_presentedHandlers copy];
            [_presentedHandlers removeAllObjects];
            for (MTLDrawablePresentedHandler h in handlers) {
                h(self);
            }
        }
    }
}

- (void)presentAtTime:(CFTimeInterval)presentationTime { (void)presentationTime; [self present]; }
- (void)presentAfterMinimumDuration:(CFTimeInterval)duration { (void)duration; [self present]; }
@end

/* ========================================================================= *
 * 6. Command Queue, Command Buffer & Encoders (Compute / Render / Blit)     *
 * ========================================================================= */
@interface MX250MTLCommandBuffer : NSObject <MTLCommandBuffer> {
@public
    MX250MTLDevice       *_dev;
    MX250MTLCommandQueue *_queue;
    VkCommandBuffer       _vkCmd;
    MTLCommandBufferStatus _status;
    MX250MTLBuffer       *_inlineArena;
    uint32_t              _inlineOffset;
    NSMutableArray<NSValue *> *_allocatedSets;
    NSMutableArray<MX250MTLDrawable *> *_drawablesToPresent;
    NSMutableArray       *_completedHandlers;
}
@property(nullable, copy, atomic) NSString *label;
- (uint32_t)allocInlineBytes:(const void *)bytes length:(NSUInteger)len;
- (VkDescriptorSet)allocSetForLayout:(VkDescriptorSetLayout)layout;
@end

static void mx250_bind_stage_resources(MX250MTLCommandBuffer *cb,
                                       VkDescriptorSet set,
                                       MX250MTLFunction *fn,
                                       VkBuffer *bufs, VkDeviceSize *offs,
                                       MX250MTLTexture *__strong *texs,
                                       MX250MTLSamplerState *__strong *samps) {
    if (!fn || fn->_bindingCount == 0 || !set) return;
    VkWriteDescriptorSet writes[64];
    VkDescriptorBufferInfo binfo[64];
    VkDescriptorImageInfo  iinfo[64];
    uint32_t wc = 0;

    for (uint32_t i = 0; i < fn->_bindingCount; i++) {
        MX250ShaderBindingInfo *bi = &fn->_bindings[i];
        memset(&writes[wc], 0, sizeof(VkWriteDescriptorSet));
        writes[wc].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[wc].dstSet = set;
        writes[wc].dstBinding = bi->vk_binding;
        writes[wc].descriptorCount = 1;

        if (bi->kind == 0) { /* Buffer */
            uint32_t mi = bi->metal_index;
            VkBuffer vb = (mi < MX250_MAX_BUFFERS && bufs[mi]) ? bufs[mi] : g_rt.dummy_buf;
            VkDeviceSize vo = (mi < MX250_MAX_BUFFERS && bufs[mi]) ? offs[mi] : 0;
            binfo[wc] = (VkDescriptorBufferInfo){ vb, vo, VK_WHOLE_SIZE };
            writes[wc].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[wc].pBufferInfo = &binfo[wc];
            wc++;
        } else if (bi->kind == 1 || bi->kind == 2) { /* Sampled / Storage Image */
            uint32_t mi = bi->metal_index;
            MX250MTLTexture *t = (mi < MX250_MAX_TEXTURES) ? texs[mi] : nil;
            if (!t || !t->_vkView) continue;
            iinfo[wc] = (VkDescriptorImageInfo){
                .sampler = VK_NULL_HANDLE,
                .imageView = t->_vkView,
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            };
            writes[wc].descriptorType = (bi->kind == 2) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[wc].pImageInfo = &iinfo[wc];
            wc++;
        } else if (bi->kind == 3 || bi->kind == 4) { /* Sampler / StaticSampler */
            VkSampler vs = g_rt.default_sampler;
            if (bi->kind == 4 && bi->static_vk_sampler) {
                vs = bi->static_vk_sampler;
            } else if (bi->metal_index < MX250_MAX_SAMPLERS && samps[bi->metal_index]) {
                vs = samps[bi->metal_index]->_vkSampler;
            }
            iinfo[wc] = (VkDescriptorImageInfo){ .sampler = vs };
            writes[wc].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[wc].pImageInfo = &iinfo[wc];
            wc++;
        }
    }
    if (wc > 0)
        g_rt.vkUpdateDescriptorSets(g_rt.vk_dev, wc, writes, 0, NULL);
}

/* --- Compute Command Encoder --- */
@interface MX250MTLComputeCommandEncoder : NSObject <MTLComputeCommandEncoder> {
@public
    MX250MTLCommandBuffer        *_cb;
    MX250MTLComputePipelineState *_pso;
    VkBuffer                      _bufs[MX250_MAX_BUFFERS];
    VkDeviceSize                  _offs[MX250_MAX_BUFFERS];
    MX250MTLTexture              *_texs[MX250_MAX_TEXTURES];
    MX250MTLSamplerState         *_samps[MX250_MAX_SAMPLERS];
}
@property(nullable, copy, atomic) NSString *label;
@end

@implementation MX250MTLComputeCommandEncoder
- (id<MTLDevice>)device { return [_cb device]; }
- (MTLDispatchType)dispatchType { return MTLDispatchTypeSerial; }
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state { _pso = (MX250MTLComputePipelineState *)state; }
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index >= MX250_MAX_BUFFERS) return;
    MX250MTLBuffer *b = (MX250MTLBuffer *)buffer;
    _bufs[index] = b ? b->_vkBuf : VK_NULL_HANDLE;
    _offs[index] = offset;
}
- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index < MX250_MAX_BUFFERS) _offs[index] = offset;
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    if (index >= MX250_MAX_BUFFERS || !bytes || length == 0) return;
    uint32_t off = [_cb allocInlineBytes:bytes length:length];
    _bufs[index] = _cb->_inlineArena->_vkBuf;
    _offs[index] = off;
}
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
    if (index < MX250_MAX_TEXTURES) _texs[index] = (MX250MTLTexture *)texture;
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    if (index < MX250_MAX_SAMPLERS) _samps[index] = (MX250MTLSamplerState *)sampler;
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index { (void)length; (void)index; }

- (void)_dispatchWithGridThreads:(MTLSize)gridThreads groups:(MTLSize)groups localSize:(MTLSize)tgSize {
    if (!_pso) return;
    for (int i = 0; i < MX250_MAX_TEXTURES; i++) {
        if (_texs[i] && _texs[i]->_currentLayout != VK_IMAGE_LAYOUT_GENERAL) {
            VkImageMemoryBarrier imb = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                .oldLayout = _texs[i]->_currentLayout,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = _texs[i]->_vkImage,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            g_rt.vkCmdPipelineBarrier(_cb->_vkCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &imb);
            _texs[i]->_currentLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
    }

    VkPipeline pipe = [_pso pipelineForThreadgroupSize:tgSize];
    if (!pipe) return;
    g_rt.vkCmdBindPipeline(_cb->_vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);

    VkDescriptorSet set = [_cb allocSetForLayout:_pso->_vkSetLayout];
    mx250_bind_stage_resources(_cb, set, _pso->_fn, _bufs, _offs, _texs, _samps);
    g_rt.vkCmdBindDescriptorSets(_cb->_vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pso->_vkPipeLayout, 0, 1, &set, 0, NULL);

    uint32_t pc[12] = {
        (uint32_t)gridThreads.width, (uint32_t)gridThreads.height, (uint32_t)gridThreads.depth,
        0, 0, 0,
        0, 0, 0,
        (uint32_t)groups.width, (uint32_t)groups.height, (uint32_t)groups.depth,
    };
    g_rt.vkCmdPushConstants(_cb->_vkCmd, _pso->_vkPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    g_rt.vkCmdDispatch(_cb->_vkCmd, (uint32_t)MAX(1u, groups.width), (uint32_t)MAX(1u, groups.height), (uint32_t)MAX(1u, groups.depth));

    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
    };
    g_rt.vkCmdPipelineBarrier(_cb->_vkCmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

- (void)dispatchThreadgroups:(MTLSize)threadgroupsPerGrid threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup {
    MTLSize grid = MTLSizeMake(threadgroupsPerGrid.width * threadsPerThreadgroup.width,
                               threadgroupsPerGrid.height * threadsPerThreadgroup.height,
                               threadgroupsPerGrid.depth * threadsPerThreadgroup.depth);
    [self _dispatchWithGridThreads:grid groups:threadgroupsPerGrid localSize:threadsPerThreadgroup];
}

- (void)dispatchThreads:(MTLSize)threadsPerGrid threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup {
    MTLSize groups = MTLSizeMake((threadsPerGrid.width  + threadsPerThreadgroup.width  - 1) / threadsPerThreadgroup.width,
                                 (threadsPerGrid.height + threadsPerThreadgroup.height - 1) / threadsPerThreadgroup.height,
                                 (threadsPerGrid.depth  + threadsPerThreadgroup.depth  - 1) / threadsPerThreadgroup.depth);
    [self _dispatchWithGridThreads:threadsPerGrid groups:groups localSize:threadsPerThreadgroup];
}

- (void)memoryBarrierWithScope:(MTLBarrierScope)scope { (void)scope; }
- (void)memoryBarrierWithResources:(const id<MTLResource> *)resources count:(NSUInteger)count { (void)resources; (void)count; }
- (void)pushDebugGroup:(NSString *)string { (void)string; }
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString *)string { (void)string; }
- (void)endEncoding {}
@end

/* --- Render Command Encoder --- */
@interface MX250MTLRenderCommandEncoder : NSObject <MTLRenderCommandEncoder> {
@public
    MX250MTLCommandBuffer       *_cb;
    MX250MTLRenderPipelineState *_pso;
    MX250MTLDepthStencilState   *_dss;
    uint32_t                     _fbWidth;
    uint32_t                     _fbHeight;
    VkCullModeFlags              _cullMode;
    VkFrontFace                  _frontFace;
    VkBuffer                     _vsBufs[MX250_MAX_BUFFERS];
    VkDeviceSize                 _vsOffs[MX250_MAX_BUFFERS];
    VkBuffer                     _fsBufs[MX250_MAX_BUFFERS];
    VkDeviceSize                 _fsOffs[MX250_MAX_BUFFERS];
    MX250MTLTexture             *_vsTexs[MX250_MAX_TEXTURES];
    MX250MTLTexture             *_fsTexs[MX250_MAX_TEXTURES];
    MX250MTLSamplerState        *_vsSamps[MX250_MAX_SAMPLERS];
    MX250MTLSamplerState        *_fsSamps[MX250_MAX_SAMPLERS];
    bool                         _renderingActive;
}
@property(nullable, copy, atomic) NSString *label;
@end

@implementation MX250MTLRenderCommandEncoder
- (id<MTLDevice>)device { return [_cb device]; }
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipelineState { _pso = (MX250MTLRenderPipelineState *)pipelineState; }
- (void)setDepthStencilState:(id<MTLDepthStencilState>)depthStencilState { _dss = (MX250MTLDepthStencilState *)depthStencilState; }
- (void)setCullMode:(MTLCullMode)cullMode {
    if (cullMode == MTLCullModeFront) _cullMode = VK_CULL_MODE_FRONT_BIT;
    else if (cullMode == MTLCullModeBack) _cullMode = VK_CULL_MODE_BACK_BIT;
    else _cullMode = VK_CULL_MODE_NONE;
}
- (void)setFrontFacingWinding:(MTLWinding)frontFacingWinding {
    /* Negative viewport height preserves Metal winding 1:1 in Vulkan */
    _frontFace = (frontFacingWinding == MTLWindingClockwise) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}
- (void)setViewport:(MTLViewport)viewport {
    VkViewport vp = {
        .x = (float)viewport.originX,
        .y = (float)(viewport.originY + viewport.height),
        .width = (float)viewport.width,
        .height = -(float)viewport.height,
        .minDepth = (float)viewport.znear,
        .maxDepth = (float)viewport.zfar,
    };
    g_rt.vkCmdSetViewport(_cb->_vkCmd, 0, 1, &vp);
}
- (void)setScissorRect:(MTLScissorRect)rect {
    VkRect2D sc = { { (int32_t)rect.x, (int32_t)rect.y }, { (uint32_t)rect.width, (uint32_t)rect.height } };
    g_rt.vkCmdSetScissor(_cb->_vkCmd, 0, 1, &sc);
}
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index >= MX250_MAX_BUFFERS) return;
    MX250MTLBuffer *b = (MX250MTLBuffer *)buffer;
    _vsBufs[index] = b ? b->_vkBuf : VK_NULL_HANDLE;
    _vsOffs[index] = offset;
}
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index < MX250_MAX_BUFFERS) _vsOffs[index] = offset;
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    if (index >= MX250_MAX_BUFFERS || !bytes || length == 0) return;
    uint32_t off = [_cb allocInlineBytes:bytes length:length];
    _vsBufs[index] = _cb->_inlineArena->_vkBuf;
    _vsOffs[index] = off;
}
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index >= MX250_MAX_BUFFERS) return;
    MX250MTLBuffer *b = (MX250MTLBuffer *)buffer;
    _fsBufs[index] = b ? b->_vkBuf : VK_NULL_HANDLE;
    _fsOffs[index] = offset;
}
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index < MX250_MAX_BUFFERS) _fsOffs[index] = offset;
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    if (index >= MX250_MAX_BUFFERS || !bytes || length == 0) return;
    uint32_t off = [_cb allocInlineBytes:bytes length:length];
    _fsBufs[index] = _cb->_inlineArena->_vkBuf;
    _fsOffs[index] = off;
}
- (void)setVertexTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
    if (index < MX250_MAX_TEXTURES) _vsTexs[index] = (MX250MTLTexture *)texture;
}
- (void)setFragmentTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
    if (index < MX250_MAX_TEXTURES) _fsTexs[index] = (MX250MTLTexture *)texture;
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    if (index < MX250_MAX_SAMPLERS) _vsSamps[index] = (MX250MTLSamplerState *)sampler;
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    if (index < MX250_MAX_SAMPLERS) _fsSamps[index] = (MX250MTLSamplerState *)sampler;
}

static VkPrimitiveTopology mx250_mtl_prim_to_vk(MTLPrimitiveType pt) {
    switch (pt) {
    case MTLPrimitiveTypePoint:         return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case MTLPrimitiveTypeLine:          return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case MTLPrimitiveTypeLineStrip:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case MTLPrimitiveTypeTriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    default:                            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

- (bool)_prepareDrawForPrimitive:(MTLPrimitiveType)primType {
    if (!_pso) return false;
    BOOL dTest  = _dss ? (_dss->_depthCompareOp != VK_COMPARE_OP_ALWAYS || _dss->_depthWriteEnabled) : NO;
    BOOL dWrite = _dss ? _dss->_depthWriteEnabled : NO;
    VkCompareOp dOp = _dss ? _dss->_depthCompareOp : VK_COMPARE_OP_ALWAYS;

    VkPipeline pipe = [_pso pipelineForTopology:mx250_mtl_prim_to_vk(primType)
                                       cullMode:_cullMode
                                      frontFace:_frontFace
                                      depthTest:dTest
                                     depthWrite:dWrite
                                        depthOp:dOp];
    if (!pipe) return false;
    g_rt.vkCmdBindPipeline(_cb->_vkCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

    VkDescriptorSet sets[2] = {
        [_cb allocSetForLayout:_pso->_setLayouts[0]],
        [_cb allocSetForLayout:_pso->_setLayouts[1]],
    };
    mx250_bind_stage_resources(_cb, sets[0], _pso->_vsFn, _vsBufs, _vsOffs, _vsTexs, _vsSamps);
    mx250_bind_stage_resources(_cb, sets[1], _pso->_fsFn, _fsBufs, _fsOffs, _fsTexs, _fsSamps);
    g_rt.vkCmdBindDescriptorSets(_cb->_vkCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pso->_pipeLayout, 0, 2, sets, 0, NULL);

    for (uint32_t i = 0; i < _pso->_vbBindCount; i++) {
        uint32_t slot = _pso->_vbBinds[i].binding;
        if (slot < MX250_MAX_BUFFERS && _vsBufs[slot]) {
            g_rt.vkCmdBindVertexBuffers(_cb->_vkCmd, slot, 1, &_vsBufs[slot], &_vsOffs[slot]);
        }
    }
    return true;
}

- (void)drawPrimitives:(MTLPrimitiveType)primitiveType vertexStart:(NSUInteger)vertexStart vertexCount:(NSUInteger)vertexCount instanceCount:(NSUInteger)instanceCount baseInstance:(NSUInteger)baseInstance {
    if ([self _prepareDrawForPrimitive:primitiveType])
        g_rt.vkCmdDraw(_cb->_vkCmd, (uint32_t)vertexCount, (uint32_t)instanceCount, (uint32_t)vertexStart, (uint32_t)baseInstance);
}

- (void)drawPrimitives:(MTLPrimitiveType)primitiveType vertexStart:(NSUInteger)vertexStart vertexCount:(NSUInteger)vertexCount instanceCount:(NSUInteger)instanceCount {
    [self drawPrimitives:primitiveType vertexStart:vertexStart vertexCount:vertexCount instanceCount:instanceCount baseInstance:0];
}

- (void)drawPrimitives:(MTLPrimitiveType)primitiveType vertexStart:(NSUInteger)vertexStart vertexCount:(NSUInteger)vertexCount {
    [self drawPrimitives:primitiveType vertexStart:vertexStart vertexCount:vertexCount instanceCount:1 baseInstance:0];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType indexCount:(NSUInteger)indexCount indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset instanceCount:(NSUInteger)instanceCount baseVertex:(NSInteger)baseVertex baseInstance:(NSUInteger)baseInstance {
    if (![self _prepareDrawForPrimitive:primitiveType] || !indexBuffer) return;
    MX250MTLBuffer *ib = (MX250MTLBuffer *)indexBuffer;
    VkIndexType it = (indexType == MTLIndexTypeUInt16) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    g_rt.vkCmdBindIndexBuffer(_cb->_vkCmd, ib->_vkBuf, indexBufferOffset, it);
    g_rt.vkCmdDrawIndexed(_cb->_vkCmd, (uint32_t)indexCount, (uint32_t)instanceCount, 0, (int32_t)baseVertex, (uint32_t)baseInstance);
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType indexCount:(NSUInteger)indexCount indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset instanceCount:(NSUInteger)instanceCount {
    [self drawIndexedPrimitives:primitiveType indexCount:indexCount indexType:indexType indexBuffer:indexBuffer indexBufferOffset:indexBufferOffset instanceCount:instanceCount baseVertex:0 baseInstance:0];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType indexCount:(NSUInteger)indexCount indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset {
    [self drawIndexedPrimitives:primitiveType indexCount:indexCount indexType:indexType indexBuffer:indexBuffer indexBufferOffset:indexBufferOffset instanceCount:1 baseVertex:0 baseInstance:0];
}

- (void)pushDebugGroup:(NSString *)string { (void)string; }
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString *)string { (void)string; }
- (void)endEncoding {
    if (_renderingActive) {
        g_rt.vkCmdEndRendering(_cb->_vkCmd);
        _renderingActive = false;
    }
}
@end

/* --- Blit Command Encoder --- */
@interface MX250MTLBlitCommandEncoder : NSObject <MTLBlitCommandEncoder> {
@public
    MX250MTLCommandBuffer *_cb;
}
@property(nullable, copy, atomic) NSString *label;
@end

@implementation MX250MTLBlitCommandEncoder
- (id<MTLDevice>)device { return [_cb device]; }
- (void)copyFromBuffer:(id<MTLBuffer>)sourceBuffer sourceOffset:(NSUInteger)sourceOffset toBuffer:(id<MTLBuffer>)destinationBuffer destinationOffset:(NSUInteger)destinationOffset size:(NSUInteger)size {
    if (!sourceBuffer || !destinationBuffer || size == 0) return;
    MX250MTLBuffer *sb = (MX250MTLBuffer *)sourceBuffer;
    MX250MTLBuffer *db = (MX250MTLBuffer *)destinationBuffer;
    VkBufferCopy c = { sourceOffset, destinationOffset, size };
    g_rt.vkCmdCopyBuffer(_cb->_vkCmd, sb->_vkBuf, db->_vkBuf, 1, &c);
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT,
    };
    g_rt.vkCmdPipelineBarrier(_cb->_vkCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
- (void)fillBuffer:(id<MTLBuffer>)buffer range:(NSRange)range value:(uint8_t)value {
    if (!buffer || range.length == 0) return;
    MX250MTLBuffer *b = (MX250MTLBuffer *)buffer;
    uint32_t v32 = (uint32_t)value * 0x01010101u;
    g_rt.vkCmdFillBuffer(_cb->_vkCmd, b->_vkBuf, range.location & ~3ULL, (range.length + 3ULL) & ~3ULL, v32);
}
- (void)synchronizeResource:(id<MTLResource>)resource { (void)resource; }
- (void)pushDebugGroup:(NSString *)string { (void)string; }
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString *)string { (void)string; }
- (void)endEncoding {}
@end

/* --- Command Buffer & Command Queue --- */
@implementation MX250MTLCommandBuffer
- (id<MTLDevice>)device { return (id<MTLDevice>)_dev; }
- (id<MTLCommandQueue>)commandQueue { return (id<MTLCommandQueue>)_queue; }
- (BOOL)retainedReferences { return YES; }
- (MTLCommandBufferStatus)status { return _status; }
- (nullable NSError *)error { return nil; }
- (CFTimeInterval)kernelStartTime { return 0; }
- (CFTimeInterval)kernelEndTime { return 0; }
- (CFTimeInterval)GPUStartTime { return 0; }
- (CFTimeInterval)GPUEndTime { return 0; }

- (uint32_t)allocInlineBytes:(const void *)bytes length:(NSUInteger)len {
    if (!_inlineArena) {
        _inlineArena = (MX250MTLBuffer *)[(id<MTLDevice>)_dev newBufferWithLength:262144 options:MTLResourceStorageModeShared];
        _inlineOffset = 0;
    }
    uint32_t aligned = (uint32_t)((len + 63u) & ~63u);
    if (_inlineOffset + aligned > _inlineArena->_length) _inlineOffset = 0;
    uint32_t off = _inlineOffset;
    memcpy((uint8_t *)_inlineArena->_mapped + off, bytes, len);
    _inlineOffset += aligned;
    return off;
}

- (VkDescriptorSet)allocSetForLayout:(VkDescriptorSetLayout)layout {
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g_rt.vk_desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    };
    VkDescriptorSet set = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_rt.queue_lock);
    g_rt.vkAllocateDescriptorSets(g_rt.vk_dev, &dsai, &set);
    pthread_mutex_unlock(&g_rt.queue_lock);
    if (set) [_allocatedSets addObject:[NSValue valueWithPointer:set]];
    return set;
}

- (nullable id<MTLComputeCommandEncoder>)computeCommandEncoder {
    MX250MTLComputeCommandEncoder *enc = [[MX250MTLComputeCommandEncoder alloc] init];
    enc->_cb = self;
    return enc;
}

- (nullable id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)dispatchType {
    (void)dispatchType;
    return [self computeCommandEncoder];
}

- (nullable id<MTLBlitCommandEncoder>)blitCommandEncoder {
    MX250MTLBlitCommandEncoder *enc = [[MX250MTLBlitCommandEncoder alloc] init];
    enc->_cb = self;
    return enc;
}

- (nullable id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)renderPassDescriptor {
    if (!renderPassDescriptor) return nil;
    MX250MTLRenderCommandEncoder *enc = [[MX250MTLRenderCommandEncoder alloc] init];
    enc->_cb = self;
    enc->_cullMode = VK_CULL_MODE_NONE;
    enc->_frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkRenderingAttachmentInfo colorAtts[8];
    uint32_t colorCount = 0;
    uint32_t fbW = 0, fbH = 0;

    for (uint32_t i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *ca = renderPassDescriptor.colorAttachments[i];
        MX250MTLTexture *tex = (MX250MTLTexture *)ca.texture;
        if (!tex) break;
        fbW = (uint32_t)tex->_width;
        fbH = (uint32_t)tex->_height;

        VkImageMemoryBarrier imb = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
            .oldLayout = tex->_currentLayout,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = tex->_vkImage,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        g_rt.vkCmdPipelineBarrier(_vkCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &imb);
        tex->_currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentLoadOp lop = VK_ATTACHMENT_LOAD_OP_LOAD;
        if (ca.loadAction == MTLLoadActionClear) lop = VK_ATTACHMENT_LOAD_OP_CLEAR;
        else if (ca.loadAction == MTLLoadActionDontCare) lop = VK_ATTACHMENT_LOAD_OP_DONT_CARE;

        VkAttachmentStoreOp sop = (ca.storeAction == MTLStoreActionDontCare)
                                      ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        MTLClearColor cc = ca.clearColor;
        colorAtts[colorCount++] = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = tex->_vkView,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = lop,
            .storeOp = sop,
            .clearValue.color.float32 = { (float)cc.red, (float)cc.green, (float)cc.blue, (float)cc.alpha },
        };
    }

    VkRenderingAttachmentInfo depthAtt;
    bool hasDepth = false;
    MX250MTLTexture *dtex = (MX250MTLTexture *)renderPassDescriptor.depthAttachment.texture;
    if (dtex) {
        hasDepth = true;
        if (fbW == 0) { fbW = (uint32_t)dtex->_width; fbH = (uint32_t)dtex->_height; }
        VkImageMemoryBarrier dimb = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
            .oldLayout = dtex->_currentLayout,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dtex->_vkImage,
            .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 },
        };
        g_rt.vkCmdPipelineBarrier(_vkCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &dimb);
        dtex->_currentLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        MTLRenderPassDepthAttachmentDescriptor *da = renderPassDescriptor.depthAttachment;
        depthAtt = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = dtex->_vkView,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .loadOp = (da.loadAction == MTLLoadActionClear) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = (da.storeAction == MTLStoreActionStore) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .clearValue.depthStencil = { (float)da.clearDepth, 0 },
        };
    }

    enc->_fbWidth = MAX(1u, fbW);
    enc->_fbHeight = MAX(1u, fbH);

    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { enc->_fbWidth, enc->_fbHeight } },
        .layerCount = 1,
        .colorAttachmentCount = colorCount,
        .pColorAttachments = colorAtts,
        .pDepthAttachment = hasDepth ? &depthAtt : NULL,
    };
    g_rt.vkCmdBeginRendering(_vkCmd, &ri);
    enc->_renderingActive = true;

    [enc setViewport:(MTLViewport){ 0, 0, enc->_fbWidth, enc->_fbHeight, 0.0, 1.0 }];
    [enc setScissorRect:(MTLScissorRect){ 0, 0, enc->_fbWidth, enc->_fbHeight }];
    return enc;
}

- (void)presentDrawable:(id<MTLDrawable>)drawable {
    if ([drawable isKindOfClass:[MX250MTLDrawable class]]) {
        [_drawablesToPresent addObject:(MX250MTLDrawable *)drawable];
    }
}

- (void)addCompletedHandler:(MTLCommandBufferHandler)block {
    if (block) [_completedHandlers addObject:[block copy]];
}

- (void)addScheduledHandler:(MTLCommandBufferHandler)block {
    if (block) block(self);
}

- (void)enqueue {}

- (void)commit {
    if (_status >= MTLCommandBufferStatusCommitted) return;

    /* For every Zero-Copy IOSurface drawable queued for presentation, record
     * MX250 PASCAL_DMA_COPY_B (0xc1b5) GDDR5 BlockLinear -> IOSurface DDR4 DMA!
     */
    for (MX250MTLDrawable *drw in _drawablesToPresent) {
        MX250MTLTexture *t = drw->_texture;
        if (!t || !t->_iosurfaceVkBuf) continue;

        VkImageMemoryBarrier imb = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = (t->_currentLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_IMAGE_LAYOUT_GENERAL : t->_currentLayout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = t->_vkImage,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        g_rt.vkCmdPipelineBarrier(_vkCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &imb);

        uint32_t bpp = mx250_mtl_bpp(t->_pixelFormat);
        VkBufferImageCopy bic = {
            .bufferOffset = 0,
            .bufferRowLength = t->_iosurfacePitch / bpp,
            .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = { (uint32_t)t->_width, (uint32_t)t->_height, 1 },
        };
        g_rt.vkCmdCopyImageToBuffer(_vkCmd, t->_vkImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->_iosurfaceVkBuf, 1, &bic);

        VkBufferMemoryBarrier bmb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = t->_iosurfaceVkBuf,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        g_rt.vkCmdPipelineBarrier(_vkCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bmb, 0, NULL);
        t->_currentLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    g_rt.vkEndCommandBuffer(_vkCmd);
    _status = MTLCommandBufferStatusCommitted;

    pthread_mutex_lock(&g_rt.queue_lock);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    g_rt.vkCreateFence(g_rt.vk_dev, &fci, NULL, &fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &_vkCmd };
    g_rt.vkQueueSubmit(g_rt.vk_queue, 1, &si, fence);
    g_rt.vkWaitForFences(g_rt.vk_dev, 1, &fence, VK_TRUE, UINT64_MAX);
    g_rt.vkDestroyFence(g_rt.vk_dev, fence, NULL);

    for (NSValue *v in _allocatedSets) {
        VkDescriptorSet s = (VkDescriptorSet)[v pointerValue];
        if (s) g_rt.vkFreeDescriptorSets(g_rt.vk_dev, g_rt.vk_desc_pool, 1, &s);
    }
    [_allocatedSets removeAllObjects];
    pthread_mutex_unlock(&g_rt.queue_lock);

    _status = MTLCommandBufferStatusCompleted;

    for (MX250MTLDrawable *drw in _drawablesToPresent) {
        [drw present];
    }
    [_drawablesToPresent removeAllObjects];

    for (MTLCommandBufferHandler h in _completedHandlers) {
        h(self);
    }
    [_completedHandlers removeAllObjects];
}

- (void)waitUntilScheduled {}
- (void)waitUntilCompleted {
    if (_status < MTLCommandBufferStatusCommitted) [self commit];
}

- (void)pushDebugGroup:(NSString *)string { (void)string; }
- (void)popDebugGroup {}

- (void)dealloc {
    if (_vkCmd) {
        pthread_mutex_lock(&g_rt.queue_lock);
        g_rt.vkFreeCommandBuffers(g_rt.vk_dev, g_rt.vk_cmd_pool, 1, &_vkCmd);
        pthread_mutex_unlock(&g_rt.queue_lock);
    }
}
@end

@interface MX250MTLCommandQueue : NSObject <MTLCommandQueue> {
@public
    MX250MTLDevice *_dev;
}
@property(nullable, copy, atomic) NSString *label;
@end

@implementation MX250MTLCommandQueue
- (id<MTLDevice>)device { return (id<MTLDevice>)_dev; }
- (nullable id<MTLCommandBuffer>)commandBuffer {
    MX250MTLCommandBuffer *cb = [[MX250MTLCommandBuffer alloc] init];
    cb->_dev = _dev;
    cb->_queue = self;
    cb->_status = MTLCommandBufferStatusNotEnqueued;
    cb->_allocatedSets = [NSMutableArray array];
    cb->_drawablesToPresent = [NSMutableArray array];
    cb->_completedHandlers = [NSMutableArray array];

    pthread_mutex_lock(&g_rt.queue_lock);
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_rt.vk_cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    g_rt.vkAllocateCommandBuffers(g_rt.vk_dev, &cbai, &cb->_vkCmd);
    pthread_mutex_unlock(&g_rt.queue_lock);

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    g_rt.vkBeginCommandBuffer(cb->_vkCmd, &bi);
    return cb;
}
- (nullable id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return [self commandBuffer]; }
- (void)insertDebugCaptureBoundary {}
@end

/* ========================================================================= *
 * 7. MX250MTLDevice Implementation                                          *
 * ========================================================================= */
static VkDescriptorSetLayout mx250_build_set_layout(MX250MTLFunction *fn, VkShaderStageFlags stage) {
    VkDescriptorSetLayoutBinding binds[64];
    uint32_t bc = 0;
    if (fn) {
        for (uint32_t i = 0; i < fn->_bindingCount && bc < 64; i++) {
            MX250ShaderBindingInfo *bi = &fn->_bindings[i];
            VkDescriptorType dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (bi->kind == 1) dt = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            else if (bi->kind == 2) dt = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            else if (bi->kind == 3 || bi->kind == 4) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
            binds[bc++] = (VkDescriptorSetLayoutBinding){
                .binding = bi->vk_binding,
                .descriptorType = dt,
                .descriptorCount = 1,
                .stageFlags = stage,
            };
        }
    }
    VkDescriptorSetLayoutCreateInfo slci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = bc,
        .pBindings = binds,
    };
    VkDescriptorSetLayout out = VK_NULL_HANDLE;
    g_rt.vkCreateDescriptorSetLayout(g_rt.vk_dev, &slci, NULL, &out);
    return out;
}

@interface MX250MTLDevice : NSObject <MTLDevice>
+ (instancetype)sharedDevice;
- (MX250MTLTexture *)newZeroCopyDrawableTextureWidth:(uint32_t)w height:(uint32_t)h format:(MTLPixelFormat)fmt;
@end

@implementation MX250MTLDevice

+ (instancetype)sharedDevice {
    static MX250MTLDevice *s_dev = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        if (mx250_runtime_init())
            s_dev = [[MX250MTLDevice alloc] init];
    });
    return s_dev;
}

- (NSString *)name { return @"NVIDIA GeForce MX250 (Metal-on-Vulkan P0)"; }
- (uint64_t)registryID { return 0x10de1d13ULL; }
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(1024, 1024, 64); }
- (BOOL)isLowPower { return NO; }
- (BOOL)isHeadless { return NO; }
- (BOOL)isRemovable { return NO; }
- (BOOL)hasUnifiedMemory { return NO; }
- (uint64_t)recommendedMaxWorkingSetSize { return 2048ULL * 1024ULL * 1024ULL; }
- (MTLDeviceLocation)location { return MTLDeviceLocationBuiltIn; }
- (NSUInteger)locationNumber { return 1; }
- (uint64_t)maxTransferRate { return 4000ULL * 1000000ULL; }
- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }
- (MTLReadWriteTextureTier)readWriteTextureSupport { return MTLReadWriteTextureTier2; }
- (MTLArgumentBuffersTier)argumentBuffersSupport { return MTLArgumentBuffersTier1; }
- (BOOL)areRasterOrderGroupsSupported { return NO; }
- (BOOL)supports32BitFloatFiltering { return YES; }
- (BOOL)supports32BitMSAA { return YES; }
- (BOOL)supportsQueryTextureLOD { return YES; }
- (BOOL)supportsBCTextureCompression { return YES; }
- (BOOL)supportsPullModelInterpolation { return YES; }
- (BOOL)areBarycentricCoordsSupported { return NO; }
- (BOOL)supportsShaderBarycentricCoordinates { return NO; }
- (NSUInteger)currentAllocatedSize { return 64ULL * 1024ULL * 1024ULL; }
- (NSUInteger)maxThreadgroupMemoryLength { return 49152; }
- (NSUInteger)maxArgumentBufferSamplerCount { return 16; }
- (BOOL)areProgrammableSamplePositionsSupported { return NO; }
- (NSUInteger)maxBufferLength { return 1024ULL * 1024ULL * 1024ULL; }
- (NSArray<MTLArchitecture *> *)counterSets { return @[]; }
- (BOOL)supportsDynamicLibraries { return NO; }
- (BOOL)supportsRenderDynamicLibraries { return NO; }
- (BOOL)supportsRaytracing { return NO; }
- (BOOL)supportsFunctionPointers { return NO; }
- (BOOL)supportsFunctionPointersFromRender { return NO; }
- (BOOL)supportsRaytracingFromRender { return NO; }
- (BOOL)supportsPrimitiveMotionBlur { return NO; }

- (BOOL)supportsFamily:(MTLGPUFamily)gpuFamily {
    switch (gpuFamily) {
    case MTLGPUFamilyCommon1:
    case MTLGPUFamilyCommon2:
    case MTLGPUFamilyCommon3:
    case MTLGPUFamilyMac2:
    case MTLGPUFamilyApple1:
    case MTLGPUFamilyApple2:
    case MTLGPUFamilyApple3:
    case MTLGPUFamilyApple4:
        return YES;
    default:
        return NO;
    }
}

- (BOOL)supportsFeatureSet:(MTLFeatureSet)featureSet { (void)featureSet; return YES; }
- (BOOL)supportsTextureSampleCount:(NSUInteger)sampleCount { return (sampleCount == 1); }
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format { (void)format; return 256; }
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)format { (void)format; return 256; }

- (nullable id<MTLCommandQueue>)newCommandQueue {
    MX250MTLCommandQueue *q = [[MX250MTLCommandQueue alloc] init];
    q->_dev = self;
    return q;
}
- (nullable id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)maxCommandBufferCount {
    (void)maxCommandBufferCount;
    return [self newCommandQueue];
}

- (nullable id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    NSUInteger allocLen = MAX(length, 64u);
    MX250MTLBuffer *b = [[MX250MTLBuffer alloc] init];
    b->_dev = self;
    b->_length = length;
    b->_options = options;

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = allocLen,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    if (g_rt.vkCreateBuffer(g_rt.vk_dev, &bci, NULL, &b->_vkBuf) != VK_SUCCESS)
        return nil;

    VkMemoryRequirements mr;
    g_rt.vkGetBufferMemoryRequirements(g_rt.vk_dev, b->_vkBuf, &mr);

    bool is_priv = ((options & MTLResourceStorageModeMask) == MTLResourceStorageModePrivate);
    uint32_t mti = is_priv
        ? mx250_find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        : mx250_find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mti,
    };
    if (g_rt.vkAllocateMemory(g_rt.vk_dev, &mai, NULL, &b->_vkMem) != VK_SUCCESS)
        return nil;
    g_rt.vkBindBufferMemory(g_rt.vk_dev, b->_vkBuf, b->_vkMem, 0);
    if (!is_priv)
        g_rt.vkMapMemory(g_rt.vk_dev, b->_vkMem, 0, allocLen, 0, &b->_mapped);
    return b;
}

- (nullable id<MTLBuffer>)newBufferWithBytes:(const void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options {
    MX250MTLBuffer *b = (MX250MTLBuffer *)[self newBufferWithLength:length options:MTLResourceStorageModeShared];
    if (b && b->_mapped && pointer && length > 0)
        memcpy(b->_mapped, pointer, length);
    b->_options = options;
    return b;
}

- (nullable id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options deallocator:(void (^)(void *, NSUInteger))deallocator {
    id<MTLBuffer> b = [self newBufferWithBytes:pointer length:length options:options];
    if (deallocator) deallocator(pointer, length);
    return b;
}

- (nullable id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor {
    if (!descriptor) return nil;
    MX250MTLTexture *t = [[MX250MTLTexture alloc] init];
    t->_dev = self;
    t->_pixelFormat = descriptor.pixelFormat;
    t->_textureType = descriptor.textureType;
    t->_usage = descriptor.usage;
    t->_width = MAX(1u, descriptor.width);
    t->_height = MAX(1u, descriptor.height);
    t->_depth = MAX(1u, descriptor.depth);
    t->_mipmapLevelCount = MAX(1u, descriptor.mipmapLevelCount);
    t->_sampleCount = 1;
    t->_vkFormat = mx250_mtl_to_vk_format(descriptor.pixelFormat);
    t->_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    bool is_depth = (descriptor.pixelFormat == MTLPixelFormatDepth32Float);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (is_depth) {
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    }

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = t->_vkFormat,
        .extent = { (uint32_t)t->_width, (uint32_t)t->_height, 1 },
        .mipLevels = (uint32_t)t->_mipmapLevelCount,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (g_rt.vkCreateImage(g_rt.vk_dev, &ici, NULL, &t->_vkImage) != VK_SUCCESS)
        return nil;

    VkMemoryRequirements mr;
    g_rt.vkGetImageMemoryRequirements(g_rt.vk_dev, t->_vkImage, &mr);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mx250_find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0),
    };
    if (g_rt.vkAllocateMemory(g_rt.vk_dev, &mai, NULL, &t->_vkMem) != VK_SUCCESS)
        return nil;
    g_rt.vkBindImageMemory(g_rt.vk_dev, t->_vkImage, t->_vkMem, 0);

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = t->_vkImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = t->_vkFormat,
        .subresourceRange = {
            .aspectMask = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = (uint32_t)t->_mipmapLevelCount,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    g_rt.vkCreateImageView(g_rt.vk_dev, &ivci, NULL, &t->_vkView);
    return t;
}

- (nullable id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor iosurface:(IOSurfaceRef)iosurface plane:(NSUInteger)plane {
    (void)plane;
    MX250MTLTexture *t = (MX250MTLTexture *)[self newTextureWithDescriptor:descriptor];
    if (!t || !iosurface) return t;

    CFRetain(iosurface);
    t->_iosurface = iosurface;
    t->_iosurfacePitch = (uint32_t)IOSurfaceGetBytesPerRow(iosurface);
    t->_iosurfaceAllocSize = (uint64_t)IOSurfaceGetAllocSize(iosurface);

    if (g_rt.set_next_iosurface_id && g_rt.was_last_iosurface_ok) {
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = t->_iosurfaceAllocSize,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        };
        if (g_rt.vkCreateBuffer(g_rt.vk_dev, &bci, NULL, &t->_iosurfaceVkBuf) == VK_SUCCESS) {
            VkMemoryRequirements mr;
            g_rt.vkGetBufferMemoryRequirements(g_rt.vk_dev, t->_iosurfaceVkBuf, &mr);
            uint32_t mti = mx250_find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkMemoryAllocateInfo mai = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = mr.size,
                .memoryTypeIndex = mti,
            };
            g_rt.set_next_iosurface_id((uint32_t)IOSurfaceGetID(iosurface));
            VkResult ar = g_rt.vkAllocateMemory(g_rt.vk_dev, &mai, NULL, &t->_iosurfaceVkMem);
            bool ok = g_rt.was_last_iosurface_ok();
            g_rt.set_next_iosurface_id(0);
            if (ar == VK_SUCCESS && ok) {
                g_rt.vkBindBufferMemory(g_rt.vk_dev, t->_iosurfaceVkBuf, t->_iosurfaceVkMem, 0);
            } else {
                if (ar == VK_SUCCESS) g_rt.vkFreeMemory(g_rt.vk_dev, t->_iosurfaceVkMem, NULL);
                t->_iosurfaceVkMem = VK_NULL_HANDLE;
                g_rt.vkDestroyBuffer(g_rt.vk_dev, t->_iosurfaceVkBuf, NULL);
                t->_iosurfaceVkBuf = VK_NULL_HANDLE;
            }
        }
    }
    return t;
}

- (MX250MTLTexture *)newZeroCopyDrawableTextureWidth:(uint32_t)w height:(uint32_t)h format:(MTLPixelFormat)fmt {
    uint32_t bpp = mx250_mtl_bpp(fmt);
    uint32_t pitch = (w * bpp + 255u) & ~255u;
    uint64_t allocSize = ((uint64_t)pitch * h + 65535ULL) & ~65535ULL;
    uint32_t pfmt = (fmt == MTLPixelFormatRGBA16Float) ? 'RGhA' : 'BGRA';

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSDictionary *props = @{
        (id)kIOSurfaceWidth: @(w),
        (id)kIOSurfaceHeight: @(h),
        (id)kIOSurfaceBytesPerElement: @(bpp),
        (id)kIOSurfaceBytesPerRow: @(pitch),
        (id)kIOSurfaceAllocSize: @(allocSize),
        (id)kIOSurfacePixelFormat: @(pfmt),
        (id)kIOSurfaceIsGlobal: @YES,
    };
#pragma clang diagnostic pop

    IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    if (!surf) return nil;
    IOSurfaceSetValue(surf, CFSTR("IOSurfaceColorSpace"), kCGColorSpaceSRGB);

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    MX250MTLTexture *tex = (MX250MTLTexture *)[self newTextureWithDescriptor:td iosurface:surf plane:0];
    CFRelease(surf);
    return tex;
}

- (nullable id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor {
    MX250MTLSamplerState *s = [[MX250MTLSamplerState alloc] init];
    s->_dev = self;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = (descriptor.magFilter == MTLSamplerMinMagFilterNearest) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
        .minFilter = (descriptor.minFilter == MTLSamplerMinMagFilterNearest) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
        .mipmapMode = (descriptor.mipFilter == MTLSamplerMipFilterLinear) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = (descriptor.sAddressMode == MTLSamplerAddressModeRepeat) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = (descriptor.tAddressMode == MTLSamplerAddressModeRepeat) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = (descriptor.rAddressMode == MTLSamplerAddressModeRepeat) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxAnisotropy = 1.0f,
        .minLod = descriptor.lodMinClamp,
        .maxLod = descriptor.lodMaxClamp,
    };
    g_rt.vkCreateSampler(g_rt.vk_dev, &sci, NULL, &s->_vkSampler);
    return s;
}

- (nullable id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)descriptor {
    MX250MTLDepthStencilState *ds = [[MX250MTLDepthStencilState alloc] init];
    ds->_dev = self;
    ds->_depthWriteEnabled = descriptor ? descriptor.isDepthWriteEnabled : NO;
    ds->_depthCompareOp = descriptor ? mx250_mtl_compare_to_vk(descriptor.depthCompareFunction) : VK_COMPARE_OP_ALWAYS;
    return ds;
}

- (nullable id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(nullable MTLCompileOptions *)options error:(NSError **)error {
    id<MTLLibrary> alib = [g_rt.compiler_device newLibraryWithSource:source options:options error:error];
    if (!alib) return nil;
    MX250MTLLibrary *lib = [[MX250MTLLibrary alloc] init];
    lib->_dev = self;
    lib->_appleLib = alib;
    return lib;
}

- (void)newLibraryWithSource:(NSString *)source options:(nullable MTLCompileOptions *)options completionHandler:(MTLNewLibraryCompletionHandler)completionHandler {
    NSError *err = nil;
    id<MTLLibrary> lib = [self newLibraryWithSource:source options:options error:&err];
    if (completionHandler) completionHandler(lib, err);
}

- (nullable id<MTLLibrary>)newDefaultLibrary {
    id<MTLLibrary> alib = [g_rt.compiler_device newDefaultLibrary];
    if (!alib) return nil;
    MX250MTLLibrary *lib = [[MX250MTLLibrary alloc] init];
    lib->_dev = self;
    lib->_appleLib = alib;
    return lib;
}

- (nullable id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error {
    id<MTLLibrary> alib = [g_rt.compiler_device newDefaultLibraryWithBundle:bundle error:error];
    if (!alib) return nil;
    MX250MTLLibrary *lib = [[MX250MTLLibrary alloc] init];
    lib->_dev = self;
    lib->_appleLib = alib;
    return lib;
}

- (nullable id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error {
    id<MTLLibrary> alib = [g_rt.compiler_device newLibraryWithURL:url error:error];
    if (!alib) return nil;
    MX250MTLLibrary *lib = [[MX250MTLLibrary alloc] init];
    lib->_dev = self;
    lib->_appleLib = alib;
    return lib;
}

- (nullable id<MTLLibrary>)newLibraryWithFile:(NSString *)filepath error:(NSError **)error {
    return [self newLibraryWithURL:[NSURL fileURLWithPath:filepath] error:error];
}

- (nullable id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {
    id<MTLLibrary> alib = [g_rt.compiler_device newLibraryWithData:data error:error];
    if (!alib) return nil;
    MX250MTLLibrary *lib = [[MX250MTLLibrary alloc] init];
    lib->_dev = self;
    lib->_appleLib = alib;
    return lib;
}

- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)computeFunction error:(NSError **)error {
    MX250MTLFunction *fn = (MX250MTLFunction *)computeFunction;
    if (!fn || ![fn ensureTranslatedWithError:error]) return nil;

    MX250MTLComputePipelineState *pso = [[MX250MTLComputePipelineState alloc] init];
    pso->_dev = self;
    pso->_fn = fn;

    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fn->_spirvData.length,
        .pCode = (const uint32_t *)fn->_spirvData.bytes,
    };
    if (g_rt.vkCreateShaderModule(g_rt.vk_dev, &smci, NULL, &pso->_vkShader) != VK_SUCCESS)
        return nil;

    pso->_vkSetLayout = mx250_build_set_layout(fn, VK_SHADER_STAGE_COMPUTE_BIT);
    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = 48,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &pso->_vkSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    g_rt.vkCreatePipelineLayout(g_rt.vk_dev, &plci, NULL, &pso->_vkPipeLayout);

    /* Pre-warm default local_size pipeline */
    [pso pipelineForThreadgroupSize:MTLSizeMake(fn->_localSize[0], fn->_localSize[1], fn->_localSize[2])];
    return pso;
}

- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)computeFunction options:(MTLPipelineOption)options reflection:(MTLAutoreleasedComputePipelineReflection *)reflection error:(NSError **)error {
    (void)options;
    if (reflection) *reflection = nil;
    return [self newComputePipelineStateWithFunction:computeFunction error:error];
}

- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor options:(MTLPipelineOption)options reflection:(MTLAutoreleasedComputePipelineReflection *)reflection error:(NSError **)error {
    (void)options;
    if (reflection) *reflection = nil;
    return [self newComputePipelineStateWithFunction:descriptor.computeFunction error:error];
}

- (nullable id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    if (!descriptor || !descriptor.vertexFunction) return nil;
    MX250MTLFunction *vs = (MX250MTLFunction *)descriptor.vertexFunction;
    MX250MTLFunction *fs = (MX250MTLFunction *)descriptor.fragmentFunction;
    if (![vs ensureTranslatedWithError:error]) return nil;
    if (fs && ![fs ensureTranslatedWithError:error]) return nil;

    MX250MTLRenderPipelineState *pso = [[MX250MTLRenderPipelineState alloc] init];
    pso->_dev = self;
    pso->_vsFn = vs;
    pso->_fsFn = fs;

    VkShaderModuleCreateInfo vsm = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vs->_spirvData.length,
        .pCode = (const uint32_t *)vs->_spirvData.bytes,
    };
    g_rt.vkCreateShaderModule(g_rt.vk_dev, &vsm, NULL, &pso->_vsMod);
    if (fs) {
        VkShaderModuleCreateInfo fsm = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = fs->_spirvData.length,
            .pCode = (const uint32_t *)fs->_spirvData.bytes,
        };
        g_rt.vkCreateShaderModule(g_rt.vk_dev, &fsm, NULL, &pso->_fsMod);
    }

    pso->_setLayouts[0] = mx250_build_set_layout(vs, VK_SHADER_STAGE_VERTEX_BIT);
    pso->_setLayouts[1] = mx250_build_set_layout(fs, VK_SHADER_STAGE_FRAGMENT_BIT);
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2,
        .pSetLayouts = pso->_setLayouts,
    };
    g_rt.vkCreatePipelineLayout(g_rt.vk_dev, &plci, NULL, &pso->_pipeLayout);

    pso->_colorCount = 0;
    for (uint32_t i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = descriptor.colorAttachments[i];
        if (ca.pixelFormat == MTLPixelFormatInvalid) break;
        pso->_colorFormats[pso->_colorCount++] = mx250_mtl_to_vk_format(ca.pixelFormat);
        if (i == 0) pso->_blendEnabled = ca.isBlendingEnabled;
    }
    pso->_depthFormat = (descriptor.depthAttachmentPixelFormat == MTLPixelFormatDepth32Float)
                            ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;

    MTLVertexDescriptor *vd = descriptor.vertexDescriptor;
    if (vd) {
        bool used_slots[32] = {false};
        for (uint32_t a = 0; a < 31; a++) {
            MTLVertexAttributeDescriptor *ad = vd.attributes[a];
            if (!ad || ad.format == MTLVertexFormatInvalid) continue;
            uint32_t bidx = (uint32_t)ad.bufferIndex;
            pso->_vaAttrs[pso->_vaAttrCount++] = (VkVertexInputAttributeDescription){
                .location = a,
                .binding = bidx,
                .format = mx250_mtl_vertex_to_vk_format(ad.format),
                .offset = (uint32_t)ad.offset,
            };
            used_slots[bidx] = true;
        }
        for (uint32_t b = 0; b < 31; b++) {
            if (!used_slots[b]) continue;
            MTLVertexBufferLayoutDescriptor *ld = vd.layouts[b];
            if (!ld || ld.stride == 0) continue;
            pso->_vbBinds[pso->_vbBindCount++] = (VkVertexInputBindingDescription){
                .binding = b,
                .stride = (uint32_t)ld.stride,
                .inputRate = (ld.stepFunction == MTLVertexStepFunctionPerInstance)
                                 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
            };
            pso->_isStageInVB[b] = true;
        }
    }

    /* Pre-warm default triangle pipeline */
    [pso pipelineForTopology:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                    cullMode:VK_CULL_MODE_NONE
                   frontFace:VK_FRONT_FACE_COUNTER_CLOCKWISE
                   depthTest:(pso->_depthFormat != VK_FORMAT_UNDEFINED)
                  depthWrite:(pso->_depthFormat != VK_FORMAT_UNDEFINED)
                     depthOp:VK_COMPARE_OP_LESS];
    return pso;
}

- (nullable id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor options:(MTLPipelineOption)options reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection error:(NSError **)error {
    (void)options;
    if (reflection) *reflection = nil;
    return [self newRenderPipelineStateWithDescriptor:descriptor error:error];
}

@end

/* ========================================================================= *
 * 8. CAMetalLayer Zero-Copy IOSurface Drawable Ring & Mach-O Interposition  *
 * ========================================================================= */
@interface MX250LayerSwapState : NSObject {
@public
    uint32_t          width;
    uint32_t          height;
    MTLPixelFormat    format;
    uint32_t          cursor;
    MX250MTLDrawable *drawables[3];
}
@end
@implementation MX250LayerSwapState
@end

static IMP g_orig_layer_setDevice    = NULL;
static IMP g_orig_layer_device       = NULL;
static IMP g_orig_layer_nextDrawable = NULL;

static void mx250_swizzled_setDevice(CAMetalLayer *self, SEL _cmd, id<MTLDevice> device) {
    if ([device isKindOfClass:[MX250MTLDevice class]]) {
        objc_setAssociatedObject(self, &kMX250LayerDeviceKey, device, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return;
    }
    objc_setAssociatedObject(self, &kMX250LayerDeviceKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    ((void (*)(id, SEL, id))g_orig_layer_setDevice)(self, _cmd, device);
}

static id<MTLDevice> mx250_swizzled_device(CAMetalLayer *self, SEL _cmd) {
    id<MTLDevice> d = objc_getAssociatedObject(self, &kMX250LayerDeviceKey);
    if (d) return d;
    return ((id (*)(id, SEL))g_orig_layer_device)(self, _cmd);
}

static id<CAMetalDrawable> mx250_swizzled_nextDrawable(CAMetalLayer *self, SEL _cmd) {
    id<MTLDevice> d = objc_getAssociatedObject(self, &kMX250LayerDeviceKey);
    if (![d isKindOfClass:[MX250MTLDevice class]]) {
        return ((id (*)(id, SEL))g_orig_layer_nextDrawable)(self, _cmd);
    }
    MX250MTLDevice *mxDev = (MX250MTLDevice *)d;
    CGSize ds = self.drawableSize;
    if (ds.width <= 0 || ds.height <= 0) {
        CGFloat sc = self.contentsScale > 0 ? self.contentsScale : 1.0;
        ds = CGSizeMake(self.bounds.size.width * sc, self.bounds.size.height * sc);
    }
    uint32_t w = (uint32_t)MAX(1.0, ds.width);
    uint32_t h = (uint32_t)MAX(1.0, ds.height);
    MTLPixelFormat fmt = self.pixelFormat ?: MTLPixelFormatBGRA8Unorm;

    MX250LayerSwapState *st = objc_getAssociatedObject(self, &kMX250LayerStateKey);
    if (!st || st->width != w || st->height != h || st->format != fmt) {
        st = [[MX250LayerSwapState alloc] init];
        st->width = w;
        st->height = h;
        st->format = fmt;
        st->cursor = 0;
        for (int i = 0; i < 3; i++) {
            MX250MTLDrawable *drw = [[MX250MTLDrawable alloc] init];
            drw->_layer = self;
            drw->_drawableID = (NSUInteger)(i + 1);
            drw->_texture = [mxDev newZeroCopyDrawableTextureWidth:w height:h format:fmt];
            st->drawables[i] = drw;
        }
        objc_setAssociatedObject(self, &kMX250LayerStateKey, st, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        if (g_rt.log_enabled) {
            fprintf(stderr,
                    "[MX250-Metal] Zero-Copy CAMetalLayer Swapchain ready: %ux%u (pitch=%u, 3 IOSurfaces, 0 CPU memcpy, 0 Intel Metal blit)\n",
                    w, h, st->drawables[0]->_texture->_iosurfacePitch);
        }
    }
    uint32_t idx = (st->cursor++) % 3u;
    return st->drawables[idx];
}

__attribute__((constructor))
static void mx250_metal_interpose_ctor(void) {
    Class cls = [CAMetalLayer class];
    Method mSet = class_getInstanceMethod(cls, @selector(setDevice:));
    Method mGet = class_getInstanceMethod(cls, @selector(device));
    Method mNxt = class_getInstanceMethod(cls, @selector(nextDrawable));
    if (mSet) g_orig_layer_setDevice    = method_setImplementation(mSet, (IMP)mx250_swizzled_setDevice);
    if (mGet) g_orig_layer_device       = method_setImplementation(mGet, (IMP)mx250_swizzled_device);
    if (mNxt) g_orig_layer_nextDrawable = method_setImplementation(mNxt, (IMP)mx250_swizzled_nextDrawable);
}

/* Exported Interposed C Entry Points */
static bool mx250_caller_is_apple_system_ui(const void *ret_addr) {
    Dl_info info;
    if (ret_addr && dladdr(ret_addr, &info) && info.dli_fname) {
        if (strncmp(info.dli_fname, "/System/Library/", 16) == 0)
            return true;
    }
    return false;
}

NS_RETURNS_RETAINED id<MTLDevice> mx250_MTLCreateSystemDefaultDevice(void) {
    MX250MTLDevice *dev = [MX250MTLDevice sharedDevice];
    if (mx250_caller_is_apple_system_ui(__builtin_return_address(0))) {
        return g_rt.compiler_device;
    }
    if (dev) return dev;
    return g_rt.compiler_device;
}

NS_RETURNS_RETAINED NSArray<id<MTLDevice>> *mx250_MTLCopyAllDevices(void) {
    MX250MTLDevice *dev = [MX250MTLDevice sharedDevice];
    if (mx250_caller_is_apple_system_ui(__builtin_return_address(0))) {
        return g_rt.compiler_device ? @[g_rt.compiler_device] : @[];
    }
    NSMutableArray<id<MTLDevice>> *arr = [NSMutableArray array];
    if (dev) [arr addObject:dev];
    if (g_rt.compiler_device) [arr addObject:g_rt.compiler_device];
    return arr;
}

typedef struct mx250_dyld_interpose_tuple {
    const void *replacement;
    const void *replacee;
} mx250_dyld_interpose_tuple;

__attribute__((used))
static const mx250_dyld_interpose_tuple g_mx250_metal_interpose[]
    __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)&mx250_MTLCreateSystemDefaultDevice, (const void *)&MTLCreateSystemDefaultDevice },
    { (const void *)&mx250_MTLCopyAllDevices,            (const void *)&MTLCopyAllDevices },
};
