/* SPDX-License-Identifier: MIT
 * Phase 2b End-to-End Hardware Self-Test on Live NVIDIA GeForce MX250 (GP108)
 * Verifies:
 *   1. Full NVKM + NVIF bring-up on macOS Sequoia userspace
 *   2. Official GP108 ACR + SEC2 WPR secure boot & FECS/GPCCS golden context
 *   3. GP100 40-bit MMU + VMM virtual address space & VRAM/Host memory mapping
 *   4. KEPLER_CHANNEL_GROUP_A (0xa06c) + PASCAL_CHANNEL_GPFIFO_A (0xc06f)
 *   5. Instantiation of PASCAL_DMA_COPY_B (0xc1b5), PASCAL_COMPUTE_B (0xc1c0), PASCAL_B (0xc197)
 *   6. 16 MB Host -> GDDR5 VRAM -> Host hardware GPFIFO DMA benchmark & byte-for-byte check
 */
#include <execinfo.h>
#include <signal.h>
#include <nvif/os.h>
#include <core/device.h>
#include <nvif/client.h>
#include <nvif/device.h>
#include <nvif/mmu.h>
#include <nvif/vmm.h>
#include <nvif/mem.h>
#include <nvif/fifo.h>
#include <nvif/chan.h>
#include <nvif/class.h>
#include <nvif/cl0080.h>
#include <nvif/ifc00d.h>
#include <nvif/if0020.h>
#include <nvif/if0021.h>
#include <nvif/push906f.h>
#include <nvhw/class/cl906f.h>
#include <nvhw/class/cla0b5.h>

#include "../nvkm_macos/gp108_nvkm_macos.h"

static void crash_handler(int sig)
{
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "\n[CRASH] Caught signal %d, backtrace (%d frames):\n", sig, n);
    backtrace_symbols_fd(bt, n, 2);
    _exit(128 + sig);
}

#define TEST_DMA_BYTES  (16ULL * 1024ULL * 1024ULL) /* 16 MiB */
#define TEST_DMA_PAGES  (TEST_DMA_BYTES / 4096ULL)

static int map_into_vmm(struct nvif_vmm *vmm, struct nvif_mem *mem,
                        bool is_vram, struct nvif_vma *out_vma)
{
    int ret = nvif_vmm_get(vmm, PTES, false, mem->page, 0, mem->size, out_vma);
    if (ret)
        return ret;

    struct gp100_vmm_map_v0 map_args = {
        .version = 0,
        .vol     = is_vram ? 0 : 1, /* coherent sysmem = volatile (snoop PCIe) */
        .ro      = 0,
        .priv    = 0,
        .kind    = 0,               /* pitch linear */
    };
    ret = nvif_vmm_map(vmm, out_vma->addr, mem->size,
                       &map_args, sizeof(map_args), mem, 0);
    if (ret)
        nvif_vmm_put(vmm, out_vma);
    return ret;
}

static int submit_ce_copy_and_fence(struct nvif_chan *chan,
                                    u64 src_gpu_va, u64 dst_gpu_va,
                                    u32 total_pages,
                                    volatile uint32_t *fence_cpu,
                                    u64 fence_gpu_va, uint32_t fence_val)
{
    struct nvif_push *push = &chan->push;
    u32 rem_pages = total_pages;

    while (rem_pages > 0) {
        u32 chunk_pages = (rem_pages > 8192U) ? 8192U : rem_pages;
        u32 line_len    = (chunk_pages >= 16U && (chunk_pages & 15U) == 0) ? 65536U : 4096U;
        u32 line_cnt    = (chunk_pages * 4096U) / line_len;
        int ret = PUSH_WAIT(push, 12);
        if (ret)
            return ret;

        PUSH_MTHD(push, NVA0B5, OFFSET_IN_UPPER,
                  NVVAL(NVA0B5, OFFSET_IN_UPPER, UPPER, upper_32_bits(src_gpu_va)),
                  OFFSET_IN_LOWER, lower_32_bits(src_gpu_va),
                  OFFSET_OUT_UPPER,
                  NVVAL(NVA0B5, OFFSET_OUT_UPPER, UPPER, upper_32_bits(dst_gpu_va)),
                  OFFSET_OUT_LOWER, lower_32_bits(dst_gpu_va),
                  PITCH_IN, line_len,
                  PITCH_OUT, line_len,
                  LINE_LENGTH_IN, line_len,
                  LINE_COUNT, line_cnt);

        PUSH_IMMD(push, NVA0B5, LAUNCH_DMA,
                  NVDEF(NVA0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
                  NVDEF(NVA0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
                  NVDEF(NVA0B5, LAUNCH_DMA, SEMAPHORE_TYPE, NONE) |
                  NVDEF(NVA0B5, LAUNCH_DMA, INTERRUPT_TYPE, NONE) |
                  NVDEF(NVA0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
                  NVDEF(NVA0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
                  NVDEF(NVA0B5, LAUNCH_DMA, MULTI_LINE_ENABLE, TRUE) |
                  NVDEF(NVA0B5, LAUNCH_DMA, REMAP_ENABLE, FALSE) |
                  NVDEF(NVA0B5, LAUNCH_DMA, BYPASS_L2, USE_PTE_SETTING) |
                  NVDEF(NVA0B5, LAUNCH_DMA, SRC_TYPE, VIRTUAL) |
                  NVDEF(NVA0B5, LAUNCH_DMA, DST_TYPE, VIRTUAL));

        src_gpu_va += (u64)chunk_pages * 4096ULL;
        dst_gpu_va += (u64)chunk_pages * 4096ULL;
        rem_pages  -= chunk_pages;
    }

    int ret = PUSH_WAIT(push, 6);
    if (ret)
        return ret;

    PUSH_MTHD(push, NV906F, SEMAPHOREA,
              NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(fence_gpu_va)),
              SEMAPHOREB, lower_32_bits(fence_gpu_va),
              SEMAPHOREC, fence_val,
              SEMAPHORED,
              NVDEF(NV906F, SEMAPHORED, OPERATION, RELEASE) |
              NVDEF(NV906F, SEMAPHORED, RELEASE_WFI, EN) |
              NVDEF(NV906F, SEMAPHORED, RELEASE_SIZE, 16BYTE));

    PUSH_KICK(push);

    u64 deadline = ktime_get_ns() + 3000000000ULL; /* 3s timeout */
    while (*fence_cpu != fence_val) {
        if (ktime_get_ns() > deadline) {
            fprintf(stderr, "[FAIL] Timeout waiting for GPU fence %u (current=%u)\n",
                    fence_val, *fence_cpu);
            return -ETIMEDOUT;
        }
        __asm__ volatile("pause");
    }
    return 0;
}

int gp108_nvkm_selftest_run(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== GP108 (MX250) macOS Userspace NVKM/NVIF Full Hardware Self-Test ===\n");

    struct nvkm_device *nvkm_dev = NULL;
    struct nvif_client  client;
    struct nvif_device  device;

    u64 t_boot0 = ktime_get_ns();
    int ret = gp108_nvkm_bootstrap("NvBios=mx250_vbios.rom", "info",
                                   &nvkm_dev, &client, &device);
    u64 t_boot1 = ktime_get_ns();
    if (ret) {
        fprintf(stderr, "[FAIL] gp108_nvkm_bootstrap failed: %d\n", ret);
        return 1;
    }

    printf("[OK] NVKM + ACR/SEC2 Secure Boot + Golden Context initialized in %.2f ms!\n",
           (double)(t_boot1 - t_boot0) / 1e6);
    printf("     Chip: %s (%s) | Arch=0x%02x Chipset=0x%03x Rev=0x%02x\n",
           device.info.chip, device.info.name,
           device.info.family, device.info.chipset, device.info.revision);
    printf("     VRAM Total: %llu MiB | Usable VRAM: %llu MiB | PTIMER: %llu ns\n",
           (unsigned long long)(device.info.ram_size >> 20),
           (unsigned long long)(device.info.ram_user >> 20),
           (unsigned long long)nvif_device_time(&device));

    /* 1. Construct MMU & GP100 VMM */
    struct nvif_mmu mmu = {};
    ret = nvif_mmu_ctor(&device.object, "selftestMmu", NVIF_CLASS_MMU_GF100, &mmu);
    if (ret) {
        fprintf(stderr, "[FAIL] nvif_mmu_ctor failed: %d\n", ret);
        return 2;
    }
    printf("[OK] MMU constructed: dmabits=%u heaps=%u types=%u kinds=%u\n",
           mmu.dmabits, mmu.heap_nr, mmu.type_nr, mmu.kind_nr);

    struct gp100_vmm_v0 vmm_args = { .version = 0, .fault_replay = 0 };
    struct nvif_vmm vmm = {};
    ret = nvif_vmm_ctor(&mmu, "selftestVmm", NVIF_CLASS_VMM_GP100,
                        UNMANAGED, 0x10000, 0, &vmm_args, sizeof(vmm_args), &vmm);
    if (ret) {
        fprintf(stderr, "[FAIL] nvif_vmm_ctor failed: %d\n", ret);
        return 3;
    }
    printf("[OK] GP100 49-bit VMM constructed: VA range [0x%llx .. 0x%llx]\n",
           (unsigned long long)vmm.start, (unsigned long long)vmm.limit);

    /* 2. Allocate Pushbuffer + GPFIFO + Semaphores in Coherent Host DMA */
    const u32 push_len   = 0x10000; /* 64 KiB pushbuffer */
    const u32 gpfifo_len = 0x02000; /* 8 KiB GPFIFO (1024 entries) */
    const u32 sema_len   = 0x01000; /* 4 KiB semaphores + user fences */

    struct nvif_mem cmd_mem = {};
    struct nvif_vma cmd_vma = {};
    ret = nvif_mem_ctor_map(&mmu, "chanCmdMem", NVIF_MEM_HOST | NVIF_MEM_COHERENT,
                            push_len + gpfifo_len + sema_len, &cmd_mem);
    if (ret) {
        fprintf(stderr, "[FAIL] cmd_mem alloc failed: %d\n", ret);
        return 4;
    }
    ret = map_into_vmm(&vmm, &cmd_mem, false, &cmd_vma);
    if (ret) {
        fprintf(stderr, "[FAIL] cmd_mem VMM map failed: %d\n", ret);
        return 5;
    }

    uint8_t *cmd_cpu  = (uint8_t *)cmd_mem.object.map.ptr;
    void *push_cpu    = cmd_cpu;
    void *gpfifo_cpu  = cmd_cpu + push_len;
    void *sema_cpu    = cmd_cpu + push_len + gpfifo_len;
    u64   push_gpu    = cmd_vma.addr;
    u64   gpfifo_gpu  = cmd_vma.addr + push_len;
    u64   sema_gpu    = cmd_vma.addr + push_len + gpfifo_len;
    volatile uint32_t *user_fence_cpu = (volatile uint32_t *)((uint8_t *)sema_cpu + 0x100);
    u64                user_fence_gpu = sema_gpu + 0x100;

    /* 3. Create TSG (KEPLER_CHANNEL_GROUP_A) + Channel (PASCAL_CHANNEL_GPFIFO_A) */
    u64 runm = nvif_fifo_runlist(&device, NV_DEVICE_HOST_RUNLIST_ENGINES_GR);
    if (!runm) {
        fprintf(stderr, "[FAIL] No GR runlist found!\n");
        return 6;
    }
    u8 runlist_id = (u8)__ffs64(runm);

    struct {
        struct nvif_cgrp_v0 v0;
        char name[16];
    } __packed cgrp_args = {};
    cgrp_args.v0.version = 0;
    cgrp_args.v0.namelen = sizeof(cgrp_args.name);
    cgrp_args.v0.runlist = runlist_id;
    cgrp_args.v0.vmm     = nvif_handle(&vmm.object);
    snprintf(cgrp_args.name, sizeof(cgrp_args.name), "gp108Tsg0");

    struct nvif_object cgrp_obj = {};
    ret = nvif_object_ctor(&device.object, "selftestCgrp", 0x100,
                           KEPLER_CHANNEL_GROUP_A, &cgrp_args, sizeof(cgrp_args), &cgrp_obj);
    if (ret) {
        fprintf(stderr, "[FAIL] KEPLER_CHANNEL_GROUP_A ctor failed: %d\n", ret);
        return 7;
    }

    struct {
        struct nvif_chan_v0 v0;
        char name[16];
    } __packed chan_args = {};
    chan_args.v0.version = 0;
    chan_args.v0.namelen = sizeof(chan_args.name);
    chan_args.v0.runlist = runlist_id;
    chan_args.v0.runq    = 0;
    chan_args.v0.priv    = 1;
    chan_args.v0.devm    = 1;
    chan_args.v0.vmm     = nvif_handle(&vmm.object);
    chan_args.v0.offset  = gpfifo_gpu;
    chan_args.v0.length  = gpfifo_len;
    snprintf(chan_args.name, sizeof(chan_args.name), "gp108Chan0");

    struct nvif_object chan_obj = {};
    ret = nvif_object_ctor(&cgrp_obj, "selftestChan", 0x101,
                           PASCAL_CHANNEL_GPFIFO_A, &chan_args, sizeof(chan_args), &chan_obj);
    if (ret) {
        fprintf(stderr, "[FAIL] PASCAL_CHANNEL_GPFIFO_A ctor failed: %d\n", ret);
        return 8;
    }
    ret = nvif_object_map(&chan_obj, NULL, 0);
    if (ret) {
        fprintf(stderr, "[FAIL] USERD map failed: %d\n", ret);
        return 9;
    }

    struct nvif_chan gpfifo_chan = {};
    nvif_chan906f_ctor(&gpfifo_chan, chan_obj.map.ptr, gpfifo_cpu, gpfifo_len,
                       push_cpu, push_gpu, push_len, sema_cpu, sema_gpu);
    printf("[OK] TSG cgid=%u + PASCAL_CHANNEL_GPFIFO_A (0xc06f) chid=%u inst=0x%llx online!\n",
           cgrp_args.v0.cgid, chan_args.v0.chid, (unsigned long long)chan_args.v0.inst);

    /* 4. Instantiate Copy, Compute, and 3D Hardware Engine Classes */
    struct nvif_object ce_obj = {}, comp_obj = {}, eng3d_obj = {};
    ret = nvif_object_ctor(&chan_obj, "pascalCe", PASCAL_DMA_COPY_B,
                           PASCAL_DMA_COPY_B, NULL, 0, &ce_obj);
    if (ret) {
        fprintf(stderr, "[FAIL] PASCAL_DMA_COPY_B (0xc1b5) ctor failed: %d\n", ret);
        return 10;
    }
    ret = nvif_object_ctor(&chan_obj, "pascalCompute", PASCAL_COMPUTE_B,
                           PASCAL_COMPUTE_B, NULL, 0, &comp_obj);
    if (ret) {
        fprintf(stderr, "[FAIL] PASCAL_COMPUTE_B (0xc1c0) ctor failed: %d\n", ret);
        return 11;
    }
    ret = nvif_object_ctor(&chan_obj, "pascal3D", PASCAL_B,
                           PASCAL_B, NULL, 0, &eng3d_obj);
    if (ret) {
        fprintf(stderr, "[FAIL] PASCAL_B 3D (0xc197) ctor failed: %d\n", ret);
        return 12;
    }
    printf("[OK] Hardware engines bound on channel: CE=0x%04x, COMPUTE=0x%04x, 3D=0x%04x!\n",
           PASCAL_DMA_COPY_B, PASCAL_COMPUTE_B, PASCAL_B);

    /* Bind subchannel 4 (NVA0B5 = subc 4 in push906f) to PASCAL_DMA_COPY_B */
    ret = PUSH_WAIT(&gpfifo_chan.push, 2);
    if (ret) return 13;
    PUSH_NVSQ(&gpfifo_chan.push, NVA0B5, 0x0000, PASCAL_DMA_COPY_B);
    PUSH_KICK(&gpfifo_chan.push);

    /* 5. 16 MiB Host -> GDDR5 VRAM -> Host DMA Benchmark & Verification */
    struct nvif_mem src_host = {}, mid_vram = {}, dst_host = {};
    struct nvif_vma src_vma  = {}, mid_vma  = {}, dst_vma  = {};

    ret = nvif_mem_ctor_map(&mmu, "dmaSrcHost", NVIF_MEM_HOST | NVIF_MEM_COHERENT, TEST_DMA_BYTES, &src_host);
    if (ret) { fprintf(stderr, "[FAIL] src_host alloc: %d\n", ret); return 14; }
    ret = nvif_mem_ctor(&mmu, "dmaMidVram", mmu.mem, NVIF_MEM_VRAM, 12, TEST_DMA_BYTES, NULL, 0, &mid_vram);
    if (ret) { fprintf(stderr, "[FAIL] mid_vram alloc: %d\n", ret); return 15; }
    ret = nvif_mem_ctor_map(&mmu, "dmaDstHost", NVIF_MEM_HOST | NVIF_MEM_COHERENT, TEST_DMA_BYTES, &dst_host);
    if (ret) { fprintf(stderr, "[FAIL] dst_host alloc: %d\n", ret); return 16; }

    if (map_into_vmm(&vmm, &src_host, false, &src_vma) ||
        map_into_vmm(&vmm, &mid_vram, true,  &mid_vma) ||
        map_into_vmm(&vmm, &dst_host, false, &dst_vma)) {
        fprintf(stderr, "[FAIL] VMM mapping for 16 MiB test buffers failed\n");
        return 17;
    }

    uint32_t *src_u32 = (uint32_t *)src_host.object.map.ptr;
    uint32_t *dst_u32 = (uint32_t *)dst_host.object.map.ptr;
    size_t words = TEST_DMA_BYTES / sizeof(uint32_t);
    uint32_t state = 0x10de1d13U;
    for (size_t i = 0; i < words; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        src_u32[i] = state ^ (uint32_t)i;
    }
    memset(dst_u32, 0xaa, TEST_DMA_BYTES);
    *user_fence_cpu = 0;
    mb();

    /* Warmup 1 page first so TLBs are hot, then time 16 MiB Host -> VRAM and 16 MiB VRAM -> Host */
    ret = submit_ce_copy_and_fence(&gpfifo_chan, src_vma.addr, mid_vma.addr, 1,
                                   user_fence_cpu, user_fence_gpu, 1);
    if (ret) return 18;

    u64 t0 = ktime_get_ns();
    ret = submit_ce_copy_and_fence(&gpfifo_chan, src_vma.addr, mid_vma.addr,
                                   (u32)TEST_DMA_PAGES, user_fence_cpu, user_fence_gpu, 2);
    u64 t1 = ktime_get_ns();
    if (ret) return 19;

    u64 t2 = ktime_get_ns();
    ret = submit_ce_copy_and_fence(&gpfifo_chan, mid_vma.addr, dst_vma.addr,
                                   (u32)TEST_DMA_PAGES, user_fence_cpu, user_fence_gpu, 3);
    u64 t3 = ktime_get_ns();
    if (ret) return 20;

    size_t mismatches = 0;
    for (size_t i = 0; i < words; i++) {
        if (dst_u32[i] != src_u32[i]) {
            if (mismatches < 4) {
                fprintf(stderr, "[MISMATCH] word[%zu]: expected 0x%08x got 0x%08x\n",
                        i, src_u32[i], dst_u32[i]);
            }
            mismatches++;
        }
    }

    double h2d_ms  = (double)(t1 - t0) / 1e6;
    double d2h_ms  = (double)(t3 - t2) / 1e6;
    double h2d_gbs = ((double)TEST_DMA_BYTES / 1e9) / ((double)(t1 - t0) / 1e9);
    double d2h_gbs = ((double)TEST_DMA_BYTES / 1e9) / ((double)(t3 - t2) / 1e9);

    struct GP108DeviceInfo live_info = {};
    gp108_bridge_get_info(&live_info);

    printf("[OK] 16 MiB Host -> GDDR5 VRAM DMA: %.2f ms (%.2f GB/s)\n", h2d_ms, h2d_gbs);
    printf("[OK] 16 MiB GDDR5 VRAM -> Host DMA: %.2f ms (%.2f GB/s)\n", d2h_ms, d2h_gbs);
    printf("[OK] Data integrity verification: %zu / %zu words mismatched (%s)!\n",
           mismatches, words, mismatches == 0 ? "100% BIT-EXACT PASS" : "FAILED");
    printf("[OK] MSI Interrupts delivered: %llu | Die Temp: %d C\n",
           (unsigned long long)live_info.irq_count, live_info.temp_celsius);

    /* Clean teardown */
    nvif_vmm_unmap(&vmm, dst_vma.addr);
    nvif_vmm_put(&vmm, &dst_vma);
    nvif_mem_dtor(&dst_host);

    nvif_vmm_unmap(&vmm, mid_vma.addr);
    nvif_vmm_put(&vmm, &mid_vma);
    nvif_mem_dtor(&mid_vram);

    nvif_vmm_unmap(&vmm, src_vma.addr);
    nvif_vmm_put(&vmm, &src_vma);
    nvif_mem_dtor(&src_host);

    nvif_object_dtor(&eng3d_obj);
    nvif_object_dtor(&comp_obj);
    nvif_object_dtor(&ce_obj);
    nvif_object_dtor(&chan_obj);
    nvif_object_dtor(&cgrp_obj);

    nvif_vmm_unmap(&vmm, cmd_vma.addr);
    nvif_vmm_put(&vmm, &cmd_vma);
    nvif_mem_dtor(&cmd_mem);

    nvif_vmm_dtor(&vmm);
    nvif_mmu_dtor(&mmu);
    gp108_nvkm_shutdown(&nvkm_dev, &client, &device);

    if (mismatches != 0)
        return 21;

    printf("=== ALL PHASE 2 HARDWARE ENGINE SELF-TESTS PASSED! ===\n");
    return 0;
}

int main(void)
{
    signal(SIGSEGV, crash_handler);
    signal(SIGBUS, crash_handler);
    return gp108_nvkm_selftest_run();
}
