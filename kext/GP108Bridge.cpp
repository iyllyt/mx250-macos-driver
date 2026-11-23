#include "GP108Bridge.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOInterrupts.h>
#include <libkern/OSAtomic.h>
#include <mach/clock_types.h>
#include <mach/kmod.h>
#include <string.h>

#define LOG(fmt, ...) IOLog("[GP108Bridge] " fmt "\n", ##__VA_ARGS__)

/* NVIDIA Pascal MMIO register offsets in BAR0 */
static constexpr uint32_t NV_PMC_BOOT_0          = 0x000000;
static constexpr uint32_t NV_PMC_INTR_0          = 0x000100;
static constexpr uint32_t NV_PMC_INTR_EN_0       = 0x000140;
static constexpr uint32_t NV_PMC_INTR_EN_SET_0   = 0x000160;
static constexpr uint32_t NV_PMC_INTR_EN_CLR_0   = 0x000180;
static constexpr uint32_t NV_PMC_ENABLE          = 0x000200;
static constexpr uint32_t NV_THERM_TSENSOR       = 0x020460;
static constexpr uint32_t NV_PBUS_PCI_NV_20      = 0x088050;
static constexpr uint32_t NV_PFB_CSTATUS         = 0x100ce4;
static constexpr uint32_t NV_PFB_PRI_MMU_LOCAL   = 0x10f20c;
static constexpr uint32_t NV_PROM_OFFSET         = 0x300000;
static constexpr uint32_t NV_PRAMIN_OFFSET       = 0x700000;

/* ========================================================================= *
 * com_maple_driver_GP108Bridge                                              *
 * ========================================================================= */

#define super IOService
OSDefineMetaClassAndStructors(com_maple_driver_GP108Bridge, IOService)

IOService *com_maple_driver_GP108Bridge::probe(IOService *provider, SInt32 *score) {
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        return nullptr;
    }

    uint32_t vd = pci->configRead32(kIOPCIConfigVendorID);
    uint16_t vendor = static_cast<uint16_t>(vd & 0xFFFF);
    uint16_t device = static_cast<uint16_t>((vd >> 16) & 0xFFFF);
    uint32_t classRev = pci->configRead32(kIOPCIConfigRevisionID);
    uint8_t baseClass = static_cast<uint8_t>((classRev >> 24) & 0xFF);

    if (vendor != 0x10DE || baseClass != 0x03) {
        LOG("probe: skipping non-display NVIDIA function %04x:%04x (class=0x%06x)",
            vendor, device, classRev >> 8);
        return nullptr;
    }

    if (score) {
        *score = 100000;
    }
    LOG("probe: matched NVIDIA GPU %04x:%04x (raw class=0x%06x)",
        vendor, device, classRev >> 8);
    return super::probe(provider, score);
}

bool com_maple_driver_GP108Bridge::locateACPIDevices() {
    fACPIPath[0] = '\0';

    OSString *pathStr = OSDynamicCast(OSString, fPCIDevice->getProperty("acpi-path"));
    if (pathStr && pathStr->getCStringNoCopy()) {
        strlcpy(fACPIPath, pathStr->getCStringNoCopy(), sizeof(fACPIPath));
    }

    fACPIDevice = OSDynamicCast(IOACPIPlatformDevice, fPCIDevice->getProperty("acpi-device"));
    if (fACPIDevice) {
        fACPIDevice->retain();
    } else if (fACPIPath[0] != '\0') {
        IORegistryEntry *entry = IORegistryEntry::fromPath(fACPIPath);
        if (entry) {
            fACPIDevice = OSDynamicCast(IOACPIPlatformDevice, entry);
            if (!fACPIDevice) {
                entry->release();
            }
        }
    }

    /* Fallback to known Honor MagicBook / MateBook RP05 path if needed */
    if (!fACPIDevice) {
        IORegistryEntry *entry = IORegistryEntry::fromPath("IOACPIPlane:/_SB/PCI0@0/RP05@1c0004/PXSX@0");
        if (entry) {
            fACPIDevice = OSDynamicCast(IOACPIPlatformDevice, entry);
            if (fACPIDevice) {
                strlcpy(fACPIPath, "IOACPIPlane:/_SB/PCI0@0/RP05@1c0004/PXSX@0", sizeof(fACPIPath));
            } else {
                entry->release();
            }
        }
    }

    /* Locate parent PCIe Root Port (RP05) */
    IORegistryEntry *p = fPCIDevice->getParentEntry(gIODTPlane);
    while (p && !fBridgeDevice) {
        IOPCIDevice *cand = OSDynamicCast(IOPCIDevice, p);
        if (cand && cand != fPCIDevice) {
            fBridgeDevice = cand;
            fBridgeDevice->retain();
            break;
        }
        p = p->getParentEntry(gIODTPlane);
    }

    if (fBridgeDevice) {
        fBridgeACPIDevice = OSDynamicCast(IOACPIPlatformDevice, fBridgeDevice->getProperty("acpi-device"));
        if (fBridgeACPIDevice) {
            fBridgeACPIDevice->retain();
        }
    }

    LOG("ACPI path: %s (acpiDev=%p, bridgeDev=%p)",
        fACPIPath[0] ? fACPIPath : "<unknown>", fACPIDevice, fBridgeDevice);
    return (fACPIDevice != nullptr);
}

void com_maple_driver_GP108Bridge::savePCIConfig() {
    if (!fPCIDevice) {
        return;
    }
    uint32_t vd = fPCIDevice->configRead32(0x00);
    if (vd == 0xFFFFFFFF || (vd & 0xFFFF) != 0x10DE) {
        return;
    }
    for (uint32_t i = 0; i < 64; i++) {
        fSavedConfig[i] = fPCIDevice->configRead32(static_cast<UInt8>(i * 4));
    }
}

void com_maple_driver_GP108Bridge::restorePCIConfig() {
    if (!fPCIDevice) {
        return;
    }
    /* Restore BARs (dwords 4..9 -> 0x10..0x24) and CacheLine/Latency (dword 3) first */
    for (uint32_t i = 4; i <= 9; i++) {
        fPCIDevice->configWrite32(static_cast<UInt8>(i * 4), fSavedConfig[i]);
    }
    fPCIDevice->configWrite32(0x0C, fSavedConfig[3]);
    fPCIDevice->configWrite32(0x3C, fSavedConfig[15]);

    /* Restore extended capability dwords (0x40..0xFC) except read-only status */
    for (uint32_t i = 16; i < 64; i++) {
        fPCIDevice->configWrite32(static_cast<UInt8>(i * 4), fSavedConfig[i]);
    }

    /* Finally restore Command register + enable Memory & Bus Lead */
    fPCIDevice->configWrite16(kIOPCIConfigCommand,
                              static_cast<uint16_t>((fSavedConfig[1] & 0xFFFF) | 0x0006));
    fPCIDevice->setMemoryEnable(true);
    fPCIDevice->setBusLeadEnable(true);
}

void com_maple_driver_GP108Bridge::maskHardwareInterrupts() {
    if (!fPoweredOn || !fBar0Virt) {
        return;
    }
    uint32_t boot0 = fBar0Virt[NV_PMC_BOOT_0 / 4];
    if (boot0 == 0xFFFFFFFF || boot0 == 0) {
        return;
    }
    fBar0Virt[NV_PMC_INTR_EN_CLR_0 / 4] = 0xFFFFFFFFU;
    fBar0Virt[NV_PMC_INTR_EN_0 / 4]     = 0x00000000U;
    (void)fBar0Virt[NV_PMC_BOOT_0 / 4];
}

int32_t com_maple_driver_GP108Bridge::readDieTempCelsius() {
    if (!fPoweredOn || !fBar0Virt) {
        return -1;
    }
    uint32_t ts = fBar0Virt[NV_THERM_TSENSOR / 4];
    if (ts == 0xFFFFFFFF || (ts & 0x20000000U) == 0) {
        return -1;
    }
    return static_cast<int32_t>((ts & 0x0001FFF8U) >> 8);
}

uint64_t com_maple_driver_GP108Bridge::readVRAMBytesStrap() {
    if (!fPoweredOn || !fBar0Virt) {
        return 0;
    }
    /* GP100/GP108 PFB_PRI_MMU_LOCAL_MEMORY_RANGE or PFB_CSTATUS (in MB << 20) */
    uint32_t cstatus = fBar0Virt[NV_PFB_CSTATUS / 4];
    if (cstatus != 0 && cstatus != 0xFFFFFFFF) {
        /* On Pascal, 0x100ce4 bits [31:0] report RAM in MB when shifted or bytes>>20 */
        if (cstatus <= 32768) {
            return static_cast<uint64_t>(cstatus) << 20;
        }
    }
    uint32_t localRange = fBar0Virt[NV_PFB_PRI_MMU_LOCAL / 4];
    if (localRange != 0 && localRange != 0xFFFFFFFF) {
        uint32_t mag = localRange & 0x3FU;
        uint32_t scale = (localRange >> 6) & 0xFU;
        if (mag > 0) {
            return (static_cast<uint64_t>(mag) << (scale + 20));
        }
    }
    return 0;
}

bool com_maple_driver_GP108Bridge::extractVBIOSFromACPI(IOACPIPlatformDevice *acpiDev) {
    if (!acpiDev) {
        return false;
    }
    if (acpiDev->validateObject("_ROM") != kIOReturnSuccess) {
        return false;
    }

    LOG("extractVBIOS: found ACPI _ROM on %s, reading chunks...", acpiDev->getName());
    uint32_t totalRead = 0;
    while (totalRead < kGP108MaxVBIOSBytes) {
        OSNumber *offNum = OSNumber::withNumber(static_cast<uint64_t>(totalRead), 32);
        OSNumber *lenNum = OSNumber::withNumber(static_cast<uint64_t>(kGP108MaxChunkBytes), 32);
        if (!offNum || !lenNum) {
            OSSafeReleaseNULL(offNum);
            OSSafeReleaseNULL(lenNum);
            break;
        }
        OSObject *params[2] = { offNum, lenNum };
        OSObject *result = nullptr;
        IOReturn kr = acpiDev->evaluateObject("_ROM", &result, params, 2);
        offNum->release();
        lenNum->release();

        if (kr != kIOReturnSuccess || !result) {
            OSSafeReleaseNULL(result);
            break;
        }
        OSData *data = OSDynamicCast(OSData, result);
        if (!data || data->getLength() == 0) {
            result->release();
            break;
        }

        uint32_t chunkLen = data->getLength();
        if (chunkLen > kGP108MaxVBIOSBytes - totalRead) {
            chunkLen = kGP108MaxVBIOSBytes - totalRead;
        }
        memcpy(fVBIOSBuffer + totalRead, data->getBytesNoCopy(), chunkLen);
        result->release();

        /* Check first chunk for 0x55 0xAA ROM signature or NPDE/PCIR */
        if (totalRead == 0 && (fVBIOSBuffer[0] != 0x55 || fVBIOSBuffer[1] != 0xAA)) {
            LOG("extractVBIOS: ACPI _ROM chunk 0 lacks 0x55AA header (%02x %02x)",
                fVBIOSBuffer[0], fVBIOSBuffer[1]);
            return false;
        }

        totalRead += chunkLen;
        if (chunkLen < kGP108MaxChunkBytes) {
            break;
        }
    }

    if (totalRead >= 512 && fVBIOSBuffer[0] == 0x55 && fVBIOSBuffer[1] == 0xAA) {
        fVBIOSSize = totalRead;
        fVBIOSSource = kGP108VBIOSAcpiROM;
        LOG("extractVBIOS: extracted %u bytes via ACPI _ROM", fVBIOSSize);
        return true;
    }
    return false;
}

bool com_maple_driver_GP108Bridge::extractVBIOSFromBAR0() {
    if (!fPoweredOn || !fBar0Virt || !fBar0Map || fBar0Map->getLength() < 0x800000) {
        return false;
    }

    /* Method A: BAR0 + 0x300000 PROM shadow (disable ROM shadow protection bit first) */
    uint32_t nv20 = fBar0Virt[NV_PBUS_PCI_NV_20 / 4];
    fBar0Virt[NV_PBUS_PCI_NV_20 / 4] = nv20 & ~1U;
    (void)fBar0Virt[NV_PMC_BOOT_0 / 4];

    uint32_t sigProm = fBar0Virt[NV_PROM_OFFSET / 4];
    if ((sigProm & 0xFFFFU) == 0xAA55U) {
        for (uint32_t off = 0; off < kGP108MaxVBIOSBytes; off += 4) {
            uint32_t w = fBar0Virt[(NV_PROM_OFFSET + off) / 4];
            memcpy(fVBIOSBuffer + off, &w, 4);
        }
        fBar0Virt[NV_PBUS_PCI_NV_20 / 4] = nv20;
        fVBIOSSize = kGP108MaxVBIOSBytes;
        fVBIOSSource = kGP108VBIOSBar0PROM;
        LOG("extractVBIOS: extracted %u bytes via BAR0 PROM", fVBIOSSize);
        return true;
    }
    fBar0Virt[NV_PBUS_PCI_NV_20 / 4] = nv20;

    /* Method B: BAR0 + 0x700000 PRAMIN */
    uint32_t sigRam = fBar0Virt[NV_PRAMIN_OFFSET / 4];
    if ((sigRam & 0xFFFFU) == 0xAA55U) {
        uint32_t maxPramin = 128U * 1024U;
        for (uint32_t off = 0; off < maxPramin; off += 4) {
            uint32_t w = fBar0Virt[(NV_PRAMIN_OFFSET + off) / 4];
            memcpy(fVBIOSBuffer + off, &w, 4);
        }
        fVBIOSSize = maxPramin;
        fVBIOSSource = kGP108VBIOSBar0RAM;
        LOG("extractVBIOS: extracted %u bytes via BAR0 PRAMIN", fVBIOSSize);
        return true;
    }
    return false;
}

bool com_maple_driver_GP108Bridge::extractVBIOS() {
    if (!fVBIOSBuffer) {
        fVBIOSBuffer = static_cast<uint8_t *>(IOMalloc(kGP108MaxVBIOSBytes));
        if (!fVBIOSBuffer) {
            return false;
        }
    }
    memset(fVBIOSBuffer, 0, kGP108MaxVBIOSBytes);
    fVBIOSSize = 0;
    fVBIOSSource = kGP108VBIOSNone;

    if (extractVBIOSFromACPI(fACPIDevice)) {
        return true;
    }
    if (extractVBIOSFromACPI(fBridgeACPIDevice)) {
        return true;
    }
    /* Also probe sibling PEGA@1 under RP05 if present */
    IORegistryEntry *pega = IORegistryEntry::fromPath("IOACPIPlane:/_SB/PCI0@0/RP05@1c0004/PEGA@1");
    if (pega) {
        IOACPIPlatformDevice *pegaAcpi = OSDynamicCast(IOACPIPlatformDevice, pega);
        bool ok = extractVBIOSFromACPI(pegaAcpi);
        pega->release();
        if (ok) {
            return true;
        }
    }
    return extractVBIOSFromBAR0();
}

bool com_maple_driver_GP108Bridge::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *) {
    auto *self = OSDynamicCast(com_maple_driver_GP108Bridge, owner);
    if (!self || !self->fPoweredOn || !self->fBar0Virt) {
        return false;
    }
    uint32_t intr0 = self->fBar0Virt[NV_PMC_INTR_0 / 4];
    if (intr0 == 0 || intr0 == 0xFFFFFFFFU) {
        return false;
    }
    /* Un-arm master GPU interrupt line (gp100_mc_intr_unarm) preserving sub-engine mask */
    self->fBar0Virt[NV_PMC_INTR_EN_0 / 4] = 0x00000000U;
    (void)self->fBar0Virt[NV_PMC_BOOT_0 / 4];

    OSBitOrAtomic(intr0, &self->fPendingIntr0);
    self->fLastIntr0 = intr0;
    OSIncrementAtomic64(reinterpret_cast<volatile SInt64 *>(&self->fIrqCount));
    return true;
}

void com_maple_driver_GP108Bridge::interruptAction(OSObject *owner, IOInterruptEventSource *, int) {
    auto *self = OSDynamicCast(com_maple_driver_GP108Bridge, owner);
    if (!self) {
        return;
    }
    uint32_t pending = OSBitAndAtomic(0U, &self->fPendingIntr0);
    if (pending == 0) {
        return;
    }

    uint64_t nowNs = 0;
    clock_get_uptime(&nowNs);

    IOLockLock(self->fLock);
    if (self->fActiveQueue) {
        GP108IrqEvent ev{};
        ev.seq          = self->fIrqCount;
        ev.timestamp_ns = nowNs;
        ev.pmc_intr_0   = pending;
        self->fActiveQueue->enqueue(&ev, sizeof(ev));
    }
    IOLockUnlock(self->fLock);
}

bool com_maple_driver_GP108Bridge::start(IOService *provider) {
    if (!super::start(provider)) {
        return false;
    }

    fLock = IOLockAlloc();
    if (!fLock) {
        return false;
    }

    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDevice) {
        IOLockFree(fLock);
        fLock = nullptr;
        return false;
    }
    fPCIDevice->retain();

    if (!fPCIDevice->open(this)) {
        LOG("start: failed to open IOPCIDevice");
        OSSafeReleaseNULL(fPCIDevice);
        IOLockFree(fLock);
        fLock = nullptr;
        return false;
    }

    /* Ensure WindowServer / AppleGPUWrangler ignores this headless 3D controller,
     * while publishing clean macOS System Information (SPPCIDataType) metadata. */
    fPCIDevice->setProperty("IOName", "#display");
    uint32_t hiddenClass = 0xFFFFFFFFU;
    fPCIDevice->setProperty("class-code", &hiddenClass, sizeof(hiddenClass));

    static const char kModel[]   = "NVIDIA GeForce MX250 (2 GB GDDR5)";
    static const char kDevType[] = "GPU / 3D Accelerator";
    static const char kSlot[]    = "Built-In PCIe x4";
    static const char kRomRev[]  = "NVK 26.3.1 / NVKM 6.19 (Vulkan 1.3)";
    uint64_t vramTotalBytes      = 2048ULL * 1024ULL * 1024ULL;
    fPCIDevice->setProperty("model", const_cast<char *>(kModel), sizeof(kModel));
    fPCIDevice->setProperty("device_type", const_cast<char *>(kDevType), sizeof(kDevType));
    fPCIDevice->setProperty("AAPL,slot-name", const_cast<char *>(kSlot), sizeof(kSlot));
    fPCIDevice->setProperty("rom-revision", const_cast<char *>(kRomRev), sizeof(kRomRev));
    fPCIDevice->setProperty("VRAM,totalMB", 2048ULL, 32);
    fPCIDevice->setProperty("VRAM,totalsize", &vramTotalBytes, sizeof(vramTotalBytes));

    locateACPIDevices();

    fPCIDevice->setMemoryEnable(true);
    fPCIDevice->setBusLeadEnable(true);
    IOSleep(2);

    savePCIConfig();

    /* Query BAR0 (0x10), BAR1 (0x14), BAR3 (0x1C) via exact PCI register offsets */
    const UInt8 kBarRegs[3] = {
        kIOPCIConfigBaseAddress0,
        kIOPCIConfigBaseAddress1,
        kIOPCIConfigBaseAddress3,
    };
    for (int i = 0; i < 3; i++) {
        fBarMemory[i] = fPCIDevice->getDeviceMemoryWithRegister(kBarRegs[i]);
        if (fBarMemory[i]) {
            fBarMemory[i]->retain();
            LOG("BAR slot %d (reg 0x%02x): phys=0x%llx size=0x%llx",
                i, kBarRegs[i],
                static_cast<unsigned long long>(fBarMemory[i]->getPhysicalAddress()),
                static_cast<unsigned long long>(fBarMemory[i]->getLength()));
        }
    }

    if (fBarMemory[0] && fBarMemory[0]->getLength() >= 0x100000) {
        fBar0Map = fPCIDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,
                                                           kIOMapInhibitCache);
        if (fBar0Map) {
            fBar0Virt = reinterpret_cast<volatile uint32_t *>(fBar0Map->getVirtualAddress());
        }
    }

    uint32_t boot0 = (fBar0Virt != nullptr) ? fBar0Virt[NV_PMC_BOOT_0 / 4] : 0xFFFFFFFFU;
    fPoweredOn = (boot0 != 0xFFFFFFFFU && boot0 != 0U);
    if (fPoweredOn) {
        maskHardwareInterrupts();
        LOG("GPU online! PMC_BOOT_0=0x%08x PMC_ENABLE=0x%08x temp=%dC VRAM=%lluMB",
            boot0,
            fBar0Virt[NV_PMC_ENABLE / 4],
            readDieTempCelsius(),
            static_cast<unsigned long long>(readVRAMBytesStrap() >> 20));
    } else {
        LOG("WARNING: BAR0 MMIO read returned 0x%08x (bar0Map=%p)", boot0, fBar0Map);
    }

    extractVBIOS();

    /* Set up MSI interrupt source on our dedicated workloop (preferring MSI over INTx) */
    fMsiIndex = -1;
    for (int idx = 0; idx < 8; idx++) {
        int irqType = 0;
        if (fPCIDevice->getInterruptType(idx, &irqType) != kIOReturnSuccess) {
            break;
        }
        if (irqType & kIOInterruptTypePCIMessaged) {
            fMsiIndex = idx;
            break;
        }
    }

    fWorkLoop = IOWorkLoop::workLoop();
    if (fWorkLoop && fMsiIndex >= 0) {
        fInterruptSource = IOFilterInterruptEventSource::filterInterruptEventSource(
            this,
            &com_maple_driver_GP108Bridge::interruptAction,
            &com_maple_driver_GP108Bridge::interruptFilter,
            fPCIDevice,
            fMsiIndex);
        if (fInterruptSource) {
            if (fWorkLoop->addEventSource(fInterruptSource) == kIOReturnSuccess) {
                LOG("Registered MSI interrupt handler at index %d", fMsiIndex);
            } else {
                OSSafeReleaseNULL(fInterruptSource);
            }
        }
    } else {
        LOG("MSI index search result: %d", fMsiIndex);
    }

    registerService();
    LOG("GP108Bridge service registered and ready for userspace clients.");
    return true;
}

void com_maple_driver_GP108Bridge::stop(IOService *provider) {
    maskHardwareInterrupts();

    if (fInterruptSource && fWorkLoop) {
        fInterruptSource->disable();
        fWorkLoop->removeEventSource(fInterruptSource);
    }
    OSSafeReleaseNULL(fInterruptSource);
    OSSafeReleaseNULL(fWorkLoop);

    OSSafeReleaseNULL(fBar0Map);
    fBar0Virt = nullptr;

    for (int i = 0; i < 3; i++) {
        OSSafeReleaseNULL(fBarMemory[i]);
    }

    OSSafeReleaseNULL(fBridgeACPIDevice);
    OSSafeReleaseNULL(fBridgeDevice);
    OSSafeReleaseNULL(fACPIDevice);

    if (fPCIDevice) {
        fPCIDevice->close(this);
        OSSafeReleaseNULL(fPCIDevice);
    }

    if (fVBIOSBuffer) {
        IOFree(fVBIOSBuffer, kGP108MaxVBIOSBytes);
        fVBIOSBuffer = nullptr;
    }

    if (fLock) {
        IOLockFree(fLock);
        fLock = nullptr;
    }

    super::stop(provider);
}

void com_maple_driver_GP108Bridge::free(void) {
    super::free();
}

IOReturn com_maple_driver_GP108Bridge::newUserClient(task_t owningTask, void *securityID,
                                                     UInt32 type, OSDictionary *properties,
                                                     IOUserClient **handler) {
    auto *client = OSTypeAlloc(com_maple_driver_GP108BridgeUserClient);
    if (!client) {
        return kIOReturnNoMemory;
    }
    if (!client->initWithTask(owningTask, securityID, type, properties)) {
        client->release();
        return kIOReturnBadArgument;
    }
    if (!client->attach(this)) {
        client->release();
        return kIOReturnInternalError;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnInternalError;
    }
    *handler = client;
    return kIOReturnSuccess;
}

IODeviceMemory *com_maple_driver_GP108Bridge::getBarMemory(uint32_t barSlot) {
    if (barSlot >= 3) {
        return nullptr;
    }
    return fBarMemory[barSlot];
}

void com_maple_driver_GP108Bridge::setActiveEventQueue(IOSharedDataQueue *queue) {
    IOLockLock(fLock);
    fActiveQueue = queue;
    IOLockUnlock(fLock);
}

void com_maple_driver_GP108Bridge::fillDeviceInfo(GP108DeviceInfo *outInfo) {
    memset(outInfo, 0, sizeof(*outInfo));
    outInfo->magic       = kGP108BridgeMagic;
    outInfo->abi_version = kGP108BridgeABIVersion;

    IOLockLock(fLock);
    outInfo->power_state         = fPoweredOn ? 1U : 0U;
    outInfo->vbios_size          = fVBIOSSize;
    outInfo->vbios_source        = fVBIOSSource;
    outInfo->irq_count           = fIrqCount;
    outInfo->last_pmc_intr_0     = fLastIntr0;
    outInfo->msi_interrupt_index = fMsiIndex;
    strlcpy(outInfo->acpi_path, fACPIPath, sizeof(outInfo->acpi_path));

    for (int i = 0; i < 3; i++) {
        if (fBarMemory[i]) {
            outInfo->bar_phys[i] = fBarMemory[i]->getPhysicalAddress();
            outInfo->bar_size[i] = fBarMemory[i]->getLength();
        }
    }

    if (fPCIDevice) {
        outInfo->bus      = fPCIDevice->getBusNumber();
        outInfo->device   = fPCIDevice->getDeviceNumber();
        outInfo->function = fPCIDevice->getFunctionNumber();

        uint32_t vd = fPoweredOn ? fPCIDevice->configRead32(0x00) : fSavedConfig[0];
        uint32_t cr = fPoweredOn ? fPCIDevice->configRead32(0x08) : fSavedConfig[2];
        uint32_t ss = fPoweredOn ? fPCIDevice->configRead32(0x2C) : fSavedConfig[11];

        outInfo->vendor_id        = static_cast<uint16_t>(vd & 0xFFFFU);
        outInfo->device_id        = static_cast<uint16_t>((vd >> 16) & 0xFFFFU);
        outInfo->revision_id      = static_cast<uint8_t>(cr & 0xFFU);
        outInfo->class_code       = (cr >> 8) & 0xFFFFFFU;
        outInfo->subsys_vendor_id = static_cast<uint16_t>(ss & 0xFFFFU);
        outInfo->subsys_id        = static_cast<uint16_t>((ss >> 16) & 0xFFFFU);
        outInfo->pci_command      = fPoweredOn ? fPCIDevice->configRead16(0x04)
                                               : static_cast<uint16_t>(fSavedConfig[1] & 0xFFFFU);
    }

    if (fBridgeDevice) {
        /* Comet Lake Root Port PCIe Capability is at offset 0x40: LinkCap=0x4C, LinkSts=0x52 */
        IOByteCount pcieCapOff = 0;
        if (fBridgeDevice->extendedFindPCICapability(kIOPCIPCIExpressCapability, &pcieCapOff)) {
            outInfo->pcie_link_caps   = fBridgeDevice->configRead32(static_cast<UInt8>(pcieCapOff + 0x0C));
            outInfo->pcie_link_status = fBridgeDevice->configRead16(static_cast<UInt8>(pcieCapOff + 0x12));
        }
    }

    if (fPoweredOn && fBar0Virt) {
        outInfo->pmc_boot_0       = fBar0Virt[NV_PMC_BOOT_0 / 4];
        outInfo->pmc_enable       = fBar0Virt[NV_PMC_ENABLE / 4];
        outInfo->temp_celsius     = readDieTempCelsius();
        outInfo->vram_bytes_strap = readVRAMBytesStrap();
    } else {
        outInfo->temp_celsius = -1;
    }
    IOLockUnlock(fLock);
}

IOReturn com_maple_driver_GP108Bridge::readVBIOSChunk(uint32_t offset, void *dst, uint32_t *inOutSize) {
    if (!dst || !inOutSize) {
        return kIOReturnBadArgument;
    }
    IOLockLock(fLock);
    if (!fVBIOSBuffer || fVBIOSSize == 0 || offset >= fVBIOSSize) {
        *inOutSize = 0;
        IOLockUnlock(fLock);
        return kIOReturnNotFound;
    }
    uint32_t avail = fVBIOSSize - offset;
    uint32_t toCopy = (*inOutSize < avail) ? *inOutSize : avail;
    if (toCopy > kGP108MaxChunkBytes) {
        toCopy = kGP108MaxChunkBytes;
    }
    memcpy(dst, fVBIOSBuffer + offset, toCopy);
    *inOutSize = toCopy;
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn com_maple_driver_GP108Bridge::pciConfigAccess(bool isWrite, bool targetBridge,
                                                       uint32_t width, uint32_t offset,
                                                       uint32_t valueIn, uint32_t *valueOut) {
    if (offset >= 4096 || (width != 1 && width != 2 && width != 4) || (offset & (width - 1)) != 0) {
        return kIOReturnBadArgument;
    }
    if (isWrite && targetBridge) {
        return kIOReturnNotPermitted; /* Keep parent bridge read-only from userspace */
    }

    IOLockLock(fLock);
    IOPCIDevice *dev = targetBridge ? fBridgeDevice : fPCIDevice;
    if (!dev || (!targetBridge && !fPoweredOn)) {
        IOLockUnlock(fLock);
        return kIOReturnOffline;
    }

    uint32_t res = 0;
    if (isWrite) {
        if (width == 1)      dev->extendedConfigWrite8(offset, static_cast<uint8_t>(valueIn));
        else if (width == 2) dev->extendedConfigWrite16(offset, static_cast<uint16_t>(valueIn));
        else                 dev->extendedConfigWrite32(offset, valueIn);
    } else {
        if (width == 1)      res = dev->extendedConfigRead8(offset);
        else if (width == 2) res = dev->extendedConfigRead16(offset);
        else                 res = dev->extendedConfigRead32(offset);
    }
    if (valueOut) {
        *valueOut = res;
    }
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn com_maple_driver_GP108Bridge::powerControl(uint32_t action) {
    IOLockLock(fLock);
    IOReturn ret = kIOReturnSuccess;

    if (action == kGP108PowerOff) {
        if (!fACPIDevice || fACPIDevice->validateObject("_OFF") != kIOReturnSuccess) {
            IOLockUnlock(fLock);
            return kIOReturnUnsupported;
        }
        if (fPoweredOn) {
            maskHardwareInterrupts();
            if (fInterruptSource) {
                fInterruptSource->disable();
                fIrqEnabled = false;
            }
            savePCIConfig();
            ret = fACPIDevice->evaluateObject("_OFF");
            if (ret == kIOReturnSuccess) {
                fPoweredOn = false;
                LOG("powerControl: GPU powered OFF via ACPI _OFF");
            }
        }
    } else if (action == kGP108PowerOn) {
        if (!fACPIDevice || fACPIDevice->validateObject("_ON") != kIOReturnSuccess) {
            IOLockUnlock(fLock);
            return kIOReturnUnsupported;
        }
        if (!fPoweredOn) {
            ret = fACPIDevice->evaluateObject("_ON");
            if (ret == kIOReturnSuccess) {
                /* Poll up to 300ms for PCIe link recovery */
                bool linkUp = false;
                for (int attempt = 0; attempt < 60; attempt++) {
                    IOSleep(5);
                    uint32_t vd = fPCIDevice->configRead32(0x00);
                    if (vd != 0xFFFFFFFFU && (vd & 0xFFFFU) == 0x10DEU) {
                        linkUp = true;
                        break;
                    }
                }
                if (linkUp) {
                    IOSleep(10);
                    restorePCIConfig();
                    IOSleep(2);
                    uint32_t boot0 = fBar0Virt ? fBar0Virt[NV_PMC_BOOT_0 / 4] : 0xFFFFFFFFU;
                    fPoweredOn = (boot0 != 0xFFFFFFFFU && boot0 != 0U);
                    maskHardwareInterrupts();
                    LOG("powerControl: GPU powered ON! PMC_BOOT_0=0x%08x", boot0);
                    if (!fPoweredOn) {
                        ret = kIOReturnDeviceError;
                    }
                } else {
                    LOG("powerControl: timeout waiting for PCIe link after _ON");
                    ret = kIOReturnTimeout;
                }
            }
        }
    } else if (action == kGP108PowerRefreshVBIOS) {
        ret = extractVBIOS() ? kIOReturnSuccess : kIOReturnNotFound;
    } else {
        ret = kIOReturnBadArgument;
    }

    IOLockUnlock(fLock);
    return ret;
}

IOReturn com_maple_driver_GP108Bridge::interruptControl(bool enable, uint64_t *outIrqCount, uint32_t *outLastIntr0) {
    IOLockLock(fLock);
    if (!fInterruptSource) {
        IOLockUnlock(fLock);
        return kIOReturnUnsupported;
    }
    if (enable && fPoweredOn) {
        if (!fIrqEnabled) {
            fInterruptSource->enable();
            fIrqEnabled = true;
        }
        if (fBar0Virt) {
            /* Arm master GPU interrupt line (gp100_mc_intr_rearm); userspace sets 0x160 mask */
            (void)NV_PMC_INTR_EN_SET_0;
            fBar0Virt[NV_PMC_INTR_EN_0 / 4] = 0x00000001U;
            (void)fBar0Virt[NV_PMC_BOOT_0 / 4];
        }
    } else {
        maskHardwareInterrupts();
        if (fIrqEnabled) {
            fInterruptSource->disable();
            fIrqEnabled = false;
        }
    }
    if (outIrqCount)  *outIrqCount  = fIrqCount;
    if (outLastIntr0) *outLastIntr0 = fLastIntr0;
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn com_maple_driver_GP108Bridge::readACPITableChunk(uint32_t sigU32, uint32_t instance,
                                                          uint32_t offset, void *dst,
                                                          uint32_t *inOutSize) {
    if (!fACPIDevice || !dst || !inOutSize) {
        return kIOReturnBadArgument;
    }
    char tableName[5];
    tableName[0] = static_cast<char>(sigU32 & 0xFF);
    tableName[1] = static_cast<char>((sigU32 >> 8) & 0xFF);
    tableName[2] = static_cast<char>((sigU32 >> 16) & 0xFF);
    tableName[3] = static_cast<char>((sigU32 >> 24) & 0xFF);
    tableName[4] = '\0';

    /* Note: getACPITableData() follows IOKit Get-rule (returns unretained borrowed OSData*) */
    const OSData *tableData = fACPIDevice->getACPITableData(tableName, instance);
    if (!tableData) {
        *inOutSize = 0;
        return kIOReturnNotFound;
    }
    uint32_t totalLen = tableData->getLength();
    if (offset >= totalLen) {
        *inOutSize = 0;
        return kIOReturnSuccess;
    }
    uint32_t avail = totalLen - offset;
    uint32_t toCopy = (*inOutSize < avail) ? *inOutSize : avail;
    if (toCopy > kGP108MaxChunkBytes) {
        toCopy = kGP108MaxChunkBytes;
    }
    memcpy(dst, static_cast<const uint8_t *>(tableData->getBytesNoCopy()) + offset, toCopy);
    *inOutSize = toCopy;
    return kIOReturnSuccess;
}

/* ========================================================================= *
 * com_maple_driver_GP108BridgeUserClient                                    *
 * ========================================================================= */

#undef super
#define super IOUserClient
OSDefineMetaClassAndStructors(com_maple_driver_GP108BridgeUserClient, IOUserClient)

const IOExternalMethodDispatch com_maple_driver_GP108BridgeUserClient::sMethods[kGP108MethodCount] = {
    /* 0: kGP108MethodGetInfo */
    { &sGetInfo,       0, 0, 0, sizeof(GP108DeviceInfo) },
    /* 1: kGP108MethodReadVBIOS */
    { &sReadVBIOS,     1, 0, 0, kIOUCVariableStructureSize },
    /* 2: kGP108MethodPCIConfig */
    { &sPCIConfig,     5, 0, 1, 0 },
    /* 3: kGP108MethodAllocDMA */
    { &sAllocDMA,      3, 0, 4, 0 },
    /* 4: kGP108MethodFreeDMA */
    { &sFreeDMA,       1, 0, 0, 0 },
    /* 5: kGP108MethodGetPhysPages */
    { &sGetPhysPages,  4, 0, 1, kIOUCVariableStructureSize },
    /* 6: kGP108MethodPinUserMem */
    { &sPinUserMem,    3, 0, 3, 0 },
    /* 7: kGP108MethodUnpinUserMem */
    { &sUnpinUserMem,  1, 0, 0, 0 },
    /* 8: kGP108MethodPowerControl */
    { &sPowerControl,  1, 0, 0, 0 },
    /* 9: kGP108MethodInterruptCtrl */
    { &sInterruptCtrl, 1, 0, 2, 0 },
    /* 10: kGP108MethodReadACPITable */
    { &sReadACPITable, 3, 0, 0, kIOUCVariableStructureSize },
};

bool com_maple_driver_GP108BridgeUserClient::initWithTask(task_t owningTask, void *securityToken,
                                                          UInt32 type, OSDictionary *properties) {
    if (!super::initWithTask(owningTask, securityToken, type, properties)) {
        return false;
    }
    fTask = owningTask;
    fNotificationPort = MACH_PORT_NULL;
    memset(fDMASlots, 0, sizeof(fDMASlots));
    memset(fPinnedSlots, 0, sizeof(fPinnedSlots));
    fClientLock = IOLockAlloc();
    return (fClientLock != nullptr);
}

bool com_maple_driver_GP108BridgeUserClient::start(IOService *provider) {
    if (!super::start(provider)) {
        return false;
    }
    fProvider = OSDynamicCast(com_maple_driver_GP108Bridge, provider);
    if (!fProvider) {
        return false;
    }
    fProvider->retain();

    fEventQueue = IOSharedDataQueue::withEntries(256, sizeof(GP108IrqEvent));
    if (fEventQueue) {
        fProvider->setActiveEventQueue(fEventQueue);
    }
    return true;
}

void com_maple_driver_GP108BridgeUserClient::releaseAllResources() {
    if (!fClientLock) {
        return;
    }
    IOLockLock(fClientLock);

    if (fProvider) {
        uint64_t dummyCnt = 0;
        uint32_t dummyIntr = 0;
        fProvider->interruptControl(false, &dummyCnt, &dummyIntr);
        fProvider->setActiveEventQueue(nullptr);
    }

    for (uint32_t i = 0; i < kGP108MaxDMASlots; i++) {
        if (fDMASlots[i].desc) {
            if (fDMASlots[i].prepared) {
                fDMASlots[i].desc->complete(kIODirectionInOut);
                fDMASlots[i].prepared = false;
            }
            fDMASlots[i].desc->release();
            fDMASlots[i].desc = nullptr;
            fDMASlots[i].size = 0;
        }
    }

    for (uint32_t i = 0; i < kGP108MaxPinnedSlots; i++) {
        if (fPinnedSlots[i].desc) {
            if (fPinnedSlots[i].prepared) {
                fPinnedSlots[i].desc->complete(kIODirectionInOut);
                fPinnedSlots[i].prepared = false;
            }
            fPinnedSlots[i].desc->release();
            fPinnedSlots[i].desc = nullptr;
            fPinnedSlots[i].size = 0;
        }
    }

    if (fEventQueue) {
        fEventQueue->setNotificationPort(MACH_PORT_NULL);
        OSSafeReleaseNULL(fEventQueue);
    }
    if (fNotificationPort != MACH_PORT_NULL) {
        releaseNotificationPort(fNotificationPort);
        fNotificationPort = MACH_PORT_NULL;
    }

    IOLockUnlock(fClientLock);
}

IOReturn com_maple_driver_GP108BridgeUserClient::clientClose(void) {
    releaseAllResources();
    terminate();
    return kIOReturnSuccess;
}

IOReturn com_maple_driver_GP108BridgeUserClient::clientDied(void) {
    releaseAllResources();
    return super::clientDied();
}

void com_maple_driver_GP108BridgeUserClient::stop(IOService *provider) {
    releaseAllResources();
    OSSafeReleaseNULL(fProvider);
    super::stop(provider);
}

void com_maple_driver_GP108BridgeUserClient::free(void) {
    if (fClientLock) {
        IOLockFree(fClientLock);
        fClientLock = nullptr;
    }
    super::free();
}

IOReturn com_maple_driver_GP108BridgeUserClient::registerNotificationPort(mach_port_t port,
                                                                          UInt32 /*type*/,
                                                                          io_user_reference_t /*refCon*/) {
    IOLockLock(fClientLock);
    if (fNotificationPort != MACH_PORT_NULL) {
        releaseNotificationPort(fNotificationPort);
    }
    fNotificationPort = port;
    if (fEventQueue) {
        fEventQueue->setNotificationPort(port);
    }
    IOLockUnlock(fClientLock);
    return kIOReturnSuccess;
}

IOReturn com_maple_driver_GP108BridgeUserClient::clientMemoryForType(UInt32 type,
                                                                     IOOptionBits *options,
                                                                     IOMemoryDescriptor **memory) {
    if (!fProvider || !options || !memory) {
        return kIOReturnBadArgument;
    }

    if (type <= kGP108MapBAR3) {
        IODeviceMemory *barMem = fProvider->getBarMemory(type);
        if (!barMem) {
            return kIOReturnNotFound;
        }
        if (type == kGP108MapBAR1) {
            *options |= kIOMapWriteCombineCache;
        } else {
            *options |= kIOMapInhibitCache;
        }
        barMem->retain();
        *memory = barMem;
        return kIOReturnSuccess;
    }

    if (type == kGP108MapEventQueue) {
        IOLockLock(fClientLock);
        if (!fEventQueue) {
            IOLockUnlock(fClientLock);
            return kIOReturnNotFound;
        }
        /* getMemoryDescriptor() returns an already-retained new descriptor */
        IOMemoryDescriptor *qDesc = fEventQueue->getMemoryDescriptor();
        IOLockUnlock(fClientLock);
        if (!qDesc) {
            return kIOReturnNoMemory;
        }
        *memory = qDesc;
        return kIOReturnSuccess;
    }

    if (type >= kGP108MapDMABase && type < kGP108MapDMABase + kGP108MaxDMASlots) {
        uint32_t slot = type - kGP108MapDMABase;
        IOLockLock(fClientLock);
        IOBufferMemoryDescriptor *buf = fDMASlots[slot].desc;
        if (!buf) {
            IOLockUnlock(fClientLock);
            return kIOReturnNotFound;
        }
        buf->retain();
        IOLockUnlock(fClientLock);
        *memory = buf;
        return kIOReturnSuccess;
    }

    return kIOReturnBadArgument;
}

IOReturn com_maple_driver_GP108BridgeUserClient::externalMethod(uint32_t selector,
                                                                IOExternalMethodArguments *arguments,
                                                                IOExternalMethodDispatch *dispatch,
                                                                OSObject *target,
                                                                void *reference) {
    if (selector >= kGP108MethodCount) {
        return kIOReturnUnsupported;
    }
    dispatch = const_cast<IOExternalMethodDispatch *>(&sMethods[selector]);
    target   = this;
    return super::externalMethod(selector, arguments, dispatch, target, reference);
}

IOReturn com_maple_driver_GP108BridgeUserClient::sGetInfo(OSObject *target, void *,
                                                          IOExternalMethodArguments *args) {
    auto *self = OSDynamicCast(com_maple_driver_GP108BridgeUserClient, target);
    if (!self || !self->fProvider || !args->structureOutput ||
        args->structureOutputSize < sizeof(GP108DeviceInfo)) {
        return kIOReturnBadArgument;
    }
    self->fProvider->fillDeviceInfo(static_cast<GP108DeviceInfo *>(args->structureOutput));
    args->structureOutputSize = sizeof(GP108DeviceInfo);
    return kIOReturnSuccess;
}

IOReturn com_maple_driver_GP108BridgeUserClient::sReadVBIOS(OSObject *target, void *,
                                                            IOExternalMethodArguments *args) {
    auto *self = OSDynamicCast(com_maple_driver_GP108BridgeUserClient, target);
    if (!self || !self->fProvider || !args->structureOutput || args->structureOutputSize == 0) {
        return kIOReturnBadArgument;
    }
    uint32_t chunkSize = args->structureOutputSize;
    IOReturn kr = self->fProvider->readVBIOSChunk(static_cast<uint32_t>(args->scalarInput[0]),
                                                  args->structureOutput,
                                                  &chunkSize);
    args->structureOutputSize = chunkSize;
    return kr;
}

IOReturn com_maple_driver_GP108BridgeUserClient::sPCIConfig(OSObject *target, void *,
                                                            IOExternalMethodArguments *args) {
    auto *self = OSDynamicCast(com_maple_driver_GP108BridgeUserClient, target);
    if (!self || !self->fProvider) {
        return kIOReturnBadArgument;
    }
    uint32_t outVal = 0;
    IOReturn kr = self->fProvider->pciConfigAccess(
        args->scalarInput[0] != 0,
        args->scalarInput[1] != 0,
        static_cast<uint32_t>(args->scalarInput[2]),
        static_cast<uint32_t>(args->scalarInput[3]),
        static_cast<uint32_t>(args->scalarInput[4]),
        &outVal);
    args->scalarOutput[0] = outVal;
    return kr;
}

IOReturn com_maple_driver_GP108BridgeUserClient::sAllocDMA(OSObject *target, void *,
                                                           IOExternalMethodArguments *args) {
    auto *self = OSDynamicCast(com_maple_driver_GP108BridgeUserClient, target);
    if (!self) {
        return kIOReturnBadArgument;
    }
    mach_vm_size_t reqSize = (args->scalarInput[0] + PAGE_SIZE - 1) & ~static_cast<mach_vm_size_t>(PAGE_SIZE - 1);
    uint32_t cacheMode     = static_cast<uint32_t>(args->scalarInput[1]);
    uint32_t maskBits      = static_cast<uint32_t>(args->scalarInput[2]);

    if (reqSize == 0 || reqSize > (512ULL * 1024ULL * 1024ULL)) {
        return kIOReturnBadArgument;
    }
    if (maskBits < 32 || maskBits > 40) {
        maskBits = 40; /* Pascal GP108 supports 40-bit physical DMA addresses */
    }
    mach_vm_address_t physMask = ((1ULL << maskBits) - 1ULL) & ~static_cast<mach_vm_address_t>(PAGE_SIZE - 1);

    IOOptionBits opts = kIODirectionInOut | kIOMemoryKernelUserShared;
    if (cacheMode == kGP108CacheWriteCombine) {
        opts |= kIOMapWriteCombineCache;
    } else if (cacheMode == kGP108CacheUncached) {
        opts |= kIOMapInhibitCache;
    }
    if (reqSize <= 64 * 1024 && maskBits == 32) {
        opts |= kIOMemoryPhysicallyContiguous;
    }

    IOLockLock(self->fClientLock);
    int32_t freeSlot = -1;
    for (uint32_t i = 0; i < kGP108MaxDMASlots; i++) {
        if (!self->fDMASlots[i].desc) {
            freeSlot = static_cast<int32_t>(i);
            break;
        }
    }
    if (freeSlot < 0) {
        IOLockUnlock(self->fClientLock);
        return kIOReturnNoResources;
    }
    IOLockUnlock(self->fClientLock);

    IOBufferMemoryDescriptor *buf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, opts, reqSize, physMask);
    if (!buf) {
        return kIOReturnNoMemory;
    }
    if (buf->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        buf->release();
        return kIOReturnVMError;
    }
    memset(buf->getBytesNoCopy(), 0, reqSize);

    IOByteCount segLen = 0;
    addr64_t firstPhys = buf->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);

    IOLockLock(self->fClientLock);
    if (self->fDMASlots[freeSlot].desc != nullptr) {
        IOLockUnlock(self->fClientLock);
        buf->complete(kIODirectionInOut);
        buf->release();
        return kIOReturnBusy;
    }
    self->fDMASlots[freeSlot].desc     = buf;
    self->fDMASlots[freeSlot].size     = reqSize;
    self->fDMASlots[freeSlot].prepared = true;
    IOLockUnlock(self->fClientLock);

    args->scalarOutput[0] = static_cast<uint64_t>(freeSlot);
    args->scalarOutput[1] = reqSize;
    args->scalarOutput[2] = reqSize / PAGE_SIZE;
    args->scalarOutput[3] = firstPhys;
    return kIOReturnSuccess;
}

IOReturn com_maple_driver_GP108BridgeUserClient::sFreeDMA(OSObject *target, void *,
                                                          IOExternalMethodArguments *args) {
    auto *self = OSDynamicCast(com_maple_driver_GP108BridgeUserClient, target);
    if (!self) {
        return kIOReturnBadArgument;
    }
    uint32_t slot = static_cast<uint32_t>(args->scalarInput[0]);
    if (slot >= kGP108MaxDMASlots) {
        return kIOReturnBadArgument;
    }

    IOLockLock(self->fClientLock);
    IOBufferMemoryDescriptor *buf = self->fDMASlots[slot].desc;
    bool wasPrep = self->fDMASlots[slot].prepared;
    self->fDMASlots[slot].desc     = nullptr;
    self->fDMASlots[slot].size     = 0;
    self->fDMASlots[slot].prepared = false;
    IOLockUnlock(self->fClientLock);

    if (!buf) {
        return kIOReturnNotFound;
    }
    if (wasPrep) {
        buf->complete(kIODirectionInOut);
    }
    buf->release();
    return kIOReturnSuccess;
}

IOReturn com_maple_driver_GP108BridgeUserClient::sGetPhysPages(OSObject *target, void *,
                                                               IOExternalMethodArguments *args) {
    auto *self = OSDynamicCast(com_maple_driver_GP108BridgeUserClient, target);
    if (!self || !args->structureOutput) {
        return kIOReturnBadArgument;
    }
    bool isPinned     = (args->scalarInput[0] != 0);
    uint32_t slot     = static_cast<uint32_t>(args->scalarInput[1]);
    uint64_t startPg  = args->scalarInput[2];
    uint64_t reqCount = args->scalarInput[3];

    uint64_t maxOut = args->structureOutputSize / sizeof(uint64_t);
    if (reqCount > maxOut) {
        reqCount = maxOut;
    }
    if (reqCount > kGP108MaxPagesPerQuery) {
        reqCount = kGP108MaxPagesPerQuery;
    }

    IOLockLock(self->fClientLock);
    IOMemoryDescriptor *desc = nullptr;
    mach_vm_size_t totalSize = 0;
    if (isPinned) {
        if (slot < kGP108MaxPinnedSlots) {
            desc      = self->fPinnedSlots[slot].desc;
            totalSize = self->fPinnedSlots[slot].size;
        }
    } else {
        if (slot < kGP108MaxDMASlots) {
            desc      = self->fDMASlots[slot].desc;
            totalSize = self->fDMASlots[slot].size;
        }
    }
    if (!desc) {
        IOLockUnlock(self->fClientLock);
        return kIOReturnNotFound;
    }
    desc->retain();
    IOLockUnlock(self->fClientLock);

    uint64_t totalPages = totalSize / PAGE_SIZE;
    if (startPg >= totalPages) {
        desc->release();
        args->scalarOutput[0] = 0;
        args->structureOutputSize = 0;
        return kIOReturnSuccess;
    }
    if (reqCount > totalPages - startPg) {
        reqCount = totalPages - startPg;
    }

    auto *outAddrs = static_cast<uint64_t *>(args->structureOutput);
    for (uint64_t i = 0; i < reqCount; i++) {
        IOByteCount segLen = 0;
        outAddrs[i] = desc->getPhysicalSegment(static_cast<IOByteCount>((startPg + i) * PAGE_SIZE),
                                               &segLen,
                                               kIOMemoryMapperNone);
    }
    desc->release();

    args->scalarOutput[0] = reqCount;
    args->structureOutputSize = static_cast<uint32_t>(reqCount * sizeof(uint64_t));
    return kIOReturnSuccess;
}

IOReturn com_maple_driver_GP108BridgeUserClient::sPinUserMem(OSObject *target, void *,
                                                             IOExternalMethodArguments *args) {
    auto *self = OSDynamicCast(com_maple_driver_GP108BridgeUserClient, target);
    if (!self || !self->fTask) {
        return kIOReturnBadArgument;
    }
    mach_vm_address_t userVA = args->scalarInput[0];
    mach_vm_size_t size      = args->scalarInput[1];
    if (userVA == 0 || size == 0 || (userVA & (PAGE_SIZE - 1)) != 0 || (size & (PAGE_SIZE - 1)) != 0) {
        return kIOReturnBadArgument;
    }

    IOLockLock(self->fClientLock);
    int32_t freeSlot = -1;
    for (uint32_t i = 0; i < kGP108MaxPinnedSlots; i++) {
        if (!self->fPinnedSlots[i].desc) {
            freeSlot = static_cast<int32_t>(i);
            break;
        }
    }
    IOLockUnlock(self->fClientLock);
    if (freeSlot < 0) {
        return kIOReturnNoResources;
    }

    IOMemoryDescriptor *desc = IOMemoryDescriptor::withAddressRange(
        userVA, size, kIODirectionInOut, self->fTask);
    if (!desc) {
        return kIOReturnVMError;
    }
    if (desc->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        desc->release();
        return kIOReturnCannotWire;
    }

    IOByteCount segLen = 0;
    addr64_t firstPhys = desc->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);

    IOLockLock(self->fClientLock);
    if (self->fPinnedSlots[freeSlot].desc != nullptr) {
        IOLockUnlock(self->fClientLock);
        desc->complete(kIODirectionInOut);
        desc->release();
        return kIOReturnBusy;
    }
    self->fPinnedSlots[freeSlot].desc     = desc;
    self->fPinnedSlots[freeSlot].size     = size;
    self->fPinnedSlots[freeSlot].prepared = true;
    IOLockUnlock(self->fClientLock);

    args->scalarOutput[0] = static_cast<uint64_t>(freeSlot);
    args->scalarOutput[1] = size / PAGE_SIZE;
    args->scalarOutput[2] = firstPhys;
    return kIOReturnSuccess;
}

IOReturn com_maple_driver_GP108BridgeUserClient::sUnpinUserMem(OSObject *target, void *,
                                                               IOExternalMethodArguments *args) {
    auto *self = OSDynamicCast(com_maple_driver_GP108BridgeUserClient, target);
    if (!self) {
        return kIOReturnBadArgument;
    }
    uint32_t slot = static_cast<uint32_t>(args->scalarInput[0]);
    if (slot >= kGP108MaxPinnedSlots) {
        return kIOReturnBadArgument;
    }

    IOLockLock(self->fClientLock);
    IOMemoryDescriptor *desc = self->fPinnedSlots[slot].desc;
    bool wasPrep = self->fPinnedSlots[slot].prepared;
    self->fPinnedSlots[slot].desc     = nullptr;
    self->fPinnedSlots[slot].size     = 0;
    self->fPinnedSlots[slot].prepared = false;
    IOLockUnlock(self->fClientLock);

    if (!desc) {
        return kIOReturnNotFound;
    }
    if (wasPrep) {
        desc->complete(kIODirectionInOut);
    }
    desc->release();
    return kIOReturnSuccess;
}

IOReturn com_maple_driver_GP108BridgeUserClient::sPowerControl(OSObject *target, void *,
                                                               IOExternalMethodArguments *args) {
    auto *self = OSDynamicCast(com_maple_driver_GP108BridgeUserClient, target);
    if (!self || !self->fProvider) {
        return kIOReturnBadArgument;
    }
    return self->fProvider->powerControl(static_cast<uint32_t>(args->scalarInput[0]));
}

IOReturn com_maple_driver_GP108BridgeUserClient::sInterruptCtrl(OSObject *target, void *,
                                                                IOExternalMethodArguments *args) {
    auto *self = OSDynamicCast(com_maple_driver_GP108BridgeUserClient, target);
    if (!self || !self->fProvider) {
        return kIOReturnBadArgument;
    }
    uint64_t cnt = 0;
    uint32_t lastIntr = 0;
    IOReturn kr = self->fProvider->interruptControl(args->scalarInput[0] != 0, &cnt, &lastIntr);
    args->scalarOutput[0] = cnt;
    args->scalarOutput[1] = lastIntr;
    return kr;
}

IOReturn com_maple_driver_GP108BridgeUserClient::sReadACPITable(OSObject *target, void *,
                                                                IOExternalMethodArguments *args) {
    auto *self = OSDynamicCast(com_maple_driver_GP108BridgeUserClient, target);
    if (!self || !self->fProvider || !args->structureOutput || args->structureOutputSize == 0) {
        return kIOReturnBadArgument;
    }
    uint32_t chunkSize = args->structureOutputSize;
    IOReturn kr = self->fProvider->readACPITableChunk(
        static_cast<uint32_t>(args->scalarInput[0]),
        static_cast<uint32_t>(args->scalarInput[1]),
        static_cast<uint32_t>(args->scalarInput[2]),
        args->structureOutput,
        &chunkSize);
    args->structureOutputSize = chunkSize;
    return kr;
}

extern "C" {
kern_return_t _start(kmod_info_t *ki, void *data);
kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.maple.driver.GP108Bridge, "1.0.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
}
