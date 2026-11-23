/* SPDX-License-Identifier: MIT
 * Live Userspace NVIDIA RM Bootstrap, P0 Boost, Fixed-VA VMM, GPFIFO Channel,
 * and Hardware Zero-Copy IOSurface DMA Verification for MX250 (GP108)
 */
#include "gp108_rm_host.h"
#include "gp108_rm_api_defs.h"

#include <IOSurface/IOSurface.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct nv2080_perf_boost_params {
    NvU32 flags;
    NvU32 duration;
};

struct nv2080_perf_cur_pstate_params {
    NvU32 currPstate;
};

static NvU32 rm_alloc(NvHandle hRoot, NvHandle hParent, NvHandle *hNew, NvU32 hClass, void *allocParams)
{
    NVOS21_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hRoot         = hRoot;
    p.hObjectParent = hParent;
    p.hObjectNew    = *hNew;
    p.hClass        = hClass;
    p.pAllocParms   = (NvP64)(uintptr_t)allocParams;

    int rc = gp108_rm_call_ioctl(NULL, true, NV_ESC_RM_ALLOC, &p, sizeof(p));
    if (rc != 0) return (NvU32)(-rc);
    *hNew = p.hObjectNew;
    return p.status;
}

static NvU32 rm_control(NvHandle hClient, NvHandle hObject, NvU32 cmd, void *params, NvU32 paramsSize)
{
    NVOS54_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hClient    = hClient;
    p.hObject    = hObject;
    p.cmd        = cmd;
    p.params     = (NvP64)(uintptr_t)params;
    p.paramsSize = paramsSize;

    int rc = gp108_rm_call_ioctl(NULL, true, NV_ESC_RM_CONTROL, &p, sizeof(p));
    if (rc != 0) return (NvU32)(-rc);
    return p.status;
}

static void rm_free_obj(NvHandle hClient, NvHandle hObject)
{
    if (!hObject) return;
    NVOS00_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hRoot      = hClient;
    p.hObjectOld = hObject;
    gp108_rm_call_ioctl(NULL, true, NV_ESC_RM_FREE, &p, sizeof(p));
}

static NvU32 rm_map_cpu(NvHandle hClient, NvHandle hDevice, NvHandle hMemory,
                        NvU64 offset, NvU64 length, bool is_sysmem, NvU32 flags, void **out_va)
{
    nv_ioctl_nvos33_with_fd_t p;
    memset(&p, 0, sizeof(p));
    p.params.hClient = hClient;
    p.params.hDevice = hDevice;
    p.params.hMemory = hMemory;
    p.params.offset  = offset;
    p.params.length  = length;
    p.params.flags   = flags;
    p.fd             = is_sysmem ? 0 : 1;

    int rc = gp108_rm_call_ioctl(NULL, true, NV_ESC_RM_MAP_MEMORY, &p, sizeof(p));
    if (rc != 0) return (NvU32)(-rc);
    if (p.params.status != NV_OK) return p.params.status;

    if (gp108_rm_get_last_mmap(p.fd, NULL, NULL, NULL, NULL, out_va) != 0)
        return NV_ERR_GENERIC;
    return NV_OK;
}

static NvU32 rm_alloc_fixed_va_and_bind(NvHandle hClient, NvHandle hDevice, NvHandle hVaSpace,
                                        NvHandle hMemPhys, bool is_sysmem,
                                        uint64_t gpu_va, uint64_t size_B, NvHandle *out_hVirt)
{
    NV_MEMORY_ALLOCATION_PARAMS vp;
    memset(&vp, 0, sizeof(vp));
    vp.owner    = hClient;
    vp.type     = NVOS32_TYPE_IMAGE;
    vp.flags    = NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE;
    vp.size     = size_B;
    vp.offset   = gpu_va;
    vp.hVASpace = hVaSpace;

    NvHandle hVirt = 0;
    NvU32 st = rm_alloc(hClient, hDevice, &hVirt, NV50_MEMORY_VIRTUAL_CLASS, &vp);
    if (st != NV_OK) return st;

    NVOS46_PARAMETERS mp;
    memset(&mp, 0, sizeof(mp));
    mp.hClient   = hClient;
    mp.hDevice   = hDevice;
    mp.hDma      = hVirt;
    mp.hMemory   = hMemPhys;
    mp.offset    = 0;
    mp.length    = size_B;
    mp.dmaOffset = 0;
    mp.flags     = DRF_DEF(OS46, _FLAGS, _PAGE_KIND, _VIRTUAL) |
                   (is_sysmem ? DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE)
                              : DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _DISABLE));

    int rc = gp108_rm_call_ioctl(NULL, true, NV_ESC_RM_MAP_MEMORY_DMA, &mp, sizeof(mp));
    if (rc != 0 || mp.status != NV_OK) {
        rm_free_obj(hClient, hVirt);
        return rc != 0 ? (NvU32)(-rc) : mp.status;
    }
    *out_hVirt = hVirt;
    return NV_OK;
}

/* NV906F / NVC1B5 method header: Type 2 = increasing methods (1 << 29) */
#define NVPUSH_INC(subc, mthd, cnt) ((1U << 29) | ((uint32_t)(cnt) << 16) | ((uint32_t)(subc) << 13) | ((uint32_t)(mthd) >> 2))

int main(int argc, char **argv)
{
    const char *elf_path = (argc > 1)
        ? argv[1]
        : "/Users/maple/projects/temp/mx250-macos/third_party/nv_rm_freebsd/src/nvidia/nv-kernel.o";

    if (gp108_rm_bootstrap(elf_path) != 0)
        return 1;

    nv_state_t *gpu = gp108_rm_get_gpu_state();
    NvHandle hClient = 0, hDevice = 0, hSubdev = 0, hVaSpace = 0;

    /* Register dev fd (1) with ctl fd (0) so NV_ESC_RM_ALLOC_MEMORY works on dev node */
    nv_ioctl_register_fd_t reg_fd = { .ctl_fd = 0 };
    gp108_rm_call_ioctl(NULL, false, NV_ESC_REGISTER_FD, &reg_fd, sizeof(reg_fd));

    NvU32 st = rm_alloc(0, 0, &hClient, NV01_ROOT_CLIENT_CLASS, NULL);
    printf("[1] Root Client: st=0x%x hClient=0x%x\n", st, hClient);

    NV0000_CTRL_GPU_ATTACH_IDS_PARAMS att;
    memset(&att, 0, sizeof(att));
    att.gpuIds[0] = gpu->gpu_id;
    att.gpuIds[1] = 0xffffffffU;
    rm_control(hClient, hClient, NV0000_CTRL_CMD_GPU_ATTACH_IDS, &att, sizeof(att));

    NV0000_CTRL_GPU_GET_ID_INFO_PARAMS idi;
    memset(&idi, 0, sizeof(idi));
    idi.gpuId = gpu->gpu_id;
    rm_control(hClient, hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO, &idi, sizeof(idi));

    NV0080_ALLOC_PARAMETERS ap0080;
    memset(&ap0080, 0, sizeof(ap0080));
    ap0080.deviceId     = idi.deviceInstance;
    ap0080.hClientShare = hClient;
    st = rm_alloc(hClient, hClient, &hDevice, NV01_DEVICE_0_CLASS, &ap0080);
    printf("[2] NV01_DEVICE_0: st=0x%x hDevice=0x%x\n", st, hDevice);

    NV2080_ALLOC_PARAMETERS ap2080 = { .subDeviceId = idi.subDeviceInstance };
    st = rm_alloc(hClient, hDevice, &hSubdev, NV20_SUBDEVICE_0_CLASS, &ap2080);
    printf("[3] NV20_SUBDEVICE_0: st=0x%x hSubdev=0x%x\n", st, hSubdev);

    /* Activate Full-Blood P0 Boost */
    struct nv2080_perf_boost_params bp = { .flags = 0x12U, .duration = 0xffffffffU };
    st = rm_control(hClient, hSubdev, NV2080_CTRL_CMD_PERF_BOOST_ID, &bp, sizeof(bp));
    struct nv2080_perf_cur_pstate_params ps = { 0 };
    rm_control(hClient, hSubdev, NV2080_CTRL_CMD_PERF_GET_CUR_PSTATE, &ps, sizeof(ps));
    printf("[4] PERF_BOOST(P0): st=0x%x currPstate=0x%x (P0)\n", st, ps.currPstate);

    /* Query live P0 clock frequencies (470 ABI: 20-byte NV2080_CTRL_CLK_INFO) */
    struct {
        NvU32 flags, clkDomain, actualFreq, targetFreq, clkSource;
    } clk_infos[4] = {
        { 0, 0x00010000U, 0, 0, 0 }, /* GPCCLK (Graphics/Shader) */
        { 0, 0x00000010U, 0, 0, 0 }, /* MCLK   (GDDR5 Memory)    */
        { 0, 0x00080000U, 0, 0, 0 }, /* SYSCLK                   */
        { 0, 0x00040000U, 0, 0, 0 }, /* XBARCLK                  */
    };
    struct {
        NvU32 flags;
        NvU32 clkInfoListSize;
        NvP64 clkInfoList;
    } clk_get = { 0, 4, (NvP64)(uintptr_t)clk_infos };
    NvU32 st_clk = rm_control(hClient, hSubdev, 0x20801002U, &clk_get, sizeof(clk_get));
    printf("[4b] Live Clocks (st=0x%x): GPCCLK=%u kHz (%.1f MHz)  MCLK=%u kHz (%.1f MHz)  SYSCLK=%u kHz  XBAR=%u kHz\n",
           st_clk,
           clk_infos[0].actualFreq, clk_infos[0].actualFreq / 1000.0,
           clk_infos[1].actualFreq, clk_infos[1].actualFreq / 1000.0,
           clk_infos[2].actualFreq, clk_infos[3].actualFreq);

    /* Allocate 49-bit FERMI_VASPACE_A */
    NV_VASPACE_ALLOCATION_PARAMETERS vap;
    memset(&vap, 0, sizeof(vap));
    vap.flags = NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS;
    st = rm_alloc(hClient, hDevice, &hVaSpace, FERMI_VASPACE_A_CLASS, &vap);
    printf("[5] FERMI_VASPACE_A: st=0x%x hVaSpace=0x%x\n", st, hVaSpace);

    /* Allocate 64 KB command ring in System RAM + 64 KB source buffer in GDDR5 VRAM */
    NV_MEMORY_ALLOCATION_PARAMS sys_mp;
    memset(&sys_mp, 0, sizeof(sys_mp));
    sys_mp.owner = hClient;
    sys_mp.type  = NVOS32_TYPE_IMAGE;
    sys_mp.attr  = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB) |
                   DRF_DEF(OS32, _ATTR, _LOCATION, _PCI) |
                   DRF_DEF(OS32, _ATTR, _COHERENCY, _CACHED);
    sys_mp.size  = 0x10000ULL;
    NvHandle hSysCmd = 0, hSysNotif = 0;
    st = rm_alloc(hClient, hDevice, &hSysCmd, NV01_MEMORY_SYSTEM_CLASS, &sys_mp);
    void *cmd_cpu = NULL;
    rm_map_cpu(hClient, hSubdev, hSysCmd, 0, 0x10000ULL, true, (1U << 15), &cmd_cpu);

    sys_mp.size = 0x1000ULL;
    rm_alloc(hClient, hDevice, &hSysNotif, NV01_MEMORY_SYSTEM_CLASS, &sys_mp);
    printf("[6] SysMem CmdRing: st=0x%x cpu=%p\n", st, cmd_cpu);

    NV_MEMORY_ALLOCATION_PARAMS vram_mp;
    memset(&vram_mp, 0, sizeof(vram_mp));
    vram_mp.owner = hClient;
    vram_mp.type  = NVOS32_TYPE_IMAGE;
    vram_mp.flags = NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM;
    vram_mp.attr  = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB) |
                    DRF_DEF(OS32, _ATTR, _LOCATION, _VIDMEM) |
                    DRF_DEF(OS32, _ATTR, _COHERENCY, _WRITE_COMBINE) |
                    DRF_DEF(OS32, _ATTR, _PHYSICALITY, _ALLOW_NONCONTIGUOUS);
    vram_mp.size  = 0x10000ULL;
    NvHandle hVramSrc = 0;
    st = rm_alloc(hClient, hDevice, &hVramSrc, NV01_MEMORY_LOCAL_USER_CLASS, &vram_mp);
    uint32_t *vram_cpu = NULL;
    NvU32 st_vmap = rm_map_cpu(hClient, hSubdev, hVramSrc, 0, 0x10000ULL, false, 0, (void **)&vram_cpu);
    printf("[7] VRAM BO + BAR1 WC Map: alloc=0x%x map=0x%x vram_cpu=%p\n", st, st_vmap, (void *)vram_cpu);
    if (!vram_cpu || !cmd_cpu) {
        fprintf(stderr, "Failed to map CPU pointers!\n");
        gp108_rm_teardown();
        return 1;
    }

    /* Fill GDDR5 VRAM with test pattern */
    for (int i = 0; i < 4096; i++) {
        vram_cpu[i] = 0xA5000000U | (uint32_t)i;
    }
    __asm__ __volatile__("sfence" ::: "memory");

    /* Create a native macOS IOSurfaceRef and import it ZERO-COPY into GP108 RM! */
    const void *keys[4] = {
        kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat
    };
    int w = 64, h = 64, bpe = 4, pfmt = 'BGRA';
    CFNumberRef nw = CFNumberCreate(NULL, kCFNumberIntType, &w);
    CFNumberRef nh = CFNumberCreate(NULL, kCFNumberIntType, &h);
    CFNumberRef nb = CFNumberCreate(NULL, kCFNumberIntType, &bpe);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberIntType, &pfmt);
    const void *vals[4] = { nw, nh, nb, np };
    CFDictionaryRef dict = CFDictionaryCreate(NULL, keys, vals, 4,
                                              &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef ioSurf = IOSurfaceCreate(dict);
    CFRelease(dict); CFRelease(nw); CFRelease(nh); CFRelease(nb); CFRelease(np);

    void *iosurf_base = IOSurfaceGetBaseAddress(ioSurf);
    size_t iosurf_sz  = (IOSurfaceGetAllocSize(ioSurf) + 4095UL) & ~4095UL;
    memset(iosurf_base, 0, iosurf_sz);

    NvHandle hIoSurfMem = 0x5eaf0001U;
    nv_ioctl_nvos02_with_fd_t os02;
    memset(&os02, 0, sizeof(os02));
    os02.params.hRoot         = hClient;
    os02.params.hObjectParent = hDevice;
    os02.params.hObjectNew    = hIoSurfMem;
    os02.params.hClass        = NV01_MEMORY_SYSTEM_OS_DESC_CLASS;
    os02.params.flags         = DRF_DEF(OS02, _FLAGS, _LOCATION, _PCI) |
                                DRF_DEF(OS02, _FLAGS, _MAPPING, _NO_MAP) |
                                DRF_DEF(OS02, _FLAGS, _COHERENCY, _CACHED) |
                                DRF_DEF(OS02, _FLAGS, _PHYSICALITY, _NONCONTIGUOUS);
    os02.params.pMemory       = (NvP64)(uintptr_t)iosurf_base;
    os02.params.limit         = iosurf_sz - 1ULL;
    os02.fd                   = 1;
    int rc02 = gp108_rm_call_ioctl(NULL, false, NV_ESC_RM_ALLOC_MEMORY, &os02, sizeof(os02));
    printf("[8] Zero-Copy IOSurface Import (ID=%u VA=%p sz=%zu): rc=%d st=0x%x\n",
           IOSurfaceGetID(ioSurf), iosurf_base, iosurf_sz, rc02, os02.params.status);

    /* Bind CmdRing (0x10000000), VRAM (0x20000000), and Zero-Copy IOSurface (0x30000000) at fixed GPU VAs! */
    NvHandle hVirtCmd = 0, hVirtVram = 0, hVirtSurf = 0;
    st = rm_alloc_fixed_va_and_bind(hClient, hDevice, hVaSpace, hSysCmd,   true,  0x10000000ULL, 0x10000ULL, &hVirtCmd);
    NvU32 st2 = rm_alloc_fixed_va_and_bind(hClient, hDevice, hVaSpace, hVramSrc,  false, 0x20000000ULL, 0x10000ULL, &hVirtVram);
    NvU32 st3 = rm_alloc_fixed_va_and_bind(hClient, hDevice, hVaSpace, hIoSurfMem, true, 0x30000000ULL, iosurf_sz,  &hVirtSurf);
    printf("[9] Fixed GPU VA Bindings: cmd=0x%x vram=0x%x iosurf=0x%x\n", st, st2, st3);

    /* Allocate Error Context DMA + PASCAL_CHANNEL_GPFIFO_A (0xc06f) */
    NV_CONTEXT_DMA_ALLOCATION_PARAMS cdp;
    memset(&cdp, 0, sizeof(cdp));
    cdp.hMemory = hSysNotif;
    cdp.flags   = DRF_DEF(OS03, _FLAGS, _MAPPING, _KERNEL) | DRF_DEF(OS03, _FLAGS, _HASH_TABLE, _DISABLE);
    cdp.limit   = 0xfffULL;
    NvHandle hCtxDma = 0;
    rm_alloc(hClient, hDevice, &hCtxDma, NV01_CONTEXT_DMA_CLASS, &cdp);

    NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS chp;
    memset(&chp, 0, sizeof(chp));
    chp.hObjectError  = hCtxDma;
    chp.gpFifoOffset  = 0x10000000ULL; /* first 16 KB of CmdRing = 2048 GPFIFO entries */
    chp.gpFifoEntries = 2048;
    chp.hVASpace      = hVaSpace;
    chp.engineType    = NV2080_ENGINE_TYPE_GRAPHICS;

    NvHandle hChan = 0;
    st = rm_alloc(hClient, hDevice, &hChan, PASCAL_CHANNEL_GPFIFO_A_CLASS, &chp);
    printf("[10] PASCAL_CHANNEL_GPFIFO_A (0xc06f): st=0x%x hChan=0x%x\n", st, hChan);

    /* Allocate Engine Objects on Channel */
    NvHandle hCe = 0, h3d = 0, hComp = 0, h2d = 0, hI2m = 0;
    NvU32 s_ce   = rm_alloc(hClient, hChan, &hCe,   PASCAL_DMA_COPY_B_CLASS,         NULL);
    NvU32 s_2d   = rm_alloc(hClient, hChan, &h2d,   FERMI_TWOD_A_CLASS,              NULL);
    NvU32 s_3d   = rm_alloc(hClient, hChan, &h3d,   PASCAL_B_CLASS,                  NULL);
    NvU32 s_i2m  = rm_alloc(hClient, hChan, &hI2m,  KEPLER_INLINE_TO_MEMORY_B_CLASS, NULL);
    NvU32 s_comp = rm_alloc(hClient, hChan, &hComp, PASCAL_COMPUTE_B_CLASS,          NULL);
    printf("[11] Engines: CE(c1b5)=0x%x 2D(902d)=0x%x 3D(c197)=0x%x I2M(a140)=0x%x COMP(c1c0)=0x%x\n",
           s_ce, s_2d, s_3d, s_i2m, s_comp);

    NVA06F_CTRL_BIND_PARAMS bind_p = { .engineType = NV2080_ENGINE_TYPE_GRAPHICS };
    NvU32 s_bind = rm_control(hClient, hChan, NVA06F_CTRL_CMD_BIND, &bind_p, sizeof(bind_p));
    NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS sched_p = { .bEnable = NV_TRUE };
    NvU32 s_sched = rm_control(hClient, hChan, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &sched_p, sizeof(sched_p));

    volatile uint32_t *userd = NULL;
    NvU32 s_umap = rm_map_cpu(hClient, hSubdev, hChan, 0, 512, false, 0, (void **)&userd);
    printf("[12] Channel Bind=0x%x Schedule=0x%x USERD=%p (map=0x%x)\n",
           s_bind, s_sched, (void *)userd, s_umap);

    if (userd && s_sched == NV_OK) {
        uint64_t *gpfifo = (uint64_t *)cmd_cpu;
        uint32_t *push   = (uint32_t *)((uint8_t *)cmd_cpu + 0x4000);
        volatile uint32_t *sema = (volatile uint32_t *)((uint8_t *)cmd_cpu + 0x8000);
        sema[0] = 0;
        uint64_t push_gpu = 0x10004000ULL;
        uint64_t sema_gpu = 0x10008000ULL;

        int p = 0;
        /* Subc 4 = PASCAL_DMA_COPY_B (0xc1b5): SET_OBJECT (0x0000) = 0xc1b5 */
        push[p++] = NVPUSH_INC(4, 0x0000, 1);
        push[p++] = PASCAL_DMA_COPY_B_CLASS;
        /* OFFSET_IN_UPPER (0x0400), OFFSET_IN_LOWER, OFFSET_OUT_UPPER, OFFSET_OUT_LOWER */
        push[p++] = NVPUSH_INC(4, 0x0400, 4);
        push[p++] = 0x00000000U;
        push[p++] = 0x20000000U; /* Src = GDDR5 VRAM */
        push[p++] = 0x00000000U;
        push[p++] = 0x30000000U; /* Dst = Zero-Copy macOS IOSurface! */
        /* LINE_LENGTH_IN (0x0418), LINE_COUNT (0x041c) */
        push[p++] = NVPUSH_INC(4, 0x0418, 2);
        push[p++] = 16384U; /* 16 KB */
        push[p++] = 1U;
        /* LAUNCH_DMA (0x0300): DATA_TRANSFER_TYPE=NON_PIPELINED(2) | FLUSH_ENABLE(1<<2) | SRC_LINEAR(1<<7) | DST_LINEAR(1<<8) = 0x186 */
        push[p++] = NVPUSH_INC(4, 0x0300, 1);
        push[p++] = 0x00000186U;

        /* Host Semaphore Release (NVC06F_SEMAPHOREA=0x0010 .. D=0x001c on subc 0) */
        push[p++] = NVPUSH_INC(0, 0x0010, 4);
        push[p++] = (uint32_t)(sema_gpu >> 32);
        push[p++] = (uint32_t)(sema_gpu & 0xffffffffU);
        push[p++] = 0xcafebabeU;
        push[p++] = (1U << 24) | 0x2U; /* RELEASE_SIZE_4BYTE(1<<24) | RELEASE_WFI_EN(0<<20) | OPERATION_RELEASE(2) */

        /* Format GPFIFO entry: bits [39:2]=gpu_va>>2, bit 41=priv(0), bits [61:42]=dwords */
        gpfifo[0] = (push_gpu & ~3ULL) | ((uint64_t)p << 42);
        __asm__ __volatile__("mfence" ::: "memory");

        /* Kick Pascal GPFIFO via USERD->GPPut (offset 0x8c = dword 35)! */
        userd[0x8c / 4] = 1U;
        __asm__ __volatile__("mfence" ::: "memory");
        (void)userd[0x8c / 4];

        for (int spin = 0; spin < 50000 && sema[0] != 0xcafebabeU; spin++) {
            usleep(10);
        }
        const uint32_t *surf_u32 = (const uint32_t *)iosurf_base;
        printf("[13] GPU Execution Result: sema=0x%08x  IOSurface[0]=0x%08x IOSurface[4095]=0x%08x -> %s!\n",
               sema[0], surf_u32[0], surf_u32[4095],
               (sema[0] == 0xcafebabeU && surf_u32[0] == 0xA5000000U && surf_u32[4095] == 0xA5000FFFU)
               ? "ZERO-COPY IOSURFACE DMA PASS" : "CHECK");
    }

    bp.flags = 0; bp.duration = 0;
    rm_control(hClient, hSubdev, NV2080_CTRL_CMD_PERF_BOOST_ID, &bp, sizeof(bp));

    rm_free_obj(hClient, hChan);
    rm_free_obj(hClient, hVirtSurf);
    rm_free_obj(hClient, hVirtVram);
    rm_free_obj(hClient, hVirtCmd);
    rm_free_obj(hClient, hIoSurfMem);
    rm_free_obj(hClient, hVramSrc);
    rm_free_obj(hClient, hSysNotif);
    rm_free_obj(hClient, hSysCmd);
    rm_free_obj(hClient, hVaSpace);
    rm_free_obj(hClient, hSubdev);
    rm_free_obj(hClient, hDevice);
    rm_free_obj(hClient, hClient);
    CFRelease(ioSurf);

    gp108_rm_teardown();
    printf("[14] Clean teardown complete!\n");
    return 0;
}
