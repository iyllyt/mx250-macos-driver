/* SPDX-License-Identifier: MIT
 * macOS Userspace OS & UNIX Interface Layer for NVIDIA FreeBSD x86_64 RM (nv-kernel.o)
 * Connects nv-kernel.o directly to GP108Bridge.kext with zero kernel changes and SIP ON.
 */
#include "gp108_rm_host.h"
#include "../include/GP108BridgeShared.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IODataQueueClient.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

/* ========================================================================= *
 * Exported Globals Copied Into Low <2 GiB Arena for nv-kernel.o             *
 * ========================================================================= */
NvU32 os_page_size  = 4096U;
NvU64 os_page_mask  = ~0xfffULL;
NvU8  os_page_shift = 12U;

nv_cap_t *nvidia_caps_root = NULL;
const NvBool nv_is_rm_firmware_supported_os = NV_FALSE;

/* Minimal terminated registry table (we also push keys via rm_write_registry_dword) */
nv_parm_t nv_parms[] = {
    { NULL, NULL }
};

struct gp108_rm_exports g_rm;

/* ========================================================================= *
 * Hardware & OS Runtime State                                               *
 * ========================================================================= */
#define RM_STACK_ALLOC_BYTES   (64U * 1024U)
#define DMA_SLAB_PAGES         4096U /* 16 MiB per slab */
#define MAX_DMA_SLABS          64U

struct rm_dma_slab {
    bool      active;
    uint32_t  dma_slot;
    uint32_t  cache_mode;
    uint8_t  *base_va;
    size_t    size_bytes;
    uint32_t  total_pages;
    uint32_t  free_count;
    uint64_t *free_bmap;
    uint64_t *phys_pages;
};

struct rm_page_alloc {
    bool      contiguous;
    bool      dedicated_slot;
    bool      remapped_va;
    uint32_t  dma_slot;
    uint32_t  slab_idx;
    uint32_t  slab_start_pg;
    uint32_t  count;
    uint32_t  cache_type;
    void     *map_base_va;
    size_t    map_size;
    uint8_t  *virt_addr;
    uint64_t *phys_array;
    uint32_t *slab_indices;
    uint32_t *page_indices;
};

struct rm_file_ctx {
    nv_file_private_t          nvfp;
    bool                       in_use;
    bool                       is_ctl;
    nv_alloc_mapping_context_t mmap_ctx;
    void                      *last_alloc_priv;
    uint64_t                   last_page_index;
};

#define MAX_RM_FILES 64

static io_service_t           g_service  = IO_OBJECT_NULL;
static io_connect_t           g_connect  = IO_OBJECT_NULL;
static struct GP108DeviceInfo g_dev_info;
static mach_vm_address_t      g_bar_va[3];
static mach_vm_size_t         g_bar_sz[3];

static uint8_t               *g_vbios_rom  = NULL;
static uint32_t               g_vbios_size = 0;

static nv_state_t             g_ctl_state;
static nv_state_t             g_gpu_state;
static nvidia_stack_t        *g_init_sp  = NULL;
static nvidia_stack_t        *g_api_sp   = NULL;
static nvidia_stack_t        *g_isr_sp   = NULL;
static nvidia_stack_t        *g_timer_sp = NULL;

static pthread_mutex_t        g_api_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t        g_dma_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rm_dma_slab     g_slabs[MAX_DMA_SLABS];
static struct rm_file_ctx     g_files[MAX_RM_FILES];

/* PCI handle tags */
#define PCI_HANDLE_GPU         ((void *)(uintptr_t)0x10de1d13ULL)
#define PCI_HANDLE_ROOT_BRIDGE ((void *)(uintptr_t)0x808602bcULL)
#define PCI_HANDLE_HOST_BRIDGE ((void *)(uintptr_t)0x80869b61ULL)

/* Thread-local ISR indicator (matches FreeBSD __NV_ITHREAD) */
static __thread bool          tls_in_ithread = false;

/* MSI Event Queue & IRQ Thread */
static mach_vm_address_t      g_evq_va      = 0;
static mach_vm_size_t         g_evq_sz      = 0;
static IODataQueueMemory     *g_evq_mem     = NULL;
static mach_port_t            g_evq_port    = MACH_PORT_NULL;
static pthread_t              g_irq_tid;
static volatile bool          g_irq_running = false;
static volatile bool          g_adapter_up  = false;

/* Background Workqueue Thread */
struct rm_work_node {
    void                *data;
    struct rm_work_node *next;
};
static pthread_mutex_t        g_wq_lock    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t         g_wq_cv      = PTHREAD_COND_INITIALIZER;
static pthread_cond_t         g_wq_done_cv = PTHREAD_COND_INITIALIZER;
static struct rm_work_node   *g_wq_head    = NULL;
static struct rm_work_node   *g_wq_tail    = NULL;
static uint32_t               g_wq_active  = 0;
static volatile bool          g_wq_running = false;
static pthread_t              g_wq_tid;

/* 1 Hz RC Timer Thread */
static volatile bool          g_rc_running = false;
static pthread_t              g_rc_tid;

/* Debug print filter (set GP108_RM_VERBOSE=1 for full RM logs) */
static int                    g_verbose_rm = -1;

/* ========================================================================= *
 * Stack Helper                                                              *
 * ========================================================================= */
nvidia_stack_t *gp108_alloc_rm_stack(void)
{
    nvidia_stack_t *sp = NULL;
    if (posix_memalign((void **)&sp, 4096, RM_STACK_ALLOC_BYTES) != 0 || !sp)
        return NULL;
    memset(sp, 0, RM_STACK_ALLOC_BYTES);
    uint32_t usable = RM_STACK_ALLOC_BYTES - (uint32_t)offsetof(nvidia_stack_t, stack);
    usable &= ~15U;
    sp->size = usable;
    sp->top  = sp->stack + usable;
    return sp;
}

void gp108_free_rm_stack(nvidia_stack_t *sp)
{
    free(sp);
}

nv_state_t *gp108_rm_get_gpu_state(void) { return &g_gpu_state; }
nv_state_t *gp108_rm_get_ctl_state(void) { return &g_ctl_state; }

/* ========================================================================= *
 * 1. Core OS Memory, String, Time & Thread Primitives                       *
 * ========================================================================= */
static NvU64 NV_API_CALL macos_os_get_num_phys_pages(void)
{
    uint64_t memsize = 16ULL * 1024ULL * 1024ULL * 1024ULL;
    size_t len = sizeof(memsize);
    sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);
    return (NvU64)(memsize >> 12);
}

static NV_STATUS NV_API_CALL macos_os_alloc_mem(void **address, NvU64 size)
{
    if (!address)
        return NV_ERR_INVALID_PARAMETER;
    if (size == 0)
        size = 8;
    void *p = calloc(1, (size_t)size);
    *address = p;
    return p ? NV_OK : NV_ERR_NO_MEMORY;
}

static void NV_API_CALL macos_os_free_mem(void *address)
{
    free(address);
}

static NV_STATUS NV_API_CALL macos_os_get_current_time(NvU32 *sec, NvU32 *usec)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (sec)  *sec  = (NvU32)tv.tv_sec;
    if (usec) *usec = (NvU32)tv.tv_usec;
    return NV_OK;
}

static NvU64 mono_ns_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (NvU64)ts.tv_sec * 1000000000ULL + (NvU64)ts.tv_nsec;
}

static NvU64 NV_API_CALL macos_os_get_current_tick_hr(void)
{
    return mono_ns_now();
}

static NvU64 NV_API_CALL macos_os_get_current_tick(void)
{
    return mono_ns_now();
}

static NvU64 NV_API_CALL macos_os_get_tick_resolution(void)
{
    return 1000ULL; /* microsecond resolution compatibility */
}

static NV_STATUS NV_API_CALL macos_os_delay_us(NvU32 us)
{
    if (tls_in_ithread && us > NV_MAX_ISR_DELAY_US)
        return NV_ERR_GENERIC;
    if (us <= 50) {
        uint64_t target = mono_ns_now() + (uint64_t)us * 1000ULL;
        while (mono_ns_now() < target) {
            __asm__ __volatile__("pause");
        }
    } else {
        usleep(us);
    }
    return NV_OK;
}

static NV_STATUS NV_API_CALL macos_os_delay(NvU32 ms)
{
    if (tls_in_ithread && ms > NV_MAX_ISR_DELAY_MS)
        return NV_ERR_GENERIC;
    usleep((useconds_t)ms * 1000U);
    return NV_OK;
}

static NvU64 NV_API_CALL macos_os_get_cpu_frequency(void)
{
    uint64_t freq = 2100000000ULL;
    size_t len = sizeof(freq);
    if (sysctlbyname("machdep.tsc.frequency", &freq, &len, NULL, 0) != 0) {
        sysctlbyname("hw.cpufrequency", &freq, &len, NULL, 0);
    }
    return (NvU64)(freq + 4999ULL);
}

static NvU32 NV_API_CALL macos_os_get_current_process(void)
{
    return (NvU32)getpid();
}

static void NV_API_CALL macos_os_get_current_process_name(char *buf, NvU32 len)
{
    if (!buf || len == 0) return;
    const char *prog = getprogname();
    snprintf(buf, len, "%s", prog ? prog : "gp108-rmd");
}

static NvU32 NV_API_CALL macos_os_get_current_pasid(void) { return 0; }

static NV_STATUS NV_API_CALL macos_os_get_current_thread(NvU64 *threadId)
{
    if (!threadId) return NV_ERR_INVALID_PARAMETER;
    if (tls_in_ithread) {
        *threadId = 0;
    } else {
        uint64_t tid = 0;
        pthread_threadid_np(NULL, &tid);
        *threadId = (NvU64)tid;
    }
    return NV_OK;
}

static char *NV_API_CALL macos_os_string_copy(char *dst, const char *src) { return strcpy(dst, src); }
static NvU32 NV_API_CALL macos_os_string_length(const char *s) { return (NvU32)strlen(s); }
static NvU32 NV_API_CALL macos_os_strtoul(const char *s, char **endp, NvU32 base) { return (NvU32)strtoul(s, endp, (int)base); }
static NvS32 NV_API_CALL macos_os_string_compare(const char *a, const char *b) { return (NvS32)strcmp(a, b); }

static NvS32 NV_API_CALL macos_os_vsnprintf(char *buf, NvU32 size, const char *fmt, va_list ap)
{
    return (NvS32)vsnprintf(buf, size, fmt, ap);
}

static NvS32 NV_API_CALL macos_os_snprintf(char *buf, NvU32 size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return (NvS32)n;
}

static void NV_API_CALL macos_out_string(const char *str)
{
    if (str) fputs(str, stderr);
}

static int NV_API_CALL macos_nv_printf(NvU32 debuglevel, const char *fmt, ...)
{
    if (g_verbose_rm < 0) {
        const char *e = getenv("GP108_RM_VERBOSE");
        g_verbose_rm = (e && e[0] == '1') ? 1 : 0;
    }
    if (!g_verbose_rm && debuglevel < NV_DBG_WARNINGS)
        return 0;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stderr);
    return n;
}

static void NV_API_CALL macos_os_log_error(const char *fmt, va_list ap)
{
    vfprintf(stderr, fmt, ap);
}

static void *NV_API_CALL macos_os_mem_copy(void *dst, const void *src, NvU32 len)
{
    return memcpy(dst, src, len);
}

static NV_STATUS NV_API_CALL macos_os_memcpy_from_user(void *dst, const void *src, NvU32 len)
{
    if (!dst || !src) return NV_ERR_INVALID_POINTER;
    memcpy(dst, src, len);
    return NV_OK;
}

static NV_STATUS NV_API_CALL macos_os_memcpy_to_user(void *dst, const void *src, NvU32 len)
{
    if (!dst || !src) return NV_ERR_INVALID_POINTER;
    memcpy(dst, src, len);
    return NV_OK;
}

static void *NV_API_CALL macos_os_mem_set(void *dst, NvU8 c, NvU32 len) { return memset(dst, c, len); }
static NvS32 NV_API_CALL macos_os_mem_cmp(const NvU8 *a, const NvU8 *b, NvU32 len) { return (NvS32)memcmp(a, b, len); }

/* ========================================================================= *
 * 2. PCI Config Space & BAR Kernel Space Mapping                            *
 * ========================================================================= */
static void *NV_API_CALL macos_os_pci_init_handle(
    NvU32 domain, NvU8 bus, NvU8 slot, NvU8 function, NvU16 *vendor, NvU16 *device)
{
    if (domain != 0)
        return NULL;

    if (bus == g_dev_info.bus && slot == g_dev_info.device && function == g_dev_info.function) {
        if (vendor) *vendor = g_dev_info.vendor_id;
        if (device) *device = g_dev_info.device_id;
        return PCI_HANDLE_GPU;
    }
    /* Upstream Comet Lake PCIe Root Port RP05 (00:1c.4) */
    if (bus == 0 && slot == 0x1c && function == 4) {
        uint64_t in_s[5] = { 0, 1, 4, 0x00, 0 };
        uint64_t out_s[1] = { 0 };
        uint32_t out_c = 1;
        if (IOConnectCallScalarMethod(g_connect, kGP108MethodPCIConfig, in_s, 5, out_s, &out_c) == KERN_SUCCESS) {
            uint32_t vd = (uint32_t)out_s[0];
            if ((vd & 0xffffU) != 0xffffU) {
                if (vendor) *vendor = (NvU16)(vd & 0xffffU);
                if (device) *device = (NvU16)((vd >> 16) & 0xffffU);
                return PCI_HANDLE_ROOT_BRIDGE;
            }
        }
    }
    /* Intel Comet Lake Host Bridge (00:00.0) for chipset coherence detection */
    if (bus == 0 && slot == 0 && function == 0) {
        if (vendor) *vendor = 0x8086;
        if (device) *device = 0x9b61;
        return PCI_HANDLE_HOST_BRIDGE;
    }
    return NULL;
}

static NV_STATUS pci_cfg_rw(void *handle, bool is_write, uint32_t width, NvU32 offset, uint32_t val_in, uint32_t *val_out)
{
    if (!handle || offset >= 0x1000)
        return NV_ERR_NOT_SUPPORTED;

    if (handle == PCI_HANDLE_HOST_BRIDGE) {
        if (is_write) return NV_OK;
        uint32_t dword = 0xffffffffU;
        uint32_t aligned = offset & ~3U;
        if (aligned == 0x00) dword = 0x9b618086U;
        else if (aligned == 0x04) dword = 0x00100006U;
        else if (aligned == 0x08) dword = 0x0600000cU;
        else if (aligned == 0x0c) dword = 0x00000000U;
        else if (aligned == 0x2c) dword = 0x1260152dU;
        else dword = 0x00000000U;
        uint32_t shift = (offset & 3U) * 8U;
        if (val_out) *val_out = dword >> shift;
        return NV_OK;
    }

    bool target_bridge = (handle == PCI_HANDLE_ROOT_BRIDGE);
    uint64_t in_s[5] = {
        is_write ? 1ULL : 0ULL,
        target_bridge ? 1ULL : 0ULL,
        width,
        offset,
        val_in,
    };
    uint64_t out_s[1] = { 0 };
    uint32_t out_c = 1;
    kern_return_t kr = IOConnectCallScalarMethod(
        g_connect, kGP108MethodPCIConfig, in_s, 5, out_s, &out_c);
    if (kr != KERN_SUCCESS) {
        if (is_write && target_bridge)
            return NV_OK;
        return NV_ERR_NOT_SUPPORTED;
    }
    if (val_out)
        *val_out = (uint32_t)out_s[0];
    return NV_OK;
}

static NV_STATUS NV_API_CALL macos_os_pci_read_byte(void *h, NvU32 off, NvU8 *v)
{
    uint32_t out = 0xffU;
    NV_STATUS st = pci_cfg_rw(h, false, 1, off, 0, &out);
    if (v) *v = (NvU8)(out & 0xffU);
    return st;
}

static NV_STATUS NV_API_CALL macos_os_pci_read_word(void *h, NvU32 off, NvU16 *v)
{
    uint32_t out = 0xffffU;
    NV_STATUS st = pci_cfg_rw(h, false, 2, off, 0, &out);
    if (v) *v = (NvU16)(out & 0xffffU);
    return st;
}

static NV_STATUS NV_API_CALL macos_os_pci_read_dword(void *h, NvU32 off, NvU32 *v)
{
    uint32_t out = 0xffffffffU;
    NV_STATUS st = pci_cfg_rw(h, false, 4, off, 0, &out);
    if (v) *v = out;
    return st;
}

static NV_STATUS NV_API_CALL macos_os_pci_write_byte(void *h, NvU32 off, NvU8 v)   { return pci_cfg_rw(h, true, 1, off, v, NULL); }
static NV_STATUS NV_API_CALL macos_os_pci_write_word(void *h, NvU32 off, NvU16 v)  { return pci_cfg_rw(h, true, 2, off, v, NULL); }
static NV_STATUS NV_API_CALL macos_os_pci_write_dword(void *h, NvU32 off, NvU32 v) { return pci_cfg_rw(h, true, 4, off, v, NULL); }
static NvBool    NV_API_CALL macos_os_pci_remove_supported(void)                   { return NV_FALSE; }
static void      NV_API_CALL macos_os_pci_remove(void *h)                          { (void)h; }

static void *NV_API_CALL macos_os_map_kernel_space(NvU64 start, NvU64 size, NvU32 mode)
{
    (void)mode;
    if (start == 0 || size == 0)
        return NULL;

    for (int i = 0; i < 3; i++) {
        uint64_t bstart = g_dev_info.bar_phys[i];
        uint64_t bsize  = g_dev_info.bar_size[i];
        if (bsize > 0 && start >= bstart && (start + size) <= (bstart + bsize)) {
            return (void *)((uint8_t *)(uintptr_t)g_bar_va[i] + (start - bstart));
        }
    }

    /* Check DMA slabs as fallback */
    pthread_mutex_lock(&g_dma_lock);
    for (uint32_t s = 0; s < MAX_DMA_SLABS; s++) {
        if (!g_slabs[s].active) continue;
        for (uint32_t p = 0; p < g_slabs[s].total_pages; p++) {
            uint64_t paddr = g_slabs[s].phys_pages[p];
            if (start >= paddr && (start + size) <= paddr + 4096ULL) {
                void *va = g_slabs[s].base_va + (size_t)p * 4096UL + (start - paddr);
                pthread_mutex_unlock(&g_dma_lock);
                return va;
            }
        }
    }
    pthread_mutex_unlock(&g_dma_lock);

    fprintf(stderr, "[gp108-rm] os_map_kernel_space unmapped phys=0x%llx size=0x%llx\n",
            (unsigned long long)start, (unsigned long long)size);
    return NULL;
}

static void NV_API_CALL macos_os_unmap_kernel_space(void *addr, NvU64 size)
{
    (void)addr;
    (void)size;
}

static void *NV_API_CALL macos_os_map_user_space(NvU64 start, NvU64 size, NvU32 mode, NvU32 prot, void **priv)
{
    (void)size; (void)mode; (void)prot;
    if (priv) *priv = NULL;
    /* FreeBSD contract: return physical BAR address so IS_FB_OFFSET/IS_UD_OFFSET/IS_REG_OFFSET match! */
    return (void *)(uintptr_t)start;
}

static void NV_API_CALL macos_os_unmap_user_space(void *addr, NvU64 size, void *priv)
{
    (void)addr; (void)size; (void)priv;
}

/* ========================================================================= *
 * 3. Locks, Semaphores, Workqueue & Misc OS Queries                         *
 * ========================================================================= */
static NV_STATUS NV_API_CALL macos_os_flush_cpu_cache(void)     { __asm__ __volatile__("mfence" ::: "memory"); return NV_OK; }
static NV_STATUS NV_API_CALL macos_os_flush_cpu_cache_all(void) { __asm__ __volatile__("mfence" ::: "memory"); return NV_ERR_NOT_SUPPORTED; }
static NV_STATUS NV_API_CALL macos_os_flush_user_cache(void)    { return NV_ERR_NOT_SUPPORTED; }
static void      NV_API_CALL macos_os_flush_cpu_write_combine_buffer(void) { __asm__ __volatile__("sfence" ::: "memory"); }

static NvU8  NV_API_CALL macos_os_io_read_byte(NvU32 a)            { (void)a; return 0xff; }
static NvU16 NV_API_CALL macos_os_io_read_word(NvU32 a)            { (void)a; return 0xffff; }
static NvU32 NV_API_CALL macos_os_io_read_dword(NvU32 a)           { (void)a; return 0xffffffffU; }
static void  NV_API_CALL macos_os_io_write_byte(NvU32 a, NvU8 v)   { (void)a; (void)v; }
static void  NV_API_CALL macos_os_io_write_word(NvU32 a, NvU16 v)  { (void)a; (void)v; }
static void  NV_API_CALL macos_os_io_write_dword(NvU32 a, NvU32 v) { (void)a; (void)v; }

static NvBool    NV_API_CALL macos_os_is_administrator(void)        { return NV_TRUE; }
static NvBool    NV_API_CALL macos_os_allow_priority_override(void) { return NV_TRUE; }
static void      NV_API_CALL macos_os_dbg_init(void)                {}
static void      NV_API_CALL macos_os_dbg_breakpoint(void)          {}
static void      NV_API_CALL macos_os_dbg_set_level(NvU32 lvl)      { (void)lvl; }

static NvU32 NV_API_CALL macos_os_get_cpu_count(void)
{
    int ncpu = 8;
    size_t len = sizeof(ncpu);
    sysctlbyname("hw.logicalcpu", &ncpu, &len, NULL, 0);
    return (NvU32)(ncpu > 0 ? ncpu : 1);
}

static NvU32 NV_API_CALL macos_os_get_cpu_number(void) { return 0; }
static NV_STATUS NV_API_CALL macos_os_disable_console_access(void) { return NV_OK; }
static NV_STATUS NV_API_CALL macos_os_enable_console_access(void)  { return NV_OK; }

static NV_STATUS NV_API_CALL macos_os_registry_init(void)
{
    nvidia_stack_t *sp = gp108_alloc_rm_stack();
    if (!sp) return NV_ERR_NO_MEMORY;
    /* Enable MSI and full AC P0 Boost performance management on Pascal */
    g_rm.rm_write_registry_dword(sp, NULL, "EnableMSI", 1);
    g_rm.rm_write_registry_dword(sp, NULL, "InitializeSystemMemoryAllocations", 1);
    g_rm.rm_write_registry_dword(sp, NULL, "DynamicPowerManagement", 0);
    g_rm.rm_write_registry_dword(sp, NULL, "PerfLevelSrc", 0x2222);
    g_rm.rm_write_registry_dword(sp, NULL, "PowerMizerEnable", 1);
    g_rm.rm_write_registry_dword(sp, NULL, "PowerMizerDefault", 1);
    g_rm.rm_write_registry_dword(sp, NULL, "PowerMizerDefaultAC", 1);
    g_rm.rm_write_registry_dword(sp, NULL, "PowerMizerLevel", 1);
    g_rm.rm_write_registry_dword(sp, NULL, "PowerMizerLevelAC", 1);
    g_rm.rm_write_registry_dword(sp, NULL, "RMClkSlowDown", 0);
    g_rm.rm_write_registry_dword(sp, NULL, "RmPerfRatedTdpLimit", 0);
    g_rm.rm_write_registry_dword(sp, NULL, "RMBug1785342War", 0);
    gp108_free_rm_stack(sp);
    return NV_OK;
}

static NV_STATUS NV_API_CALL macos_os_schedule(void)
{
    sched_yield();
    usleep(200);
    return NV_OK;
}

static NV_STATUS NV_API_CALL macos_os_alloc_spinlock(void **lock)
{
    pthread_mutex_t *m = calloc(1, sizeof(*m));
    if (!m) return NV_ERR_NO_MEMORY;
    pthread_mutex_init(m, NULL);
    *lock = m;
    return NV_OK;
}

static void NV_API_CALL macos_os_free_spinlock(void *lock)
{
    if (lock) {
        pthread_mutex_destroy((pthread_mutex_t *)lock);
        free(lock);
    }
}

static NvU64 NV_API_CALL macos_os_acquire_spinlock(void *lock)
{
    if (lock) pthread_mutex_lock((pthread_mutex_t *)lock);
    return 0;
}

static void NV_API_CALL macos_os_release_spinlock(void *lock, NvU64 oldIrql)
{
    (void)oldIrql;
    if (lock) pthread_mutex_unlock((pthread_mutex_t *)lock);
}

static NV_STATUS NV_API_CALL macos_os_alloc_mutex(void **mutex)
{
    pthread_mutex_t *m = calloc(1, sizeof(*m));
    if (!m) return NV_ERR_NO_MEMORY;
    pthread_mutex_init(m, NULL);
    *mutex = m;
    return NV_OK;
}

static void NV_API_CALL macos_os_free_mutex(void *mutex)
{
    if (mutex) {
        pthread_mutex_destroy((pthread_mutex_t *)mutex);
        free(mutex);
    }
}

static NV_STATUS NV_API_CALL macos_os_acquire_mutex(void *mutex)
{
    if (tls_in_ithread) return NV_ERR_INVALID_REQUEST;
    if (mutex) pthread_mutex_lock((pthread_mutex_t *)mutex);
    return NV_OK;
}

static NV_STATUS NV_API_CALL macos_os_cond_acquire_mutex(void *mutex)
{
    if (tls_in_ithread) return NV_ERR_INVALID_REQUEST;
    if (!mutex) return NV_ERR_INVALID_PARAMETER;
    return (pthread_mutex_trylock((pthread_mutex_t *)mutex) == 0) ? NV_OK : NV_ERR_TIMEOUT_RETRY;
}

static void NV_API_CALL macos_os_release_mutex(void *mutex)
{
    if (mutex) pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

struct macos_sema {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    int32_t         count;
};

static void *NV_API_CALL macos_os_alloc_semaphore(NvU32 initialValue)
{
    struct macos_sema *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->count = (int32_t)initialValue;
    return s;
}

static void NV_API_CALL macos_os_free_semaphore(void *sem)
{
    struct macos_sema *s = sem;
    if (s) {
        pthread_mutex_destroy(&s->mtx);
        pthread_cond_destroy(&s->cv);
        free(s);
    }
}

static NV_STATUS NV_API_CALL macos_os_acquire_semaphore(void *sem)
{
    struct macos_sema *s = sem;
    if (!s) return NV_ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&s->mtx);
    while (s->count <= 0) {
        pthread_cond_wait(&s->cv, &s->mtx);
    }
    s->count--;
    pthread_mutex_unlock(&s->mtx);
    return NV_OK;
}

static NV_STATUS NV_API_CALL macos_os_cond_acquire_semaphore(void *sem)
{
    struct macos_sema *s = sem;
    if (!s) return NV_ERR_INVALID_PARAMETER;
    NV_STATUS st = NV_ERR_TIMEOUT_RETRY;
    if (pthread_mutex_trylock(&s->mtx) == 0) {
        if (s->count > 0) {
            s->count--;
            st = NV_OK;
        }
        pthread_mutex_unlock(&s->mtx);
    }
    return st;
}

static NV_STATUS NV_API_CALL macos_os_release_semaphore(void *sem)
{
    struct macos_sema *s = sem;
    if (!s) return NV_ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&s->mtx);
    s->count++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mtx);
    return NV_OK;
}

static NvBool NV_API_CALL macos_os_semaphore_may_sleep(void) { return tls_in_ithread ? NV_FALSE : NV_TRUE; }
static NvBool NV_API_CALL macos_os_is_isr(void)              { return tls_in_ithread ? NV_TRUE  : NV_FALSE; }
static NvBool NV_API_CALL macos_os_pat_supported(void)       { return NV_TRUE; }
static void   NV_API_CALL macos_os_dump_stack(void)          {}
static NvBool NV_API_CALL macos_os_is_efi_enabled(void)      { return NV_FALSE; }
static NvBool NV_API_CALL macos_os_is_xen_dom0(void)         { return NV_FALSE; }
static NvBool NV_API_CALL macos_os_is_vgx_hyper(void)        { return NV_FALSE; }
static NV_STATUS NV_API_CALL macos_os_inject_vgx_msi(NvU16 g, NvU64 a, NvU32 d) { (void)g;(void)a;(void)d; return NV_ERR_NOT_SUPPORTED; }
static NvBool NV_API_CALL macos_os_is_grid_supported(void)   { return NV_FALSE; }
static NvU32  NV_API_CALL macos_os_get_grid_csp_support(void){ return 0; }
static void   NV_API_CALL macos_os_bug_check(NvU32 c, const char *s) { fprintf(stderr, "[gp108-rm] BUGCHECK %u: %s\n", c, s ? s : ""); }

static NV_STATUS NV_API_CALL macos_os_get_address_space_info(NvU64 *uS, NvU64 *uE, NvU64 *kS, NvU64 *kE)
{
    (void)uS; (void)uE; (void)kS; (void)kE;
    return NV_ERR_NOT_SUPPORTED;
}

static NV_STATUS NV_API_CALL macos_os_get_version_info(os_version_info *v)
{
    (void)v;
    return NV_ERR_NOT_SUPPORTED;
}

static NV_STATUS NV_API_CALL macos_os_get_euid(NvU32 *uid)
{
    if (uid) *uid = 0; /* Grant root-equivalent RM client privileges (PERF_BOOST, etc.) */
    return NV_OK;
}

static void NV_API_CALL macos_os_get_random_bytes(NvU8 *bytes, NvU16 numBytes)
{
    arc4random_buf(bytes, numBytes);
}

static NvBool NV_API_CALL macos_os_is_nvswitch_present(void) { return NV_FALSE; }

/* Background Workqueue */
static void *wq_thread_main(void *arg)
{
    (void)arg;
    nvidia_stack_t *sp = gp108_alloc_rm_stack();
    pthread_mutex_lock(&g_wq_lock);
    while (g_wq_running) {
        while (g_wq_running && !g_wq_head) {
            pthread_cond_wait(&g_wq_cv, &g_wq_lock);
        }
        if (!g_wq_running && !g_wq_head)
            break;
        struct rm_work_node *node = g_wq_head;
        g_wq_head = node->next;
        if (!g_wq_head) g_wq_tail = NULL;
        g_wq_active++;
        pthread_mutex_unlock(&g_wq_lock);

        if (sp && g_rm.rm_execute_work_item) {
            g_rm.rm_execute_work_item(sp, node->data);
        }
        free(node);

        pthread_mutex_lock(&g_wq_lock);
        g_wq_active--;
        if (!g_wq_head && g_wq_active == 0) {
            pthread_cond_broadcast(&g_wq_done_cv);
        }
    }
    pthread_mutex_unlock(&g_wq_lock);
    gp108_free_rm_stack(sp);
    return NULL;
}

static NV_STATUS NV_API_CALL macos_os_queue_work_item(struct os_work_queue *q, void *data)
{
    (void)q;
    struct rm_work_node *n = calloc(1, sizeof(*n));
    if (!n) return NV_ERR_NO_MEMORY;
    n->data = data;

    pthread_mutex_lock(&g_wq_lock);
    if (g_wq_tail) g_wq_tail->next = n;
    else g_wq_head = n;
    g_wq_tail = n;
    pthread_cond_signal(&g_wq_cv);
    pthread_mutex_unlock(&g_wq_lock);
    return NV_OK;
}

static NV_STATUS NV_API_CALL macos_os_flush_work_queue(struct os_work_queue *q)
{
    (void)q;
    pthread_mutex_lock(&g_wq_lock);
    while (g_wq_head || g_wq_active > 0) {
        pthread_cond_wait(&g_wq_done_cv, &g_wq_lock);
    }
    pthread_mutex_unlock(&g_wq_lock);
    return NV_OK;
}

/* ========================================================================= *
 * 4. 40-bit Physical DMA Page Pool & User Page Pinning                      *
 * ========================================================================= */
static int query_bridge_phys_pages(bool is_pinned, uint32_t slot, uint64_t npages, uint64_t *out_phys)
{
    uint64_t done = 0;
    while (done < npages) {
        uint64_t batch = npages - done;
        if (batch > kGP108MaxPagesPerQuery) batch = kGP108MaxPagesPerQuery;
        uint64_t in_s[4] = { is_pinned ? 1ULL : 0ULL, slot, done, batch };
        uint64_t out_s[1] = { 0 };
        uint32_t out_c = 1;
        size_t sz = (size_t)(batch * sizeof(uint64_t));
        kern_return_t kr = IOConnectCallMethod(
            g_connect, kGP108MethodGetPhysPages,
            in_s, 4, NULL, 0, out_s, &out_c, &out_phys[done], &sz);
        if (kr != KERN_SUCCESS || out_s[0] == 0)
            return -EIO;
        done += out_s[0];
    }
    return 0;
}

static uint32_t nv_cache_to_bridge_mode(NvU32 nv_cache)
{
    switch (nv_cache) {
        case NV_MEMORY_WRITECOMBINED: return kGP108CacheWriteCombine;
        case NV_MEMORY_UNCACHED:
        case NV_MEMORY_UNCACHED_WEAK: return kGP108CacheUncached;
        default:                      return kGP108CacheDefault;
    }
}

static int alloc_dma_slab_locked(uint32_t min_pages, uint32_t cache_mode, uint32_t *out_idx)
{
    int idx = -1;
    for (uint32_t i = 0; i < MAX_DMA_SLABS; i++) {
        if (!g_slabs[i].active) { idx = (int)i; break; }
    }
    if (idx < 0) return -ENOMEM;

    uint32_t npages = min_pages > DMA_SLAB_PAGES ? min_pages : DMA_SLAB_PAGES;
    npages = (npages + 63U) & ~63U;
    size_t bytes = (size_t)npages * 4096UL;

    uint64_t in_s[3] = { bytes, cache_mode, 40 };
    uint64_t out_s[4] = { 0 };
    uint32_t out_c = 4;
    if (IOConnectCallScalarMethod(g_connect, kGP108MethodAllocDMA, in_s, 3, out_s, &out_c) != KERN_SUCCESS)
        return -ENOMEM;

    uint32_t slot = (uint32_t)out_s[0];
    mach_vm_address_t map_va = 0;
    mach_vm_size_t map_sz = 0;
    if (IOConnectMapMemory64(g_connect, kGP108MapDMABase + slot, mach_task_self(),
                             &map_va, &map_sz, kIOMapAnywhere) != KERN_SUCCESS || !map_va) {
        uint64_t s = slot;
        IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
        return -ENOMEM;
    }

    uint64_t *phys = calloc(npages, sizeof(uint64_t));
    uint64_t *bmap = calloc(npages / 64U, sizeof(uint64_t));
    if (!phys || !bmap || query_bridge_phys_pages(false, slot, npages, phys) != 0) {
        free(phys);
        free(bmap);
        IOConnectUnmapMemory64(g_connect, kGP108MapDMABase + slot, mach_task_self(), map_va);
        uint64_t s = slot;
        IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
        return -EIO;
    }

    memset(bmap, 0xff, (size_t)(npages / 64U) * sizeof(uint64_t));
    struct rm_dma_slab *sl = &g_slabs[idx];
    sl->active      = true;
    sl->dma_slot    = slot;
    sl->cache_mode  = cache_mode;
    sl->base_va     = (uint8_t *)(uintptr_t)map_va;
    sl->size_bytes  = bytes;
    sl->total_pages = npages;
    sl->free_count  = npages;
    sl->free_bmap   = bmap;
    sl->phys_pages  = phys;
    *out_idx        = (uint32_t)idx;
    return 0;
}

static bool slab_take_run(struct rm_dma_slab *sl, uint32_t count, uint32_t *out_start)
{
    if (!sl->active || sl->free_count < count) return false;
    uint32_t run = 0, start = 0;
    for (uint32_t p = 0; p < sl->total_pages; p++) {
        uint32_t w = p >> 6;
        if (sl->free_bmap[w] == 0) {
            run = 0;
            p = (w << 6) + 63U;
            continue;
        }
        if (sl->free_bmap[w] & (1ULL << (p & 63U))) {
            if (run == 0) start = p;
            if (++run == count) {
                for (uint32_t k = start; k < start + count; k++)
                    sl->free_bmap[k >> 6] &= ~(1ULL << (k & 63U));
                sl->free_count -= count;
                *out_start = start;
                return true;
            }
        } else {
            run = 0;
        }
    }
    return false;
}

static NV_STATUS NV_API_CALL macos_nv_alloc_pages(
    nv_state_t *nv, NvU32 count, NvBool contig, NvU32 cache_type,
    NvBool zeroed, NvU64 *pte_array, void **priv_out)
{
    (void)nv;
    if (count == 0 || !pte_array || !priv_out)
        return NV_ERR_INVALID_PARAMETER;

    uint32_t bcache = nv_cache_to_bridge_mode(cache_type);
    struct rm_page_alloc *at = calloc(1, sizeof(*at));
    if (!at) return NV_ERR_NO_MEMORY;
    at->contiguous = (contig != NV_FALSE);
    at->count      = count;
    at->cache_type = cache_type;
    at->phys_array = calloc(count, sizeof(uint64_t));
    if (!at->phys_array) {
        free(at);
        return NV_ERR_NO_MEMORY;
    }

    if (contig && count > 1) {
        /* Physically contiguous multi-page allocation */
        size_t req_bytes = (size_t)count * 4096UL;
        size_t alloc_sz  = (req_bytes <= 65536UL) ? req_bytes : (req_bytes * 8UL + (4UL << 20));
        uint32_t mask    = (req_bytes <= 65536UL) ? 32U : 40U;

        uint64_t in_s[3] = { alloc_sz, bcache, mask };
        uint64_t out_s[4] = { 0 };
        uint32_t out_c = 4;
        if (IOConnectCallScalarMethod(g_connect, kGP108MethodAllocDMA, in_s, 3, out_s, &out_c) != KERN_SUCCESS) {
            free(at->phys_array);
            free(at);
            return NV_ERR_NO_MEMORY;
        }
        uint32_t slot = (uint32_t)out_s[0];
        size_t tot_pg = (size_t)out_s[2];
        mach_vm_address_t map_va = 0;
        mach_vm_size_t map_sz = 0;
        if (IOConnectMapMemory64(g_connect, kGP108MapDMABase + slot, mach_task_self(),
                                 &map_va, &map_sz, kIOMapAnywhere) != KERN_SUCCESS || !map_va) {
            uint64_t s = slot;
            IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
            free(at->phys_array);
            free(at);
            return NV_ERR_NO_MEMORY;
        }

        uint64_t *all_phys = calloc(tot_pg, sizeof(uint64_t));
        if (!all_phys || query_bridge_phys_pages(false, slot, tot_pg, all_phys) != 0) {
            free(all_phys);
            IOConnectUnmapMemory64(g_connect, kGP108MapDMABase + slot, mach_task_self(), map_va);
            uint64_t s = slot;
            IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
            free(at->phys_array);
            free(at);
            return NV_ERR_NO_MEMORY;
        }

        ssize_t run_start = -1;
        for (size_t s = 0; s + count <= tot_pg; s++) {
            bool ok = true;
            for (uint32_t j = 1; j < count; j++) {
                if (all_phys[s + j] != all_phys[s] + (uint64_t)j * 4096ULL) {
                    ok = false;
                    s += j - 1;
                    break;
                }
            }
            if (ok) { run_start = (ssize_t)s; break; }
        }
        if (run_start < 0) {
            fprintf(stderr, "[gp108-rm] Failed to find %u contiguous DMA pages\n", count);
            free(all_phys);
            IOConnectUnmapMemory64(g_connect, kGP108MapDMABase + slot, mach_task_self(), map_va);
            uint64_t s = slot;
            IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
            free(at->phys_array);
            free(at);
            return NV_ERR_NO_MEMORY;
        }

        at->dedicated_slot = true;
        at->dma_slot       = slot;
        at->map_base_va    = (void *)(uintptr_t)map_va;
        at->map_size       = (size_t)map_sz;
        at->virt_addr      = (uint8_t *)(uintptr_t)map_va + (size_t)run_start * 4096UL;
        for (uint32_t i = 0; i < count; i++) {
            at->phys_array[i] = all_phys[(size_t)run_start + i];
        }
        pte_array[0] = at->phys_array[0];
        free(all_phys);

        if (zeroed) memset(at->virt_addr, 0, req_bytes);
        *priv_out = at;
        return NV_OK;
    }

    /* Single page or non-contiguous system pages: allocate virtually contiguous run from slab */
    pthread_mutex_lock(&g_dma_lock);
    int chosen = -1;
    uint32_t start_pg = 0;
    for (uint32_t i = 0; i < MAX_DMA_SLABS; i++) {
        if (g_slabs[i].active && g_slabs[i].cache_mode == bcache &&
            slab_take_run(&g_slabs[i], count, &start_pg)) {
            chosen = (int)i;
            break;
        }
    }
    if (chosen < 0) {
        uint32_t nidx = 0;
        if (alloc_dma_slab_locked(count, bcache, &nidx) == 0 &&
            slab_take_run(&g_slabs[nidx], count, &start_pg)) {
            chosen = (int)nidx;
        }
    }
    if (chosen < 0) {
        pthread_mutex_unlock(&g_dma_lock);
        free(at->phys_array);
        free(at);
        return NV_ERR_NO_MEMORY;
    }

    struct rm_dma_slab *sl = &g_slabs[chosen];
    at->slab_idx      = (uint32_t)chosen;
    at->slab_start_pg = start_pg;
    at->virt_addr     = sl->base_va + (size_t)start_pg * 4096UL;
    for (uint32_t i = 0; i < count; i++) {
        at->phys_array[i] = sl->phys_pages[start_pg + i];
        if (!contig) pte_array[i] = at->phys_array[i];
    }
    if (contig) pte_array[0] = at->phys_array[0];
    pthread_mutex_unlock(&g_dma_lock);

    if (zeroed) memset(at->virt_addr, 0, (size_t)count * 4096UL);
    *priv_out = at;
    return NV_OK;
}

static NV_STATUS NV_API_CALL macos_nv_free_pages(
    nv_state_t *nv, NvU32 count, NvBool contig, NvU32 cache_type, void *priv)
{
    (void)nv; (void)count; (void)contig; (void)cache_type;
    struct rm_page_alloc *at = priv;
    if (!at) return NV_OK;

    if (at->dedicated_slot) {
        IOConnectUnmapMemory64(g_connect, kGP108MapDMABase + at->dma_slot,
                               mach_task_self(), (mach_vm_address_t)at->map_base_va);
        uint64_t s = at->dma_slot;
        IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
    } else {
        pthread_mutex_lock(&g_dma_lock);
        if (at->slab_idx < MAX_DMA_SLABS && g_slabs[at->slab_idx].active) {
            struct rm_dma_slab *sl = &g_slabs[at->slab_idx];
            for (uint32_t k = at->slab_start_pg; k < at->slab_start_pg + at->count; k++) {
                sl->free_bmap[k >> 6] |= (1ULL << (k & 63U));
            }
            sl->free_count += at->count;
        }
        pthread_mutex_unlock(&g_dma_lock);
    }
    free(at->phys_array);
    free(at);
    return NV_OK;
}

static void *lookup_pinned_user_va(void *pAllocPrivate, NvU64 pageIndex, NvU32 pageOffset);

static void *NV_API_CALL macos_nv_alloc_kernel_mapping(
    nv_state_t *nv, void *pAllocPrivate, NvU64 pageIndex,
    NvU32 pageOffset, NvU64 size, void **ppPrivate)
{
    (void)nv; (void)size;
    if (ppPrivate) *ppPrivate = NULL;
    void *pinned_va = lookup_pinned_user_va(pAllocPrivate, pageIndex, pageOffset);
    if (pinned_va) return pinned_va;
    struct rm_page_alloc *at = pAllocPrivate;
    if (!at || !at->virt_addr) return NULL;
    return at->virt_addr + (size_t)pageIndex * 4096UL + pageOffset;
}

static NV_STATUS NV_API_CALL macos_nv_free_kernel_mapping(
    nv_state_t *nv, void *pAllocPrivate, void *address, void *pPrivate)
{
    (void)nv; (void)pAllocPrivate; (void)address; (void)pPrivate;
    return NV_OK;
}

static NV_STATUS NV_API_CALL macos_nv_alloc_user_mapping(
    nv_state_t *nv, void *pAllocPrivate, NvU64 pageIndex, NvU32 pageOffset,
    NvU64 size, NvU32 protect, NvU64 *pUserAddress, void **ppPrivate)
{
    (void)nv; (void)size; (void)protect;
    struct rm_page_alloc *at = pAllocPrivate;
    if (!at || !pUserAddress || pageIndex >= at->count) return NV_ERR_INVALID_ARGUMENT;
    if (ppPrivate) *ppPrivate = NULL;
    *pUserAddress = at->phys_array[pageIndex] + pageOffset;
    return NV_OK;
}

static NV_STATUS NV_API_CALL macos_nv_free_user_mapping(
    nv_state_t *nv, void *pAllocPrivate, NvU64 userAddress, void *pPrivate)
{
    (void)nv; (void)pAllocPrivate; (void)userAddress; (void)pPrivate;
    return NV_OK;
}

static NV_STATUS NV_API_CALL macos_os_match_mmap_offset(void *pAllocPrivate, NvU64 offset, NvU64 *pPageIndex)
{
    struct rm_page_alloc *at = pAllocPrivate;
    if (!at || !pPageIndex) return NV_ERR_OBJECT_NOT_FOUND;
    for (uint32_t i = 0; i < at->count; i++) {
        uint64_t va = (uint64_t)(uintptr_t)(at->virt_addr + (size_t)i * 4096UL);
        if (offset == va || offset == at->phys_array[i]) {
            *pPageIndex = i;
            return NV_OK;
        }
    }
    return NV_ERR_OBJECT_NOT_FOUND;
}

/* Pin userspace memory (e.g. NV01_MEMORY_SYSTEM_OS_DESCRIPTOR / IOSurface!) */
struct rm_pinned_entry {
    bool      used;
    uint32_t  pin_slot;
    uint64_t  npages;
    uint64_t *phys_pages;
    uint8_t  *user_va;
};
static struct rm_pinned_entry g_pinned_table[kGP108MaxPinnedSlots];

static void *lookup_pinned_user_va(void *pAllocPrivate, NvU64 pageIndex, NvU32 pageOffset)
{
    if (!pAllocPrivate) return NULL;
    void *ret = NULL;
    pthread_mutex_lock(&g_dma_lock);
    for (uint32_t i = 0; i < kGP108MaxPinnedSlots; i++) {
        if (g_pinned_table[i].used && g_pinned_table[i].phys_pages == pAllocPrivate) {
            if (pageIndex < g_pinned_table[i].npages && g_pinned_table[i].user_va)
                ret = g_pinned_table[i].user_va + (size_t)pageIndex * 4096UL + pageOffset;
            break;
        }
    }
    pthread_mutex_unlock(&g_dma_lock);
    return ret;
}

static NV_STATUS NV_API_CALL macos_os_lock_user_pages(void *address, NvU64 page_count, void **page_array, NvU32 flags)
{
    (void)flags;
    if (!address || page_count == 0 || !page_array)
        return NV_ERR_INVALID_ARGUMENT;

    uint64_t in_s[3] = { (uint64_t)(uintptr_t)address, page_count * 4096ULL, 0 };
    uint64_t out_s[3] = { 0 };
    uint32_t out_c = 3;
    if (IOConnectCallScalarMethod(g_connect, kGP108MethodPinUserMem, in_s, 3, out_s, &out_c) != KERN_SUCCESS)
        return NV_ERR_OPERATING_SYSTEM;

    uint32_t slot = (uint32_t)out_s[0];
    uint64_t *phys = calloc(page_count, sizeof(uint64_t));
    if (!phys || query_bridge_phys_pages(true, slot, page_count, phys) != 0) {
        free(phys);
        uint64_t s = slot;
        IOConnectCallScalarMethod(g_connect, kGP108MethodUnpinUserMem, &s, 1, NULL, NULL);
        return NV_ERR_NO_MEMORY;
    }

    pthread_mutex_lock(&g_dma_lock);
    if (slot < kGP108MaxPinnedSlots) {
        g_pinned_table[slot].used       = true;
        g_pinned_table[slot].pin_slot   = slot;
        g_pinned_table[slot].npages     = page_count;
        g_pinned_table[slot].phys_pages = phys;
        g_pinned_table[slot].user_va    = (uint8_t *)address;
    }
    pthread_mutex_unlock(&g_dma_lock);

    *page_array = phys;
    return NV_OK;
}

static NV_STATUS NV_API_CALL macos_os_unlock_user_pages(NvU64 page_count, void *page_array)
{
    (void)page_count;
    if (!page_array) return NV_OK;

    pthread_mutex_lock(&g_dma_lock);
    for (uint32_t i = 0; i < kGP108MaxPinnedSlots; i++) {
        if (g_pinned_table[i].used && g_pinned_table[i].phys_pages == page_array) {
            uint64_t s = g_pinned_table[i].pin_slot;
            IOConnectCallScalarMethod(g_connect, kGP108MethodUnpinUserMem, &s, 1, NULL, NULL);
            g_pinned_table[i].used       = false;
            g_pinned_table[i].phys_pages = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_dma_lock);

    free(page_array);
    return NV_OK;
}

static NV_STATUS NV_API_CALL macos_os_lookup_user_io_memory(void *addr, NvU64 count, NvU64 **pte_arr)
{
    (void)addr; (void)count; (void)pte_arr;
    return NV_ERR_NOT_SUPPORTED;
}

/* ========================================================================= *
 * 5. ACPI VBIOS (_ROM) & UNIX Helper Callbacks                              *
 * ========================================================================= */
static void      NV_API_CALL macos_nv_acpi_methods_init(NvU32 *handlesPresent) { if (handlesPresent) *handlesPresent = 0; }
static void      NV_API_CALL macos_nv_acpi_methods_uninit(void) {}
static NV_STATUS NV_API_CALL macos_nv_acpi_method(NvU32 m, NvU32 f, NvU32 sf, void *inP, NvU16 inS, NvU32 *st, void *outD, NvU16 *outS)
{ (void)m;(void)f;(void)sf;(void)inP;(void)inS;(void)st;(void)outD;(void)outS; return NV_ERR_NOT_SUPPORTED; }
static NV_STATUS NV_API_CALL macos_nv_acpi_dsm_method(nv_state_t *nv, NvU8 *guid, NvU32 rev, NvU32 sf, void *inP, NvU16 inS, NvU32 *st, void *outD, NvU16 *outS)
{ (void)nv;(void)guid;(void)rev;(void)sf;(void)inP;(void)inS;(void)st;(void)outD;(void)outS; return NV_ERR_NOT_SUPPORTED; }
static NV_STATUS NV_API_CALL macos_nv_acpi_ddc_method(nv_state_t *nv, void *edid, NvU32 *sz, NvBool multi)
{ (void)nv;(void)edid;(void)sz;(void)multi; return NV_ERR_NOT_SUPPORTED; }
static NV_STATUS NV_API_CALL macos_nv_acpi_dod_method(nv_state_t *nv, NvU32 *out, NvU32 *sz)
{ (void)nv;(void)out;(void)sz; return NV_ERR_NOT_SUPPORTED; }
static NV_STATUS NV_API_CALL macos_nv_acpi_get_powersource(NvU32 *ac) { if (ac) *ac = 1; return NV_OK; }
static NvBool    NV_API_CALL macos_nv_acpi_is_battery_present(void)   { return NV_FALSE; }
static NV_STATUS NV_API_CALL macos_nv_acpi_mux_method(nv_state_t *nv, NvU32 *io, NvU32 id, char *m)
{ (void)nv;(void)io;(void)id;(void)m; return NV_ERR_NOT_SUPPORTED; }

static NV_STATUS NV_API_CALL macos_nv_acpi_rom_method(nv_state_t *nv, NvU32 *pInData, NvU32 *pOutData)
{
    (void)nv;
    if (!pInData || !pOutData || !g_vbios_rom || g_vbios_size == 0)
        return NV_ERR_NOT_SUPPORTED;

    uint32_t offset = pInData[0];
    uint32_t length = pInData[1];
    if (offset >= g_vbios_size)
        return NV_ERR_GENERIC;

    memset(pOutData, 0, length);
    uint32_t avail = g_vbios_size - offset;
    uint32_t to_copy = (length < avail) ? length : avail;
    memcpy(pOutData, g_vbios_rom + offset, to_copy);
    return NV_OK;
}

static nv_file_private_t *NV_API_CALL macos_nv_get_file_private(NvS32 fd, NvBool ctl, void **os_priv)
{
    bool want_ctl = (ctl != NV_FALSE);
    if (fd < 0 || fd >= MAX_RM_FILES || !g_files[fd].in_use || g_files[fd].is_ctl != want_ctl) {
        if (os_priv) *os_priv = NULL;
        return NULL;
    }
    if (os_priv) *os_priv = &g_files[fd];
    return &g_files[fd].nvfp;
}

static void NV_API_CALL macos_nv_put_file_private(void *os_priv) { (void)os_priv; }

static NV_STATUS NV_API_CALL macos_nv_add_mapping_context_to_file(
    nv_state_t *nv, nv_usermap_access_params_t *nvuap, NvU32 prot,
    void *pAllocPriv, NvU64 pageIndex, NvU32 fd)
{
    if (fd >= MAX_RM_FILES || !g_files[fd].in_use)
        return NV_ERR_INVALID_ARGUMENT;
    struct rm_file_ctx *fc = &g_files[fd];
    memset(&fc->mmap_ctx, 0, sizeof(fc->mmap_ctx));
    fc->mmap_ctx.prot  = prot;
    fc->mmap_ctx.valid = NV_TRUE;
    if (NV_IS_CTL_DEVICE(nv)) {
        fc->mmap_ctx.alloc      = pAllocPriv;
        fc->mmap_ctx.page_index = pageIndex;
    } else if (nvuap) {
        fc->mmap_ctx.mmap_start   = nvuap->mmap_start;
        fc->mmap_ctx.mmap_size    = nvuap->mmap_size;
        fc->mmap_ctx.access_start = nvuap->access_start;
        fc->mmap_ctx.access_size  = nvuap->access_size;
    }
    return NV_OK;
}

static nv_state_t *NV_API_CALL macos_nv_get_adapter_state(NvU32 dom, NvU8 bus, NvU8 slot)
{
    if (dom == 0 && bus == g_dev_info.bus && slot == g_dev_info.device)
        return &g_gpu_state;
    return NULL;
}

static nv_state_t *NV_API_CALL macos_nv_get_ctl_state(void) { return &g_ctl_state; }
static NvU32 NV_API_CALL macos_nv_get_dev_minor(nv_state_t *nv) { return (nv == &g_gpu_state) ? 0U : 255U; }
static void  NV_API_CALL macos_nv_set_dma_address_size(nv_state_t *nv, NvU32 bits) { (void)nv; (void)bits; }
static NV_STATUS NV_API_CALL macos_nv_alias_pages(nv_state_t *nv, NvU32 c, NvU32 ct, NvU32 ca, NvU64 gid, NvU64 *pte, void **p)
{ (void)nv;(void)c;(void)ct;(void)ca;(void)gid;(void)pte;(void)p; return NV_ERR_NOT_SUPPORTED; }

static void *rc_timer_main(void *arg)
{
    (void)arg;
    while (g_rc_running) {
        usleep(1000000);
        if (!g_rc_running) break;
        if (g_timer_sp && g_rm.rm_run_rc_callback) {
            pthread_mutex_lock(&g_api_lock);
            g_rm.rm_run_rc_callback(g_timer_sp, &g_gpu_state);
            pthread_mutex_unlock(&g_api_lock);
        }
    }
    return NULL;
}

static NvS32 NV_API_CALL macos_nv_start_rc_timer(nv_state_t *nv)
{
    if (nv->rc_timer_enabled) return -1;
    nv->rc_timer_enabled = 1;
    g_rc_running = true;
    pthread_create(&g_rc_tid, NULL, rc_timer_main, NULL);
    return 0;
}

static NvS32 NV_API_CALL macos_nv_stop_rc_timer(nv_state_t *nv)
{
    if (!nv->rc_timer_enabled) return 0;
    g_rc_running = false;
    pthread_join(g_rc_tid, NULL);
    nv->rc_timer_enabled = 0;
    return 0;
}

static void  NV_API_CALL macos_nv_post_event(nv_event_t *ev, NvHandle hObj, NvU32 idx, NvU32 i32, NvU16 i16, NvBool valid)
{ (void)ev;(void)hObj;(void)idx;(void)i32;(void)i16;(void)valid; }
static NvS32 NV_API_CALL macos_nv_get_event(nv_file_private_t *fp, nv_event_t *ev, NvU32 *pend)
{ (void)fp;(void)ev; if (pend) *pend = 0; return NV_ERR_GENERIC; }
static void *NV_API_CALL macos_nv_i2c_add_adapter(nv_state_t *nv, NvU32 port) { (void)nv;(void)port; return NULL; }
static void  NV_API_CALL macos_nv_i2c_del_adapter(nv_state_t *nv, void *adp)  { (void)nv;(void)adp; }
static NV_STATUS NV_API_CALL macos_nv_log_error(nv_state_t *nv, NvU32 err, const char *fmt, va_list ap)
{ (void)nv;(void)err;(void)fmt;(void)ap; return NV_OK; }
static NvU64     NV_API_CALL macos_nv_get_dma_start_address(nv_state_t *nv) { (void)nv; return 0; }
static NV_STATUS NV_API_CALL macos_nv_set_primary_vga_status(nv_state_t *nv){ (void)nv; return NV_ERR_NOT_SUPPORTED; }
static NV_STATUS NV_API_CALL macos_nv_pci_trigger_recovery(nv_state_t *nv)  { (void)nv; return NV_ERR_NOT_SUPPORTED; }
static NvBool    NV_API_CALL macos_nv_requires_dma_remap(nv_state_t *nv)    { (void)nv; return NV_FALSE; }
static NvBool    NV_API_CALL macos_nv_is_rm_firmware_active(nv_state_t *nv) { (void)nv; return NV_FALSE; }
static const void *NV_API_CALL macos_nv_get_firmware(nv_state_t *nv, nv_firmware_t t, const void **b, NvU32 *s)
{ (void)nv;(void)t;(void)b;(void)s; return NULL; }
static void      NV_API_CALL macos_nv_put_firmware(const void *h) { (void)h; }
static NV_STATUS NV_API_CALL macos_nv_register_user_pages(nv_state_t *nv, NvU64 c, NvU64 *pa, void *ip, void **p)
{
    (void)nv;
    const uint64_t *phys = (const uint64_t *)(ip ? ip : (p ? *p : NULL));
    if (!pa || !phys) return NV_ERR_INVALID_ARGUMENT;
    for (NvU64 i = 0; i < c; i++) {
        pa[i] = phys[i];
    }
    return NV_OK;
}
static void      NV_API_CALL macos_nv_unregister_user_pages(nv_state_t *nv, NvU64 c, void **ip, void **p)
{ (void)nv;(void)c;(void)ip;(void)p; }
static NV_STATUS NV_API_CALL macos_nv_register_peer_io_mem(nv_state_t *nv, NvU64 *pa, NvU64 c, void **p)
{ (void)nv;(void)pa;(void)c;(void)p; return NV_OK; }
static void      NV_API_CALL macos_nv_unregister_peer_io_mem(nv_state_t *nv, void *p) { (void)nv;(void)p; }
static NV_STATUS NV_API_CALL macos_nv_register_sgt(nv_state_t *nv, NvU64 *pa, NvU64 c, NvU32 ct, void **p, struct sg_table *s, void *ip)
{ (void)nv;(void)pa;(void)c;(void)ct;(void)p;(void)s;(void)ip; return NV_OK; }
static void      NV_API_CALL macos_nv_unregister_sgt(nv_state_t *nv, struct sg_table **s, void **ip, void *p)
{ (void)nv;(void)s;(void)ip;(void)p; }
static NV_STATUS NV_API_CALL macos_nv_register_phys_pages(nv_state_t *nv, NvU64 *pa, NvU64 c, NvU32 ct, void **p)
{ (void)nv;(void)pa;(void)c;(void)ct;(void)p; return NV_OK; }
static void      NV_API_CALL macos_nv_unregister_phys_pages(nv_state_t *nv, void *p) { (void)nv;(void)p; }
static NV_STATUS NV_API_CALL macos_nv_get_usermap_access_params(nv_state_t *nv, nv_usermap_access_params_t *p)
{ (void)nv;(void)p; return NV_OK; }
static NvBool    NV_API_CALL macos_nv_is_gpu_accessible(nv_state_t *nv) { (void)nv; return NV_TRUE; }
static NvU32     NV_API_CALL macos_nv_get_os_type(void) { return OS_TYPE_FREEBSD; }
static NvBool    NV_API_CALL macos_nv_is_chassis_notebook(void) { return NV_FALSE; }
static NvBool    NV_API_CALL macos_nv_dynamic_power_available(nv_state_t *nv) { (void)nv; return NV_FALSE; }
static NvBool    NV_API_CALL macos_nv_platform_supports_s0ix(void) { return NV_FALSE; }
static NvBool    NV_API_CALL macos_nv_s2idle_pm_configured(void) { return NV_FALSE; }
static NvBool    NV_API_CALL macos_nv_get_all_mappings_revoked_locked(nv_state_t *nv) { (void)nv; return NV_FALSE; }
static nv_soc_irq_type_t NV_API_CALL macos_nv_get_current_irq_type(nv_state_t *nv) { (void)nv; return NV_SOC_IRQ_INVALID_TYPE; }

static void NV_API_CALL macos_nv_get_screen_info(
    nv_state_t *nv, NvU64 *pPhys, NvU16 *pW, NvU16 *pH, NvU16 *pD, NvU16 *pPitch, NvU64 *pSz)
{
    (void)nv;
    if (pPhys)  *pPhys  = 0;
    if (pW)     *pW     = 0;
    if (pH)     *pH     = 0;
    if (pD)     *pD     = 0;
    if (pPitch) *pPitch = 0;
    if (pSz)    *pSz    = 0;
}

static void noop_void(void) {}
static NV_STATUS not_supp_fn(void) { return NV_ERR_NOT_SUPPORTED; }
static void *null_ret_fn(void)     { return NULL; }
static int   neg1_ret_fn(void)     { return -1; }
static NvU32 zero_ret_fn(void)     { return 0; }

/* ========================================================================= *
 * 6. Complete Symbol Lookup Table for elf64_rm_loader.c                     *
 * ========================================================================= */
#define FN_SYM(nm, fn)   { nm, (void *)(uintptr_t)&(fn), false, 0 }
#define DATA_SYM(nm, vr) { nm, (void *)&(vr), true, sizeof(vr) }

static const struct gp108_ext_sym kOsSymTable[] = {
    /* Global Data Symbols */
    DATA_SYM("os_page_size",                   os_page_size),
    DATA_SYM("os_page_mask",                   os_page_mask),
    DATA_SYM("os_page_shift",                  os_page_shift),
    DATA_SYM("nv_parms",                       nv_parms),
    DATA_SYM("nvidia_caps_root",               nvidia_caps_root),
    DATA_SYM("nv_is_rm_firmware_supported_os", nv_is_rm_firmware_supported_os),

    /* Core OS Functions */
    FN_SYM("os_get_num_phys_pages",            macos_os_get_num_phys_pages),
    FN_SYM("os_alloc_mem",                     macos_os_alloc_mem),
    FN_SYM("os_free_mem",                      macos_os_free_mem),
    FN_SYM("os_get_current_time",              macos_os_get_current_time),
    FN_SYM("os_get_current_tick",              macos_os_get_current_tick),
    FN_SYM("os_get_current_tick_hr",           macos_os_get_current_tick_hr),
    FN_SYM("os_get_tick_resolution",           macos_os_get_tick_resolution),
    FN_SYM("os_delay",                         macos_os_delay),
    FN_SYM("os_delay_us",                      macos_os_delay_us),
    FN_SYM("os_get_cpu_frequency",             macos_os_get_cpu_frequency),
    FN_SYM("os_get_current_process",           macos_os_get_current_process),
    FN_SYM("os_get_current_process_name",      macos_os_get_current_process_name),
    FN_SYM("os_get_current_pasid",             macos_os_get_current_pasid),
    FN_SYM("os_get_current_thread",            macos_os_get_current_thread),
    FN_SYM("os_string_copy",                   macos_os_string_copy),
    FN_SYM("os_string_length",                 macos_os_string_length),
    FN_SYM("os_strtoul",                       macos_os_strtoul),
    FN_SYM("os_string_compare",                macos_os_string_compare),
    FN_SYM("os_snprintf",                      macos_os_snprintf),
    FN_SYM("os_vsnprintf",                     macos_os_vsnprintf),
    FN_SYM("os_log_error",                     macos_os_log_error),
    FN_SYM("out_string",                       macos_out_string),
    FN_SYM("nv_printf",                        macos_nv_printf),
    FN_SYM("os_mem_copy",                      macos_os_mem_copy),
    FN_SYM("os_memcpy_from_user",              macos_os_memcpy_from_user),
    FN_SYM("os_memcpy_to_user",                macos_os_memcpy_to_user),
    FN_SYM("os_mem_set",                       macos_os_mem_set),
    FN_SYM("os_mem_cmp",                       macos_os_mem_cmp),

    /* PCI & Space Mapping */
    FN_SYM("os_pci_init_handle",               macos_os_pci_init_handle),
    FN_SYM("os_pci_read_byte",                 macos_os_pci_read_byte),
    FN_SYM("os_pci_read_word",                 macos_os_pci_read_word),
    FN_SYM("os_pci_read_dword",                macos_os_pci_read_dword),
    FN_SYM("os_pci_write_byte",                macos_os_pci_write_byte),
    FN_SYM("os_pci_write_word",                macos_os_pci_write_word),
    FN_SYM("os_pci_write_dword",               macos_os_pci_write_dword),
    FN_SYM("os_pci_remove_supported",          macos_os_pci_remove_supported),
    FN_SYM("os_pci_remove",                    macos_os_pci_remove),
    FN_SYM("os_map_kernel_space",              macos_os_map_kernel_space),
    FN_SYM("os_unmap_kernel_space",            macos_os_unmap_kernel_space),
    FN_SYM("os_map_user_space",                macos_os_map_user_space),
    FN_SYM("os_unmap_user_space",              macos_os_unmap_user_space),

    /* Cache, IO, CPU, Locks, Semaphores, Workqueue */
    FN_SYM("os_flush_cpu_cache",               macos_os_flush_cpu_cache),
    FN_SYM("os_flush_cpu_cache_all",           macos_os_flush_cpu_cache_all),
    FN_SYM("os_flush_user_cache",              macos_os_flush_user_cache),
    FN_SYM("os_flush_cpu_write_combine_buffer",macos_os_flush_cpu_write_combine_buffer),
    FN_SYM("os_io_read_byte",                  macos_os_io_read_byte),
    FN_SYM("os_io_read_word",                  macos_os_io_read_word),
    FN_SYM("os_io_read_dword",                 macos_os_io_read_dword),
    FN_SYM("os_io_write_byte",                 macos_os_io_write_byte),
    FN_SYM("os_io_write_word",                 macos_os_io_write_word),
    FN_SYM("os_io_write_dword",                macos_os_io_write_dword),
    FN_SYM("os_is_administrator",              macos_os_is_administrator),
    FN_SYM("os_allow_priority_override",       macos_os_allow_priority_override),
    FN_SYM("os_dbg_init",                      macos_os_dbg_init),
    FN_SYM("os_dbg_breakpoint",                macos_os_dbg_breakpoint),
    FN_SYM("os_dbg_set_level",                 macos_os_dbg_set_level),
    FN_SYM("os_get_cpu_count",                 macos_os_get_cpu_count),
    FN_SYM("os_get_cpu_number",                macos_os_get_cpu_number),
    FN_SYM("os_disable_console_access",        macos_os_disable_console_access),
    FN_SYM("os_enable_console_access",         macos_os_enable_console_access),
    FN_SYM("os_registry_init",                 macos_os_registry_init),
    FN_SYM("os_schedule",                      macos_os_schedule),
    FN_SYM("os_alloc_spinlock",                macos_os_alloc_spinlock),
    FN_SYM("os_free_spinlock",                 macos_os_free_spinlock),
    FN_SYM("os_acquire_spinlock",              macos_os_acquire_spinlock),
    FN_SYM("os_release_spinlock",              macos_os_release_spinlock),
    FN_SYM("os_get_address_space_info",        macos_os_get_address_space_info),
    FN_SYM("os_queue_work_item",               macos_os_queue_work_item),
    FN_SYM("os_flush_work_queue",              macos_os_flush_work_queue),
    FN_SYM("os_alloc_mutex",                   macos_os_alloc_mutex),
    FN_SYM("os_free_mutex",                    macos_os_free_mutex),
    FN_SYM("os_acquire_mutex",                 macos_os_acquire_mutex),
    FN_SYM("os_cond_acquire_mutex",            macos_os_cond_acquire_mutex),
    FN_SYM("os_release_mutex",                 macos_os_release_mutex),
    FN_SYM("os_alloc_semaphore",               macos_os_alloc_semaphore),
    FN_SYM("os_free_semaphore",                macos_os_free_semaphore),
    FN_SYM("os_acquire_semaphore",             macos_os_acquire_semaphore),
    FN_SYM("os_cond_acquire_semaphore",        macos_os_cond_acquire_semaphore),
    FN_SYM("os_release_semaphore",             macos_os_release_semaphore),
    FN_SYM("os_semaphore_may_sleep",           macos_os_semaphore_may_sleep),
    FN_SYM("os_get_version_info",              macos_os_get_version_info),
    FN_SYM("os_is_isr",                        macos_os_is_isr),
    FN_SYM("os_pat_supported",                 macos_os_pat_supported),
    FN_SYM("os_dump_stack",                    macos_os_dump_stack),
    FN_SYM("os_is_efi_enabled",                macos_os_is_efi_enabled),
    FN_SYM("os_is_xen_dom0",                   macos_os_is_xen_dom0),
    FN_SYM("os_is_vgx_hyper",                  macos_os_is_vgx_hyper),
    FN_SYM("os_inject_vgx_msi",                macos_os_inject_vgx_msi),
    FN_SYM("os_is_grid_supported",             macos_os_is_grid_supported),
    FN_SYM("os_get_grid_csp_support",          macos_os_get_grid_csp_support),
    FN_SYM("os_bug_check",                     macos_os_bug_check),
    FN_SYM("os_lock_user_pages",               macos_os_lock_user_pages),
    FN_SYM("os_lookup_user_io_memory",         macos_os_lookup_user_io_memory),
    FN_SYM("os_unlock_user_pages",             macos_os_unlock_user_pages),
    FN_SYM("os_match_mmap_offset",             macos_os_match_mmap_offset),
    FN_SYM("os_get_euid",                      macos_os_get_euid),
    FN_SYM("os_get_smbios_header",             not_supp_fn),
    FN_SYM("os_get_acpi_rsdp_from_uefi",       not_supp_fn),
    FN_SYM("os_add_record_for_crashLog",       noop_void),
    FN_SYM("os_delete_record_for_crashLog",    noop_void),
    FN_SYM("os_call_vgpu_vfio",                not_supp_fn),
    FN_SYM("os_numa_memblock_size",            not_supp_fn),
    FN_SYM("os_alloc_pages_node",              not_supp_fn),
    FN_SYM("os_get_page",                      not_supp_fn),
    FN_SYM("os_put_page",                      not_supp_fn),
    FN_SYM("os_get_page_refcount",             zero_ret_fn),
    FN_SYM("os_count_tail_pages",              zero_ret_fn),
    FN_SYM("os_free_pages_phys",               noop_void),
    FN_SYM("os_call_nv_vmbus",                 not_supp_fn),
    FN_SYM("os_open_temporary_file",           not_supp_fn),
    FN_SYM("os_close_file",                    noop_void),
    FN_SYM("os_write_file",                    not_supp_fn),
    FN_SYM("os_read_file",                     not_supp_fn),
    FN_SYM("os_open_readonly_file",            not_supp_fn),
    FN_SYM("os_open_and_read_file",            not_supp_fn),
    FN_SYM("os_is_nvswitch_present",           macos_os_is_nvswitch_present),
    FN_SYM("os_get_random_bytes",              macos_os_get_random_bytes),
    FN_SYM("os_alloc_wait_queue",              not_supp_fn),
    FN_SYM("os_free_wait_queue",               noop_void),
    FN_SYM("os_wait_uninterruptible",          noop_void),
    FN_SYM("os_wake_up",                       noop_void),

    /* UNIX / nv_* Layer */
    FN_SYM("nv_alloc_pages",                   macos_nv_alloc_pages),
    FN_SYM("nv_free_pages",                    macos_nv_free_pages),
    FN_SYM("nv_alias_pages",                   macos_nv_alias_pages),
    FN_SYM("nv_alloc_kernel_mapping",          macos_nv_alloc_kernel_mapping),
    FN_SYM("nv_free_kernel_mapping",           macos_nv_free_kernel_mapping),
    FN_SYM("nv_alloc_user_mapping",            macos_nv_alloc_user_mapping),
    FN_SYM("nv_free_user_mapping",             macos_nv_free_user_mapping),
    FN_SYM("nv_add_mapping_context_to_file",   macos_nv_add_mapping_context_to_file),
    FN_SYM("nv_get_file_private",              macos_nv_get_file_private),
    FN_SYM("nv_put_file_private",              macos_nv_put_file_private),
    FN_SYM("nv_get_adapter_state",             macos_nv_get_adapter_state),
    FN_SYM("nv_get_ctl_state",                 macos_nv_get_ctl_state),
    FN_SYM("nv_get_dev_minor",                 macos_nv_get_dev_minor),
    FN_SYM("nv_set_dma_address_size",          macos_nv_set_dma_address_size),
    FN_SYM("nv_start_rc_timer",                macos_nv_start_rc_timer),
    FN_SYM("nv_stop_rc_timer",                 macos_nv_stop_rc_timer),
    FN_SYM("nv_post_event",                    macos_nv_post_event),
    FN_SYM("nv_get_event",                     macos_nv_get_event),
    FN_SYM("nv_i2c_add_adapter",               macos_nv_i2c_add_adapter),
    FN_SYM("nv_i2c_del_adapter",               macos_nv_i2c_del_adapter),
    FN_SYM("nv_acpi_methods_init",             macos_nv_acpi_methods_init),
    FN_SYM("nv_acpi_methods_uninit",           macos_nv_acpi_methods_uninit),
    FN_SYM("nv_acpi_method",                   macos_nv_acpi_method),
    FN_SYM("nv_acpi_dsm_method",               macos_nv_acpi_dsm_method),
    FN_SYM("nv_acpi_ddc_method",               macos_nv_acpi_ddc_method),
    FN_SYM("nv_acpi_dod_method",               macos_nv_acpi_dod_method),
    FN_SYM("nv_acpi_rom_method",               macos_nv_acpi_rom_method),
    FN_SYM("nv_acpi_get_powersource",          macos_nv_acpi_get_powersource),
    FN_SYM("nv_acpi_is_battery_present",       macos_nv_acpi_is_battery_present),
    FN_SYM("nv_acpi_mux_method",               macos_nv_acpi_mux_method),
    FN_SYM("nv_log_error",                     macos_nv_log_error),
    FN_SYM("nv_get_dma_start_address",         macos_nv_get_dma_start_address),
    FN_SYM("nv_set_primary_vga_status",        macos_nv_set_primary_vga_status),
    FN_SYM("nv_pci_trigger_recovery",          macos_nv_pci_trigger_recovery),
    FN_SYM("nv_requires_dma_remap",            macos_nv_requires_dma_remap),
    FN_SYM("nv_is_rm_firmware_active",         macos_nv_is_rm_firmware_active),
    FN_SYM("nv_get_firmware",                  macos_nv_get_firmware),
    FN_SYM("nv_put_firmware",                  macos_nv_put_firmware),
    FN_SYM("nv_register_user_pages",           macos_nv_register_user_pages),
    FN_SYM("nv_unregister_user_pages",         macos_nv_unregister_user_pages),
    FN_SYM("nv_register_peer_io_mem",          macos_nv_register_peer_io_mem),
    FN_SYM("nv_unregister_peer_io_mem",        macos_nv_unregister_peer_io_mem),
    FN_SYM("nv_register_sgt",                  macos_nv_register_sgt),
    FN_SYM("nv_unregister_sgt",                macos_nv_unregister_sgt),
    FN_SYM("nv_register_phys_pages",           macos_nv_register_phys_pages),
    FN_SYM("nv_unregister_phys_pages",         macos_nv_unregister_phys_pages),
    FN_SYM("nv_dma_map_pages",                 not_supp_fn),
    FN_SYM("nv_dma_unmap_pages",               not_supp_fn),
    FN_SYM("nv_dma_map_alloc",                 not_supp_fn),
    FN_SYM("nv_dma_unmap_alloc",               not_supp_fn),
    FN_SYM("nv_dma_map_peer",                  not_supp_fn),
    FN_SYM("nv_dma_unmap_peer",                noop_void),
    FN_SYM("nv_dma_map_mmio",                  not_supp_fn),
    FN_SYM("nv_dma_unmap_mmio",                noop_void),
    FN_SYM("nv_dma_cache_invalidate",          noop_void),
    FN_SYM("nv_dma_enable_nvlink",             noop_void),
    FN_SYM("nv_dma_import_sgt",                not_supp_fn),
    FN_SYM("nv_dma_release_sgt",               noop_void),
    FN_SYM("nv_dma_import_dma_buf",            not_supp_fn),
    FN_SYM("nv_dma_import_from_fd",            not_supp_fn),
    FN_SYM("nv_dma_release_dma_buf",           noop_void),
    FN_SYM("nv_get_device_memory_config",      not_supp_fn),
    FN_SYM("nv_get_ibmnpu_genreg_info",        not_supp_fn),
    FN_SYM("nv_get_ibmnpu_relaxed_ordering_mode", not_supp_fn),
    FN_SYM("nv_wait_for_ibmnpu_rsync",         noop_void),
    FN_SYM("nv_revoke_gpu_mappings",           not_supp_fn),
    FN_SYM("nv_acquire_mmap_lock",             noop_void),
    FN_SYM("nv_release_mmap_lock",             noop_void),
    FN_SYM("nv_get_all_mappings_revoked_locked", macos_nv_get_all_mappings_revoked_locked),
    FN_SYM("nv_set_safe_to_mmap_locked",       noop_void),
    FN_SYM("nv_indicate_idle",                 not_supp_fn),
    FN_SYM("nv_indicate_not_idle",             not_supp_fn),
    FN_SYM("nv_idle_holdoff",                  noop_void),
    FN_SYM("nv_dynamic_power_available",       macos_nv_dynamic_power_available),
    FN_SYM("nv_audio_dynamic_power",           noop_void),
    FN_SYM("nv_control_soc_irqs",              noop_void),
    FN_SYM("nv_acquire_fabric_mgmt_cap",       not_supp_fn),
    FN_SYM("nv_cap_create_dir_entry",          null_ret_fn),
    FN_SYM("nv_cap_create_file_entry",         null_ret_fn),
    FN_SYM("nv_cap_destroy_entry",             noop_void),
    FN_SYM("nv_cap_validate_and_dup_fd",       neg1_ret_fn),
    FN_SYM("nv_cap_close_fd",                  noop_void),
    FN_SYM("nv_is_gpu_accessible",             macos_nv_is_gpu_accessible),
    FN_SYM("nv_get_os_type",                   macos_nv_get_os_type),
    FN_SYM("nv_get_screen_info",               macos_nv_get_screen_info),
    FN_SYM("nv_schedule_uvm_isr",              noop_void),
    FN_SYM("nv_platform_supports_s0ix",        macos_nv_platform_supports_s0ix),
    FN_SYM("nv_s2idle_pm_configured",          macos_nv_s2idle_pm_configured),
    FN_SYM("nv_is_chassis_notebook",           macos_nv_is_chassis_notebook),
    FN_SYM("nv_allow_runtime_suspend",         noop_void),
    FN_SYM("nv_disallow_runtime_suspend",      noop_void),
    FN_SYM("nv_get_usermap_access_params",     macos_nv_get_usermap_access_params),
    FN_SYM("nv_get_current_irq_type",          macos_nv_get_current_irq_type),

    /* NVLink & NVSwitch Stubs (unused on Pascal GP108) */
    FN_SYM("nvlink_malloc",                    null_ret_fn),
    FN_SYM("nvlink_free",                      noop_void),
    FN_SYM("nvlink_strcpy",                    macos_os_string_copy),
    FN_SYM("nvlink_strcmp",                    macos_os_string_compare),
    FN_SYM("nvlink_strlen",                    macos_os_string_length),
    FN_SYM("nvlink_memset",                    memset),
    FN_SYM("nvlink_memcpy",                    memcpy),
    FN_SYM("nvlink_sleep",                     macos_os_delay),
    FN_SYM("nvlink_assert",                    noop_void),
    FN_SYM("nvlink_acquire_fabric_mgmt_cap",   neg1_ret_fn),
    FN_SYM("nvlink_is_fabric_manager",         zero_ret_fn),
    FN_SYM("nvlink_is_admin",                  macos_os_is_administrator),
    FN_SYM("nvswitch_os_acquire_fabric_mgmt_cap", neg1_ret_fn),
    FN_SYM("nvswitch_os_add_client_event",     neg1_ret_fn),
    FN_SYM("nvswitch_os_alloc_contig_memory",  neg1_ret_fn),
    FN_SYM("nvswitch_os_assert_log",           noop_void),
    FN_SYM("nvswitch_os_free",                 noop_void),
    FN_SYM("nvswitch_os_free_contig_memory",   noop_void),
    FN_SYM("nvswitch_os_get_os_version",       neg1_ret_fn),
    FN_SYM("nvswitch_os_get_platform_time",    zero_ret_fn),
    FN_SYM("nvswitch_os_get_supported_register_events_params", neg1_ret_fn),
    FN_SYM("nvswitch_os_is_admin",             macos_os_is_administrator),
    FN_SYM("nvswitch_os_is_fabric_manager",    zero_ret_fn),
    FN_SYM("nvswitch_os_is_uuid_in_blacklist", zero_ret_fn),
    FN_SYM("nvswitch_os_malloc",               null_ret_fn),
    FN_SYM("nvswitch_os_map_dma_region",       neg1_ret_fn),
    FN_SYM("nvswitch_os_mem_read32",           zero_ret_fn),
    FN_SYM("nvswitch_os_mem_write32",          noop_void),
    FN_SYM("nvswitch_os_memcmp",               memcmp),
    FN_SYM("nvswitch_os_memcpy",               memcpy),
    FN_SYM("nvswitch_os_memset",               memset),
    FN_SYM("nvswitch_os_notify_client_event",  neg1_ret_fn),
    FN_SYM("nvswitch_os_print",                noop_void),
    FN_SYM("nvswitch_os_read_registry_dword",  neg1_ret_fn),
    FN_SYM("nvswitch_os_remove_client_event",  neg1_ret_fn),
    FN_SYM("nvswitch_os_set_dma_mask",         neg1_ret_fn),
    FN_SYM("nvswitch_os_sleep",                noop_void),
    FN_SYM("nvswitch_os_snprintf",             macos_os_snprintf),
    FN_SYM("nvswitch_os_strlen",               strlen),
    FN_SYM("nvswitch_os_strncmp",              strncmp),
    FN_SYM("nvswitch_os_strncpy",              strncpy),
    FN_SYM("nvswitch_os_sync_dma_region_for_cpu", neg1_ret_fn),
    FN_SYM("nvswitch_os_sync_dma_region_for_device", neg1_ret_fn),
    FN_SYM("nvswitch_os_unmap_dma_region",     neg1_ret_fn),
    FN_SYM("nvswitch_os_vsnprintf",            macos_os_vsnprintf),
};

const struct gp108_ext_sym *gp108_os_lookup_symbol(const char *name)
{
    for (size_t i = 0; i < sizeof(kOsSymTable) / sizeof(kOsSymTable[0]); i++) {
        if (strcmp(name, kOsSymTable[i].name) == 0)
            return &kOsSymTable[i];
    }
    return NULL;
}

/* ========================================================================= *
 * 7. Hybrid MSI + Poll Interrupt Delivery Thread                            *
 * ========================================================================= */
static void service_rm_interrupt_once(void)
{
    if (!g_isr_sp || !g_rm.rm_isr)
        return;

    NvU32 run_bh = 0;
    tls_in_ithread = true;
    NvBool handled = g_rm.rm_isr(g_isr_sp, &g_gpu_state, &run_bh);
    if (run_bh) {
        NvU32 faults = 0;
        if (g_rm.rm_gpu_copy_mmu_faults)
            g_rm.rm_gpu_copy_mmu_faults(g_isr_sp, &g_gpu_state, &faults);
        if (g_rm.rm_isr_bh)
            g_rm.rm_isr_bh(g_isr_sp, &g_gpu_state);
    }
    tls_in_ithread = false;

    /* Re-arm GP108Bridge master MSI line (interruptFilter cleared NV_PMC_INTR_EN_0) */
    if (handled || run_bh) {
        uint64_t en = 1;
        uint64_t out_s[2] = { 0 };
        uint32_t out_c = 2;
        IOConnectCallScalarMethod(g_connect, kGP108MethodInterruptCtrl, &en, 1, out_s, &out_c);
    }
}

static void *rm_irq_thread_main(void *arg)
{
    (void)arg;
    struct {
        mach_msg_header_t hdr;
        uint8_t           pad[64];
    } msg;

    volatile const uint32_t *bar0 = (volatile const uint32_t *)(uintptr_t)g_bar_va[0];

    while (g_irq_running) {
        memset(&msg, 0, sizeof(msg));
        mach_msg(&msg.hdr,
                 MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                 0, sizeof(msg), g_evq_port,
                 2, /* 2 ms hybrid poll safety net */
                 MACH_PORT_NULL);

        if (!g_irq_running)
            break;

        bool fired = false;
        while (g_evq_mem && IODataQueueDataAvailable(g_evq_mem)) {
            struct GP108IrqEvent ev;
            uint32_t sz = sizeof(ev);
            if (IODataQueueDequeue(g_evq_mem, &ev, &sz) == kIOReturnSuccess)
                fired = true;
            else
                break;
        }

        uint32_t intr0 = bar0 ? bar0[0x100 / 4] : 0;
        if (fired || (intr0 != 0 && intr0 != 0xffffffffU)) {
            service_rm_interrupt_once();
        }
    }
    return NULL;
}

/* ========================================================================= *
 * 8. VBIOS Loader & Full RM Bootstrap                                       *
 * ========================================================================= */
static int load_vbios_image(void)
{
    /* 1. Try GP108Bridge kGP108MethodReadVBIOS */
    if (g_dev_info.vbios_size >= 512 && g_dev_info.vbios_size <= kGP108MaxVBIOSBytes) {
        uint8_t *buf = calloc(1, g_dev_info.vbios_size);
        if (buf) {
            uint32_t off = 0;
            bool ok = true;
            while (off < g_dev_info.vbios_size) {
                uint64_t in_off = off;
                size_t chunk = g_dev_info.vbios_size - off;
                if (chunk > kGP108MaxChunkBytes) chunk = kGP108MaxChunkBytes;
                if (IOConnectCallMethod(g_connect, kGP108MethodReadVBIOS,
                                        &in_off, 1, NULL, 0,
                                        NULL, NULL, buf + off, &chunk) != KERN_SUCCESS || chunk == 0) {
                    ok = false;
                    break;
                }
                off += (uint32_t)chunk;
            }
            if (ok && buf[0] == 0x55 && buf[1] == 0xaa) {
                g_vbios_rom  = buf;
                g_vbios_size = g_dev_info.vbios_size;
                return 0;
            }
            free(buf);
        }
    }

    /* 2. Fallback to dumped VBIOS file on disk */
    const char *paths[] = {
        "/Library/Application Support/NVIDIA-GP108/firmware/mx250_vbios.rom",
        "/Users/maple/projects/temp/mx250-macos/hw_dump/mx250_vbios.rom",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int fd = open(paths[i], O_RDONLY);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size >= 512) {
                uint8_t *buf = malloc((size_t)st.st_size);
                if (buf && read(fd, buf, (size_t)st.st_size) == st.st_size &&
                    buf[0] == 0x55 && buf[1] == 0xaa) {
                    close(fd);
                    g_vbios_rom  = buf;
                    g_vbios_size = (uint32_t)st.st_size;
                    return 0;
                }
                free(buf);
            }
            close(fd);
        }
    }
    return -ENOENT;
}

int gp108_rm_bootstrap(const char *elf_path)
{
    if (g_adapter_up)
        return 0;

    /* 1. Load ELF64 nv-kernel.o FIRST so low <2 GiB VA space is reserved before 256 MB BAR1 mapping */
    int rc = gp108_elf64_load_rm(elf_path, &g_rm);
    if (rc != 0)
        return rc;

    /* 2. Open GP108Bridge.kext and map BAR0, BAR1, BAR3 */
    g_service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching(kGP108BridgeServiceName));
    if (g_service == IO_OBJECT_NULL) {
        fprintf(stderr, "[gp108-rm] GP108Bridge.kext not found!\n");
        return -ENODEV;
    }

    if (IOServiceOpen(g_service, mach_task_self(), 0, &g_connect) != KERN_SUCCESS) {
        IOObjectRelease(g_service);
        g_service = IO_OBJECT_NULL;
        return -EACCES;
    }

    size_t info_sz = sizeof(g_dev_info);
    if (IOConnectCallStructMethod(g_connect, kGP108MethodGetInfo, NULL, 0, &g_dev_info, &info_sz) != KERN_SUCCESS ||
        g_dev_info.magic != kGP108BridgeMagic) {
        return -EIO;
    }

    if (!g_dev_info.power_state) {
        uint64_t act = kGP108PowerOn;
        IOConnectCallScalarMethod(g_connect, kGP108MethodPowerControl, &act, 1, NULL, NULL);
        info_sz = sizeof(g_dev_info);
        IOConnectCallStructMethod(g_connect, kGP108MethodGetInfo, NULL, 0, &g_dev_info, &info_sz);
    }

    const uint32_t map_types[3] = { kGP108MapBAR0, kGP108MapBAR1, kGP108MapBAR3 };
    for (int i = 0; i < 3; i++) {
        if (g_dev_info.bar_size[i] == 0) continue;
        if (IOConnectMapMemory64(g_connect, map_types[i], mach_task_self(),
                                 &g_bar_va[i], &g_bar_sz[i], kIOMapAnywhere) != KERN_SUCCESS) {
            fprintf(stderr, "[gp108-rm] Failed to map BAR slot %d\n", i);
            return -ENOMEM;
        }
    }

    if (load_vbios_image() != 0) {
        fprintf(stderr, "[gp108-rm] WARNING: VBIOS image not found!\n");
    } else {
        fprintf(stderr, "[gp108-rm] VBIOS ready: %u bytes (0x%02x 0x%02x)\n",
                g_vbios_size, g_vbios_rom[0], g_vbios_rom[1]);
    }

    /* 3. Start background workqueue thread */
    g_wq_running = true;
    pthread_create(&g_wq_tid, NULL, wq_thread_main, NULL);

    /* 4. Allocate RM stacks and initialize global RM core (rm_init_rm) */
    g_init_sp  = gp108_alloc_rm_stack();
    g_api_sp   = gp108_alloc_rm_stack();
    g_isr_sp   = gp108_alloc_rm_stack();
    g_timer_sp = gp108_alloc_rm_stack();

    memset(&g_ctl_state, 0, sizeof(g_ctl_state));
    g_ctl_state.flags = NV_FLAG_OPEN | NV_FLAG_CONTROL;
    g_files[0].in_use = true;
    g_files[0].is_ctl = true;
    g_files[1].in_use = true;
    g_files[1].is_ctl = false;

    fprintf(stderr, "[gp108-rm] Calling rm_init_rm()...\n");
    if (!g_rm.rm_init_rm(g_init_sp)) {
        fprintf(stderr, "[gp108-rm] rm_init_rm() FAILED!\n");
        return -EIO;
    }
    g_rm.rm_init_event_locks(g_init_sp, &g_ctl_state);

    /* 5. Populate nv_state_t for GP108 (10de:1d13) */
    memset(&g_gpu_state, 0, sizeof(g_gpu_state));
    g_gpu_state.pci_info.domain    = 0;
    g_gpu_state.pci_info.bus       = g_dev_info.bus;
    g_gpu_state.pci_info.slot      = g_dev_info.device;
    g_gpu_state.pci_info.function  = g_dev_info.function;
    g_gpu_state.pci_info.vendor_id = g_dev_info.vendor_id;
    g_gpu_state.pci_info.device_id = g_dev_info.device_id;
    g_gpu_state.subsystem_vendor   = g_dev_info.subsys_vendor_id;
    g_gpu_state.subsystem_id       = g_dev_info.subsys_id;
    g_gpu_state.handle             = PCI_HANDLE_GPU;
    g_gpu_state.interrupt_line     = 16;
    g_gpu_state.flags              = NV_FLAG_USES_MSI;

    const uint32_t bar_offsets[3] = { 0x10, 0x14, 0x1c };
    for (int i = 0; i < 3; i++) {
        g_gpu_state.bars[i].offset        = bar_offsets[i];
        g_gpu_state.bars[i].cpu_address   = g_dev_info.bar_phys[i];
        g_gpu_state.bars[i].strapped_size = g_dev_info.bar_size[i];
        g_gpu_state.bars[i].size          = g_dev_info.bar_size[i];
    }
    g_gpu_state.regs = &g_gpu_state.bars[NV_GPU_BAR_INDEX_REGS];
    g_gpu_state.fb   = &g_gpu_state.bars[NV_GPU_BAR_INDEX_FB];

    for (int i = 0; i < NVRM_PCICFG_NUM_DWORDS; i++) {
        macos_os_pci_read_dword(PCI_HANDLE_GPU, (NvU32)(i << 2), &g_gpu_state.pci_cfg_space[i]);
    }

    const char *dev_name = g_rm.rm_get_device_name
        ? g_rm.rm_get_device_name(g_dev_info.device_id, g_dev_info.subsys_vendor_id, g_dev_info.subsys_id)
        : "GP108";
    fprintf(stderr, "[gp108-rm] RM identified PCI %02x:%02x.%x (%04x:%04x) as: %s\n",
            g_dev_info.bus, g_dev_info.device, g_dev_info.function,
            g_dev_info.vendor_id, g_dev_info.device_id,
            dev_name ? dev_name : "NVIDIA GPU");

    NV_STATUS sup = g_rm.rm_is_supported_device(g_init_sp, &g_gpu_state);
    if (sup != NV_OK) {
        fprintf(stderr, "[gp108-rm] rm_is_supported_device returned 0x%x!\n", sup);
        return -ENOTSUP;
    }

    /* 6. Map MSI Event Queue & start hybrid IRQ thread BEFORE rm_init_private_state / rm_init_adapter */
    if (IOConnectMapMemory64(g_connect, kGP108MapEventQueue, mach_task_self(),
                             &g_evq_va, &g_evq_sz, kIOMapAnywhere) == KERN_SUCCESS && g_evq_va) {
        g_evq_mem  = (IODataQueueMemory *)(uintptr_t)g_evq_va;
        g_evq_port = IODataQueueAllocateNotificationPort();
        IOConnectSetNotificationPort(g_connect, kGP108MapEventQueue, g_evq_port, 0);
        uint64_t en = 1;
        uint64_t out_s[2] = { 0 };
        uint32_t out_c = 2;
        IOConnectCallScalarMethod(g_connect, kGP108MethodInterruptCtrl, &en, 1, out_s, &out_c);
        g_irq_running = true;
        pthread_create(&g_irq_tid, NULL, rm_irq_thread_main, NULL);
    }

    /* 7. Initialize per-GPU private state and boot the adapter! */
    fprintf(stderr, "[gp108-rm] Calling rm_init_private_state() (gpu_id=0x%x)...\n", g_gpu_state.gpu_id);
    if (!g_rm.rm_init_private_state(g_init_sp, &g_gpu_state)) {
        fprintf(stderr, "[gp108-rm] rm_init_private_state() FAILED!\n");
        return -EIO;
    }
    g_rm.rm_init_event_locks(g_init_sp, &g_gpu_state);
    g_rm.rm_write_registry_dword(g_init_sp, &g_gpu_state, "PerfLevelSrc", 0x2222);
    g_rm.rm_write_registry_dword(g_init_sp, &g_gpu_state, "PowerMizerEnable", 1);
    g_rm.rm_write_registry_dword(g_init_sp, &g_gpu_state, "PowerMizerDefault", 1);
    g_rm.rm_write_registry_dword(g_init_sp, &g_gpu_state, "PowerMizerDefaultAC", 1);
    g_rm.rm_write_registry_dword(g_init_sp, &g_gpu_state, "PowerMizerLevel", 1);
    g_rm.rm_write_registry_dword(g_init_sp, &g_gpu_state, "PowerMizerLevelAC", 1);
    g_rm.rm_write_registry_dword(g_init_sp, &g_gpu_state, "RMClkSlowDown", 0);
    g_rm.rm_write_registry_dword(g_init_sp, &g_gpu_state, "RmPerfRatedTdpLimit", 0);
    g_rm.rm_write_registry_dword(g_init_sp, &g_gpu_state, "RmPerfLimitsOverride", 1);
    g_rm.rm_write_registry_dword(g_init_sp, &g_gpu_state, "RMBug1785342War", 0);
    g_rm.rm_write_registry_dword(g_init_sp, &g_gpu_state, "RMSkipACPIBattCap", 1);

    uint64_t t0 = mono_ns_now();
    fprintf(stderr, "[gp108-rm] Calling rm_init_adapter() — loading signed PMU/ACR/SEC2 & GDDR5 training...\n");
    if (!g_rm.rm_init_adapter(g_api_sp, &g_gpu_state)) {
        fprintf(stderr, "[gp108-rm] First rm_init_adapter() failed (dirty WPR/ACR?); cycling ACPI _OFF/_ON and retrying...\n");
        uint64_t off_act = kGP108PowerOff, on_act = kGP108PowerOn;
        IOConnectCallScalarMethod(g_connect, kGP108MethodPowerControl, &off_act, 1, NULL, NULL);
        usleep(30000);
        IOConnectCallScalarMethod(g_connect, kGP108MethodPowerControl, &on_act, 1, NULL, NULL);
        uint64_t en = 1, out_s[2] = { 0 };
        uint32_t out_c = 2;
        IOConnectCallScalarMethod(g_connect, kGP108MethodInterruptCtrl, &en, 1, out_s, &out_c);
        t0 = mono_ns_now();
        if (!g_rm.rm_init_adapter(g_api_sp, &g_gpu_state)) {
            fprintf(stderr, "[gp108-rm] rm_init_adapter() FAILED after power cycle!\n");
            return -EIO;
        }
    }
    double boot_ms = (double)(mono_ns_now() - t0) / 1e6;

    g_gpu_state.flags |= NV_FLAG_OPEN;
    g_adapter_up = true;

    char vbios_ver[64] = { 0 };
    if (g_rm.rm_get_vbios_version)
        g_rm.rm_get_vbios_version(g_api_sp, &g_gpu_state, vbios_ver);
    char *uuid = g_rm.rm_get_gpu_uuid ? g_rm.rm_get_gpu_uuid(g_api_sp, &g_gpu_state) : NULL;

    fprintf(stderr, "[gp108-rm] SUCCESS! rm_init_adapter() completed in %.1f ms! VBIOS=%s UUID=%s\n",
            boot_ms, vbios_ver[0] ? vbios_ver : "ok", uuid ? uuid : "n/a");
    if (uuid) macos_os_free_mem(uuid);
    return 0;
}

void *gp108_rm_bar_va(int bar_idx, uint64_t *out_size, uint64_t *out_phys)
{
    if (bar_idx < 0 || bar_idx >= 3) return NULL;
    if (out_size) *out_size = g_bar_sz[bar_idx];
    if (out_phys) *out_phys = g_dev_info.bar_phys[bar_idx];
    return (void *)(uintptr_t)g_bar_va[bar_idx];
}

int gp108_rm_get_last_mmap(int fd, uint64_t *out_phys, uint64_t *out_size,
                           int *out_bar_idx, uint64_t *out_bar_off, void **out_cpu_va)
{
    if (fd < 0 || fd >= MAX_RM_FILES || !g_files[fd].in_use || !g_files[fd].mmap_ctx.valid)
        return -EINVAL;

    nv_alloc_mapping_context_t *mc = &g_files[fd].mmap_ctx;
    if (g_files[fd].is_ctl) {
        struct rm_page_alloc *at = mc->alloc;
        if (!at || !at->virt_addr) return -EINVAL;
        void *va = at->virt_addr + (size_t)mc->page_index * 4096UL;
        if (out_phys)    *out_phys    = at->phys_array[mc->page_index];
        if (out_size)    *out_size    = (uint64_t)(at->count - mc->page_index) * 4096ULL;
        if (out_bar_idx) *out_bar_idx = -1;
        if (out_bar_off) *out_bar_off = 0;
        if (out_cpu_va)  *out_cpu_va  = va;
        mc->valid = NV_FALSE;
        return 0;
    }

    uint64_t pstart = mc->access_start ? mc->access_start : mc->mmap_start;
    uint64_t psize  = mc->access_size  ? mc->access_size  : mc->mmap_size;
    mc->valid = NV_FALSE;

    for (int i = 0; i < 3; i++) {
        uint64_t bstart = g_dev_info.bar_phys[i];
        uint64_t bsize  = g_dev_info.bar_size[i];
        if (bsize > 0 && pstart >= bstart && (pstart + psize) <= (bstart + bsize)) {
            uint64_t boff = pstart - bstart;
            if (out_phys)    *out_phys    = pstart;
            if (out_size)    *out_size    = psize;
            if (out_bar_idx) *out_bar_idx = i;
            if (out_bar_off) *out_bar_off = boff;
            if (out_cpu_va)  *out_cpu_va  = (uint8_t *)(uintptr_t)g_bar_va[i] + boff;
            return 0;
        }
    }
    return -ENOENT;
}

int gp108_rm_call_ioctl(nv_file_private_t *nvfp, bool is_ctl, uint32_t nr, void *params, uint32_t size)
{
    if (!g_adapter_up || !g_rm.rm_ioctl)
        return -ENODEV;

    nv_file_private_t *fp = nvfp ? nvfp : (is_ctl ? &g_files[0].nvfp : &g_files[1].nvfp);
    nv_state_t *st = is_ctl ? &g_ctl_state : &g_gpu_state;

    pthread_mutex_lock(&g_api_lock);
    NV_STATUS status = g_rm.rm_ioctl(g_api_sp, st, fp, nr, params, size);
    pthread_mutex_unlock(&g_api_lock);
    return (status == NV_OK) ? 0 : -(int)status;
}

void gp108_rm_teardown(void)
{
    if (!g_adapter_up)
        return;

    volatile const uint32_t *bar0 = (volatile const uint32_t *)(uintptr_t)g_bar_va[0];
    bool bus_alive = (bar0 != NULL && bar0[0] != 0xffffffffU);

    if (bus_alive) {
        pthread_mutex_lock(&g_api_lock);
        g_rm.rm_cleanup_file_private(g_api_sp, &g_gpu_state, &g_files[1].nvfp);
        g_rm.rm_cleanup_file_private(g_api_sp, &g_ctl_state, &g_files[0].nvfp);

        g_rm.rm_disable_adapter(g_api_sp, &g_gpu_state);
        g_rm.rm_shutdown_adapter(g_api_sp, &g_gpu_state);
        g_gpu_state.flags &= ~NV_FLAG_OPEN;
        g_adapter_up = false;
        pthread_mutex_unlock(&g_api_lock);

        macos_os_flush_work_queue(NULL);
    } else {
        g_adapter_up = false;
    }

    if (g_irq_running) {
        g_irq_running = false;
        pthread_join(g_irq_tid, NULL);
    }
    if (bus_alive && g_connect != IO_OBJECT_NULL) {
        uint64_t dis = 0, out_s[2] = { 0 };
        uint32_t out_c = 2;
        IOConnectCallScalarMethod(g_connect, kGP108MethodInterruptCtrl, &dis, 1, out_s, &out_c);
    }

    if (bus_alive) {
        g_rm.rm_destroy_event_locks(g_init_sp, &g_gpu_state);
        g_rm.rm_free_private_state(g_init_sp, &g_gpu_state);
        g_rm.rm_destroy_event_locks(g_init_sp, &g_ctl_state);
        g_rm.rm_shutdown_rm(g_init_sp);
    }

    g_wq_running = false;
    pthread_mutex_lock(&g_wq_lock);
    pthread_cond_broadcast(&g_wq_cv);
    pthread_mutex_unlock(&g_wq_lock);
    pthread_join(g_wq_tid, NULL);

    if (g_connect != IO_OBJECT_NULL) {
        if (getenv("GP108_KEEP_D0_ON_EXIT") == NULL) {
            uint64_t off_act = kGP108PowerOff;
            IOConnectCallScalarMethod(g_connect, kGP108MethodPowerControl, &off_act, 1, NULL, NULL);
            fprintf(stderr, "[gp108-rm] Entered ACPI _OFF (D3cold 0W).\n");
        }
        IOServiceClose(g_connect);
        g_connect = IO_OBJECT_NULL;
    }
    if (g_service != IO_OBJECT_NULL) {
        IOObjectRelease(g_service);
        g_service = IO_OBJECT_NULL;
    }
}
