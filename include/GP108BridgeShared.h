#ifndef GP108_BRIDGE_SHARED_H
#define GP108_BRIDGE_SHARED_H

#include <stdint.h>

#define kGP108BridgeServiceName     "com_maple_driver_GP108Bridge"
#define kGP108BridgeUserClientName  "com_maple_driver_GP108BridgeUserClient"
#define kGP108BridgeMagic           0x47503138U /* 'GP18' */
#define kGP108BridgeABIVersion      1U

#define kGP108MaxDMASlots           256U
#define kGP108MaxPinnedSlots        128U
#define kGP108MaxVBIOSBytes         (256U * 1024U)
#define kGP108MaxChunkBytes         4096U
#define kGP108MaxPagesPerQuery      512U

/* IOConnectMapMemory64 memory types */
enum GP108MapType : uint32_t {
    kGP108MapBAR0        = 0,      /* 16 MB MMIO registers (uncached) */
    kGP108MapBAR1        = 1,      /* VRAM aperture (write-combine) */
    kGP108MapBAR3        = 2,      /* PRAMIN / instmem aperture (uncached) */
    kGP108MapEventQueue  = 3,      /* IOSharedDataQueue for MSI events */
    kGP108MapDMABase     = 0x1000, /* kGP108MapDMABase + slot maps DMA buffer */
};

/* IOConnectCallMethod selectors */
enum GP108MethodSelector : uint32_t {
    kGP108MethodGetInfo        = 0,
    kGP108MethodReadVBIOS      = 1,
    kGP108MethodPCIConfig      = 2,
    kGP108MethodAllocDMA       = 3,
    kGP108MethodFreeDMA        = 4,
    kGP108MethodGetPhysPages   = 5,
    kGP108MethodPinUserMem     = 6,
    kGP108MethodUnpinUserMem   = 7,
    kGP108MethodPowerControl   = 8,
    kGP108MethodInterruptCtrl  = 9,
    kGP108MethodReadACPITable  = 10,
    kGP108MethodCount          = 11,
};

/* Cache modes for kGP108MethodAllocDMA */
enum GP108CacheMode : uint32_t {
    kGP108CacheDefault      = 0, /* Coherent write-back (x86_64 PCIe snoop) */
    kGP108CacheWriteCombine = 1, /* Write-combining (pushbuffers / staging) */
    kGP108CacheUncached     = 2, /* Strongly uncached */
};

/* Power actions for kGP108MethodPowerControl */
enum GP108PowerAction : uint32_t {
    kGP108PowerOff          = 0, /* Mask IRQs, save PCI state, call ACPI _OFF */
    kGP108PowerOn           = 1, /* Call ACPI _ON, wait link, restore PCI state */
    kGP108PowerRefreshVBIOS = 2, /* Re-attempt ACPI _ROM / PROM VBIOS extraction */
};

/* VBIOS source indicator */
enum GP108VBIOSource : uint32_t {
    kGP108VBIOSNone     = 0,
    kGP108VBIOSAcpiROM  = 1, /* Extracted via ACPI _ROM method */
    kGP108VBIOSBar0PROM = 2, /* Extracted via BAR0 + 0x300000 PROM shadow */
    kGP108VBIOSBar0RAM  = 3, /* Extracted via BAR0 + 0x700000 PRAMIN */
};

struct __attribute__((aligned(8))) GP108DeviceInfo {
    uint32_t magic;
    uint32_t abi_version;

    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsys_vendor_id;
    uint16_t subsys_id;
    uint8_t  revision_id;
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;

    uint32_t class_code;          /* Raw 24-bit PCI class code from config 0x08 */
    uint16_t pci_command;
    uint16_t pcie_link_status;    /* From parent RP05 or endpoint PCIe cap */
    uint32_t pcie_link_caps;

    uint64_t bar_phys[3];         /* [0]=BAR0, [1]=BAR1, [2]=BAR3 */
    uint64_t bar_size[3];

    uint32_t power_state;         /* 1 = D0 powered ON, 0 = D3cold (_OFF) */
    uint32_t pmc_boot_0;          /* BAR0[0x000000] architecture ID */
    uint32_t pmc_enable;          /* BAR0[0x000200] engine enable mask */
    int32_t  temp_celsius;        /* BAR0[0x020460] die temp in C (-1 if N/A) */
    uint64_t vram_bytes_strap;    /* BAR0[0x100ce4] / [0x10f20c] VRAM size */

    uint32_t vbios_size;
    uint32_t vbios_source;
    uint64_t irq_count;
    uint32_t last_pmc_intr_0;
    int32_t  msi_interrupt_index;
    char     acpi_path[128];
};

struct __attribute__((aligned(8))) GP108IrqEvent {
    uint64_t seq;
    uint64_t timestamp_ns;
    uint32_t pmc_intr_0;
    uint32_t reserved;
};

#endif /* GP108_BRIDGE_SHARED_H */
