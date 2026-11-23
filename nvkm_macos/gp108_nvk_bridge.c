/* SPDX-License-Identifier: MIT
 * Clean Standard-C Bridge from Mesa NVK (nvkmd_macos) -> libnvkm_gp108.a
 */
#include <nvif/os.h>
#include <core/device.h>
#include <engine/gr.h>
#include <nvif/client.h>
#include <nvif/device.h>
#include <nvif/mmu.h>
#include <nvif/vmm.h>
#include <nvif/mem.h>
#include <nvif/fifo.h>
#include <nvif/chan.h>
#include <nvif/class.h>
#include <nvif/cl0080.h>
#include <nvif/if900b.h>
#include <nvif/ifc00d.h>
#include <nvif/if0020.h>
#include <nvif/if0021.h>
#include <nvif/push906f.h>
#include <nvhw/class/cl906f.h>

#include "gp108_nvkm_macos.h"
#include "gp108_nvk_bridge.h"

extern void gp108_telem_on_retain(void);
extern void gp108_telem_on_release(void);
extern void gp108_telem_on_bo_alloc(uint64_t size_B, bool is_vram);
extern void gp108_telem_on_bo_free(uint64_t size_B, bool is_vram);
extern void gp108_telem_on_kick(uint32_t engine_flags);
extern void gp108_telem_on_gpu_busy(uint64_t busy_ns);

#define KERNEL_MANAGED_START  (1ULL << 39) /* 512 GiB .. 1 TiB reserved for internal rings */
#define KERNEL_MANAGED_SIZE   ((1ULL << 40) - KERNEL_MANAGED_START)

struct gp108_nvk_bo {
    struct nvif_mem  mem;
    uint64_t         size_B;
    uint8_t          pte_kind;
    bool             is_vram;
    bool             mappable;
    bool             has_bar_map;
    void            *cpu_ptr;
    uint32_t         handle_id;
};

struct gp108_nvk_ctx {
    pthread_mutex_t    submit_lock;
    uint32_t           ctx_id;
    struct nvif_mem    cmd_mem;
    struct nvif_vma    cmd_vma;
    struct nvif_object cgrp_obj;
    struct nvif_object chan_obj;
    struct nvif_chan   chan;

    struct nvif_object obj_copy;
    struct nvif_object obj_2d;
    struct nvif_object obj_3d;
    struct nvif_object obj_m2mf;
    struct nvif_object obj_comp;

    volatile uint32_t *fence_lo_cpu;
    volatile uint32_t *fence_hi_cpu;
    uint64_t           fence_lo_gpu;
    uint64_t           fence_hi_gpu;
    uint64_t           next_seq;
    uint32_t           engine_mask;
    volatile uint64_t  kick_ns_ring[64];
};

static pthread_mutex_t          g_bridge_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t          g_vmm_lock    = PTHREAD_MUTEX_INITIALIZER;
static uint32_t                 g_refcnt      = 0;
static struct nvkm_device      *g_nvkm_dev    = NULL;
static struct nvif_client       g_client;
static struct nvif_device       g_device;
static struct nvif_mmu          g_mmu;
static struct nvif_vmm          g_vmm;
static struct gp108_nvk_hw_info g_hw_info;
static volatile uint64_t        g_vram_used_B = (16ULL << 20);
static volatile uint32_t        g_next_bo_id  = 1;
static volatile uint32_t        g_next_ctx_id = 1;

static int map_internal_cmd_mem(struct nvif_mem *mem, struct nvif_vma *out_vma)
{
    pthread_mutex_lock(&g_vmm_lock);
    int ret = nvif_vmm_get(&g_vmm, PTES, false, mem->page, 0, mem->size, out_vma);
    if (ret == 0) {
        struct gp100_vmm_map_v0 map_args = {
            .version = 0,
            .vol     = 1,
            .ro      = 0,
            .priv    = 0,
            .kind    = 0,
        };
        ret = nvif_vmm_map(&g_vmm, out_vma->addr, mem->size,
                           &map_args, sizeof(map_args), mem, 0);
        if (ret)
            nvif_vmm_put(&g_vmm, out_vma);
    }
    pthread_mutex_unlock(&g_vmm_lock);
    return ret;
}

static void unmap_internal_cmd_mem(struct nvif_vma *vma)
{
    if (!vma || !vma->size)
        return;
    pthread_mutex_lock(&g_vmm_lock);
    nvif_vmm_unmap(&g_vmm, vma->addr);
    nvif_vmm_put(&g_vmm, vma);
    pthread_mutex_unlock(&g_vmm_lock);
}

int gp108_nvk_retain(struct gp108_nvk_hw_info *out_info)
{
    pthread_mutex_lock(&g_bridge_lock);
    if (g_refcnt > 0) {
        g_refcnt++;
        if (out_info)
            *out_info = g_hw_info;
        pthread_mutex_unlock(&g_bridge_lock);
        return 0;
    }

    struct pci_dev *pdev = NULL;
    struct GP108DeviceInfo binfo = {};
    int ret = gp108_bridge_open(&pdev, &binfo);
    if (ret) {
        pthread_mutex_unlock(&g_bridge_lock);
        return ret;
    }

    ret = gp108_nvkm_bootstrap("NvBios=mx250_vbios.rom", "error",
                               &g_nvkm_dev, &g_client, &g_device);
    if (ret) {
        pthread_mutex_unlock(&g_bridge_lock);
        return ret;
    }

    memset(&g_mmu, 0, sizeof(g_mmu));
    ret = nvif_mmu_ctor(&g_device.object, "nvkMmu", NVIF_CLASS_MMU_GF100, &g_mmu);
    if (ret) {
        gp108_nvkm_shutdown(&g_nvkm_dev, &g_client, &g_device);
        pthread_mutex_unlock(&g_bridge_lock);
        return ret;
    }

    struct gp100_vmm_v0 vmm_args = { .version = 0, .fault_replay = 0 };
    memset(&g_vmm, 0, sizeof(g_vmm));
    ret = nvif_vmm_ctor(&g_mmu, "nvkRawVmm", NVIF_CLASS_VMM_GP100,
                        RAW, KERNEL_MANAGED_START, KERNEL_MANAGED_SIZE,
                        &vmm_args, sizeof(vmm_args), &g_vmm);
    if (ret) {
        nvif_mmu_dtor(&g_mmu);
        gp108_nvkm_shutdown(&g_nvkm_dev, &g_client, &g_device);
        pthread_mutex_unlock(&g_bridge_lock);
        return ret;
    }

    u64 units = g_nvkm_dev->gr ? nvkm_gr_units(g_nvkm_dev->gr) : 0x0301ULL;

    memset(&g_hw_info, 0, sizeof(g_hw_info));
    g_hw_info.vendor_id     = binfo.vendor_id;
    g_hw_info.device_id     = binfo.device_id;
    g_hw_info.subvendor_id  = binfo.subsys_vendor_id;
    g_hw_info.subdevice_id  = binfo.subsys_id;
    g_hw_info.chipset       = (uint16_t)g_device.info.chipset;
    g_hw_info.revision_id   = binfo.revision_id;
    g_hw_info.pci_bus       = binfo.bus;
    g_hw_info.pci_dev       = binfo.device;
    g_hw_info.pci_func      = binfo.function;
    g_hw_info.gpc_count     = (uint8_t)(units & 0xffU);
    g_hw_info.tpc_count     = (uint8_t)((units >> 8) & 0xffU);
    if (g_hw_info.gpc_count == 0) g_hw_info.gpc_count = 1;
    if (g_hw_info.tpc_count == 0) g_hw_info.tpc_count = 3;

    g_hw_info.vram_total_B  = g_device.info.ram_size;
    g_hw_info.vram_usable_B = g_device.info.ram_user;
    g_hw_info.bar1_size_B   = binfo.bar_size[1];

    g_hw_info.cls_copy      = PASCAL_DMA_COPY_B;          /* 0xc1b5 */
    g_hw_info.cls_eng2d     = FERMI_TWOD_A;               /* 0x902d */
    g_hw_info.cls_eng3d     = PASCAL_B;                   /* 0xc197 */
    g_hw_info.cls_m2mf      = KEPLER_INLINE_TO_MEMORY_B;  /* 0xa140 */
    g_hw_info.cls_compute   = PASCAL_COMPUTE_B;           /* 0xc1c0 */

    strlcpy(g_hw_info.chip_name, g_device.info.chip[0] ? g_device.info.chip : "GP108",
            sizeof(g_hw_info.chip_name));
    strlcpy(g_hw_info.dev_name, "NVIDIA GeForce MX250", sizeof(g_hw_info.dev_name));

    g_vram_used_B = (16ULL << 20);
    g_refcnt = 1;
    gp108_telem_on_retain();
    if (out_info)
        *out_info = g_hw_info;
    pthread_mutex_unlock(&g_bridge_lock);
    return 0;
}

void gp108_nvk_release(void)
{
    pthread_mutex_lock(&g_bridge_lock);
    if (g_refcnt == 0) {
        pthread_mutex_unlock(&g_bridge_lock);
        return;
    }
    if (--g_refcnt == 0) {
        gp108_telem_on_release();
        nvif_vmm_dtor(&g_vmm);
        nvif_mmu_dtor(&g_mmu);
        gp108_nvkm_shutdown(&g_nvkm_dev, &g_client, &g_device);
    }
    pthread_mutex_unlock(&g_bridge_lock);
}

uint64_t gp108_nvk_get_timestamp(void)
{
    if (!g_refcnt)
        return 0;
    return nvif_device_time(&g_device);
}

uint64_t gp108_nvk_get_vram_used(void)
{
    return __sync_fetch_and_add(&g_vram_used_B, 0);
}

int gp108_nvk_bo_alloc(uint64_t size_B, uint8_t pte_kind,
                       bool in_vram, bool mappable,
                       struct gp108_nvk_bo **out_bo)
{
    if (!g_refcnt || !out_bo || size_B == 0)
        return -EINVAL;

    size_B = ALIGN(size_B, 4096ULL);
    struct gp108_nvk_bo *bo = calloc(1, sizeof(*bo));
    if (!bo)
        return -ENOMEM;

    bo->size_B    = size_B;
    bo->pte_kind  = pte_kind;
    bo->handle_id = __sync_fetch_and_add(&g_next_bo_id, 1U);

    int ret = -EINVAL;
    if (in_vram) {
        u8 mem_type = NVIF_MEM_VRAM | (mappable ? NVIF_MEM_MAPPABLE : 0);
        ret = nvif_mem_ctor(&g_mmu, "nvkVramBo", NVIF_CLASS_MEM_GF100,
                            mem_type, 12, size_B, NULL, 0, &bo->mem);
        if (ret == 0 && mappable) {
            struct gf100_mem_map_v0 map_args = {
                .version = 0,
                .ro      = 0,
                .kind    = pte_kind,
            };
            u64 bar_handle = 0, bar_len = 0;
            ret = nvif_object_map_handle(&bo->mem.object, &map_args, sizeof(map_args),
                                         &bar_handle, &bar_len);
            if (ret == 0) {
                bo->cpu_ptr = (void *)ioremap_wc(bar_handle, (size_t)bar_len);
                bo->has_bar_map = (bo->cpu_ptr != NULL);
                if (!bo->cpu_ptr) {
                    nvif_object_unmap_handle(&bo->mem.object);
                    nvif_mem_dtor(&bo->mem);
                    ret = -ENOMEM;
                }
            } else {
                nvif_mem_dtor(&bo->mem);
            }
        }
        if (ret == 0) {
            bo->is_vram  = true;
            bo->mappable = mappable;
            __sync_add_and_fetch(&g_vram_used_B, size_B);
            gp108_telem_on_bo_alloc(size_B, true);
            *out_bo = bo;
            return 0;
        }
        /* Fall back to coherent GART if mappable BAR1 or VRAM is full */
        memset(&bo->mem, 0, sizeof(bo->mem));
    }

    ret = nvif_mem_ctor_map(&g_mmu, "nvkHostBo",
                            NVIF_MEM_HOST | NVIF_MEM_COHERENT,
                            size_B, &bo->mem);
    if (ret) {
        free(bo);
        return ret;
    }
    bo->is_vram  = false;
    bo->mappable = true;
    bo->cpu_ptr  = (void *)bo->mem.object.map.ptr;
    gp108_telem_on_bo_alloc(size_B, false);
    *out_bo = bo;
    return 0;
}

void gp108_nvk_bo_free(struct gp108_nvk_bo *bo)
{
    if (!bo)
        return;
    if (bo->has_bar_map)
        nvif_object_unmap_handle(&bo->mem.object);
    if (bo->is_vram)
        __sync_sub_and_fetch(&g_vram_used_B, bo->size_B);
    gp108_telem_on_bo_free(bo->size_B, bo->is_vram);
    nvif_mem_dtor(&bo->mem);
    free(bo);
}

void *gp108_nvk_bo_map(struct gp108_nvk_bo *bo)
{
    return bo ? bo->cpu_ptr : NULL;
}

uint32_t gp108_nvk_bo_handle_id(struct gp108_nvk_bo *bo)
{
    return bo ? bo->handle_id : 0;
}

uint64_t gp108_nvk_bo_size(struct gp108_nvk_bo *bo)
{
    return bo ? bo->size_B : 0;
}

int gp108_nvk_vm_sparse(uint64_t addr, uint64_t size_B, bool enable)
{
    pthread_mutex_lock(&g_vmm_lock);
    int ret = nvif_vmm_raw_sparse(&g_vmm, addr, size_B, enable);
    pthread_mutex_unlock(&g_vmm_lock);
    return ret;
}

int gp108_nvk_vm_get_ptes(uint64_t addr, uint64_t size_B)
{
    pthread_mutex_lock(&g_vmm_lock);
    int ret = nvif_vmm_raw_get(&g_vmm, addr, size_B, 12);
    pthread_mutex_unlock(&g_vmm_lock);
    return ret;
}

int gp108_nvk_vm_put_ptes(uint64_t addr, uint64_t size_B)
{
    pthread_mutex_lock(&g_vmm_lock);
    int ret = nvif_vmm_raw_put(&g_vmm, addr, size_B, 12);
    pthread_mutex_unlock(&g_vmm_lock);
    return ret;
}

int gp108_nvk_vm_map(uint64_t addr, uint64_t range_B, uint8_t pte_kind,
                     struct gp108_nvk_bo *bo, uint64_t bo_offset_B)
{
    if (!bo)
        return -EINVAL;
    struct gp100_vmm_map_v0 map_args = {
        .version = 0,
        .vol     = bo->is_vram ? 0 : 1,
        .ro      = 0,
        .priv    = 0,
        .kind    = pte_kind,
    };
    pthread_mutex_lock(&g_vmm_lock);
    int ret = nvif_vmm_raw_map(&g_vmm, addr, range_B, 12,
                               &map_args, sizeof(map_args),
                               &bo->mem, bo_offset_B);
    pthread_mutex_unlock(&g_vmm_lock);
    return ret;
}

int gp108_nvk_vm_unmap(uint64_t addr, uint64_t range_B, bool is_sparse)
{
    pthread_mutex_lock(&g_vmm_lock);
    int ret = nvif_vmm_raw_unmap(&g_vmm, addr, range_B, 12, is_sparse);
    pthread_mutex_unlock(&g_vmm_lock);
    return ret;
}

int gp108_nvk_ctx_create(uint32_t engine_mask, struct gp108_nvk_ctx **out_ctx)
{
    if (!g_refcnt || !out_ctx)
        return -EINVAL;

    struct gp108_nvk_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -ENOMEM;

    pthread_mutex_init(&ctx->submit_lock, NULL);
    ctx->ctx_id = __sync_fetch_and_add(&g_next_ctx_id, 1U);

    const u32 push_len   = 0x10000U; /* 64 KiB internal semaphore ring */
    const u32 gpfifo_len = 0x08000U; /* 32 KiB GPFIFO (4096 IB entries) */
    const u32 sema_len   = 0x01000U; /* 4 KiB semaphores + 64-bit fence */

    int ret = nvif_mem_ctor_map(&g_mmu, "nvkChanCmd",
                                NVIF_MEM_HOST | NVIF_MEM_COHERENT,
                                push_len + gpfifo_len + sema_len, &ctx->cmd_mem);
    if (ret) {
        free(ctx);
        return ret;
    }

    ret = map_internal_cmd_mem(&ctx->cmd_mem, &ctx->cmd_vma);
    if (ret) {
        nvif_mem_dtor(&ctx->cmd_mem);
        free(ctx);
        return ret;
    }

    uint8_t *cmd_cpu = (uint8_t *)ctx->cmd_mem.object.map.ptr;
    void *push_cpu   = cmd_cpu;
    void *gpfifo_cpu = cmd_cpu + push_len;
    void *sema_cpu   = cmd_cpu + push_len + gpfifo_len;
    u64   push_gpu   = ctx->cmd_vma.addr;
    u64   gpfifo_gpu = ctx->cmd_vma.addr + push_len;
    u64   sema_gpu   = ctx->cmd_vma.addr + push_len + gpfifo_len;

    ctx->fence_lo_cpu = (volatile uint32_t *)((uint8_t *)sema_cpu + 0x100);
    ctx->fence_hi_cpu = (volatile uint32_t *)((uint8_t *)sema_cpu + 0x110);
    ctx->fence_lo_gpu = sema_gpu + 0x100ULL;
    ctx->fence_hi_gpu = sema_gpu + 0x110ULL;
    *ctx->fence_lo_cpu = 0;
    *ctx->fence_hi_cpu = 0;
    ctx->next_seq = 0;
    ctx->engine_mask = engine_mask;

    u64 runm = nvif_fifo_runlist(&g_device, NV_DEVICE_HOST_RUNLIST_ENGINES_GR);
    if (!runm) {
        ret = -ENODEV;
        goto err_vma;
    }
    u8 runlist_id = (u8)__ffs64(runm);

    struct {
        struct nvif_cgrp_v0 v0;
        char name[16];
    } __packed cgrp_args = {};
    cgrp_args.v0.version = 0;
    cgrp_args.v0.namelen = sizeof(cgrp_args.name);
    cgrp_args.v0.runlist = runlist_id;
    cgrp_args.v0.vmm     = nvif_handle(&g_vmm.object);
    snprintf(cgrp_args.name, sizeof(cgrp_args.name), "nvkTsg%u", ctx->ctx_id);

    ret = nvif_object_ctor(&g_device.object, "nvkCgrp", 0x10000U + ctx->ctx_id * 2U,
                           KEPLER_CHANNEL_GROUP_A, &cgrp_args, sizeof(cgrp_args), &ctx->cgrp_obj);
    if (ret)
        goto err_vma;

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
    chan_args.v0.vmm     = nvif_handle(&g_vmm.object);
    chan_args.v0.offset  = gpfifo_gpu;
    chan_args.v0.length  = gpfifo_len;
    snprintf(chan_args.name, sizeof(chan_args.name), "nvkCh%u", ctx->ctx_id);

    ret = nvif_object_ctor(&ctx->cgrp_obj, "nvkChan", 0x10001U + ctx->ctx_id * 2U,
                           PASCAL_CHANNEL_GPFIFO_A, &chan_args, sizeof(chan_args), &ctx->chan_obj);
    if (ret)
        goto err_cgrp;

    ret = nvif_object_map(&ctx->chan_obj, NULL, 0);
    if (ret)
        goto err_chan;

    nvif_chan906f_ctor(&ctx->chan, ctx->chan_obj.map.ptr, gpfifo_cpu, gpfifo_len,
                       push_cpu, push_gpu, push_len, sema_cpu, sema_gpu);

    /* Instantiate requested hardware engines on this channel */
    if (engine_mask & GP108_NVK_ENGINE_COPY) {
        ret = nvif_object_ctor(&ctx->chan_obj, "nvkCe", PASCAL_DMA_COPY_B,
                               PASCAL_DMA_COPY_B, NULL, 0, &ctx->obj_copy);
        if (ret) goto err_objs;
    }
    if (engine_mask & GP108_NVK_ENGINE_2D) {
        ret = nvif_object_ctor(&ctx->chan_obj, "nvk2D", FERMI_TWOD_A,
                               FERMI_TWOD_A, NULL, 0, &ctx->obj_2d);
        if (ret) goto err_objs;
    }
    if (engine_mask & GP108_NVK_ENGINE_3D) {
        ret = nvif_object_ctor(&ctx->chan_obj, "nvk3D", PASCAL_B,
                               PASCAL_B, NULL, 0, &ctx->obj_3d);
        if (ret) goto err_objs;
    }
    if (engine_mask & GP108_NVK_ENGINE_M2MF) {
        ret = nvif_object_ctor(&ctx->chan_obj, "nvkI2M", KEPLER_INLINE_TO_MEMORY_B,
                               KEPLER_INLINE_TO_MEMORY_B, NULL, 0, &ctx->obj_m2mf);
        if (ret) goto err_objs;
    }
    if (engine_mask & GP108_NVK_ENGINE_COMPUTE) {
        ret = nvif_object_ctor(&ctx->chan_obj, "nvkComp", PASCAL_COMPUTE_B,
                               PASCAL_COMPUTE_B, NULL, 0, &ctx->obj_comp);
        if (ret) goto err_objs;
    }

    *out_ctx = ctx;
    return 0;

err_objs:
    nvif_object_dtor(&ctx->obj_comp);
    nvif_object_dtor(&ctx->obj_m2mf);
    nvif_object_dtor(&ctx->obj_3d);
    nvif_object_dtor(&ctx->obj_2d);
    nvif_object_dtor(&ctx->obj_copy);
err_chan:
    nvif_object_dtor(&ctx->chan_obj);
err_cgrp:
    nvif_object_dtor(&ctx->cgrp_obj);
err_vma:
    unmap_internal_cmd_mem(&ctx->cmd_vma);
    nvif_mem_dtor(&ctx->cmd_mem);
    pthread_mutex_destroy(&ctx->submit_lock);
    free(ctx);
    return ret;
}

void gp108_nvk_ctx_destroy(struct gp108_nvk_ctx *ctx)
{
    if (!ctx)
        return;

    /* Wait up to 2s for any in-flight GPU work before tearing down channel */
    gp108_nvk_ctx_wait_fence(ctx, ctx->next_seq, 2000000000ULL);

    nvif_object_dtor(&ctx->obj_comp);
    nvif_object_dtor(&ctx->obj_m2mf);
    nvif_object_dtor(&ctx->obj_3d);
    nvif_object_dtor(&ctx->obj_2d);
    nvif_object_dtor(&ctx->obj_copy);
    nvif_object_dtor(&ctx->chan_obj);
    nvif_object_dtor(&ctx->cgrp_obj);
    unmap_internal_cmd_mem(&ctx->cmd_vma);
    nvif_mem_dtor(&ctx->cmd_mem);
    pthread_mutex_destroy(&ctx->submit_lock);
    free(ctx);
}

uint64_t gp108_nvk_ctx_completed_fence(struct gp108_nvk_ctx *ctx)
{
    if (!ctx)
        return 0;
    uint32_t hi1, lo, hi2;
    do {
        hi1 = *ctx->fence_hi_cpu;
        rmb();
        lo  = *ctx->fence_lo_cpu;
        rmb();
        hi2 = *ctx->fence_hi_cpu;
    } while (hi1 != hi2);
    return ((uint64_t)hi1 << 32) | lo;
}

int gp108_nvk_ctx_submit(struct gp108_nvk_ctx *ctx,
                         const struct gp108_nvk_push *pushes,
                         uint32_t push_count,
                         uint64_t *out_fence_seq)
{
    if (!ctx)
        return -EINVAL;

    pthread_mutex_lock(&ctx->submit_lock);
    if (push_count == 0) {
        if (out_fence_seq)
            *out_fence_seq = ctx->next_seq;
        pthread_mutex_unlock(&ctx->submit_lock);
        return 0;
    }

    /* Submit in batches of up to 1024 GPFIFO entries */
    uint32_t idx = 0;
    while (idx < push_count) {
        uint32_t batch = push_count - idx;
        if (batch > 1024U)
            batch = 1024U;
        /* Never split across an incomplete push boundary (handled by caller, or keep margin) */
        int ret = nvif_chan_gpfifo_wait(&ctx->chan, batch + 1U, 16U);
        if (ret) {
            pthread_mutex_unlock(&ctx->submit_lock);
            return ret;
        }
        for (uint32_t i = 0; i < batch; i++) {
            const struct gp108_nvk_push *p = &pushes[idx + i];
            if (p->size_B > 0)
                nvif_chan_gpfifo_push(&ctx->chan, p->addr, p->size_B, p->no_prefetch);
        }
        nvif_chan_gpfifo_post(&ctx->chan);
        idx += batch;
    }

    uint64_t seq = ++ctx->next_seq;
    struct nvif_push *push = &ctx->chan.push;
    int ret = PUSH_WAIT(push, 10);
    if (ret) {
        pthread_mutex_unlock(&ctx->submit_lock);
        return ret;
    }

    /* Release lo word with WFI, then hi word so 64-bit monotonic read is atomic */
    if (upper_32_bits(seq) != upper_32_bits(seq - 1ULL)) {
        PUSH_MTHD(push, NV906F, SEMAPHOREA,
                  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(ctx->fence_lo_gpu)),
                  SEMAPHOREB, lower_32_bits(ctx->fence_lo_gpu),
                  SEMAPHOREC, 0U,
                  SEMAPHORED,
                  NVDEF(NV906F, SEMAPHORED, OPERATION, RELEASE) |
                  NVDEF(NV906F, SEMAPHORED, RELEASE_WFI, EN) |
                  NVDEF(NV906F, SEMAPHORED, RELEASE_SIZE, 16BYTE));
        PUSH_MTHD(push, NV906F, SEMAPHOREA,
                  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(ctx->fence_hi_gpu)),
                  SEMAPHOREB, lower_32_bits(ctx->fence_hi_gpu),
                  SEMAPHOREC, upper_32_bits(seq),
                  SEMAPHORED,
                  NVDEF(NV906F, SEMAPHORED, OPERATION, RELEASE) |
                  NVDEF(NV906F, SEMAPHORED, RELEASE_WFI, DIS) |
                  NVDEF(NV906F, SEMAPHORED, RELEASE_SIZE, 16BYTE));
    }
    PUSH_MTHD(push, NV906F, SEMAPHOREA,
              NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(ctx->fence_lo_gpu)),
              SEMAPHOREB, lower_32_bits(ctx->fence_lo_gpu),
              SEMAPHOREC, lower_32_bits(seq),
              SEMAPHORED,
              NVDEF(NV906F, SEMAPHORED, OPERATION, RELEASE) |
              NVDEF(NV906F, SEMAPHORED, RELEASE_WFI, EN) |
              NVDEF(NV906F, SEMAPHORED, RELEASE_SIZE, 16BYTE));

    ctx->kick_ns_ring[seq & 63U] = ktime_get_ns();
    PUSH_KICK(push);
    gp108_telem_on_kick((ctx->engine_mask & GP108_NVK_ENGINE_COMPUTE) ? 3U : 1U);

    if (out_fence_seq)
        *out_fence_seq = seq;
    pthread_mutex_unlock(&ctx->submit_lock);
    return 0;
}

static inline void account_completed_seq(struct gp108_nvk_ctx *ctx, uint64_t target_seq)
{
    uint64_t t0 = __sync_lock_test_and_set(&ctx->kick_ns_ring[target_seq & 63U], 0ULL);
    if (t0 > 0) {
        uint64_t now = ktime_get_ns();
        if (now > t0)
            gp108_telem_on_gpu_busy(now - t0);
    }
}

int gp108_nvk_ctx_wait_fence(struct gp108_nvk_ctx *ctx,
                             uint64_t target_seq,
                             uint64_t timeout_ns)
{
    if (!ctx || target_seq == 0)
        return 0;
    if (gp108_nvk_ctx_completed_fence(ctx) >= target_seq) {
        account_completed_seq(ctx, target_seq);
        return 0;
    }

    u64 start = ktime_get_ns();
    u64 spin_until = start + 250000ULL; /* 250 us fast spin for sub-ms frame latency */
    while (gp108_nvk_ctx_completed_fence(ctx) < target_seq) {
        u64 now = ktime_get_ns();
        if (timeout_ns != UINT64_MAX && (now - start) >= timeout_ns)
            return -ETIMEDOUT;
        if (now < spin_until) {
            __asm__ volatile("pause");
        } else {
            usleep(100);
        }
    }
    account_completed_seq(ctx, target_seq);
    return 0;
}
