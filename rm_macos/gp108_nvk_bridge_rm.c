/* SPDX-License-Identifier: MIT
 * Clean Standard-C Bridge from Mesa NVK (nvkmd_macos) -> gp108-rmd (Official NVIDIA RM)
 *
 * Implements the exact 18-function gp108_nvk_bridge.h ABI over POSIX shared memory
 * and gp108-rmd, giving every Vulkan & Metal process Full-Blood P0 Clocks (1582 MHz
 * Core + 7008 Mbps GDDR5) plus zero-copy macOS IOSurface DMA with SIP/AMFI enabled.
 */
#include "../nvkm_macos/gp108_nvk_bridge.h"
#include "gp108_rmd_ipc.h"

#include <IOSurface/IOSurface.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

extern void gp108_telem_on_retain(void);
extern void gp108_telem_on_release(void);
extern void gp108_telem_on_bo_alloc(uint64_t size_B, bool is_vram);
extern void gp108_telem_on_bo_free(uint64_t size_B, bool is_vram);
extern void gp108_telem_on_kick(uint32_t engine_flags);
extern void gp108_telem_on_gpu_busy(uint64_t busy_ns);

struct gp108_nvk_bo {
    uint32_t     bo_id;
    uint64_t     size_B;
    uint8_t      pte_kind;
    bool         is_vram;
    bool         mappable;
    void        *cpu_ptr;
    size_t       mmap_size;
    IOSurfaceRef iosurf;
};

struct gp108_nvk_ctx {
    pthread_mutex_t    submit_lock;
    uint32_t           ctx_id;
    uint32_t           engine_mask;

    void              *shm_base;
    size_t             shm_size;
    uint64_t           shm_gpu_va;

    uint32_t          *push_cpu;      /* 64 KiB = 16384 dwords */
    uint64_t           push_gpu;
    uint32_t           push_dwords;
    uint32_t           push_cur;

    volatile uint64_t *gpfifo_cpu;    /* 4096 x 64-bit IB entries */
    uint32_t           gpfifo_entries;
    uint32_t           gp_put;
    uint32_t           gp_get_cached;

    volatile uint32_t *ring_progress_cpu;
    uint64_t           ring_progress_gpu;
    volatile uint32_t *fence_lo_cpu;
    volatile uint32_t *fence_hi_cpu;
    uint64_t           fence_lo_gpu;
    uint64_t           fence_hi_gpu;

    uint64_t           next_seq;
    volatile uint64_t  kick_ns_ring[64];
};

struct client_vma_rec {
    bool     used;
    uint64_t addr;
    uint64_t size_B;
};

#define MAX_CLIENT_VMAS 8192

static pthread_mutex_t          g_rpc_lock   = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t          g_vma_lock   = PTHREAD_MUTEX_INITIALIZER;
static int                      g_sock       = -1;
static uint32_t                 g_refcnt     = 0;
static struct gp108_nvk_hw_info g_hw_info;
static volatile uint64_t        g_vram_used  = (16ULL << 20);
static struct client_vma_rec    g_vmas[MAX_CLIENT_VMAS];

static uint64_t mono_ns_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int connect_rmd_once(void)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strlcpy(sun.sun_path, GP108_RMD_SOCK_PATH, sizeof(sun.sun_path));
    if (connect(s, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
        close(s);
        return -1;
    }
    return s;
}

static int ensure_rmd_connected_locked(void)
{
    if (g_sock >= 0) return 0;

    g_sock = connect_rmd_once();
    if (g_sock >= 0) return 0;

    /* Auto-spawn gp108-rmd in background on first Vulkan/Metal client startup */
    char user_cand[512] = { 0 };
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(user_cand, sizeof(user_cand),
                 "%s/Library/Application Support/NVIDIA-GP108/bin/gp108-rmd", home);
    }
    const char *cand1 = "/Library/Application Support/NVIDIA-GP108/bin/gp108-rmd";
    const char *cand2 = "/Users/maple/projects/temp/mx250-macos/build/gp108-rmd";
    const char *bin   = (user_cand[0] && access(user_cand, X_OK) == 0) ? user_cand :
                        ((access(cand1, X_OK) == 0) ? cand1 :
                        ((access(cand2, X_OK) == 0) ? cand2 : NULL));
    if (!bin) return -ENOENT;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO,  "/dev/null",          O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/tmp/gp108_rmd.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/tmp/gp108_rmd.log", O_WRONLY | O_CREAT | O_APPEND, 0666);

    char *argv[] = { (char *)bin, NULL };
    pid_t pid = 0;
    int sr = posix_spawn(&pid, bin, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (sr != 0) return -sr;

    /* Wait up to 10.0 seconds for rm_init_adapter() (incl. ACPI self-heal) to listen */
    for (int i = 0; i < 500; i++) {
        usleep(20000);
        g_sock = connect_rmd_once();
        if (g_sock >= 0) return 0;
    }
    return -ETIMEDOUT;
}

static int rmd_rpc(const struct gp108_rmd_req *req, struct gp108_rmd_resp *resp, int *out_fd)
{
    if (out_fd) *out_fd = -1;
    pthread_mutex_lock(&g_rpc_lock);
    if (g_sock < 0) {
        pthread_mutex_unlock(&g_rpc_lock);
        return -ENOTCONN;
    }

    const uint8_t *wp = (const uint8_t *)req;
    size_t wrem = sizeof(*req);
    while (wrem > 0) {
        ssize_t n = send(g_sock, wp, wrem, 0);
        if (n <= 0) {
            close(g_sock);
            g_sock = -1;
            pthread_mutex_unlock(&g_rpc_lock);
            return -EIO;
        }
        wp   += (size_t)n;
        wrem -= (size_t)n;
    }

    struct iovec iov = { .iov_base = resp, .iov_len = sizeof(*resp) };
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    ssize_t nr = recvmsg(g_sock, &msg, 0);
    if (nr <= 0) {
        close(g_sock);
        g_sock = -1;
        pthread_mutex_unlock(&g_rpc_lock);
        return -EIO;
    }
    size_t got = (size_t)nr;
    while (got < sizeof(*resp)) {
        ssize_t m = recv(g_sock, (uint8_t *)resp + got, sizeof(*resp) - got, 0);
        if (m <= 0) {
            close(g_sock);
            g_sock = -1;
            pthread_mutex_unlock(&g_rpc_lock);
            return -EIO;
        }
        got += (size_t)m;
    }

    int rfd = -1;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm != NULL; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            memcpy(&rfd, CMSG_DATA(cm), sizeof(int));
            break;
        }
    }
    pthread_mutex_unlock(&g_rpc_lock);

    if (out_fd) {
        *out_fd = rfd;
    } else if (rfd >= 0) {
        close(rfd);
    }
    return resp->status;
}

/* 1. Singleton Hardware Lifecycle & Queries */
int gp108_nvk_retain(struct gp108_nvk_hw_info *out_info)
{
    pthread_mutex_lock(&g_rpc_lock);
    if (g_refcnt > 0 && g_sock >= 0) {
        g_refcnt++;
        if (out_info) *out_info = g_hw_info;
        pthread_mutex_unlock(&g_rpc_lock);
        return 0;
    }

    int rc = ensure_rmd_connected_locked();
    pthread_mutex_unlock(&g_rpc_lock);
    if (rc != 0) return rc;

    struct gp108_rmd_req req;
    struct gp108_rmd_resp resp;
    memset(&req, 0, sizeof(req));
    req.magic              = GP108_RMD_MAGIC;
    req.op                 = RMD_OP_HELLO;
    req.u.hello.client_pid = (uint32_t)getpid();

    rc = rmd_rpc(&req, &resp, NULL);
    if (rc != 0) return rc;

    pthread_mutex_lock(&g_rpc_lock);
    g_hw_info   = resp.u.hello.hw;
    g_vram_used = (16ULL << 20);
    g_refcnt    = 1;
    pthread_mutex_unlock(&g_rpc_lock);

    gp108_telem_on_retain();
    if (out_info) *out_info = g_hw_info;
    return 0;
}

void gp108_nvk_release(void)
{
    pthread_mutex_lock(&g_rpc_lock);
    if (g_refcnt == 0) {
        pthread_mutex_unlock(&g_rpc_lock);
        return;
    }
    if (--g_refcnt == 0) {
        if (g_sock >= 0) {
            close(g_sock);
            g_sock = -1;
        }
        pthread_mutex_lock(&g_vma_lock);
        memset(g_vmas, 0, sizeof(g_vmas));
        pthread_mutex_unlock(&g_vma_lock);
        pthread_mutex_unlock(&g_rpc_lock);
        gp108_telem_on_release();
        return;
    }
    pthread_mutex_unlock(&g_rpc_lock);
}

uint64_t gp108_nvk_get_timestamp(void)
{
    if (!g_refcnt) return 0;
    struct gp108_rmd_req req;
    struct gp108_rmd_resp resp;
    memset(&req, 0, sizeof(req));
    req.magic = GP108_RMD_MAGIC;
    req.op    = RMD_OP_GET_TIMESTAMP;
    if (rmd_rpc(&req, &resp, NULL) != 0) return 0;
    return resp.u.ts.timestamp_ns;
}

uint64_t gp108_nvk_get_vram_used(void)
{
    return __sync_fetch_and_add(&g_vram_used, 0ULL);
}

/* 2. Memory Object (BO) Allocation, Zero-Copy IOSurface Import & Mapping */
int gp108_nvk_bo_alloc(uint64_t size_B, uint8_t pte_kind,
                       bool in_vram, bool mappable,
                       struct gp108_nvk_bo **out_bo)
{
    if (!g_refcnt || !out_bo || size_B == 0) return -EINVAL;

    struct gp108_rmd_req req;
    struct gp108_rmd_resp resp;
    memset(&req, 0, sizeof(req));
    req.magic               = GP108_RMD_MAGIC;
    req.op                  = RMD_OP_BO_ALLOC;
    req.u.bo_alloc.size_B   = size_B;
    req.u.bo_alloc.pte_kind = pte_kind;
    req.u.bo_alloc.in_vram  = in_vram ? 1U : 0U;
    req.u.bo_alloc.mappable = mappable ? 1U : 0U;

    int shm_fd = -1;
    int rc = rmd_rpc(&req, &resp, &shm_fd);
    if (rc != 0) {
        if (shm_fd >= 0) close(shm_fd);
        return rc;
    }

    struct gp108_nvk_bo *bo = calloc(1, sizeof(*bo));
    if (!bo) {
        if (shm_fd >= 0) close(shm_fd);
        return -ENOMEM;
    }
    bo->bo_id    = resp.u.bo.bo_id;
    bo->size_B   = resp.u.bo.actual_size_B;
    bo->pte_kind = pte_kind;
    bo->is_vram  = (resp.u.bo.is_vram != 0);
    bo->mappable = mappable || !bo->is_vram;

    if (shm_fd >= 0) {
        void *p = mmap(NULL, (size_t)bo->size_B, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        close(shm_fd);
        if (p == MAP_FAILED) {
            struct gp108_rmd_req freq = { .magic = GP108_RMD_MAGIC, .op = RMD_OP_BO_FREE };
            freq.u.bo_free.bo_id = bo->bo_id;
            rmd_rpc(&freq, &resp, NULL);
            free(bo);
            return -ENOMEM;
        }
        bo->cpu_ptr   = p;
        bo->mmap_size = (size_t)bo->size_B;
    }

    if (bo->is_vram)
        __sync_add_and_fetch(&g_vram_used, bo->size_B);
    gp108_telem_on_bo_alloc(bo->size_B, bo->is_vram);

    *out_bo = bo;
    return 0;
}

int gp108_nvk_bo_import_iosurface(uint32_t iosurface_id, uint64_t size_B,
                                  struct gp108_nvk_bo **out_bo)
{
    if (!g_refcnt || !out_bo || iosurface_id == 0) return -EINVAL;

    IOSurfaceRef surf = IOSurfaceLookup((IOSurfaceID)iosurface_id);
    if (!surf) return -ENOENT;

    struct gp108_rmd_req req;
    struct gp108_rmd_resp resp;
    memset(&req, 0, sizeof(req));
    req.magic                    = GP108_RMD_MAGIC;
    req.op                       = RMD_OP_BO_IMPORT_IOSURFACE;
    req.u.bo_iosurf.iosurface_id = iosurface_id;
    req.u.bo_iosurf.size_B       = size_B;

    int rc = rmd_rpc(&req, &resp, NULL);
    if (rc != 0) {
        CFRelease(surf);
        return rc;
    }

    struct gp108_nvk_bo *bo = calloc(1, sizeof(*bo));
    if (!bo) {
        CFRelease(surf);
        return -ENOMEM;
    }
    bo->bo_id    = resp.u.bo.bo_id;
    bo->size_B   = resp.u.bo.actual_size_B;
    bo->pte_kind = 0;
    bo->is_vram  = false;
    bo->mappable = true;
    bo->cpu_ptr  = IOSurfaceGetBaseAddress(surf);
    bo->iosurf   = surf;

    gp108_telem_on_bo_alloc(bo->size_B, false);
    *out_bo = bo;
    return 0;
}

void gp108_nvk_bo_free(struct gp108_nvk_bo *bo)
{
    if (!bo) return;

    struct gp108_rmd_req req;
    struct gp108_rmd_resp resp;
    memset(&req, 0, sizeof(req));
    req.magic            = GP108_RMD_MAGIC;
    req.op               = RMD_OP_BO_FREE;
    req.u.bo_free.bo_id  = bo->bo_id;
    rmd_rpc(&req, &resp, NULL);

    if (bo->mmap_size > 0 && bo->cpu_ptr)
        munmap(bo->cpu_ptr, bo->mmap_size);
    if (bo->iosurf)
        CFRelease(bo->iosurf);

    if (bo->is_vram)
        __sync_sub_and_fetch(&g_vram_used, bo->size_B);
    gp108_telem_on_bo_free(bo->size_B, bo->is_vram);
    free(bo);
}

void *gp108_nvk_bo_map(struct gp108_nvk_bo *bo)
{
    return bo ? bo->cpu_ptr : NULL;
}

uint32_t gp108_nvk_bo_handle_id(struct gp108_nvk_bo *bo)
{
    return bo ? bo->bo_id : 0;
}

uint64_t gp108_nvk_bo_size(struct gp108_nvk_bo *bo)
{
    return bo ? bo->size_B : 0;
}

/* 3. GPU Virtual Address Space (Backed by RM FERMI_VASPACE_A) */
int gp108_nvk_vm_sparse(uint64_t addr, uint64_t size_B, bool enable)
{
    (void)addr; (void)size_B; (void)enable;
    return 0;
}

int gp108_nvk_vm_get_ptes(uint64_t addr, uint64_t size_B)
{
    (void)addr; (void)size_B;
    return 0;
}

int gp108_nvk_vm_put_ptes(uint64_t addr, uint64_t size_B)
{
    (void)addr; (void)size_B;
    return 0;
}

int gp108_nvk_vm_map(uint64_t addr, uint64_t range_B, uint8_t pte_kind,
                     struct gp108_nvk_bo *bo, uint64_t bo_offset_B)
{
    if (!bo || range_B == 0) return -EINVAL;

    struct gp108_rmd_req req;
    struct gp108_rmd_resp resp;
    memset(&req, 0, sizeof(req));
    req.magic                = GP108_RMD_MAGIC;
    req.op                   = RMD_OP_VM_MAP;
    req.u.vm_map.addr        = addr;
    req.u.vm_map.range_B     = range_B;
    req.u.vm_map.bo_offset_B = bo_offset_B;
    req.u.vm_map.bo_id       = bo->bo_id;
    req.u.vm_map.pte_kind    = pte_kind;

    int rc = rmd_rpc(&req, &resp, NULL);
    if (rc == 0) {
        pthread_mutex_lock(&g_vma_lock);
        uint64_t u_end = addr + range_B;
        int free_idx = -1;
        for (int i = 0; i < MAX_CLIENT_VMAS; i++) {
            if (g_vmas[i].used) {
                uint64_t s_start = g_vmas[i].addr;
                uint64_t s_end   = s_start + g_vmas[i].size_B;
                if (s_start < u_end && s_end > addr)
                    g_vmas[i].used = false;
            } else if (free_idx < 0) {
                free_idx = i;
            }
        }
        if (free_idx >= 0) {
            g_vmas[free_idx].used   = true;
            g_vmas[free_idx].addr   = addr;
            g_vmas[free_idx].size_B = range_B;
        }
        pthread_mutex_unlock(&g_vma_lock);
    }
    return rc;
}

int gp108_nvk_vm_unmap(uint64_t addr, uint64_t range_B, bool is_sparse)
{
    (void)is_sparse;
    if (range_B == 0) return 0;

    bool any_overlap = false;
    uint64_t u_end = addr + range_B;
    pthread_mutex_lock(&g_vma_lock);
    for (int i = 0; i < MAX_CLIENT_VMAS; i++) {
        if (g_vmas[i].used) {
            uint64_t s_start = g_vmas[i].addr;
            uint64_t s_end   = s_start + g_vmas[i].size_B;
            if (s_start < u_end && s_end > addr) {
                g_vmas[i].used = false;
                any_overlap = true;
            }
        }
    }
    pthread_mutex_unlock(&g_vma_lock);

    if (!any_overlap)
        return 0; /* Fast 0-ns path when nvkmd_macos pre-unmaps fresh VA */

    struct gp108_rmd_req req;
    struct gp108_rmd_resp resp;
    memset(&req, 0, sizeof(req));
    req.magic              = GP108_RMD_MAGIC;
    req.op                 = RMD_OP_VM_UNMAP;
    req.u.vm_unmap.addr    = addr;
    req.u.vm_unmap.range_B = range_B;
    return rmd_rpc(&req, &resp, NULL);
}

/* 4. Hardware GPFIFO Execution Context & Zero-IPC Fence Completion */
#define NVPUSH_INC(subc, mthd, cnt) \
    ((1U << 29) | ((uint32_t)(cnt) << 16) | ((uint32_t)(subc) << 13) | ((uint32_t)(mthd) >> 2))

/* NVC06F_SEMAPHORED: OPERATION_RELEASE(2) | RELEASE_SIZE_16BYTE(0<<24) | WFI */
#define SEMA_REL_WFI_EN  0x00000002U
#define SEMA_REL_WFI_DIS 0x00100002U

int gp108_nvk_ctx_create(uint32_t engine_mask, struct gp108_nvk_ctx **out_ctx)
{
    if (!g_refcnt || !out_ctx) return -EINVAL;

    struct gp108_rmd_req req;
    struct gp108_rmd_resp resp;
    memset(&req, 0, sizeof(req));
    req.magic                    = GP108_RMD_MAGIC;
    req.op                       = RMD_OP_CTX_CREATE;
    req.u.ctx_create.engine_mask = engine_mask;

    int shm_fd = -1;
    int rc = rmd_rpc(&req, &resp, &shm_fd);
    if (rc != 0 || shm_fd < 0) {
        if (shm_fd >= 0) close(shm_fd);
        return rc != 0 ? rc : -EIO;
    }

    void *shm = mmap(NULL, GP108_CHAN_SHM_TOTAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (shm == MAP_FAILED) return -ENOMEM;

    struct gp108_nvk_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        munmap(shm, GP108_CHAN_SHM_TOTAL_SIZE);
        return -ENOMEM;
    }
    pthread_mutex_init(&ctx->submit_lock, NULL);
    ctx->ctx_id         = resp.u.ctx.ctx_id;
    ctx->engine_mask    = engine_mask;
    ctx->shm_base       = shm;
    ctx->shm_size       = GP108_CHAN_SHM_TOTAL_SIZE;
    ctx->shm_gpu_va     = resp.u.ctx.shm_gpu_va;

    uint8_t *base       = (uint8_t *)shm;
    ctx->push_cpu       = (uint32_t *)(base + GP108_CHAN_SHM_PUSH_OFF);
    ctx->push_gpu       = ctx->shm_gpu_va + GP108_CHAN_SHM_PUSH_OFF;
    ctx->push_dwords    = GP108_CHAN_SHM_PUSH_LEN / 4U; /* 16384 dwords */
    ctx->push_cur       = 0;

    ctx->gpfifo_cpu     = (volatile uint64_t *)(base + GP108_CHAN_SHM_GPFIFO_OFF);
    ctx->gpfifo_entries = resp.u.ctx.gpfifo_entries;    /* 4096 entries */
    ctx->gp_put         = 0;
    ctx->gp_get_cached  = 0;

    uint8_t *sema_page  = base + GP108_CHAN_SHM_SEMA_OFF;
    uint64_t sema_gpu   = ctx->shm_gpu_va + GP108_CHAN_SHM_SEMA_OFF;
    ctx->ring_progress_cpu = (volatile uint32_t *)(sema_page + 0x000);
    ctx->ring_progress_gpu = sema_gpu + 0x000ULL;
    ctx->fence_lo_cpu      = (volatile uint32_t *)(sema_page + 0x100);
    ctx->fence_hi_cpu      = (volatile uint32_t *)(sema_page + 0x110);
    ctx->fence_lo_gpu      = sema_gpu + 0x100ULL;
    ctx->fence_hi_gpu      = sema_gpu + 0x110ULL;

    *ctx->ring_progress_cpu = 0;
    *ctx->fence_lo_cpu      = 0;
    *ctx->fence_hi_cpu      = 0;
    ctx->next_seq           = 0;

    *out_ctx = ctx;
    return 0;
}

void gp108_nvk_ctx_destroy(struct gp108_nvk_ctx *ctx)
{
    if (!ctx) return;

    gp108_nvk_ctx_wait_fence(ctx, ctx->next_seq, 2000000000ULL);

    struct gp108_rmd_req req;
    struct gp108_rmd_resp resp;
    memset(&req, 0, sizeof(req));
    req.magic                = GP108_RMD_MAGIC;
    req.op                   = RMD_OP_CTX_DESTROY;
    req.u.ctx_destroy.ctx_id = ctx->ctx_id;
    rmd_rpc(&req, &resp, NULL);

    if (ctx->shm_base)
        munmap(ctx->shm_base, ctx->shm_size);
    pthread_mutex_destroy(&ctx->submit_lock);
    free(ctx);
}

uint64_t gp108_nvk_ctx_completed_fence(struct gp108_nvk_ctx *ctx)
{
    if (!ctx) return 0;
    uint32_t hi1, lo, hi2;
    do {
        hi1 = *ctx->fence_hi_cpu;
        __asm__ __volatile__("" ::: "memory");
        lo  = *ctx->fence_lo_cpu;
        __asm__ __volatile__("" ::: "memory");
        hi2 = *ctx->fence_hi_cpu;
    } while (hi1 != hi2);
    return ((uint64_t)hi1 << 32) | lo;
}

static inline void gpfifo_write_entry(struct gp108_nvk_ctx *ctx, bool is_main,
                                      uint64_t addr, uint32_t size_B, bool no_prefetch)
{
    (void)is_main;
    uint64_t w0 = addr & 0xfffffffcULL;
    uint64_t w1 = ((addr >> 32) & 0xffULL) |
                  ((uint64_t)(size_B >> 2) << 10) |
                  (no_prefetch ? (1ULL << 31) : 0ULL);
    ctx->gpfifo_cpu[ctx->gp_put] = w0 | (w1 << 32);
    ctx->gp_put = (ctx->gp_put + 1U) & (ctx->gpfifo_entries - 1U);
}

static int gpfifo_ensure_space_locked(struct gp108_nvk_ctx *ctx, uint32_t needed_entries)
{
    uint32_t mask = ctx->gpfifo_entries - 1U;
    for (int spin = 0; spin < 200000; spin++) {
        uint32_t used = (ctx->gp_put - ctx->gp_get_cached) & mask;
        if (ctx->gpfifo_entries - 1U - used >= needed_entries)
            return 0;

        /* Kick & refresh hardware GPGet from gp108-rmd */
        struct gp108_rmd_req req;
        struct gp108_rmd_resp resp;
        memset(&req, 0, sizeof(req));
        req.magic              = GP108_RMD_MAGIC;
        req.op                 = RMD_OP_CTX_KICK;
        req.u.ctx_kick.ctx_id  = ctx->ctx_id;
        req.u.ctx_kick.gp_put  = ctx->gp_put;
        if (rmd_rpc(&req, &resp, NULL) != 0)
            return -EIO;
        ctx->gp_get_cached = resp.u.kick.gp_get & mask;
        if (ctx->gpfifo_entries - 1U - ((ctx->gp_put - ctx->gp_get_cached) & mask) < needed_entries)
            usleep(20);
    }
    return -ETIMEDOUT;
}

int gp108_nvk_ctx_submit(struct gp108_nvk_ctx *ctx,
                         const struct gp108_nvk_push *pushes,
                         uint32_t push_count,
                         uint64_t *out_fence_seq)
{
    if (!ctx) return -EINVAL;

    pthread_mutex_lock(&ctx->submit_lock);
    if (push_count == 0 && out_fence_seq) {
        /* Even on 0 pushes (e.g. ctx_sync), emit a fence if requested */
    }

    uint32_t idx = 0;
    while (idx < push_count) {
        uint32_t batch = push_count - idx;
        if (batch > 1024U) batch = 1024U;
        int rc = gpfifo_ensure_space_locked(ctx, batch + 2U);
        if (rc != 0) {
            pthread_mutex_unlock(&ctx->submit_lock);
            return rc;
        }
        for (uint32_t i = 0; i < batch; i++) {
            const struct gp108_nvk_push *p = &pushes[idx + i];
            if (p->size_B > 0)
                gpfifo_write_entry(ctx, false, p->addr, p->size_B, p->no_prefetch);
        }
        idx += batch;
    }

    int rc = gpfifo_ensure_space_locked(ctx, 2U);
    if (rc != 0) {
        pthread_mutex_unlock(&ctx->submit_lock);
        return rc;
    }

    /* Emit 64-bit monotonic semaphore release into internal 64 KiB push ring.
     * Each slot is 16 dwords (64 bytes), giving 1024 in-flight fence slots before wrap!
     * Wait if more than 960 fences are simultaneously in flight (extremely rare).
     */
    uint64_t seq = ++ctx->next_seq;
    while (seq - gp108_nvk_ctx_completed_fence(ctx) > 960ULL) {
        usleep(20);
    }

    if (ctx->push_cur + 16U > ctx->push_dwords)
        ctx->push_cur = 0;

    uint32_t start_dw = ctx->push_cur;
    uint32_t *p = &ctx->push_cpu[start_dw];
    uint32_t n = 0;

    if ((uint32_t)(seq >> 32) != (uint32_t)((seq - 1ULL) >> 32)) {
        p[n++] = NVPUSH_INC(0, 0x0010, 4);
        p[n++] = (uint32_t)(ctx->fence_lo_gpu >> 32);
        p[n++] = (uint32_t)(ctx->fence_lo_gpu & 0xffffffffU);
        p[n++] = 0U;
        p[n++] = SEMA_REL_WFI_EN;

        p[n++] = NVPUSH_INC(0, 0x0010, 4);
        p[n++] = (uint32_t)(ctx->fence_hi_gpu >> 32);
        p[n++] = (uint32_t)(ctx->fence_hi_gpu & 0xffffffffU);
        p[n++] = (uint32_t)(seq >> 32);
        p[n++] = SEMA_REL_WFI_DIS;
    }

    p[n++] = NVPUSH_INC(0, 0x0010, 4);
    p[n++] = (uint32_t)(ctx->fence_lo_gpu >> 32);
    p[n++] = (uint32_t)(ctx->fence_lo_gpu & 0xffffffffU);
    p[n++] = (uint32_t)(seq & 0xffffffffU);
    p[n++] = SEMA_REL_WFI_EN;

    ctx->push_cur = start_dw + n;
    gpfifo_write_entry(ctx, true, ctx->push_gpu + (uint64_t)start_dw * 4ULL, n * 4U, false);

    __asm__ __volatile__("mfence" ::: "memory");
    ctx->kick_ns_ring[seq & 63U] = mono_ns_now();

    struct gp108_rmd_req kreq;
    struct gp108_rmd_resp kresp;
    memset(&kreq, 0, sizeof(kreq));
    kreq.magic             = GP108_RMD_MAGIC;
    kreq.op                = RMD_OP_CTX_KICK;
    kreq.u.ctx_kick.ctx_id = ctx->ctx_id;
    kreq.u.ctx_kick.gp_put = ctx->gp_put;
    rc = rmd_rpc(&kreq, &kresp, NULL);
    if (rc == 0)
        ctx->gp_get_cached = kresp.u.kick.gp_get & (ctx->gpfifo_entries - 1U);

    gp108_telem_on_kick((ctx->engine_mask & GP108_NVK_ENGINE_COMPUTE) ? 3U : 1U);
    if (out_fence_seq)
        *out_fence_seq = seq;

    pthread_mutex_unlock(&ctx->submit_lock);
    return rc;
}

static inline void account_completed_seq(struct gp108_nvk_ctx *ctx, uint64_t target_seq)
{
    uint64_t t0 = __sync_lock_test_and_set(&ctx->kick_ns_ring[target_seq & 63U], 0ULL);
    if (t0 > 0) {
        uint64_t now = mono_ns_now();
        if (now > t0)
            gp108_telem_on_gpu_busy(now - t0);
    }
}

int gp108_nvk_ctx_wait_fence(struct gp108_nvk_ctx *ctx,
                             uint64_t target_seq,
                             uint64_t timeout_ns)
{
    if (!ctx || target_seq == 0) return 0;
    if (gp108_nvk_ctx_completed_fence(ctx) >= target_seq) {
        account_completed_seq(ctx, target_seq);
        return 0;
    }

    uint64_t start = mono_ns_now();
    uint64_t spin_until = start + 250000ULL; /* 250 us fast zero-IPC spin */
    while (gp108_nvk_ctx_completed_fence(ctx) < target_seq) {
        uint64_t now = mono_ns_now();
        if (timeout_ns != UINT64_MAX && (now - start) >= timeout_ns)
            return -ETIMEDOUT;
        if (now < spin_until) {
            __asm__ __volatile__("pause");
        } else {
            usleep(100);
        }
    }
    account_completed_seq(ctx, target_seq);
    return 0;
}
