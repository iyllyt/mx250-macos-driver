/*
 * tools/mx250_bench.c
 *
 * Hardware Benchmark for NVIDIA GeForce MX250 (GP108M, 384 FP32 CUDA Cores):
 *   1. Sustained FP32 Compute GFLOPS & Effective Shader Core Clock (MHz)
 *      via GPU hardware PTIMER timestamps (`VK_QUERY_TYPE_TIMESTAMP`, 1.0 ns)
 *   2. 64 MiB GDDR5 Device-Local VRAM Bandwidth (GB/s)
 *   3. 16 MiB PCIe Gen3 x4 Host <-> VRAM DMA Bandwidth (GB/s)
 */

#include <vulkan/vulkan.h>
#include "gp108_bench_spirv.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define VK_CHK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "VK_ERR %d at %s:%d\n", _r, __FILE__, __LINE__); return 1; } } while (0)

static uint32_t find_mem(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

static int make_buf(VkDevice dev, const VkPhysicalDeviceMemoryProperties *mp,
                    VkDeviceSize sz, VkBufferUsageFlags usage, VkMemoryPropertyFlags mflags,
                    VkBuffer *out_buf, VkDeviceMemory *out_mem) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz,
                               .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHK(vkCreateBuffer(dev, &bci, NULL, out_buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, *out_buf, &mr);
    uint32_t mt = find_mem(mp, mr.memoryTypeBits, mflags);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size, .memoryTypeIndex = mt };
    VK_CHK(vkAllocateMemory(dev, &mai, NULL, out_mem));
    VK_CHK(vkBindBufferMemory(dev, *out_buf, *out_mem, 0));
    return 0;
}

int main(void) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst;
    VK_CHK(vkCreateInstance(&ici, NULL, &inst));

    uint32_t cnt = 1; VkPhysicalDevice pdev;
    VK_CHK(vkEnumeratePhysicalDevices(inst, &cnt, &pdev));
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceProperties(pdev, &props);
    vkGetPhysicalDeviceMemoryProperties(pdev, &mp);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev;
    VK_CHK(vkCreateDevice(pdev, &dci, NULL, &dev));
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                     .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VkCommandPool pool;
    VK_CHK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1 };
    VkCommandBuffer cb;
    VK_CHK(vkAllocateCommandBuffers(dev, &cbai, &cb));

    VkQueryPoolCreateInfo qpci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                   .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 8 };
    VkQueryPool qp;
    VK_CHK(vkCreateQueryPool(dev, &qpci, NULL, &qp));

    /* 1. Compute Pipeline for FFMA Benchmark */
    VkBuffer ssbo; VkDeviceMemory ssbo_mem;
    make_buf(dev, &mp, 65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ssbo, &ssbo_mem);

    VkDescriptorSetLayoutBinding bind0 = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .bindingCount = 1, .pBindings = &bind0 };
    VkDescriptorSetLayout dsl;
    VK_CHK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 16 };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &dsl,
                                        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VkPipelineLayout pl;
    VK_CHK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));

    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = sizeof(kGP108BenchFmaSpv), .pCode = kGP108BenchFmaSpv };
    VkShaderModule sm;
    VK_CHK(vkCreateShaderModule(dev, &smci, NULL, &sm));

    VkComputePipelineCreateInfo cp_ci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
        .layout = pl
    };
    VkPipeline pipe;
    VK_CHK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp_ci, NULL, &pipe));

    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
    VkDescriptorPool dpool;
    VK_CHK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet ds;
    VK_CHK(vkAllocateDescriptorSets(dev, &dsai, &ds));
    VkDescriptorBufferInfo dbi = { .buffer = ssbo, .offset = 0, .range = 65536 };
    VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds,
                                 .dstBinding = 0, .descriptorCount = 1,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
    vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

    /* Warmup + Timed Compute Dispatch:
     * 4096 workgroups x 64 threads = 262,144 threads
     * 2048 loop iters x 128 FLOPs/iter = 262,144 FLOPs/thread
     * Total = 68.719476736 GFLOPs per command buffer!
     */
    const uint32_t groups = 4096;
    const uint32_t threads = groups * 64;
    const uint32_t iters = 4096;
    const uint32_t pc_warmup[4] = { 1024, 1, 0x3f800001u, 0x34800000u };
    const uint32_t pc_bench[4]  = { iters, 1, 0x3f800001u, 0x34800000u };
    const double total_flops = (double)threads * (double)iters * 128.0;

    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK_CHK(vkBeginCommandBuffer(cb, &bi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc_warmup);
    vkCmdDispatch(cb, groups, 1, 1);
    VK_CHK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VK_CHK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    VK_CHK(vkQueueWaitIdle(q));

    /* Timed Compute Run */
    vkResetCommandBuffer(cb, 0);
    VK_CHK(vkBeginCommandBuffer(cb, &bi));
    vkCmdResetQueryPool(cb, qp, 0, 8);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc_bench);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
    vkCmdDispatch(cb, groups, 1, 1);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
    VK_CHK(vkEndCommandBuffer(cb));
    VK_CHK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    VK_CHK(vkQueueWaitIdle(q));

    uint64_t ts[2] = {0, 0};
    VK_CHK(vkGetQueryPoolResults(dev, qp, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    double comp_ns = (double)(ts[1] - ts[0]) * (double)props.limits.timestampPeriod;
    double gflops  = total_flops / comp_ns; /* FLOPs / ns == GFLOPS */
    /* GP108 has 3 SMs = 384 FP32 cores = 768 FLOPs/cycle peak -> effective shader MHz */
    double eff_mhz = (gflops * 1000.0) / 768.0;

    /* 2. 64 MiB GDDR5 VRAM -> VRAM Copy Bandwidth + 16 MiB PCIe Host <-> VRAM Bandwidth */
    const VkDeviceSize vram_sz = 64ULL << 20;
    const VkDeviceSize pcie_sz = 16ULL << 20;
    VkBuffer vbuf_a, vbuf_b, hbuf;
    VkDeviceMemory vmem_a, vmem_b, hmem;
    make_buf(dev, &mp, vram_sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &vbuf_a, &vmem_a);
    make_buf(dev, &mp, vram_sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &vbuf_b, &vmem_b);
    make_buf(dev, &mp, pcie_sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &hbuf, &hmem);

    vkResetCommandBuffer(cb, 0);
    VK_CHK(vkBeginCommandBuffer(cb, &bi));
    vkCmdResetQueryPool(cb, qp, 0, 8);
    VkBufferCopy c_vram = { 0, 0, vram_sz };
    VkBufferCopy c_pcie = { 0, 0, pcie_sz };
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
    for (int i = 0; i < 4; i++)
        vkCmdCopyBuffer(cb, vbuf_a, vbuf_b, 1, &c_vram); /* 256 MiB x2 R+W = 512 MiB GDDR5 traffic */
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
    vkCmdCopyBuffer(cb, vbuf_a, hbuf, 1, &c_pcie);       /* 16 MiB VRAM -> Host DMA */
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2);
    VK_CHK(vkEndCommandBuffer(cb));
    VK_CHK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    VK_CHK(vkQueueWaitIdle(q));

    uint64_t cts[3] = {0};
    VK_CHK(vkGetQueryPoolResults(dev, qp, 0, 3, sizeof(cts), cts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    double vram_ns = (double)(cts[1] - cts[0]) * (double)props.limits.timestampPeriod;
    double pcie_ns = (double)(cts[2] - cts[1]) * (double)props.limits.timestampPeriod;
    /* Copy reads 256 MiB + writes 256 MiB = 512 MiB total DRAM bus bytes */
    double vram_rw_gbps = (double)(vram_sz * 4ULL * 2ULL) / vram_ns;
    double pcie_gbps    = (double)pcie_sz / pcie_ns;

    printf("==================================================================\n");
    printf("  NVIDIA GeForce MX250 (GP108, 384 CUDA Cores) Live Benchmark\n");
    printf("==================================================================\n");
    printf("  FP32 Compute Throughput : %7.2f GFLOPS  (%.2f ms for %.2f GFLOP)\n", gflops, comp_ns / 1e6, total_flops / 1e9);
    printf("  Implied Shader Clock    : %7.1f MHz     (vs 1582.0 MHz Full P0)\n", eff_mhz);
    printf("  GDDR5 VRAM Bandwidth    : %7.2f GB/s    (vs   56.1 GB/s Full P0)\n", vram_rw_gbps);
    printf("  PCIe Gen3 x4 DMA (V->H) : %7.2f GB/s\n", pcie_gbps);
    printf("==================================================================\n");

    vkDestroyBuffer(dev, hbuf, NULL); vkFreeMemory(dev, hmem, NULL);
    vkDestroyBuffer(dev, vbuf_b, NULL); vkFreeMemory(dev, vmem_b, NULL);
    vkDestroyBuffer(dev, vbuf_a, NULL); vkFreeMemory(dev, vmem_a, NULL);
    vkDestroyBuffer(dev, ssbo, NULL); vkFreeMemory(dev, ssbo_mem, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyShaderModule(dev, sm, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorPool(dev, dpool, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyQueryPool(dev, qp, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return 0;
}
