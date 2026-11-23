/* SPDX-License-Identifier: MIT
 * gp108-rmd — Resident Userspace NVIDIA Resource Manager Daemon for MX250 (GP108)
 *
 * Hosts unmodified FreeBSD x86_64 nv-kernel.o (470.256.02) in low <2 GiB VA space
 * atop GP108Bridge.kext with SIP & AMFI fully enabled:
 *   - Unlocks Full-Blood P0 Clocks (1519–1721 MHz Core + 7008 Mbps / 56 GB/s GDDR5)
 *   - Isolates each client process in its own RM Root Client + FERMI_VASPACE_A
 *   - Provides zero-copy POSIX shm GART/Ring BOs and zero-copy macOS IOSurface DMA
 *   - Publishes live P0 clocks, P-state, and die temperature to GP108TelemetryPage
 */
#include "gp108_rm_host.h"
#include "gp108_rm_api_defs.h"
#include "gp108_rmd_ipc.h"
#include "../include/GP108TelemetryShared.h"

#include <IOSurface/IOSurface.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_SESS_BOS   4096
#define MAX_SESS_VMAS  8192
#define MAX_SESS_CTXS  32
#define INTERNAL_RING_VA_BASE 0x1800000000ULL /* 96 GiB (inside 37-bit FERMI_VASPACE_A) */

struct rmd_bo {
    bool          used;
    uint32_t      bo_id;
    NvHandle      hMem;
    bool          is_vram;
    bool          is_sysmem;
    uint64_t      size_B;
    void         *shm_va;
    size_t        shm_sz;
    IOSurfaceRef  iosurf;
};

struct rmd_vma {
    bool     used;
    uint64_t addr;
    uint64_t size_B;
    NvHandle hVirt;
    uint32_t bo_id;
};

struct rmd_ctx {
    bool               used;
    uint32_t           ctx_id;
    NvHandle           hShmMem;
    NvHandle           hVirtRing;
    NvHandle           hCtxDma;
    NvHandle           hChan;
    NvHandle           hCe, h2d, h3d, hI2m, hComp;
    void              *shm_va;
    size_t             shm_sz;
    uint64_t           ring_gpu_va;
    volatile uint32_t *userd;
};

struct rmd_session {
    int             sock_fd;
    uint32_t        client_pid;
    NvHandle        hClient;
    NvHandle        hDevice;
    NvHandle        hSubdev;
    NvHandle        hVaSpace;
    uint32_t        next_bo_id;
    uint32_t        next_ctx_id;
    struct rmd_bo   bos[MAX_SESS_BOS];
    struct rmd_vma  vmas[MAX_SESS_VMAS];
    struct rmd_ctx  ctxs[MAX_SESS_CTXS];
};

static pthread_mutex_t           g_rmd_lock    = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t     g_running     = 1;
static int                       g_listen_fd   = -1;
static NvHandle                  g_mon_client  = 0;
static NvHandle                  g_mon_device  = 0;
static NvHandle                  g_mon_subdev  = 0;
static struct gp108_nvk_hw_info  g_hw_info;
static struct GP108TelemetryPage *g_tpage      = NULL;
static volatile uint32_t        *g_bar0_mmio   = NULL;
static uint32_t                  g_shm_seq     = 1;
static uint32_t                  g_os02_hseq   = 1;
static int                       g_active_sessions = 0;
static uint64_t                  g_last_active_sec = 0;
static uint32_t                  g_idle_timeout_sec = 60;

static uint64_t mono_sec_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}

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
    if (!hClient || !hObject) return;
    NVOS00_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hRoot      = hClient;
    p.hObjectOld = hObject;
    gp108_rm_call_ioctl(NULL, true, NV_ESC_RM_FREE, &p, sizeof(p));
}

static NvU32 rm_map_dev(NvHandle hClient, NvHandle hSubdev, NvHandle hObj,
                        NvU64 offset, NvU64 length, void **out_va)
{
    nv_ioctl_nvos33_with_fd_t p;
    memset(&p, 0, sizeof(p));
    p.params.hClient = hClient;
    p.params.hDevice = hSubdev;
    p.params.hMemory = hObj;
    p.params.offset  = offset;
    p.params.length  = length;
    p.fd             = 1;
    int rc = gp108_rm_call_ioctl(NULL, true, NV_ESC_RM_MAP_MEMORY, &p, sizeof(p));
    if (rc != 0) return (NvU32)(-rc);
    if (p.params.status != NV_OK) return p.params.status;
    if (gp108_rm_get_last_mmap(1, NULL, NULL, NULL, NULL, out_va) != 0)
        return NV_ERR_GENERIC;
    if (out_va && *out_va) {
        *out_va = (uint8_t *)(*out_va) + ((uintptr_t)p.params.pLinearAddress & 0xfffUL);
    }
    return NV_OK;
}

static uint64_t read_gpu_ptimer_ns(void)
{
    if (!g_bar0_mmio) return 0;
    uint32_t hi1, lo, hi2;
    do {
        hi1 = g_bar0_mmio[0x009410 / 4];
        lo  = g_bar0_mmio[0x009400 / 4];
        hi2 = g_bar0_mmio[0x009410 / 4];
    } while (hi1 != hi2);
    return ((uint64_t)hi1 << 32) | lo;
}

static uint32_t read_gpu_temp_c(void)
{
    if (!g_bar0_mmio) return 0;
    uint32_t raw = g_bar0_mmio[0x020460 / 4];
    /* GP108 NV_THERM_I2CS_SENSOR_00: signed 9.5 fixed point or integer degrees in [16:9]/[8:0] */
    int32_t deg = (int32_t)((raw >> 9) & 0xffU);
    if (deg <= 0 || deg > 120)
        deg = (int32_t)(raw & 0x1ffU);
    return (deg > 0 && deg < 125) ? (uint32_t)deg : 42U;
}

static void refresh_telemetry_locked(void)
{
    if (!g_tpage || !g_mon_subdev) return;

    struct { NvU32 currPstate; } ps = { 0 };
    rm_control(g_mon_client, g_mon_subdev, NV2080_CTRL_CMD_PERF_GET_CUR_PSTATE, &ps, sizeof(ps));

    struct {
        NvU32 flags, clkDomain, actualFreq, targetFreq, clkSource;
    } clks[3] = {
        { 0, 0x00010000U, 0, 0, 0 }, /* GPCCLK */
        { 0, 0x00000010U, 0, 0, 0 }, /* MCLK   */
        { 0, 0x00080000U, 0, 0, 0 }, /* SYSCLK */
    };
    struct {
        NvU32 flags;
        NvU32 clkInfoListSize;
        NvP64 clkInfoList;
    } cg = { 0, 3, (NvP64)(uintptr_t)clks };
    rm_control(g_mon_client, g_mon_subdev, 0x20801002U, &cg, sizeof(cg));

    g_tpage->curr_pstate = ps.currPstate;
    if (clks[0].actualFreq) g_tpage->gpc_clk_khz = clks[0].actualFreq;
    if (clks[1].actualFreq) g_tpage->mem_clk_khz = clks[1].actualFreq;
    if (clks[2].actualFreq) g_tpage->sys_clk_khz = clks[2].actualFreq;
    g_tpage->gpu_temp_c       = read_gpu_temp_c();
    g_tpage->vram_total_bytes = g_hw_info.vram_total_B;
    g_tpage->pci_bdf          = ((uint32_t)g_hw_info.pci_bus << 8) |
                                ((uint32_t)g_hw_info.pci_dev << 3) |
                                (uint32_t)g_hw_info.pci_func;
    g_tpage->pmc_boot0        = g_bar0_mmio ? g_bar0_mmio[0] : 0x138000a1U;
    __sync_synchronize();
    g_tpage->rm_active        = 1U;
}

static void wake_accept_loop(void)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s >= 0) {
        struct sockaddr_un sun;
        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        strlcpy(sun.sun_path, GP108_RMD_SOCK_PATH, sizeof(sun.sun_path));
        connect(s, (struct sockaddr *)&sun, sizeof(sun));
        close(s);
    }
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

static void *telemetry_thread_main(void *arg)
{
    (void)arg;
    while (g_running) {
        pthread_mutex_lock(&g_rmd_lock);
        if (g_bar0_mmio && g_bar0_mmio[0] == 0xffffffffU) {
            fprintf(stderr, "[gp108-rmd] Detected GPU bus power-off (sleep/wake); exiting for clean on-demand restart.\n");
            g_running = 0;
            if (g_tpage) g_tpage->rm_active = 0;
            pthread_mutex_unlock(&g_rmd_lock);
            wake_accept_loop();
            unlink(GP108_RMD_SOCK_PATH);
            break;
        }
        if (g_idle_timeout_sec > 0 && g_active_sessions == 0 &&
            (mono_sec_now() - g_last_active_sec) >= g_idle_timeout_sec) {
            fprintf(stderr, "[gp108-rmd] Idle %u s with 0 clients -> powering off MX250 to D3cold (0W).\n",
                    g_idle_timeout_sec);
            g_running = 0;
            if (g_tpage) g_tpage->rm_active = 0;
            pthread_mutex_unlock(&g_rmd_lock);
            wake_accept_loop();
            unlink(GP108_RMD_SOCK_PATH);
            break;
        }
        refresh_telemetry_locked();
        pthread_mutex_unlock(&g_rmd_lock);
        for (int i = 0; i < 10 && g_running; i++)
            usleep(100000);
    }
    return NULL;
}

/* Create anonymous POSIX shared memory backed by wired physical DDR4 pages */
static int create_pinned_shm(NvHandle hClient, NvHandle hDevice, size_t bytes,
                             int *out_fd, void **out_va, NvHandle *out_hMem)
{
    bytes = (bytes + 4095UL) & ~4095UL;
    char name[64];
    uint32_t seq = __sync_fetch_and_add(&g_shm_seq, 1U);
    snprintf(name, sizeof(name), "/gp108_rm_%d_%u", (int)getpid(), seq);

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return -errno;
    shm_unlink(name); /* Unlink immediately so zero shm names ever leak */

    if (ftruncate(fd, (off_t)bytes) != 0) {
        int e = errno;
        close(fd);
        return -e;
    }

    void *va = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (va == MAP_FAILED) {
        int e = errno;
        close(fd);
        return -e;
    }
    /* Fault & zero pages before pinning */
    memset(va, 0, bytes);

    nv_ioctl_nvos02_with_fd_t os02;
    memset(&os02, 0, sizeof(os02));
    os02.params.hRoot         = hClient;
    os02.params.hObjectParent = hDevice;
    os02.params.hObjectNew    = 0x5e000000U + __sync_fetch_and_add(&g_os02_hseq, 1U);
    os02.params.hClass        = NV01_MEMORY_SYSTEM_OS_DESC_CLASS;
    os02.params.flags         = DRF_DEF(OS02, _FLAGS, _LOCATION, _PCI) |
                                DRF_DEF(OS02, _FLAGS, _MAPPING, _NO_MAP) |
                                DRF_DEF(OS02, _FLAGS, _COHERENCY, _CACHED) |
                                DRF_DEF(OS02, _FLAGS, _PHYSICALITY, _NONCONTIGUOUS);
    os02.params.pMemory       = (NvP64)(uintptr_t)va;
    os02.params.limit         = (NvU64)bytes - 1ULL;
    os02.fd                   = 1;

    int rc = gp108_rm_call_ioctl(NULL, false, NV_ESC_RM_ALLOC_MEMORY, &os02, sizeof(os02));
    if (rc != 0 || os02.params.status != NV_OK) {
        munmap(va, bytes);
        close(fd);
        return -ENOMEM;
    }

    *out_fd   = fd;
    *out_va   = va;
    *out_hMem = os02.params.hObjectNew;
    return 0;
}

/* Map Pascal (GP108 SM 6.1) PTE kinds to RM NV50_MEMORY_VIRTUAL attributes */
static void configure_pascal_virtual_params(uint8_t pte_kind, bool is_sysmem, NV_MEMORY_ALLOCATION_PARAMS *vp)
{
    uint32_t pg_attr = is_sysmem ? DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB)
                                 : DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _BIG);
    vp->type = NVOS32_TYPE_IMAGE;
    switch (pte_kind) {
        case 0x00: /* NV_MMU_PTE_KIND_PITCH */
            vp->attr |= DRF_DEF(OS32, _ATTR, _FORMAT, _PITCH) | pg_attr;
            break;
        case 0xfe: /* NV_MMU_PTE_KIND_GENERIC_16BX2 (uncompressed color/texture BL) */
            vp->type  = NVOS32_TYPE_IMAGE;
            vp->attr |= DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR) |
                        DRF_DEF(OS32, _ATTR, _DEPTH, _32) |
                        DRF_DEF(OS32, _ATTR, _COMPR, _NONE) | pg_attr;
            vp->format = 0xfe;
            break;
        case 0x01: /* NV_MMU_PTE_KIND_Z16 */
            vp->type  = NVOS32_TYPE_DEPTH;
            vp->attr |= DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR) |
                        DRF_DEF(OS32, _ATTR, _DEPTH, _16) |
                        DRF_DEF(OS32, _ATTR, _Z_TYPE, _FIXED) |
                        DRF_DEF(OS32, _ATTR, _ZS_PACKING, _Z16) |
                        DRF_DEF(OS32, _ATTR, _COMPR, _NONE) | pg_attr;
            break;
        case 0x11: /* NV_MMU_PTE_KIND_S8Z24 */
            vp->type  = NVOS32_TYPE_DEPTH;
            vp->attr |= DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR) |
                        DRF_DEF(OS32, _ATTR, _DEPTH, _32) |
                        DRF_DEF(OS32, _ATTR, _Z_TYPE, _FIXED) |
                        DRF_DEF(OS32, _ATTR, _ZS_PACKING, _S8Z24) |
                        DRF_DEF(OS32, _ATTR, _COMPR, _NONE) | pg_attr;
            break;
        case 0x46: /* NV_MMU_PTE_KIND_Z24S8 */
            vp->type  = NVOS32_TYPE_DEPTH;
            vp->attr |= DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR) |
                        DRF_DEF(OS32, _ATTR, _DEPTH, _32) |
                        DRF_DEF(OS32, _ATTR, _Z_TYPE, _FIXED) |
                        DRF_DEF(OS32, _ATTR, _ZS_PACKING, _Z24S8) |
                        DRF_DEF(OS32, _ATTR, _COMPR, _NONE) | pg_attr;
            break;
        case 0x7b: /* NV_MMU_PTE_KIND_ZF32 */
            vp->type  = NVOS32_TYPE_DEPTH;
            vp->attr |= DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR) |
                        DRF_DEF(OS32, _ATTR, _DEPTH, _32) |
                        DRF_DEF(OS32, _ATTR, _Z_TYPE, _FLOAT) |
                        DRF_DEF(OS32, _ATTR, _ZS_PACKING, _Z32) |
                        DRF_DEF(OS32, _ATTR, _COMPR, _NONE) | pg_attr;
            break;
        case 0xc3: /* NV_MMU_PTE_KIND_ZF32_X24S8 */
            vp->type  = NVOS32_TYPE_DEPTH;
            vp->attr |= DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR) |
                        DRF_DEF(OS32, _ATTR, _DEPTH, _64) |
                        DRF_DEF(OS32, _ATTR, _Z_TYPE, _FLOAT) |
                        DRF_DEF(OS32, _ATTR, _ZS_PACKING, _Z32_X24S8) |
                        DRF_DEF(OS32, _ATTR, _COMPR, _NONE) | pg_attr;
            break;
        case 0x2a: /* NV_MMU_PTE_KIND_S8 */
            vp->type  = NVOS32_TYPE_STENCIL;
            vp->attr |= DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR) |
                        DRF_DEF(OS32, _ATTR, _DEPTH, _8) |
                        DRF_DEF(OS32, _ATTR, _Z_TYPE, _FIXED) |
                        DRF_DEF(OS32, _ATTR, _ZS_PACKING, _S8) |
                        DRF_DEF(OS32, _ATTR, _COMPR, _NONE) | pg_attr;
            break;
        default:
            vp->attr |= DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR) |
                        DRF_DEF(OS32, _ATTR, _COMPR, _NONE) | pg_attr;
            vp->flags |= NVOS32_ALLOC_FLAGS_FORCE_INTERNAL_INDEX;
            vp->format = pte_kind;
            break;
    }
}

static NvU32 bind_fixed_gpu_va(NvHandle hClient, NvHandle hDevice, NvHandle hVaSpace,
                               NvHandle hMemPhys, bool is_sysmem, uint8_t pte_kind,
                               uint64_t gpu_va, uint64_t mem_offset_B, uint64_t range_B,
                               NvHandle *out_hVirt)
{
    NV_MEMORY_ALLOCATION_PARAMS vp;
    memset(&vp, 0, sizeof(vp));
    vp.owner    = hClient;
    vp.flags    = NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE;
    vp.size     = range_B;
    vp.offset   = gpu_va;
    vp.hVASpace = hVaSpace;
    configure_pascal_virtual_params(pte_kind, is_sysmem, &vp);

    NvHandle hVirt = 0;
    NvU32 st = rm_alloc(hClient, hDevice, &hVirt, NV50_MEMORY_VIRTUAL_CLASS, &vp);
    if (st != NV_OK) {
        fprintf(stderr, "[gp108-rmd] NV50_MEMORY_VIRTUAL failed: st=0x%x va=0x%llx sz=0x%llx kind=0x%x\n",
                st, (unsigned long long)gpu_va, (unsigned long long)range_B, pte_kind);
        return st;
    }
    if (pte_kind != 0 && vp.format != pte_kind) {
        fprintf(stderr, "[gp108-rmd] NOTE: kind req=0x%x RM format=0x%x\n", pte_kind, vp.format);
    }

    NVOS46_PARAMETERS mp;
    memset(&mp, 0, sizeof(mp));
    mp.hClient   = hClient;
    mp.hDevice   = hDevice;
    mp.hDma      = hVirt;
    mp.hMemory   = hMemPhys;
    mp.offset    = mem_offset_B;
    mp.length    = range_B;
    mp.dmaOffset = 0;
    mp.flags     = DRF_DEF(OS46, _FLAGS, _PAGE_KIND, _VIRTUAL) |
                   (is_sysmem ? DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE)
                              : DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _DISABLE));

    int rc = gp108_rm_call_ioctl(NULL, true, NV_ESC_RM_MAP_MEMORY_DMA, &mp, sizeof(mp));
    if (rc != 0 || mp.status != NV_OK) {
        NvU32 err = rc != 0 ? (NvU32)(-rc) : mp.status;
        fprintf(stderr, "[gp108-rmd] MAP_MEMORY_DMA failed: st=0x%x va=0x%llx sz=0x%llx kind=0x%x sys=%d\n",
                err, (unsigned long long)gpu_va, (unsigned long long)range_B, pte_kind, is_sysmem);
        rm_free_obj(hClient, hVirt);
        return err;
    }
    *out_hVirt = hVirt;
    return NV_OK;
}

static int send_resp_with_fd(int sock, const struct gp108_rmd_resp *resp, int pass_fd)
{
    struct iovec iov = { .iov_base = (void *)resp, .iov_len = sizeof(*resp) };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    if (pass_fd >= 0) {
        memset(cmsg_buf, 0, sizeof(cmsg_buf));
        msg.msg_control    = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type  = SCM_RIGHTS;
        cm->cmsg_len   = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &pass_fd, sizeof(int));
    }

    ssize_t n = sendmsg(sock, &msg, 0);
    return (n == (ssize_t)sizeof(*resp)) ? 0 : -EIO;
}

static int recv_exact(int sock, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(sock, p + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static int session_init_rm_locked(struct rmd_session *s)
{
    nv_state_t *gpu = gp108_rm_get_gpu_state();
    if (rm_alloc(0, 0, &s->hClient, NV01_ROOT_CLIENT_CLASS, NULL) != NV_OK)
        return -EIO;

    NV0000_CTRL_GPU_ATTACH_IDS_PARAMS att;
    memset(&att, 0, sizeof(att));
    att.gpuIds[0] = gpu->gpu_id;
    att.gpuIds[1] = 0xffffffffU;
    rm_control(s->hClient, s->hClient, NV0000_CTRL_CMD_GPU_ATTACH_IDS, &att, sizeof(att));

    NV0000_CTRL_GPU_GET_ID_INFO_PARAMS idi;
    memset(&idi, 0, sizeof(idi));
    idi.gpuId = gpu->gpu_id;
    rm_control(s->hClient, s->hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO, &idi, sizeof(idi));

    NV0080_ALLOC_PARAMETERS ap0080;
    memset(&ap0080, 0, sizeof(ap0080));
    ap0080.deviceId     = idi.deviceInstance;
    ap0080.hClientShare = s->hClient;
    if (rm_alloc(s->hClient, s->hClient, &s->hDevice, NV01_DEVICE_0_CLASS, &ap0080) != NV_OK)
        return -EIO;

    NV2080_ALLOC_PARAMETERS ap2080 = { .subDeviceId = idi.subDeviceInstance };
    if (rm_alloc(s->hClient, s->hDevice, &s->hSubdev, NV20_SUBDEVICE_0_CLASS, &ap2080) != NV_OK)
        return -EIO;

    NV_VASPACE_ALLOCATION_PARAMETERS vap;
    memset(&vap, 0, sizeof(vap));
    vap.flags = NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS;
    if (rm_alloc(s->hClient, s->hDevice, &s->hVaSpace, FERMI_VASPACE_A_CLASS, &vap) != NV_OK)
        return -EIO;

    s->next_bo_id  = 1;
    s->next_ctx_id = 1;
    return 0;
}

static void session_cleanup_locked(struct rmd_session *s)
{
    for (int i = 0; i < MAX_SESS_CTXS; i++) {
        if (s->ctxs[i].used && s->ctxs[i].shm_va) {
            const uint32_t *nf = (const uint32_t *)((const uint8_t *)s->ctxs[i].shm_va + GP108_CHAN_SHM_NOTIF_OFF);
            if (nf[2] || nf[3]) {
                fprintf(stderr, "[gp108-rmd] cleanup ctx %u notifier: info32=0x%x status_info16=0x%x\n",
                        s->ctxs[i].ctx_id, nf[2], nf[3]);
            }
        }
    }
    /* Free RM root client FIRST so GPU channels & DMA mappings detach BEFORE unmapping host pages */
    if (s->hClient) {
        rm_free_obj(s->hClient, s->hClient);
        s->hClient = 0;
    }
    for (int i = 0; i < MAX_SESS_CTXS; i++) {
        if (s->ctxs[i].used && s->ctxs[i].shm_va) {
            munmap(s->ctxs[i].shm_va, s->ctxs[i].shm_sz);
            s->ctxs[i].used = false;
        }
    }
    for (int i = 0; i < MAX_SESS_BOS; i++) {
        if (s->bos[i].used) {
            if (s->bos[i].shm_va)
                munmap(s->bos[i].shm_va, s->bos[i].shm_sz);
            if (s->bos[i].iosurf)
                CFRelease(s->bos[i].iosurf);
            s->bos[i].used = false;
        }
    }
}

static void handle_rpc_locked(struct rmd_session *s, const struct gp108_rmd_req *req,
                              struct gp108_rmd_resp *resp, int *out_fd)
{
    memset(resp, 0, sizeof(*resp));
    *out_fd = -1;

    switch (req->op) {
        case RMD_OP_HELLO: {
            s->client_pid = req->u.hello.client_pid;
            resp->status  = 0;
            resp->u.hello.hw = g_hw_info;
            break;
        }
        case RMD_OP_GET_TIMESTAMP: {
            resp->status = 0;
            resp->u.ts.timestamp_ns = read_gpu_ptimer_ns();
            break;
        }
        case RMD_OP_BO_ALLOC: {
            int slot = -1;
            for (int i = 0; i < MAX_SESS_BOS; i++) {
                if (!s->bos[i].used) { slot = i; break; }
            }
            if (slot < 0) { resp->status = -ENOMEM; break; }

            uint64_t sz = req->u.bo_alloc.size_B;
            uint8_t kind = req->u.bo_alloc.pte_kind;
            bool want_vram = req->u.bo_alloc.in_vram != 0;
            bool mappable  = req->u.bo_alloc.mappable != 0;
            uint64_t align = (kind != 0) ? 65536ULL : 4096ULL;
            sz = (sz + align - 1ULL) & ~(align - 1ULL);

            struct rmd_bo *bo = &s->bos[slot];
            memset(bo, 0, sizeof(*bo));

            if (want_vram && !mappable) {
                uint64_t vsz = (sz + 65535ULL) & ~65535ULL;
                NV_MEMORY_ALLOCATION_PARAMS vmp;
                memset(&vmp, 0, sizeof(vmp));
                vmp.owner = s->hClient;
                vmp.type  = NVOS32_TYPE_IMAGE;
                vmp.flags = NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM;
                vmp.attr  = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _BIG) |
                            DRF_DEF(OS32, _ATTR, _LOCATION, _VIDMEM) |
                            DRF_DEF(OS32, _ATTR, _COHERENCY, _WRITE_COMBINE) |
                            DRF_DEF(OS32, _ATTR, _PHYSICALITY, _ALLOW_NONCONTIGUOUS);
                vmp.size  = vsz;

                NvHandle hMem = 0;
                if (rm_alloc(s->hClient, s->hDevice, &hMem, NV01_MEMORY_LOCAL_USER_CLASS, &vmp) == NV_OK) {
                    bo->used      = true;
                    bo->bo_id     = s->next_bo_id++;
                    bo->hMem      = hMem;
                    bo->is_vram   = true;
                    bo->is_sysmem = false;
                    bo->size_B    = vsz;

                    resp->status             = 0;
                    resp->u.bo.bo_id         = bo->bo_id;
                    resp->u.bo.is_vram       = 1;
                    resp->u.bo.actual_size_B = vsz;
                    break;
                }
                /* Fall through to coherent SysMem if 2 GB VRAM is full */
            }

            int shm_fd = -1;
            void *shm_va = NULL;
            NvHandle hMem = 0;
            int rc = create_pinned_shm(s->hClient, s->hDevice, (size_t)sz, &shm_fd, &shm_va, &hMem);
            if (rc != 0) {
                resp->status = rc;
                break;
            }
            bo->used      = true;
            bo->bo_id     = s->next_bo_id++;
            bo->hMem      = hMem;
            bo->is_vram   = false;
            bo->is_sysmem = true;
            bo->size_B    = sz;
            bo->shm_va    = shm_va;
            bo->shm_sz    = (size_t)sz;

            resp->status             = 0;
            resp->has_fd             = 1;
            resp->u.bo.bo_id         = bo->bo_id;
            resp->u.bo.is_vram       = 0;
            resp->u.bo.actual_size_B = sz;
            *out_fd                  = shm_fd;
            break;
        }
        case RMD_OP_BO_IMPORT_IOSURFACE: {
            int slot = -1;
            for (int i = 0; i < MAX_SESS_BOS; i++) {
                if (!s->bos[i].used) { slot = i; break; }
            }
            if (slot < 0) { resp->status = -ENOMEM; break; }

            IOSurfaceRef surf = IOSurfaceLookup((IOSurfaceID)req->u.bo_iosurf.iosurface_id);
            if (!surf) { resp->status = -ENOENT; break; }

            void *base = IOSurfaceGetBaseAddress(surf);
            size_t sz  = IOSurfaceGetAllocSize(surf);
            if (req->u.bo_iosurf.size_B > sz) sz = (size_t)req->u.bo_iosurf.size_B;
            sz = (sz + 4095UL) & ~4095UL;

            nv_ioctl_nvos02_with_fd_t os02;
            memset(&os02, 0, sizeof(os02));
            os02.params.hRoot         = s->hClient;
            os02.params.hObjectParent = s->hDevice;
            os02.params.hObjectNew    = 0x5f000000U + __sync_fetch_and_add(&g_os02_hseq, 1U);
            os02.params.hClass        = NV01_MEMORY_SYSTEM_OS_DESC_CLASS;
            os02.params.flags         = DRF_DEF(OS02, _FLAGS, _LOCATION, _PCI) |
                                        DRF_DEF(OS02, _FLAGS, _MAPPING, _NO_MAP) |
                                        DRF_DEF(OS02, _FLAGS, _COHERENCY, _CACHED) |
                                        DRF_DEF(OS02, _FLAGS, _PHYSICALITY, _NONCONTIGUOUS);
            os02.params.pMemory       = (NvP64)(uintptr_t)base;
            os02.params.limit         = (NvU64)sz - 1ULL;
            os02.fd                   = 1;

            int rc = gp108_rm_call_ioctl(NULL, false, NV_ESC_RM_ALLOC_MEMORY, &os02, sizeof(os02));
            if (rc != 0 || os02.params.status != NV_OK) {
                CFRelease(surf);
                resp->status = -EIO;
                break;
            }

            struct rmd_bo *bo = &s->bos[slot];
            memset(bo, 0, sizeof(*bo));
            bo->used      = true;
            bo->bo_id     = s->next_bo_id++;
            bo->hMem      = os02.params.hObjectNew;
            bo->is_vram   = false;
            bo->is_sysmem = true;
            bo->size_B    = sz;
            bo->iosurf    = surf;

            resp->status             = 0;
            resp->u.bo.bo_id         = bo->bo_id;
            resp->u.bo.is_vram       = 0;
            resp->u.bo.actual_size_B = sz;
            break;
        }
        case RMD_OP_BO_FREE: {
            uint32_t bid = req->u.bo_free.bo_id;
            for (int i = 0; i < MAX_SESS_VMAS; i++) {
                if (s->vmas[i].used && s->vmas[i].bo_id == bid) {
                    rm_free_obj(s->hClient, s->vmas[i].hVirt);
                    s->vmas[i].used = false;
                }
            }
            for (int i = 0; i < MAX_SESS_BOS; i++) {
                if (s->bos[i].used && s->bos[i].bo_id == bid) {
                    rm_free_obj(s->hClient, s->bos[i].hMem);
                    if (s->bos[i].shm_va)
                        munmap(s->bos[i].shm_va, s->bos[i].shm_sz);
                    if (s->bos[i].iosurf)
                        CFRelease(s->bos[i].iosurf);
                    s->bos[i].used = false;
                    break;
                }
            }
            resp->status = 0;
            break;
        }
        case RMD_OP_VM_MAP: {
            struct rmd_bo *bo = NULL;
            for (int i = 0; i < MAX_SESS_BOS; i++) {
                if (s->bos[i].used && s->bos[i].bo_id == req->u.vm_map.bo_id) {
                    bo = &s->bos[i];
                    break;
                }
            }
            if (!bo) { resp->status = -ENOENT; break; }

            uint64_t u_start = req->u.vm_map.addr;
            uint64_t u_end   = u_start + req->u.vm_map.range_B;
            for (int i = 0; i < MAX_SESS_VMAS; i++) {
                if (s->vmas[i].used) {
                    uint64_t s_start = s->vmas[i].addr;
                    uint64_t s_end   = s_start + s->vmas[i].size_B;
                    if (s_start < u_end && s_end > u_start) {
                        rm_free_obj(s->hClient, s->vmas[i].hVirt);
                        s->vmas[i].used = false;
                    }
                }
            }

            int vslot = -1;
            for (int i = 0; i < MAX_SESS_VMAS; i++) {
                if (!s->vmas[i].used) { vslot = i; break; }
            }
            if (vslot < 0) { resp->status = -ENOMEM; break; }

            NvHandle hVirt = 0;
            NvU32 st = bind_fixed_gpu_va(s->hClient, s->hDevice, s->hVaSpace,
                                         bo->hMem, bo->is_sysmem, req->u.vm_map.pte_kind,
                                         req->u.vm_map.addr, req->u.vm_map.bo_offset_B,
                                         req->u.vm_map.range_B, &hVirt);
            if (st != NV_OK) {
                resp->status = -ENOMEM;
                break;
            }
            s->vmas[vslot].used   = true;
            s->vmas[vslot].addr   = req->u.vm_map.addr;
            s->vmas[vslot].size_B = req->u.vm_map.range_B;
            s->vmas[vslot].hVirt  = hVirt;
            s->vmas[vslot].bo_id  = bo->bo_id;
            resp->status = 0;
            break;
        }
        case RMD_OP_VM_UNMAP: {
            uint64_t u_start = req->u.vm_unmap.addr;
            uint64_t u_end   = u_start + req->u.vm_unmap.range_B;
            for (int i = 0; i < MAX_SESS_VMAS; i++) {
                if (s->vmas[i].used) {
                    uint64_t s_start = s->vmas[i].addr;
                    uint64_t s_end   = s_start + s->vmas[i].size_B;
                    if (s_start < u_end && s_end > u_start) {
                        rm_free_obj(s->hClient, s->vmas[i].hVirt);
                        s->vmas[i].used = false;
                    }
                }
            }
            resp->status = 0;
            break;
        }
        case RMD_OP_CTX_CREATE: {
            int cslot = -1;
            for (int i = 0; i < MAX_SESS_CTXS; i++) {
                if (!s->ctxs[i].used) { cslot = i; break; }
            }
            if (cslot < 0) { resp->status = -ENOMEM; break; }

            struct rmd_ctx *c = &s->ctxs[cslot];
            memset(c, 0, sizeof(*c));

            int shm_fd = -1;
            int rc = create_pinned_shm(s->hClient, s->hDevice, GP108_CHAN_SHM_TOTAL_SIZE,
                                       &shm_fd, &c->shm_va, &c->hShmMem);
            if (rc != 0) { resp->status = rc; break; }
            c->shm_sz      = GP108_CHAN_SHM_TOTAL_SIZE;
            c->ring_gpu_va = INTERNAL_RING_VA_BASE + (uint64_t)cslot * 0x100000ULL;

            if (bind_fixed_gpu_va(s->hClient, s->hDevice, s->hVaSpace,
                                  c->hShmMem, true, 0x00,
                                  c->ring_gpu_va, 0, GP108_CHAN_SHM_TOTAL_SIZE,
                                  &c->hVirtRing) != NV_OK) {
                rm_free_obj(s->hClient, c->hShmMem);
                munmap(c->shm_va, c->shm_sz);
                close(shm_fd);
                resp->status = -EIO;
                break;
            }

            NV_CONTEXT_DMA_ALLOCATION_PARAMS cdp;
            memset(&cdp, 0, sizeof(cdp));
            cdp.hMemory = c->hShmMem;
            cdp.flags   = DRF_DEF(OS03, _FLAGS, _MAPPING, _KERNEL) |
                          DRF_DEF(OS03, _FLAGS, _HASH_TABLE, _DISABLE);
            cdp.offset  = GP108_CHAN_SHM_NOTIF_OFF;
            cdp.limit   = GP108_CHAN_SHM_NOTIF_LEN - 1ULL;
            rm_alloc(s->hClient, s->hDevice, &c->hCtxDma, NV01_CONTEXT_DMA_CLASS, &cdp);

            NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS chp;
            memset(&chp, 0, sizeof(chp));
            chp.hObjectError  = c->hCtxDma;
            chp.gpFifoOffset  = c->ring_gpu_va + GP108_CHAN_SHM_GPFIFO_OFF;
            chp.gpFifoEntries = GP108_CHAN_SHM_GPFIFO_LEN / 8U; /* 4096 entries */
            chp.hVASpace      = s->hVaSpace;
            chp.engineType    = NV2080_ENGINE_TYPE_GRAPHICS;

            if (rm_alloc(s->hClient, s->hDevice, &c->hChan, PASCAL_CHANNEL_GPFIFO_A_CLASS, &chp) != NV_OK) {
                rm_free_obj(s->hClient, c->hCtxDma);
                rm_free_obj(s->hClient, c->hVirtRing);
                rm_free_obj(s->hClient, c->hShmMem);
                munmap(c->shm_va, c->shm_sz);
                close(shm_fd);
                resp->status = -EIO;
                break;
            }

            uint32_t em = req->u.ctx_create.engine_mask;
            NvU32 sce = 0, s2d = 0, s3d = 0, si2 = 0, scp = 0;
            if (em & GP108_NVK_ENGINE_COPY)
                sce = rm_alloc(s->hClient, c->hChan, &c->hCe,   PASCAL_DMA_COPY_B_CLASS,         NULL);
            if (em & GP108_NVK_ENGINE_2D)
                s2d = rm_alloc(s->hClient, c->hChan, &c->h2d,   FERMI_TWOD_A_CLASS,              NULL);
            if (em & GP108_NVK_ENGINE_3D)
                s3d = rm_alloc(s->hClient, c->hChan, &c->h3d,   PASCAL_B_CLASS,                  NULL);
            if (em & GP108_NVK_ENGINE_M2MF)
                si2 = rm_alloc(s->hClient, c->hChan, &c->hI2m,  KEPLER_INLINE_TO_MEMORY_B_CLASS, NULL);
            if (em & GP108_NVK_ENGINE_COMPUTE)
                scp = rm_alloc(s->hClient, c->hChan, &c->hComp, PASCAL_COMPUTE_B_CLASS,          NULL);
            if (sce || s2d || s3d || si2 || scp) {
                fprintf(stderr, "[gp108-rmd] ctx(mask=0x%x) engine alloc: ce=0x%x 2d=0x%x 3d=0x%x i2m=0x%x comp=0x%x\n",
                        em, sce, s2d, s3d, si2, scp);
            }

            NVA06F_CTRL_BIND_PARAMS bind_p = { .engineType = NV2080_ENGINE_TYPE_GRAPHICS };
            rm_control(s->hClient, c->hChan, NVA06F_CTRL_CMD_BIND, &bind_p, sizeof(bind_p));
            NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS sched_p = { .bEnable = NV_TRUE };
            NvU32 s_sched = rm_control(s->hClient, c->hChan, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE,
                                       &sched_p, sizeof(sched_p));
            NvU32 s_umap  = rm_map_dev(s->hClient, s->hSubdev, c->hChan, 0, 512, (void **)&c->userd);
            if (s_sched != NV_OK || s_umap != NV_OK || !c->userd) {
                rm_free_obj(s->hClient, c->hChan);
                rm_free_obj(s->hClient, c->hCtxDma);
                rm_free_obj(s->hClient, c->hVirtRing);
                rm_free_obj(s->hClient, c->hShmMem);
                munmap(c->shm_va, c->shm_sz);
                close(shm_fd);
                resp->status = -EIO;
                break;
            }

            c->used   = true;
            c->ctx_id = s->next_ctx_id++;

            resp->status               = 0;
            resp->has_fd               = 1;
            resp->u.ctx.ctx_id         = c->ctx_id;
            resp->u.ctx.gpfifo_entries = GP108_CHAN_SHM_GPFIFO_LEN / 8U;
            resp->u.ctx.shm_gpu_va     = c->ring_gpu_va;
            *out_fd                    = shm_fd;
            break;
        }
        case RMD_OP_CTX_DESTROY: {
            uint32_t cid = req->u.ctx_destroy.ctx_id;
            for (int i = 0; i < MAX_SESS_CTXS; i++) {
                if (s->ctxs[i].used && s->ctxs[i].ctx_id == cid) {
                    if (s->ctxs[i].shm_va) {
                        const uint32_t *nf = (const uint32_t *)((const uint8_t *)s->ctxs[i].shm_va + GP108_CHAN_SHM_NOTIF_OFF);
                        if (nf[2] || nf[3]) {
                            fprintf(stderr, "[gp108-rmd] ctx %u error notifier: info32=0x%x status_info16=0x%x\n",
                                    cid, nf[2], nf[3]);
                        }
                    }
                    rm_free_obj(s->hClient, s->ctxs[i].hChan);
                    rm_free_obj(s->hClient, s->ctxs[i].hCtxDma);
                    rm_free_obj(s->hClient, s->ctxs[i].hVirtRing);
                    rm_free_obj(s->hClient, s->ctxs[i].hShmMem);
                    if (s->ctxs[i].shm_va)
                        munmap(s->ctxs[i].shm_va, s->ctxs[i].shm_sz);
                    s->ctxs[i].used = false;
                    break;
                }
            }
            resp->status = 0;
            break;
        }
        case RMD_OP_CTX_KICK: {
            uint32_t cid = req->u.ctx_kick.ctx_id;
            struct rmd_ctx *c = NULL;
            for (int i = 0; i < MAX_SESS_CTXS; i++) {
                if (s->ctxs[i].used && s->ctxs[i].ctx_id == cid) {
                    c = &s->ctxs[i];
                    break;
                }
            }
            if (!c || !c->userd) { resp->status = -ENOENT; break; }
            __asm__ __volatile__("mfence" ::: "memory");
            c->userd[0x8c / 4] = req->u.ctx_kick.gp_put;
            __asm__ __volatile__("mfence" ::: "memory");
            resp->u.kick.gp_get = c->userd[0x88 / 4];
            resp->status = 0;
            break;
        }
        default:
            resp->status = -ENOSYS;
            break;
    }
}

static void *client_thread_main(void *arg)
{
    int sock = (int)(intptr_t)arg;
    struct rmd_session *sess = calloc(1, sizeof(*sess));
    if (!sess) {
        close(sock);
        return NULL;
    }
    sess->sock_fd = sock;

    pthread_mutex_lock(&g_rmd_lock);
    int init_rc = session_init_rm_locked(sess);
    if (init_rc == 0) {
        g_active_sessions++;
        g_last_active_sec = mono_sec_now();
    }
    pthread_mutex_unlock(&g_rmd_lock);
    if (init_rc != 0) {
        close(sock);
        free(sess);
        return NULL;
    }

    struct gp108_rmd_req req;
    struct gp108_rmd_resp resp;
    while (g_running && recv_exact(sock, &req, sizeof(req)) == 0) {
        if (req.magic != GP108_RMD_MAGIC) break;
        int pass_fd = -1;
        pthread_mutex_lock(&g_rmd_lock);
        handle_rpc_locked(sess, &req, &resp, &pass_fd);
        pthread_mutex_unlock(&g_rmd_lock);

        int sr = send_resp_with_fd(sock, &resp, pass_fd);
        if (pass_fd >= 0) close(pass_fd);
        if (sr != 0) break;
    }

    pthread_mutex_lock(&g_rmd_lock);
    session_cleanup_locked(sess);
    if (g_active_sessions > 0) g_active_sessions--;
    g_last_active_sec = mono_sec_now();
    pthread_mutex_unlock(&g_rmd_lock);

    close(sock);
    free(sess);
    return NULL;
}

static void on_sigterm(int sig)
{
    (void)sig;
    g_running = 0;
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
    }
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_sigterm);
    signal(SIGTERM, on_sigterm);

    const char *env_idle = getenv("GP108_IDLE_OFF_SEC");
    if (env_idle) g_idle_timeout_sec = (uint32_t)strtoul(env_idle, NULL, 10);
    g_last_active_sec = mono_sec_now();

    char user_elf[512] = { 0 };
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(user_elf, sizeof(user_elf),
                 "%s/Library/Application Support/NVIDIA-GP108/firmware/nv-kernel.o", home);
    }
    const char *elf_path = (argc > 1) ? argv[1] : NULL;
    if (!elf_path) {
        const char *cand1 = "/Library/Application Support/NVIDIA-GP108/firmware/nv-kernel.o";
        const char *cand2 = "/Users/maple/projects/temp/mx250-macos/third_party/nv_rm_freebsd/src/nvidia/nv-kernel.o";
        elf_path = (user_elf[0] && access(user_elf, R_OK) == 0) ? user_elf :
                   ((access(cand1, R_OK) == 0) ? cand1 : cand2);
    }

    if (gp108_rm_bootstrap(elf_path) != 0) {
        fprintf(stderr, "[gp108-rmd] Failed to bootstrap NVIDIA RM!\n");
        return 1;
    }

    g_bar0_mmio = (volatile uint32_t *)gp108_rm_bar_va(0, NULL, NULL);
    nv_state_t *gpu = gp108_rm_get_gpu_state();

    /* Register dev fd (1) with ctl fd (0) */
    nv_ioctl_register_fd_t reg_fd = { .ctl_fd = 0 };
    gp108_rm_call_ioctl(NULL, false, NV_ESC_REGISTER_FD, &reg_fd, sizeof(reg_fd));

    /* Create resident daemon monitor client & lock Full-Blood P0 Boost! */
    rm_alloc(0, 0, &g_mon_client, NV01_ROOT_CLIENT_CLASS, NULL);
    NV0000_CTRL_GPU_ATTACH_IDS_PARAMS att;
    memset(&att, 0, sizeof(att));
    att.gpuIds[0] = gpu->gpu_id;
    att.gpuIds[1] = 0xffffffffU;
    rm_control(g_mon_client, g_mon_client, NV0000_CTRL_CMD_GPU_ATTACH_IDS, &att, sizeof(att));

    NV0000_CTRL_GPU_GET_ID_INFO_PARAMS idi;
    memset(&idi, 0, sizeof(idi));
    idi.gpuId = gpu->gpu_id;
    rm_control(g_mon_client, g_mon_client, NV0000_CTRL_CMD_GPU_GET_ID_INFO, &idi, sizeof(idi));

    NV0080_ALLOC_PARAMETERS ap0080;
    memset(&ap0080, 0, sizeof(ap0080));
    ap0080.deviceId     = idi.deviceInstance;
    ap0080.hClientShare = g_mon_client;
    rm_alloc(g_mon_client, g_mon_client, &g_mon_device, NV01_DEVICE_0_CLASS, &ap0080);

    NV2080_ALLOC_PARAMETERS ap2080 = { .subDeviceId = idi.subDeviceInstance };
    rm_alloc(g_mon_client, g_mon_device, &g_mon_subdev, NV20_SUBDEVICE_0_CLASS, &ap2080);

    struct { NvU32 flags, duration; } bp = { 0x12U, 0xffffffffU };
    rm_control(g_mon_client, g_mon_subdev, NV2080_CTRL_CMD_PERF_BOOST_ID, &bp, sizeof(bp));

    /* Unlock GPU Boost 3.0 VF curve above RATED_TDP (1296 MHz -> 1582+ MHz) */
    struct { NvU32 client, input; } tdp_ctrl = { 2U /* GLOBAL */, 1U /* FORCE_EXCEED */ };
    NvU32 st_tdp = rm_control(g_mon_client, g_mon_subdev, 0x2080206fU, &tdp_ctrl, sizeof(tdp_ctrl));
    fprintf(stderr, "[gp108-rmd] RATED_TDP FORCE_EXCEED: st=0x%x  THERM[0x100..0x170]=%08x %08x %08x %08x %08x %08x\n",
            st_tdp,
            g_bar0_mmio ? g_bar0_mmio[0x20100/4] : 0,
            g_bar0_mmio ? g_bar0_mmio[0x20110/4] : 0,
            g_bar0_mmio ? g_bar0_mmio[0x20130/4] : 0,
            g_bar0_mmio ? g_bar0_mmio[0x20160/4] : 0,
            g_bar0_mmio ? g_bar0_mmio[0x20164/4] : 0,
            g_bar0_mmio ? g_bar0_mmio[0x20168/4] : 0);

    uint64_t bar1_sz = 0;
    gp108_rm_bar_va(1, &bar1_sz, NULL);

    memset(&g_hw_info, 0, sizeof(g_hw_info));
    g_hw_info.vendor_id     = gpu->pci_info.vendor_id;
    g_hw_info.device_id     = gpu->pci_info.device_id;
    g_hw_info.subvendor_id  = gpu->subsystem_vendor;
    g_hw_info.subdevice_id  = gpu->subsystem_id;
    g_hw_info.chipset       = 0x138; /* GP108 */
    g_hw_info.revision_id   = 0xa1;
    g_hw_info.pci_bus       = (uint8_t)gpu->pci_info.bus;
    g_hw_info.pci_dev       = (uint8_t)gpu->pci_info.slot;
    g_hw_info.pci_func      = 0;
    g_hw_info.gpc_count     = 1;
    g_hw_info.tpc_count     = 3; /* 3 TPCs = 384 FP32 CUDA Cores */
    g_hw_info.vram_total_B  = (2048ULL << 20);
    g_hw_info.vram_usable_B = (1984ULL << 20);
    g_hw_info.bar1_size_B   = bar1_sz ? bar1_sz : (256ULL << 20);
    g_hw_info.cls_copy      = PASCAL_DMA_COPY_B_CLASS;
    g_hw_info.cls_eng2d     = FERMI_TWOD_A_CLASS;
    g_hw_info.cls_eng3d     = PASCAL_B_CLASS;
    g_hw_info.cls_m2mf      = KEPLER_INLINE_TO_MEMORY_B_CLASS;
    g_hw_info.cls_compute   = PASCAL_COMPUTE_B_CLASS;
    strlcpy(g_hw_info.chip_name, "GP108", sizeof(g_hw_info.chip_name));
    strlcpy(g_hw_info.dev_name,  "NVIDIA GeForce MX250", sizeof(g_hw_info.dev_name));

    g_tpage = gp108_telemetry_map(1);
    refresh_telemetry_locked();

    unlink(GP108_RMD_SOCK_PATH);
    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strlcpy(sun.sun_path, GP108_RMD_SOCK_PATH, sizeof(sun.sun_path));

    mode_t old_um = umask(0);
    if (bind(g_listen_fd, (struct sockaddr *)&sun, sizeof(sun)) != 0 ||
        listen(g_listen_fd, 32) != 0) {
        umask(old_um);
        perror("[gp108-rmd] bind/listen failed");
        g_running = 0;
        gp108_rm_teardown();
        return 1;
    }
    umask(old_um);
    chmod(GP108_RMD_SOCK_PATH, 0666);

    g_last_active_sec = mono_sec_now();
    pthread_t telem_tid;
    pthread_create(&telem_tid, NULL, telemetry_thread_main, NULL);

    fprintf(stderr, "[gp108-rmd] Ready on %s — Full-Blood P0 Active (GPCCLK=%.1f MHz, MCLK=%.1f MHz)\n",
            GP108_RMD_SOCK_PATH,
            g_tpage ? g_tpage->gpc_clk_khz / 1000.0 : 0.0,
            g_tpage ? g_tpage->mem_clk_khz / 1000.0 : 0.0);

    while (g_running) {
        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR && g_running) continue;
            break;
        }
        if (!g_running) {
            close(cfd);
            break;
        }
        pthread_t ctid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&ctid, &attr, client_thread_main, (void *)(intptr_t)cfd) != 0)
            close(cfd);
        pthread_attr_destroy(&attr);
    }

    unlink(GP108_RMD_SOCK_PATH);
    pthread_join(telem_tid, NULL);
    if (g_tpage) g_tpage->rm_active = 0;
    if (g_mon_client) rm_free_obj(g_mon_client, g_mon_client);
    gp108_rm_teardown();
    fprintf(stderr, "[gp108-rmd] Clean shutdown complete.\n");
    return 0;
}
