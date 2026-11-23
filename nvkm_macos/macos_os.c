/* SPDX-License-Identifier: MIT
 * macOS Userspace OS Layer for Nouveau NVKM/NVIF -> GP108Bridge.kext
 */
#include <IOKit/IOKitLib.h>
#include <IOKit/IODataQueueClient.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <nvif/os.h>
#include <core/device.h>
#include <core/pci.h>
#include <subdev/pci.h>
#include <nvif/client.h>
#include <nvif/device.h>
#include <nvif/driver.h>
#include <nvif/class.h>

#include "gp108_nvkm_macos.h"

/* ========================================================================= *
 * Global GP108Bridge Connection & Mapped Hardware State                     *
 * ========================================================================= */
#define GP108_DEFAULT_FW_DIR   "/Users/maple/projects/temp/mx250-macos/firmware"
#define GP108_DEFAULT_DUMP_DIR "/Users/maple/projects/temp/mx250-macos/hw_dump"

#define DMA_SLAB_PAGES   8192U                    /* 32 MB default per slab */
#define MAX_DMA_SLABS    128U
#define MAX_COH_ALLOCS   128U
#define MAX_VMAP_ENTRIES 4096U

struct gp108_dma_slab {
    bool         active;
    uint32_t     dma_slot;
    uint8_t     *base_va;
    size_t       size_bytes;
    uint32_t     total_pages;
    uint32_t     free_count;
    uint64_t    *free_bmap; /* 1 = free, 0 = used */
    struct page *pages;
};

struct gp108_coh_alloc {
    bool        used;
    uint32_t    dma_slot;
    void       *map_base_va;
    size_t      map_size;
    void       *ret_va;
    dma_addr_t  dma_addr;
};

struct gp108_vmap_entry {
    bool          used;
    bool          owns_pages;
    bool          direct_slab;
    uintptr_t     base_va;
    unsigned int  count;
    struct page **pages;
};

static pthread_mutex_t      g_os_lock   = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t      g_mem_lock  = PTHREAD_MUTEX_INITIALIZER;
static io_service_t         g_service   = IO_OBJECT_NULL;
static io_connect_t         g_connect   = IO_OBJECT_NULL;
static bool                 g_opened    = false;

static struct GP108DeviceInfo g_dev_info;
static mach_vm_address_t      g_bar_va[3];
static mach_vm_size_t         g_bar_sz[3];

static struct pci_bus         g_pci_bus;
static struct pci_dev         g_bridge_pdev;
static struct pci_dev         g_gpu_pdev;

/* MSI Event Queue & IRQ Thread */
static mach_vm_address_t      g_evq_va    = 0;
static mach_vm_size_t         g_evq_sz    = 0;
static IODataQueueMemory     *g_evq_mem   = NULL;
static mach_port_t            g_evq_port  = MACH_PORT_NULL;
static pthread_t              g_irq_tid;
static volatile bool          g_irq_running = false;
static pthread_mutex_t        g_irq_exec_lock = PTHREAD_MUTEX_INITIALIZER;
static irq_handler_t          g_irq_handler = NULL;
static void                  *g_irq_dev_id  = NULL;
static int                    g_irq_num     = 16;

/* Memory Pools */
static struct gp108_dma_slab   g_slabs[MAX_DMA_SLABS];
static struct gp108_coh_alloc  g_coh_allocs[MAX_COH_ALLOCS];
static struct gp108_vmap_entry g_vmaps[MAX_VMAP_ENTRIES];

/* ========================================================================= *
 * Low-Level GP108Bridge Kext Helpers                                        *
 * ========================================================================= */
static int bridge_query_phys_pages(bool is_pinned, uint32_t slot,
                                   uint64_t total_pages, uint64_t *out_phys)
{
    uint64_t fetched = 0;
    while (fetched < total_pages) {
        uint64_t batch = total_pages - fetched;
        if (batch > kGP108MaxPagesPerQuery)
            batch = kGP108MaxPagesPerQuery;

        uint64_t in_scalars[4] = {
            is_pinned ? 1ULL : 0ULL,
            slot,
            fetched,
            batch,
        };
        uint64_t out_scalars[1] = { 0 };
        uint32_t out_cnt = 1;
        size_t struct_sz = (size_t)(batch * sizeof(uint64_t));

        kern_return_t kr = IOConnectCallMethod(
            g_connect, kGP108MethodGetPhysPages,
            in_scalars, 4, NULL, 0,
            out_scalars, &out_cnt,
            &out_phys[fetched], &struct_sz);
        if (kr != KERN_SUCCESS || out_scalars[0] == 0)
            return -EIO;

        fetched += out_scalars[0];
    }
    return 0;
}

static u8 find_pcie_cap_offset(const struct pci_dev *dev)
{
    u16 status = 0;
    if (macos_pci_read_config_word(dev, 0x06, &status) != 0)
        return 0;
    if (!(status & 0x0010)) /* PCI_STATUS_CAP_LIST */
        return 0;

    u8 pos = 0;
    if (macos_pci_read_config_byte(dev, 0x34, &pos) != 0)
        return 0;

    for (int ttl = 0; ttl < 48 && pos >= 0x40; ttl++) {
        pos &= ~3U;
        u8 id = 0, next = 0;
        if (macos_pci_read_config_byte(dev, pos, &id) != 0)
            break;
        if (id == 0xff)
            break;
        if (id == PCI_CAP_ID_EXP)
            return pos;
        if (macos_pci_read_config_byte(dev, pos + 1, &next) != 0)
            break;
        pos = next;
    }
    return 0;
}

int gp108_bridge_get_info(struct GP108DeviceInfo *out_info)
{
    if (!g_opened || !out_info)
        return -ENODEV;
    size_t sz = sizeof(*out_info);
    kern_return_t kr = IOConnectCallStructMethod(
        g_connect, kGP108MethodGetInfo, NULL, 0, out_info, &sz);
    return (kr == KERN_SUCCESS) ? 0 : -EIO;
}

volatile void *gp108_bridge_bar_virt(uint32_t bar_slot, uint64_t *out_size)
{
    if (!g_opened || bar_slot >= 3)
        return NULL;
    if (out_size)
        *out_size = g_bar_sz[bar_slot];
    return (volatile void *)(uintptr_t)g_bar_va[bar_slot];
}

int gp108_bridge_open(struct pci_dev **out_pdev, struct GP108DeviceInfo *out_info)
{
    pthread_mutex_lock(&g_os_lock);
    if (g_opened) {
        if (out_pdev) *out_pdev = &g_gpu_pdev;
        if (out_info) *out_info = g_dev_info;
        pthread_mutex_unlock(&g_os_lock);
        return 0;
    }

    g_service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching(kGP108BridgeServiceName));
    if (g_service == IO_OBJECT_NULL) {
        fprintf(stderr, "[gp108_os] GP108Bridge.kext service not found\n");
        pthread_mutex_unlock(&g_os_lock);
        return -ENODEV;
    }

    kern_return_t kr = IOServiceOpen(g_service, mach_task_self(), 0, &g_connect);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[gp108_os] IOServiceOpen failed: 0x%x\n", kr);
        IOObjectRelease(g_service);
        g_service = IO_OBJECT_NULL;
        pthread_mutex_unlock(&g_os_lock);
        return -EACCES;
    }

    size_t info_sz = sizeof(g_dev_info);
    kr = IOConnectCallStructMethod(
        g_connect, kGP108MethodGetInfo, NULL, 0, &g_dev_info, &info_sz);
    if (kr != KERN_SUCCESS || g_dev_info.magic != kGP108BridgeMagic) {
        fprintf(stderr, "[gp108_os] GetInfo failed: kr=0x%x magic=0x%x\n",
                kr, g_dev_info.magic);
        IOServiceClose(g_connect);
        IOObjectRelease(g_service);
        g_connect = IO_OBJECT_NULL;
        g_service = IO_OBJECT_NULL;
        pthread_mutex_unlock(&g_os_lock);
        return -EIO;
    }

    /* Ensure GPU is powered ON in D0 */
    if (!g_dev_info.power_state) {
        uint64_t act = kGP108PowerOn;
        IOConnectCallScalarMethod(g_connect, kGP108MethodPowerControl, &act, 1, NULL, NULL);
        info_sz = sizeof(g_dev_info);
        IOConnectCallStructMethod(g_connect, kGP108MethodGetInfo, NULL, 0, &g_dev_info, &info_sz);
    }

    /* Map BAR0 (UC), BAR1 (WC), BAR3 (UC) */
    const uint32_t map_types[3] = { kGP108MapBAR0, kGP108MapBAR1, kGP108MapBAR3 };
    for (int i = 0; i < 3; i++) {
        g_bar_va[i] = 0;
        g_bar_sz[i] = 0;
        if (g_dev_info.bar_size[i] == 0)
            continue;
        kr = IOConnectMapMemory64(
            g_connect, map_types[i], mach_task_self(),
            &g_bar_va[i], &g_bar_sz[i], kIOMapAnywhere);
        if (kr != KERN_SUCCESS || !g_bar_va[i]) {
            fprintf(stderr, "[gp108_os] Failed to map BAR slot %d: 0x%x\n", i, kr);
            pthread_mutex_unlock(&g_os_lock);
            gp108_bridge_close();
            return -ENOMEM;
        }
    }

    /* Synthesize Linux PCI bus, upstream RP05 bridge, and MX250 pci_dev */
    memset(&g_pci_bus, 0, sizeof(g_pci_bus));
    memset(&g_bridge_pdev, 0, sizeof(g_bridge_pdev));
    memset(&g_gpu_pdev, 0, sizeof(g_gpu_pdev));

    g_opened = true; /* Enable PCI config helpers */

    g_bridge_pdev.vendor  = 0x8086;
    g_bridge_pdev.is_pcie = true;
    g_bridge_pdev.pcie_cap = find_pcie_cap_offset(&g_bridge_pdev);

    g_pci_bus.number = g_dev_info.bus;
    g_pci_bus.domain = 0;
    g_pci_bus.max_bus_speed = PCIE_SPEED_8_0GT;
    g_pci_bus.self = &g_bridge_pdev;

    g_gpu_pdev.dev.parent = &g_bridge_pdev.dev;
    g_gpu_pdev.dev.dma_mask_val = DMA_BIT_MASK(40);
    g_gpu_pdev.dev.dma_mask = &g_gpu_pdev.dev.dma_mask_val;
    g_gpu_pdev.dev.coherent_dma_mask = DMA_BIT_MASK(40);
    g_gpu_pdev.bus = &g_pci_bus;
    g_gpu_pdev.devfn = ((unsigned int)g_dev_info.device << 3) | g_dev_info.function;
    g_gpu_pdev.vendor = g_dev_info.vendor_id;
    g_gpu_pdev.device = g_dev_info.device_id;
    g_gpu_pdev.subsystem_vendor = g_dev_info.subsys_vendor_id;
    g_gpu_pdev.subsystem_device = g_dev_info.subsys_id;
    g_gpu_pdev.class = g_dev_info.class_code;
    g_gpu_pdev.revision = g_dev_info.revision_id;
    g_gpu_pdev.irq = 16;
    g_gpu_pdev.is_pcie = true;
    g_gpu_pdev.pcie_cap = find_pcie_cap_offset(&g_gpu_pdev);

    /* Populate Linux PCI resources: [0]=BAR0 (32-bit), [1]=BAR1 (64-bit), [3]=BAR3 (64-bit) */
    g_gpu_pdev.resource[0].start = g_dev_info.bar_phys[0];
    g_gpu_pdev.resource[0].end   = g_dev_info.bar_phys[0] + g_dev_info.bar_size[0] - 1;
    g_gpu_pdev.resource[0].flags = IORESOURCE_MEM;

    g_gpu_pdev.resource[1].start = g_dev_info.bar_phys[1];
    g_gpu_pdev.resource[1].end   = g_dev_info.bar_phys[1] + g_dev_info.bar_size[1] - 1;
    g_gpu_pdev.resource[1].flags = IORESOURCE_MEM | IORESOURCE_PREFETCH | IORESOURCE_MEM_64;

    g_gpu_pdev.resource[3].start = g_dev_info.bar_phys[2];
    g_gpu_pdev.resource[3].end   = g_dev_info.bar_phys[2] + g_dev_info.bar_size[2] - 1;
    g_gpu_pdev.resource[3].flags = IORESOURCE_MEM | IORESOURCE_MEM_64;

    if (out_pdev) *out_pdev = &g_gpu_pdev;
    if (out_info) *out_info = g_dev_info;
    pthread_mutex_unlock(&g_os_lock);
    return 0;
}

void gp108_bridge_close(void)
{
    if (g_irq_running)
        macos_free_irq((unsigned int)g_irq_num, g_irq_dev_id);

    pthread_mutex_lock(&g_mem_lock);
    for (uint32_t i = 0; i < MAX_VMAP_ENTRIES; i++) {
        if (g_vmaps[i].used) {
            if (!g_vmaps[i].direct_slab) {
                mach_vm_deallocate(mach_task_self(), g_vmaps[i].base_va,
                                   (mach_vm_size_t)g_vmaps[i].count * 4096ULL);
            }
            free(g_vmaps[i].pages);
            memset(&g_vmaps[i], 0, sizeof(g_vmaps[i]));
        }
    }
    for (uint32_t i = 0; i < MAX_COH_ALLOCS; i++) {
        if (g_coh_allocs[i].used && g_connect != IO_OBJECT_NULL) {
            IOConnectUnmapMemory64(g_connect, kGP108MapDMABase + g_coh_allocs[i].dma_slot,
                                   mach_task_self(), (mach_vm_address_t)g_coh_allocs[i].map_base_va);
            uint64_t slot = g_coh_allocs[i].dma_slot;
            IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &slot, 1, NULL, NULL);
            memset(&g_coh_allocs[i], 0, sizeof(g_coh_allocs[i]));
        }
    }
    for (uint32_t i = 0; i < MAX_DMA_SLABS; i++) {
        if (g_slabs[i].active) {
            if (g_connect != IO_OBJECT_NULL) {
                IOConnectUnmapMemory64(g_connect, kGP108MapDMABase + g_slabs[i].dma_slot,
                                       mach_task_self(), (mach_vm_address_t)g_slabs[i].base_va);
                uint64_t slot = g_slabs[i].dma_slot;
                IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &slot, 1, NULL, NULL);
            }
            free(g_slabs[i].free_bmap);
            free(g_slabs[i].pages);
            memset(&g_slabs[i], 0, sizeof(g_slabs[i]));
        }
    }
    pthread_mutex_unlock(&g_mem_lock);

    pthread_mutex_lock(&g_os_lock);
    const uint32_t map_types[3] = { kGP108MapBAR0, kGP108MapBAR1, kGP108MapBAR3 };
    for (int i = 0; i < 3; i++) {
        if (g_bar_va[i] && g_connect != IO_OBJECT_NULL) {
            IOConnectUnmapMemory64(g_connect, map_types[i], mach_task_self(), g_bar_va[i]);
            g_bar_va[i] = 0;
            g_bar_sz[i] = 0;
        }
    }
    if (g_connect != IO_OBJECT_NULL) {
        IOServiceClose(g_connect);
        g_connect = IO_OBJECT_NULL;
    }
    if (g_service != IO_OBJECT_NULL) {
        IOObjectRelease(g_service);
        g_service = IO_OBJECT_NULL;
    }
    g_opened = false;
    pthread_mutex_unlock(&g_os_lock);
}

/* ========================================================================= *
 * 1. MMIO Mapping (ioremap / ioremap_wc / iounmap)                          *
 * ========================================================================= */
void __iomem *macos_ioremap(phys_addr_t phys, size_t size, bool wc)
{
    (void)wc;
    if (!g_opened || phys == 0 || size == 0)
        return NULL;

    for (int i = 0; i < 3; i++) {
        uint64_t start = g_dev_info.bar_phys[i];
        uint64_t len   = g_dev_info.bar_size[i];
        if (len > 0 && phys >= start && (phys + size) <= (start + len)) {
            return (void __iomem *)((uint8_t *)(uintptr_t)g_bar_va[i] + (phys - start));
        }
    }
    fprintf(stderr, "[gp108_os] macos_ioremap: phys 0x%llx (size 0x%zx) outside BARs!\n",
            (unsigned long long)phys, size);
    return NULL;
}

void macos_iounmap(volatile void __iomem *addr)
{
    /* BAR0/BAR1/BAR3 remain mapped for the session lifetime */
    (void)addr;
}

/* ========================================================================= *
 * 2. PCI Configuration Space Access                                         *
 * ========================================================================= */
static int bridge_pci_cfg(const struct pci_dev *dev, bool is_write,
                          uint32_t width, int where, uint32_t val_in, uint32_t *val_out)
{
    if (!g_opened || where < 0 || where >= 4096)
        return -EINVAL;

    bool target_bridge = (dev == &g_bridge_pdev);
    /* Align unaligned word/byte accesses cleanly if needed, though GP108Bridge handles aligned */
    uint64_t in_scalars[5] = {
        is_write ? 1ULL : 0ULL,
        target_bridge ? 1ULL : 0ULL,
        width,
        (uint32_t)where,
        val_in,
    };
    uint64_t out_scalars[1] = { 0 };
    uint32_t out_cnt = 1;

    kern_return_t kr = IOConnectCallScalarMethod(
        g_connect, kGP108MethodPCIConfig, in_scalars, 5, out_scalars, &out_cnt);
    if (kr != KERN_SUCCESS) {
        if (is_write && target_bridge)
            return 0; /* Bridge config writes are read-only by policy; ignore benignly */
        return -EIO;
    }
    if (val_out)
        *val_out = (uint32_t)out_scalars[0];
    return 0;
}

int macos_pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{
    uint32_t out = 0;
    int r = bridge_pci_cfg(dev, false, 1, where, 0, &out);
    if (!r && val) *val = (u8)(out & 0xffU);
    return r;
}

int macos_pci_read_config_word(const struct pci_dev *dev, int where, u16 *val)
{
    uint32_t out = 0;
    int r = bridge_pci_cfg(dev, false, 2, where, 0, &out);
    if (!r && val) *val = (u16)(out & 0xffffU);
    return r;
}

int macos_pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val)
{
    uint32_t out = 0;
    int r = bridge_pci_cfg(dev, false, 4, where, 0, &out);
    if (!r && val) *val = out;
    return r;
}

int macos_pci_write_config_byte(const struct pci_dev *dev, int where, u8 val)
{
    return bridge_pci_cfg(dev, true, 1, where, val, NULL);
}

int macos_pci_write_config_word(const struct pci_dev *dev, int where, u16 val)
{
    return bridge_pci_cfg(dev, true, 2, where, val, NULL);
}

int macos_pci_write_config_dword(const struct pci_dev *dev, int where, u32 val)
{
    return bridge_pci_cfg(dev, true, 4, where, val, NULL);
}

/* ========================================================================= *
 * 3. Firmware Loader (nvidia/gp108/* + mx250_vbios.rom)                     *
 * ========================================================================= */
int macos_request_firmware(const struct firmware **fw_out, const char *name, struct device *device)
{
    (void)device;
    if (!fw_out || !name || !name[0])
        return -EINVAL;
    *fw_out = NULL;

    char candidates[6][512];
    int ncand = 0;

    if (name[0] == '/') {
        snprintf(candidates[ncand++], 512, "%s", name);
    } else {
        const char *env_fw = getenv("GP108_FIRMWARE_DIR");
        if (env_fw && env_fw[0])
            snprintf(candidates[ncand++], 512, "%s/%s", env_fw, name);
        snprintf(candidates[ncand++], 512, "/Library/Application Support/NVIDIA-GP108/firmware/%s", name);
        snprintf(candidates[ncand++], 512, "/usr/local/share/nvidia-gp108/firmware/%s", name);
        snprintf(candidates[ncand++], 512, "%s/%s", GP108_DEFAULT_FW_DIR, name);
        snprintf(candidates[ncand++], 512, "%s/%s", GP108_DEFAULT_DUMP_DIR, name);
    }

    int fd = -1;
    struct stat st;
    for (int i = 0; i < ncand; i++) {
        fd = open(candidates[i], O_RDONLY);
        if (fd >= 0) {
            if (fstat(fd, &st) == 0 && st.st_size > 0)
                break;
            close(fd);
            fd = -1;
        }
    }
    if (fd < 0)
        return -ENOENT;

    size_t sz = (size_t)st.st_size;
    u8 *buf = malloc(sz);
    struct firmware *fw = calloc(1, sizeof(*fw));
    if (!buf || !fw) {
        free(buf);
        free(fw);
        close(fd);
        return -ENOMEM;
    }

    size_t off = 0;
    while (off < sz) {
        ssize_t r = read(fd, buf + off, sz - off);
        if (r <= 0) {
            free(buf);
            free(fw);
            close(fd);
            return -EIO;
        }
        off += (size_t)r;
    }
    close(fd);

    fw->data = buf;
    fw->size = sz;
    *fw_out = fw;
    return 0;
}

void macos_release_firmware(const struct firmware *fw)
{
    if (fw) {
        free((void *)fw->data);
        free((void *)fw);
    }
}

/* ========================================================================= *
 * 4. 40-bit DMA Slab Pool, Contiguous DMA, Mach vmap & Pinning              *
 * ========================================================================= */
static int alloc_new_dma_slab_locked(uint32_t min_pages, uint32_t *out_slab_idx)
{
    int idx = -1;
    for (uint32_t i = 0; i < MAX_DMA_SLABS; i++) {
        if (!g_slabs[i].active) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0)
        return -ENOMEM;

    uint32_t slab_pages = max(min_pages, DMA_SLAB_PAGES);
    slab_pages = ALIGN(slab_pages, 64U);
    size_t slab_bytes = (size_t)slab_pages * 4096UL;

    uint64_t in_scalars[3] = { slab_bytes, kGP108CacheDefault, 40 };
    uint64_t out_scalars[4] = { 0 };
    uint32_t out_cnt = 4;
    kern_return_t kr = IOConnectCallScalarMethod(
        g_connect, kGP108MethodAllocDMA, in_scalars, 3, out_scalars, &out_cnt);
    if (kr != KERN_SUCCESS)
        return -ENOMEM;

    uint32_t dma_slot = (uint32_t)out_scalars[0];
    mach_vm_address_t map_va = 0;
    mach_vm_size_t map_sz = 0;
    kr = IOConnectMapMemory64(
        g_connect, kGP108MapDMABase + dma_slot, mach_task_self(),
        &map_va, &map_sz, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || !map_va) {
        uint64_t s = dma_slot;
        IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
        return -ENOMEM;
    }

    uint64_t *phys_list = calloc(slab_pages, sizeof(uint64_t));
    uint64_t *free_bmap = calloc(slab_pages / 64U, sizeof(uint64_t));
    struct page *pages  = calloc(slab_pages, sizeof(struct page));
    if (!phys_list || !free_bmap || !pages ||
        bridge_query_phys_pages(false, dma_slot, slab_pages, phys_list) != 0) {
        free(phys_list);
        free(free_bmap);
        free(pages);
        IOConnectUnmapMemory64(g_connect, kGP108MapDMABase + dma_slot, mach_task_self(), map_va);
        uint64_t s = dma_slot;
        IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
        return -EIO;
    }

    struct gp108_dma_slab *slab = &g_slabs[idx];
    memset(slab, 0, sizeof(*slab));
    slab->active      = true;
    slab->dma_slot    = dma_slot;
    slab->base_va     = (uint8_t *)(uintptr_t)map_va;
    slab->size_bytes  = slab_bytes;
    slab->total_pages = slab_pages;
    slab->free_count  = slab_pages;
    slab->free_bmap   = free_bmap;
    slab->pages       = pages;
    memset(slab->free_bmap, 0xff, (size_t)(slab_pages / 64U) * sizeof(uint64_t));

    for (uint32_t p = 0; p < slab_pages; p++) {
        slab->pages[p].virt     = slab->base_va + (size_t)p * 4096UL;
        slab->pages[p].phys     = phys_list[p];
        slab->pages[p].pool_idx = (uint32_t)idx;
        slab->pages[p].page_idx = p;
    }
    free(phys_list);

    *out_slab_idx = (uint32_t)idx;
    return 0;
}

static bool slab_alloc_run_locked(struct gp108_dma_slab *slab, uint32_t npages, uint32_t *out_start)
{
    if (!slab->active || slab->free_count < npages)
        return false;

    uint32_t run = 0, start = 0;
    for (uint32_t p = 0; p < slab->total_pages; p++) {
        uint32_t w = p >> 6;
        if (slab->free_bmap[w] == 0) {
            run = 0;
            p = (w << 6) + 63U;
            continue;
        }
        if (slab->free_bmap[w] & (1ULL << (p & 63U))) {
            if (run == 0)
                start = p;
            if (++run == npages) {
                for (uint32_t k = start; k < start + npages; k++)
                    slab->free_bmap[k >> 6] &= ~(1ULL << (k & 63U));
                slab->free_count -= npages;
                *out_start = start;
                return true;
            }
        } else {
            run = 0;
        }
    }
    return false;
}

int macos_alloc_pages_bulk(gfp_t gfp, unsigned int npages, struct page **out_pages)
{
    if (npages == 0 || !out_pages)
        return -EINVAL;

    pthread_mutex_lock(&g_mem_lock);
    struct gp108_dma_slab *chosen = NULL;
    uint32_t start = 0;

    for (uint32_t i = 0; i < MAX_DMA_SLABS; i++) {
        if (slab_alloc_run_locked(&g_slabs[i], npages, &start)) {
            chosen = &g_slabs[i];
            break;
        }
    }

    if (!chosen) {
        uint32_t new_idx = 0;
        if (alloc_new_dma_slab_locked(npages, &new_idx) != 0 ||
            !slab_alloc_run_locked(&g_slabs[new_idx], npages, &start)) {
            pthread_mutex_unlock(&g_mem_lock);
            return -ENOMEM;
        }
        chosen = &g_slabs[new_idx];
    }

    if (gfp & __GFP_ZERO)
        memset(chosen->pages[start].virt, 0, (size_t)npages * 4096UL);

    for (unsigned int i = 0; i < npages; i++)
        out_pages[i] = &chosen->pages[start + i];

    pthread_mutex_unlock(&g_mem_lock);
    return 0;
}

struct page *macos_alloc_page(gfp_t gfp)
{
    struct page *pg = NULL;
    if (macos_alloc_pages_bulk(gfp, 1U, &pg) != 0)
        return NULL;
    return pg;
}

void macos_free_page(struct page *p)
{
    if (!p)
        return;
    pthread_mutex_lock(&g_mem_lock);
    if (p->pool_idx < MAX_DMA_SLABS) {
        struct gp108_dma_slab *slab = &g_slabs[p->pool_idx];
        if (slab->active && p->page_idx < slab->total_pages &&
            &slab->pages[p->page_idx] == p) {
            uint32_t w   = p->page_idx >> 6;
            uint32_t bit = p->page_idx & 63U;
            if (!(slab->free_bmap[w] & (1ULL << bit))) {
                slab->free_bmap[w] |= (1ULL << bit);
                slab->free_count++;
            }
            /* Reclaim completely idle extra or oversized slabs to conserve IOKit DMA slots */
            if (slab->free_count == slab->total_pages && g_connect != IO_OBJECT_NULL) {
                uint32_t active_default = 0;
                for (uint32_t i = 0; i < MAX_DMA_SLABS; i++) {
                    if (g_slabs[i].active && g_slabs[i].total_pages == DMA_SLAB_PAGES)
                        active_default++;
                }
                if (slab->total_pages > DMA_SLAB_PAGES || active_default > 1) {
                    IOConnectUnmapMemory64(g_connect, kGP108MapDMABase + slab->dma_slot,
                                           mach_task_self(), (mach_vm_address_t)slab->base_va);
                    uint64_t slot = slab->dma_slot;
                    IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &slot, 1, NULL, NULL);
                    free(slab->free_bmap);
                    free(slab->pages);
                    memset(slab, 0, sizeof(*slab));
                }
            }
        }
    }
    pthread_mutex_unlock(&g_mem_lock);
}

void *macos_dma_alloc(size_t size, dma_addr_t *dma_handle, bool coherent)
{
    (void)coherent;
    if (!g_opened || size == 0 || !dma_handle)
        return NULL;

    size_t req_sz = ALIGN(size, 4096UL);
    size_t req_pages = req_sz / 4096UL;

    pthread_mutex_lock(&g_mem_lock);
    int cidx = -1;
    for (uint32_t i = 0; i < MAX_COH_ALLOCS; i++) {
        if (!g_coh_allocs[i].used) {
            cidx = (int)i;
            break;
        }
    }
    if (cidx < 0) {
        pthread_mutex_unlock(&g_mem_lock);
        return NULL;
    }

    /* <= 64 KB with maskBits=32 triggers kIOMemoryPhysicallyContiguous in GP108Bridge */
    size_t alloc_sz   = (req_sz <= 65536UL) ? req_sz : max(req_sz * 4UL, req_sz + (2UL << 20));
    uint32_t maskbits = (req_sz <= 65536UL) ? 32U : 40U;

    uint64_t in_scalars[3] = { alloc_sz, kGP108CacheDefault, maskbits };
    uint64_t out_scalars[4] = { 0 };
    uint32_t out_cnt = 4;
    kern_return_t kr = IOConnectCallScalarMethod(
        g_connect, kGP108MethodAllocDMA, in_scalars, 3, out_scalars, &out_cnt);
    if (kr != KERN_SUCCESS) {
        pthread_mutex_unlock(&g_mem_lock);
        return NULL;
    }

    uint32_t dma_slot = (uint32_t)out_scalars[0];
    size_t total_pages = (size_t)out_scalars[2];
    mach_vm_address_t map_va = 0;
    mach_vm_size_t map_sz = 0;
    kr = IOConnectMapMemory64(
        g_connect, kGP108MapDMABase + dma_slot, mach_task_self(),
        &map_va, &map_sz, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || !map_va) {
        uint64_t s = dma_slot;
        IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
        pthread_mutex_unlock(&g_mem_lock);
        return NULL;
    }

    uint64_t *phys_arr = calloc(total_pages, sizeof(uint64_t));
    if (!phys_arr || bridge_query_phys_pages(false, dma_slot, total_pages, phys_arr) != 0) {
        free(phys_arr);
        IOConnectUnmapMemory64(g_connect, kGP108MapDMABase + dma_slot, mach_task_self(), map_va);
        uint64_t s = dma_slot;
        IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
        pthread_mutex_unlock(&g_mem_lock);
        return NULL;
    }

    /* Locate physically contiguous run of req_pages */
    ssize_t run_start = -1;
    for (size_t s = 0; s + req_pages <= total_pages; s++) {
        bool ok = true;
        for (size_t j = 1; j < req_pages; j++) {
            if (phys_arr[s + j] != phys_arr[s] + j * 4096ULL) {
                ok = false;
                s += j - 1;
                break;
            }
        }
        if (ok) {
            run_start = (ssize_t)s;
            break;
        }
    }

    if (run_start < 0) {
        fprintf(stderr, "[gp108_os] macos_dma_alloc: failed to find %zu contiguous pages\n", req_pages);
        free(phys_arr);
        IOConnectUnmapMemory64(g_connect, kGP108MapDMABase + dma_slot, mach_task_self(), map_va);
        uint64_t s = dma_slot;
        IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
        pthread_mutex_unlock(&g_mem_lock);
        return NULL;
    }

    void *ret_va = (uint8_t *)(uintptr_t)map_va + (size_t)run_start * 4096UL;
    dma_addr_t ret_dma = phys_arr[run_start];
    free(phys_arr);

    memset(ret_va, 0, req_sz);

    g_coh_allocs[cidx].used        = true;
    g_coh_allocs[cidx].dma_slot    = dma_slot;
    g_coh_allocs[cidx].map_base_va = (void *)(uintptr_t)map_va;
    g_coh_allocs[cidx].map_size    = (size_t)map_sz;
    g_coh_allocs[cidx].ret_va      = ret_va;
    g_coh_allocs[cidx].dma_addr    = ret_dma;

    *dma_handle = ret_dma;
    pthread_mutex_unlock(&g_mem_lock);
    return ret_va;
}

void macos_dma_free(size_t size, void *cpu_addr, dma_addr_t dma_handle)
{
    (void)size; (void)dma_handle;
    if (!cpu_addr)
        return;

    pthread_mutex_lock(&g_mem_lock);
    for (uint32_t i = 0; i < MAX_COH_ALLOCS; i++) {
        if (g_coh_allocs[i].used && g_coh_allocs[i].ret_va == cpu_addr) {
            IOConnectUnmapMemory64(
                g_connect, kGP108MapDMABase + g_coh_allocs[i].dma_slot,
                mach_task_self(), (mach_vm_address_t)g_coh_allocs[i].map_base_va);
            uint64_t s = g_coh_allocs[i].dma_slot;
            IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
            memset(&g_coh_allocs[i], 0, sizeof(g_coh_allocs[i]));
            break;
        }
    }
    pthread_mutex_unlock(&g_mem_lock);
}

static void *vmap_internal(struct page **pages, unsigned int count, bool owns_pages)
{
    if (!pages || count == 0 || !pages[0] || !pages[0]->virt)
        return NULL;

    bool direct_slab = true;
    uint8_t *first_va = (uint8_t *)pages[0]->virt;
    for (unsigned int i = 1; i < count; i++) {
        if (!pages[i] || (uint8_t *)pages[i]->virt != first_va + (size_t)i * 4096UL) {
            direct_slab = false;
            break;
        }
    }

    mach_vm_address_t base_va = 0;
    mach_vm_size_t total_sz = (mach_vm_size_t)count * 4096ULL;

    if (direct_slab) {
        base_va = (mach_vm_address_t)(uintptr_t)first_va;
    } else {
        kern_return_t kr = mach_vm_allocate(mach_task_self(), &base_va, total_sz, VM_FLAGS_ANYWHERE);
        if (kr != KERN_SUCCESS || !base_va)
            return NULL;

        for (unsigned int i = 0; i < count; i++) {
            mach_vm_address_t dst_va = base_va + (mach_vm_address_t)i * 4096ULL;
            vm_prot_t cur_prot = 0, max_prot = 0;
            kr = mach_vm_remap(
                mach_task_self(), &dst_va, 4096ULL, 0,
                VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                mach_task_self(), (mach_vm_address_t)pages[i]->virt,
                FALSE, &cur_prot, &max_prot, VM_INHERIT_NONE);
            if (kr != KERN_SUCCESS) {
                mach_vm_deallocate(mach_task_self(), base_va, total_sz);
                return NULL;
            }
        }
    }

    struct page **copy = malloc((size_t)count * sizeof(struct page *));
    if (!copy) {
        if (!direct_slab)
            mach_vm_deallocate(mach_task_self(), base_va, total_sz);
        return NULL;
    }
    memcpy(copy, pages, (size_t)count * sizeof(struct page *));

    pthread_mutex_lock(&g_mem_lock);
    int vidx = -1;
    for (uint32_t i = 0; i < MAX_VMAP_ENTRIES; i++) {
        if (!g_vmaps[i].used) {
            vidx = (int)i;
            break;
        }
    }
    if (vidx < 0) {
        pthread_mutex_unlock(&g_mem_lock);
        free(copy);
        if (!direct_slab)
            mach_vm_deallocate(mach_task_self(), base_va, total_sz);
        return NULL;
    }

    g_vmaps[vidx].used        = true;
    g_vmaps[vidx].owns_pages  = owns_pages;
    g_vmaps[vidx].direct_slab = direct_slab;
    g_vmaps[vidx].base_va     = (uintptr_t)base_va;
    g_vmaps[vidx].count       = count;
    g_vmaps[vidx].pages       = copy;
    pthread_mutex_unlock(&g_mem_lock);

    return (void *)(uintptr_t)base_va;
}

void *macos_vmap(struct page **pages, unsigned int count)
{
    return vmap_internal(pages, count, false);
}

void macos_vunmap(const void *addr)
{
    if (!addr)
        return;

    struct gp108_vmap_entry found;
    memset(&found, 0, sizeof(found));

    pthread_mutex_lock(&g_mem_lock);
    for (uint32_t i = 0; i < MAX_VMAP_ENTRIES; i++) {
        if (g_vmaps[i].used && g_vmaps[i].base_va == (uintptr_t)addr) {
            found = g_vmaps[i];
            memset(&g_vmaps[i], 0, sizeof(g_vmaps[i]));
            break;
        }
    }
    pthread_mutex_unlock(&g_mem_lock);

    if (found.used) {
        if (!found.direct_slab) {
            mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)found.base_va,
                               (mach_vm_size_t)found.count * 4096ULL);
        }
        if (found.owns_pages) {
            for (unsigned int p = 0; p < found.count; p++)
                macos_free_page(found.pages[p]);
        }
        free(found.pages);
    }
}

void *macos_vmalloc(size_t size)
{
    if (size == 0)
        size = 4096UL;
    unsigned int npages = (unsigned int)(ALIGN(size, 4096UL) / 4096UL);
    struct page **pages = calloc(npages, sizeof(struct page *));
    if (!pages)
        return NULL;

    if (macos_alloc_pages_bulk(GFP_KERNEL | __GFP_ZERO, npages, pages) != 0) {
        free(pages);
        return NULL;
    }

    void *va = vmap_internal(pages, npages, true);
    if (!va) {
        for (unsigned int i = 0; i < npages; i++)
            macos_free_page(pages[i]);
    }
    free(pages);
    return va;
}

void macos_vfree(const void *addr)
{
    if (!addr)
        return;

    bool was_vmap = false;
    pthread_mutex_lock(&g_mem_lock);
    for (uint32_t i = 0; i < MAX_VMAP_ENTRIES; i++) {
        if (g_vmaps[i].used && g_vmaps[i].base_va == (uintptr_t)addr) {
            was_vmap = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_mem_lock);

    if (was_vmap)
        macos_vunmap(addr);
    else
        free((void *)addr);
}

struct page *macos_vmalloc_to_page(const void *vmalloc_addr)
{
    uintptr_t va = (uintptr_t)vmalloc_addr;
    struct page *res = NULL;

    pthread_mutex_lock(&g_mem_lock);
    for (uint32_t i = 0; i < MAX_VMAP_ENTRIES; i++) {
        if (!g_vmaps[i].used)
            continue;
        uintptr_t start = g_vmaps[i].base_va;
        uintptr_t end   = start + (uintptr_t)g_vmaps[i].count * 4096UL;
        if (va >= start && va < end) {
            res = g_vmaps[i].pages[(va - start) >> 12];
            break;
        }
    }
    pthread_mutex_unlock(&g_mem_lock);
    return res;
}

/* ========================================================================= *
 * 5. Direct Pinning & Large Zero-Copy DMA BO Helpers (Phase 3 & 4)          *
 * ========================================================================= */
int gp108_bridge_pin_usermem(void *user_va, size_t size_bytes,
                             uint32_t *out_pin_slot, uint64_t **out_phys_pages,
                             size_t *out_num_pages)
{
    if (!g_opened || !user_va || size_bytes == 0 || !out_pin_slot || !out_phys_pages)
        return -EINVAL;

    size_t aligned_sz = ALIGN(size_bytes, 4096UL);
    uint64_t in_scalars[2] = { (uint64_t)(uintptr_t)user_va, aligned_sz };
    uint64_t out_scalars[3] = { 0 };
    uint32_t out_cnt = 3;

    kern_return_t kr = IOConnectCallScalarMethod(
        g_connect, kGP108MethodPinUserMem, in_scalars, 2, out_scalars, &out_cnt);
    if (kr != KERN_SUCCESS)
        return -EIO;

    uint32_t slot  = (uint32_t)out_scalars[0];
    size_t npages  = (size_t)out_scalars[1];
    uint64_t *phys = calloc(npages, sizeof(uint64_t));
    if (!phys || bridge_query_phys_pages(true, slot, npages, phys) != 0) {
        free(phys);
        uint64_t s = slot;
        IOConnectCallScalarMethod(g_connect, kGP108MethodUnpinUserMem, &s, 1, NULL, NULL);
        return -ENOMEM;
    }

    *out_pin_slot   = slot;
    *out_phys_pages = phys;
    if (out_num_pages) *out_num_pages = npages;
    return 0;
}

int gp108_bridge_unpin_usermem(uint32_t pin_slot)
{
    if (!g_opened)
        return -ENODEV;
    uint64_t s = pin_slot;
    kern_return_t kr = IOConnectCallScalarMethod(
        g_connect, kGP108MethodUnpinUserMem, &s, 1, NULL, NULL);
    return (kr == KERN_SUCCESS) ? 0 : -EIO;
}

int gp108_bridge_alloc_dma_pages(size_t size_bytes, uint32_t cache_mode, uint32_t mask_bits,
                                 uint32_t *out_dma_slot, void **out_user_va,
                                 uint64_t **out_phys_pages, size_t *out_num_pages)
{
    if (!g_opened || size_bytes == 0 || !out_dma_slot || !out_user_va || !out_phys_pages)
        return -EINVAL;

    size_t aligned_sz = ALIGN(size_bytes, 4096UL);
    uint64_t in_scalars[3] = { aligned_sz, cache_mode, mask_bits };
    uint64_t out_scalars[4] = { 0 };
    uint32_t out_cnt = 4;

    kern_return_t kr = IOConnectCallScalarMethod(
        g_connect, kGP108MethodAllocDMA, in_scalars, 3, out_scalars, &out_cnt);
    if (kr != KERN_SUCCESS)
        return -ENOMEM;

    uint32_t slot = (uint32_t)out_scalars[0];
    size_t npages = (size_t)out_scalars[2];
    mach_vm_address_t map_va = 0;
    mach_vm_size_t map_sz = 0;
    kr = IOConnectMapMemory64(
        g_connect, kGP108MapDMABase + slot, mach_task_self(),
        &map_va, &map_sz, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || !map_va) {
        uint64_t s = slot;
        IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
        return -ENOMEM;
    }

    uint64_t *phys = calloc(npages, sizeof(uint64_t));
    if (!phys || bridge_query_phys_pages(false, slot, npages, phys) != 0) {
        free(phys);
        IOConnectUnmapMemory64(g_connect, kGP108MapDMABase + slot, mach_task_self(), map_va);
        uint64_t s = slot;
        IOConnectCallScalarMethod(g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
        return -EIO;
    }

    *out_dma_slot   = slot;
    *out_user_va    = (void *)(uintptr_t)map_va;
    *out_phys_pages = phys;
    if (out_num_pages) *out_num_pages = npages;
    return 0;
}

int gp108_bridge_free_dma_pages(uint32_t dma_slot, void *user_va, size_t size_bytes)
{
    (void)size_bytes;
    if (!g_opened)
        return -ENODEV;
    if (user_va) {
        IOConnectUnmapMemory64(g_connect, kGP108MapDMABase + dma_slot,
                               mach_task_self(), (mach_vm_address_t)user_va);
    }
    uint64_t s = dma_slot;
    kern_return_t kr = IOConnectCallScalarMethod(
        g_connect, kGP108MethodFreeDMA, &s, 1, NULL, NULL);
    return (kr == KERN_SUCCESS) ? 0 : -EIO;
}

/* ========================================================================= *
 * 6. PCIe MSI Delivery Thread (IOSharedDataQueue -> nvkm_intr)              *
 * ========================================================================= */
static void *gp108_irq_thread_main(void *arg)
{
    (void)arg;
    struct {
        mach_msg_header_t hdr;
        uint8_t           pad[64];
    } msg;

    while (g_irq_running) {
        memset(&msg, 0, sizeof(msg));
        mach_msg(&msg.hdr,
                 MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                 0,
                 sizeof(msg),
                 g_evq_port,
                 15, /* 15 ms timeout for clean shutdown + coalesced safety check */
                 MACH_PORT_NULL);

        if (!g_irq_running)
            break;

        bool had_events = false;
        while (g_evq_mem && IODataQueueDataAvailable(g_evq_mem)) {
            struct GP108IrqEvent ev;
            uint32_t sz = sizeof(ev);
            if (IODataQueueDequeue(g_evq_mem, &ev, &sz) == kIOReturnSuccess)
                had_events = true;
            else
                break;
        }

        /* Invoke nvkm_intr whenever an MSI event arrived (or periodically if INTR_0 is non-zero) */
        bool need_dispatch = had_events;
        if (!need_dispatch && g_bar_va[0]) {
            volatile const uint32_t *bar0 = (volatile const uint32_t *)(uintptr_t)g_bar_va[0];
            if (bar0[0x100 / 4] != 0)
                need_dispatch = true;
        }

        if (need_dispatch && g_irq_handler) {
            pthread_mutex_lock(&g_irq_exec_lock);
            if (g_irq_handler)
                g_irq_handler(g_irq_num, g_irq_dev_id);
            pthread_mutex_unlock(&g_irq_exec_lock);
        }
    }
    return NULL;
}

int macos_request_irq(unsigned int irq, irq_handler_t handler,
                      unsigned long flags, const char *name, void *dev)
{
    (void)flags; (void)name;
    if (!g_opened || !handler)
        return -EINVAL;
    if (g_irq_running)
        return -EBUSY;

    /* Map IOSharedDataQueue */
    g_evq_va = 0;
    g_evq_sz = 0;
    kern_return_t kr = IOConnectMapMemory64(
        g_connect, kGP108MapEventQueue, mach_task_self(),
        &g_evq_va, &g_evq_sz, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || !g_evq_va)
        return -ENOMEM;
    g_evq_mem = (IODataQueueMemory *)(uintptr_t)g_evq_va;

    /* Allocate Mach notification port and bind to UserClient */
    g_evq_port = IODataQueueAllocateNotificationPort();
    if (g_evq_port == MACH_PORT_NULL) {
        IOConnectUnmapMemory64(g_connect, kGP108MapEventQueue, mach_task_self(), g_evq_va);
        g_evq_va = 0;
        g_evq_mem = NULL;
        return -ENOMEM;
    }
    IOConnectSetNotificationPort(g_connect, 0, g_evq_port, 0);

    /* Drain any stale events before enabling MSI */
    while (IODataQueueDataAvailable(g_evq_mem)) {
        struct GP108IrqEvent dummy;
        uint32_t sz = sizeof(dummy);
        if (IODataQueueDequeue(g_evq_mem, &dummy, &sz) != kIOReturnSuccess)
            break;
    }

    g_irq_num     = (int)irq;
    g_irq_handler = handler;
    g_irq_dev_id  = dev;
    g_irq_running = true;

    /* Enable MSI source in GP108Bridge.kext */
    uint64_t en = 1ULL;
    uint64_t out[2] = { 0 };
    uint32_t out_cnt = 2;
    IOConnectCallScalarMethod(g_connect, kGP108MethodInterruptCtrl, &en, 1, out, &out_cnt);

    if (pthread_create(&g_irq_tid, NULL, gp108_irq_thread_main, NULL) != 0) {
        g_irq_running = false;
        en = 0ULL;
        out_cnt = 2;
        IOConnectCallScalarMethod(g_connect, kGP108MethodInterruptCtrl, &en, 1, out, &out_cnt);
        IOConnectSetNotificationPort(g_connect, 0, MACH_PORT_NULL, 0);
        mach_port_destroy(mach_task_self(), g_evq_port);
        g_evq_port = MACH_PORT_NULL;
        IOConnectUnmapMemory64(g_connect, kGP108MapEventQueue, mach_task_self(), g_evq_va);
        g_evq_va = 0;
        g_evq_mem = NULL;
        return -ENOMEM;
    }

    return 0;
}

void macos_synchronize_irq(unsigned int irq)
{
    (void)irq;
    pthread_mutex_lock(&g_irq_exec_lock);
    pthread_mutex_unlock(&g_irq_exec_lock);
}

void macos_free_irq(unsigned int irq, void *dev_id)
{
    (void)irq; (void)dev_id;
    if (!g_irq_running)
        return;

    g_irq_running = false;
    pthread_join(g_irq_tid, NULL);

    pthread_mutex_lock(&g_irq_exec_lock);
    g_irq_handler = NULL;
    g_irq_dev_id  = NULL;
    pthread_mutex_unlock(&g_irq_exec_lock);

    if (g_connect != IO_OBJECT_NULL) {
        uint64_t dis = 0ULL;
        uint64_t out[2] = { 0 };
        uint32_t out_cnt = 2;
        IOConnectCallScalarMethod(g_connect, kGP108MethodInterruptCtrl, &dis, 1, out, &out_cnt);
        IOConnectSetNotificationPort(g_connect, 0, MACH_PORT_NULL, 0);
        if (g_evq_va) {
            IOConnectUnmapMemory64(g_connect, kGP108MapEventQueue, mach_task_self(), g_evq_va);
            g_evq_va = 0;
            g_evq_mem = NULL;
        }
    }
    if (g_evq_port != MACH_PORT_NULL) {
        mach_port_destroy(mach_task_self(), g_evq_port);
        g_evq_port = MACH_PORT_NULL;
    }
}

/* ========================================================================= *
 * 7. High-Level NVKM + NVIF Bootstrap                                       *
 * ========================================================================= */
extern const struct nvif_driver nvif_driver_nvkm;

int gp108_nvkm_bootstrap(const char *cfg, const char *dbg,
                         struct nvkm_device **out_nvkm_dev,
                         struct nvif_client *out_client,
                         struct nvif_device *out_device)
{
    struct pci_dev *pdev = NULL;
    int ret = gp108_bridge_open(&pdev, NULL);
    if (ret)
        return ret;

    const char *use_cfg = (cfg && cfg[0]) ? cfg : "NvBios=mx250_vbios.rom";
    const char *use_dbg = (dbg && dbg[0]) ? dbg : "info";

    struct nvkm_device *nvdev = NULL;
    ret = nvkm_device_pci_new(pdev, use_cfg, use_dbg, &nvdev);
    if (ret) {
        fprintf(stderr, "[gp108_os] nvkm_device_pci_new failed: %d\n", ret);
        return ret;
    }

    if (nvdev->pci)
        nvkm_pcie_set_link(nvdev->pci, NVKM_PCIE_SPEED_8_0, 4);

    if (out_client) {
        memset(out_client, 0, sizeof(*out_client));
        ret = nvif_driver_init(NULL, use_cfg, use_dbg, "gp108_macos",
                               nvdev->handle, out_client);
        if (ret) {
            fprintf(stderr, "[gp108_os] nvif_driver_init failed: %d\n", ret);
            nvkm_device_del(&nvdev);
            return ret;
        }
        if (out_device) {
            memset(out_device, 0, sizeof(*out_device));
            ret = nvif_device_ctor(out_client, "gp108Device", out_device);
            if (ret) {
                fprintf(stderr, "[gp108_os] nvif_device_ctor failed: %d\n", ret);
                nvif_client_dtor(out_client);
                nvkm_device_del(&nvdev);
                return ret;
            }
        }
    }
    if (out_nvkm_dev)
        *out_nvkm_dev = nvdev;
    return 0;
}

void gp108_nvkm_shutdown(struct nvkm_device **inout_nvkm_dev,
                         struct nvif_client *client,
                         struct nvif_device *device)
{
    if (device)
        nvif_device_dtor(device);
    if (client)
        nvif_client_dtor(client);
    if (inout_nvkm_dev && *inout_nvkm_dev)
        nvkm_device_del(inout_nvkm_dev);
    gp108_bridge_close();
}
