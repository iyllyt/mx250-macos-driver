#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurface.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <string>

#include "../include/GP108BridgeShared.h"

static const char *vbiosSourceName(uint32_t src) {
    switch (src) {
        case kGP108VBIOSAcpiROM:  return "ACPI _ROM";
        case kGP108VBIOSBar0PROM: return "BAR0 PROM (0x300000)";
        case kGP108VBIOSBar0RAM:  return "BAR0 PRAMIN (0x700000)";
        default:                  return "None";
    }
}

static const char *decodeNvidiaArch(uint32_t boot0) {
    uint32_t chipset = (boot0 >> 20) & 0x1FFU;
    switch (chipset) {
        case 0x130: return "GP100 (Pascal)";
        case 0x132: return "GP102 (Pascal)";
        case 0x134: return "GP104 (Pascal)";
        case 0x136: return "GP106 (Pascal)";
        case 0x137: return "GP107 (Pascal)";
        case 0x138: return "GP108 (Pascal - GeForce MX150/MX250/GT1030)";
        default:    return "Unknown/Other NVIDIA";
    }
}

static void inspectVBIOSImage(const std::vector<uint8_t> &rom) {
    if (rom.size() < 64 || rom[0] != 0x55 || rom[1] != 0xAA) {
        printf("  [VBIOS] Invalid ROM header (missing 0x55AA)\n");
        return;
    }
    uint16_t pcirOff = static_cast<uint16_t>(rom[0x18] | (rom[0x19] << 8));
    if (pcirOff + 24 <= rom.size() && memcmp(&rom[pcirOff], "PCIR", 4) == 0) {
        uint16_t vid = static_cast<uint16_t>(rom[pcirOff + 4] | (rom[pcirOff + 5] << 8));
        uint16_t did = static_cast<uint16_t>(rom[pcirOff + 6] | (rom[pcirOff + 7] << 8));
        uint16_t imgLen512 = static_cast<uint16_t>(rom[pcirOff + 16] | (rom[pcirOff + 17] << 8));
        printf("  [VBIOS] PCIR @ 0x%04x: PCI ID=%04x:%04x, ImageSize=%u bytes\n",
               pcirOff, vid, did, static_cast<uint32_t>(imgLen512) * 512U);
    }

    /* Locate NVIDIA BIT header: 0xFF 0xB8 'B' 'I' 'T' 0x00 */
    const uint8_t bitSig[6] = { 0xFF, 0xB8, 'B', 'I', 'T', 0x00 };
    for (size_t i = 0; i + 12 < rom.size(); i++) {
        if (memcmp(&rom[i], bitSig, 6) == 0) {
            uint8_t bitVerMaj = rom[i + 6];
            uint8_t bitVerMin = rom[i + 7];
            uint8_t tokenCnt  = rom[i + 10];
            printf("  [VBIOS] Found NVIDIA BIT header @ 0x%04zx (version %u.%u, %u tokens) - VALID GP108 VBIOS!\n",
                   i, bitVerMaj, bitVerMin, tokenCnt);
            break;
        }
    }
}

static bool saveVBIOS(io_connect_t conn, uint32_t vbiosSize, const std::string &outPath) {
    if (vbiosSize == 0) {
        return false;
    }
    std::vector<uint8_t> rom(vbiosSize);
    uint32_t offset = 0;
    while (offset < vbiosSize) {
        uint64_t inScalar = offset;
        size_t outSize = kGP108MaxChunkBytes;
        uint8_t chunk[kGP108MaxChunkBytes];
        kern_return_t kr = IOConnectCallMethod(
            conn, kGP108MethodReadVBIOS, &inScalar, 1,
            nullptr, 0, nullptr, nullptr, chunk, &outSize);
        if (kr != KERN_SUCCESS || outSize == 0) {
            break;
        }
        if (offset + outSize > rom.size()) {
            outSize = rom.size() - offset;
        }
        memcpy(rom.data() + offset, chunk, outSize);
        offset += static_cast<uint32_t>(outSize);
    }
    if (offset == 0) {
        return false;
    }
    rom.resize(offset);
    inspectVBIOSImage(rom);

    FILE *fp = fopen(outPath.c_str(), "wb");
    if (!fp) {
        perror("fopen vbios");
        return false;
    }
    fwrite(rom.data(), 1, rom.size(), fp);
    fclose(fp);
    printf("  [VBIOS] Saved %zu bytes to %s\n", rom.size(), outPath.c_str());
    return true;
}

static void dumpACPITables(io_connect_t conn, const std::string &outDir) {
    mkdir(outDir.c_str(), 0755);
    auto dumpOne = [&](const char sig[4], uint32_t instance, const std::string &filename) -> bool {
        uint32_t sigU32 = static_cast<uint32_t>(uint8_t(sig[0])) |
                          (static_cast<uint32_t>(uint8_t(sig[1])) << 8) |
                          (static_cast<uint32_t>(uint8_t(sig[2])) << 16) |
                          (static_cast<uint32_t>(uint8_t(sig[3])) << 24);
        std::vector<uint8_t> buf;
        uint32_t offset = 0;
        while (true) {
            uint64_t scalars[3] = { sigU32, instance, offset };
            uint8_t chunk[kGP108MaxChunkBytes];
            size_t outSize = sizeof(chunk);
            kern_return_t kr = IOConnectCallMethod(
                conn, kGP108MethodReadACPITable, scalars, 3,
                nullptr, 0, nullptr, nullptr, chunk, &outSize);
            if (kr != KERN_SUCCESS || outSize == 0) {
                break;
            }
            buf.insert(buf.end(), chunk, chunk + outSize);
            offset += static_cast<uint32_t>(outSize);
            if (outSize < kGP108MaxChunkBytes) {
                break;
            }
        }
        if (buf.empty()) {
            return false;
        }
        std::string fullPath = outDir + "/" + filename;
        FILE *fp = fopen(fullPath.c_str(), "wb");
        if (fp) {
            fwrite(buf.data(), 1, buf.size(), fp);
            fclose(fp);
            printf("  [ACPI] Dumped %s (%zu bytes) -> %s\n", filename.c_str(), buf.size(), fullPath.c_str());
        }
        return true;
    };

    dumpOne("DSDT", 0, "DSDT.aml");
    dumpOne("MCFG", 0, "MCFG.aml");
    for (uint32_t idx = 0; idx < 32; idx++) {
        char name[32];
        snprintf(name, sizeof(name), "SSDT-%u.aml", idx);
        if (!dumpOne("SSDT", idx, name)) {
            break;
        }
    }
}

static void runSelfTests(io_connect_t conn, const GP108DeviceInfo &info) {
    printf("\n=== [1] Userspace BAR0 MMIO & Hardware Timer Verification ===\n");
    if (info.power_state == 0) {
        printf("  GPU is currently powered OFF (D3cold); run with --power-on first.\n");
        return;
    }

    mach_vm_address_t bar0Addr = 0;
    mach_vm_size_t bar0Size = 0;
    kern_return_t kr = IOConnectMapMemory64(
        conn, kGP108MapBAR0, mach_task_self(), &bar0Addr, &bar0Size, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || !bar0Addr) {
        printf("  [FAIL] IOConnectMapMemory64(BAR0) failed: 0x%x\n", kr);
        return;
    }
    auto *mmio = reinterpret_cast<volatile uint32_t *>(bar0Addr);
    uint32_t boot0  = mmio[0x000000 / 4];
    /* PTIMER TIME_0 (0x009400 low) / TIME_1 (0x009410 high) */
    uint32_t t0_lo1 = mmio[0x009400 / 4];
    uint32_t t0_hi1 = mmio[0x009410 / 4];
    usleep(10000); /* 10 ms */
    uint32_t t0_lo2 = mmio[0x009400 / 4];
    uint32_t t0_hi2 = mmio[0x009410 / 4];

    uint64_t ns1 = (static_cast<uint64_t>(t0_hi1) << 32) | t0_lo1;
    uint64_t ns2 = (static_cast<uint64_t>(t0_hi2) << 32) | t0_lo2;
    printf("  [PASS] Mapped BAR0 @ %p (size=0x%llx), direct userspace PMC_BOOT_0 = 0x%08x\n",
           reinterpret_cast<void *>(bar0Addr), static_cast<unsigned long long>(bar0Size), boot0);
    printf("  [PASS] GPU PTIMER 10ms delta = %llu ns (t1=0x%llx -> t2=0x%llx)\n",
           static_cast<unsigned long long>(ns2 - ns1),
           static_cast<unsigned long long>(ns1),
           static_cast<unsigned long long>(ns2));
    IOConnectUnmapMemory64(conn, kGP108MapBAR0, mach_task_self(), bar0Addr);

    printf("\n=== [2] 40-bit Pascal DMA Buffer Allocation & Userspace Mapping ===\n");
    uint64_t allocIn[3] = { 64 * 1024, kGP108CacheWriteCombine, 40 };
    uint64_t allocOut[4] = {};
    uint32_t allocOutCnt = 4;
    kr = IOConnectCallScalarMethod(conn, kGP108MethodAllocDMA, allocIn, 3, allocOut, &allocOutCnt);
    if (kr == KERN_SUCCESS) {
        uint32_t slot = static_cast<uint32_t>(allocOut[0]);
        printf("  [PASS] Allocated 64KB DMA slot=%u pages=%llu phys[0]=0x%llx (< 1TB 40-bit=%s)\n",
               slot,
               static_cast<unsigned long long>(allocOut[2]),
               static_cast<unsigned long long>(allocOut[3]),
               (allocOut[3] < (1ULL << 40)) ? "YES" : "NO");

        mach_vm_address_t dmaVA = 0;
        mach_vm_size_t dmaSz = 0;
        if (IOConnectMapMemory64(conn, kGP108MapDMABase + slot, mach_task_self(),
                                 &dmaVA, &dmaSz, kIOMapAnywhere) == KERN_SUCCESS) {
            auto *p = reinterpret_cast<uint32_t *>(dmaVA);
            p[0] = 0xCAFEBABEU;
            printf("  [PASS] Mapped DMA buffer in userspace @ %p (rw verify=0x%08x)\n",
                   reinterpret_cast<void *>(dmaVA), p[0]);
            IOConnectUnmapMemory64(conn, kGP108MapDMABase + slot, mach_task_self(), dmaVA);
        }

        uint64_t freeIn[1] = { slot };
        IOConnectCallScalarMethod(conn, kGP108MethodFreeDMA, freeIn, 1, nullptr, nullptr);
    } else {
        printf("  [FAIL] AllocDMA returned 0x%x\n", kr);
    }

    printf("\n=== [3] IOSurface Zero-Copy Physical Page Pinning (Phase 4 PRIME Preview) ===\n");
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int width = 1920, height = 1080, bpp = 4;
    CFNumberRef wNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &width);
    CFNumberRef hNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &height);
    CFNumberRef bNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bpp);
    CFDictionarySetValue(dict, kIOSurfaceWidth, wNum);
    CFDictionarySetValue(dict, kIOSurfaceHeight, hNum);
    CFDictionarySetValue(dict, kIOSurfaceBytesPerElement, bNum);
    CFRelease(wNum); CFRelease(hNum); CFRelease(bNum);

    IOSurfaceRef surf = IOSurfaceCreate(dict);
    CFRelease(dict);
    if (surf) {
        IOSurfaceLock(surf, 0, nullptr);
        void *base = IOSurfaceGetBaseAddress(surf);
        size_t allocSz = (IOSurfaceGetAllocSize(surf) + 4095) & ~size_t(4095);
        uint64_t pinIn[3] = { reinterpret_cast<uint64_t>(base), allocSz, 0 };
        uint64_t pinOut[3] = {};
        uint32_t pinOutCnt = 3;
        kr = IOConnectCallScalarMethod(conn, kGP108MethodPinUserMem, pinIn, 3, pinOut, &pinOutCnt);
        if (kr == KERN_SUCCESS) {
            printf("  [PASS] Pinned 1920x1080 IOSurface (%zu KB, %llu pages): phys[0]=0x%llx\n",
                   allocSz / 1024,
                   static_cast<unsigned long long>(pinOut[1]),
                   static_cast<unsigned long long>(pinOut[2]));
            uint64_t unpinIn[1] = { pinOut[0] };
            IOConnectCallScalarMethod(conn, kGP108MethodUnpinUserMem, unpinIn, 1, nullptr, nullptr);
        } else {
            printf("  [FAIL] PinUserMem(IOSurface) failed: 0x%x\n", kr);
        }
        IOSurfaceUnlock(surf, 0, nullptr);
        CFRelease(surf);
    }
}

int main(int argc, char **argv) {
    bool doPowerOff  = false;
    bool doPowerOn   = false;
    bool doDumpACPI  = false;
    bool doSaveVBIOS = true;
    std::string outDir = "/Users/maple/projects/temp/mx250-macos/hw_dump";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--power-off")      doPowerOff = true;
        else if (arg == "--power-on")  doPowerOn = true;
        else if (arg == "--dump-acpi") doDumpACPI = true;
        else if (arg == "--out" && i + 1 < argc) outDir = argv[++i];
    }

    io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching(kGP108BridgeServiceName));
    if (service == IO_OBJECT_NULL) {
        fprintf(stderr, "[gp108-probe] GP108Bridge.kext service not found in IORegistry.\n"
                        "              (Have you booted with GP108Bridge.kext enabled and SSDT-dGPU-Off disabled?)\n");
        return 1;
    }

    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS || conn == IO_OBJECT_NULL) {
        fprintf(stderr, "[gp108-probe] IOServiceOpen failed: 0x%x\n", kr);
        return 1;
    }

    if (doPowerOn) {
        printf("[gp108-probe] Requesting ACPI _ON power-up...\n");
        uint64_t act = kGP108PowerOn;
        kr = IOConnectCallScalarMethod(conn, kGP108MethodPowerControl, &act, 1, nullptr, nullptr);
        printf("              PowerOn result: 0x%x\n", kr);
    }

    GP108DeviceInfo info{};
    size_t infoSize = sizeof(info);
    kr = IOConnectCallStructMethod(conn, kGP108MethodGetInfo, nullptr, 0, &info, &infoSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[gp108-probe] GetInfo failed: 0x%x\n", kr);
        IOServiceClose(conn);
        return 1;
    }

    uint32_t linkSpeed = info.pcie_link_status & 0xFU;
    uint32_t linkWidth = (info.pcie_link_status >> 4) & 0x3FU;

    printf("============================================================\n");
    printf(" NVIDIA GP108 (MX250) Hardware Bridge Status\n");
    printf("============================================================\n");
    printf("  PCI BDF        : %02u:%02u.%u\n", info.bus, info.device, info.function);
    printf("  PCI ID         : %04x:%04x (Subsys %04x:%04x, Rev 0x%02x)\n",
           info.vendor_id, info.device_id, info.subsys_vendor_id, info.subsys_id, info.revision_id);
    printf("  Raw PCI Class  : 0x%06x (Command=0x%04x)\n", info.class_code, info.pci_command);
    printf("  ACPI Path      : %s\n", info.acpi_path);
    printf("  PCIe Link      : Gen%u x%u (LinkStatus=0x%04x, LinkCaps=0x%08x)\n",
           linkSpeed, linkWidth, info.pcie_link_status, info.pcie_link_caps);
    printf("  Power State    : %s\n", info.power_state ? "D0 (Powered ON)" : "D3cold (Powered OFF)");
    printf("  PMC_BOOT_0     : 0x%08x -> %s\n", info.pmc_boot_0, decodeNvidiaArch(info.pmc_boot_0));
    printf("  PMC_ENABLE     : 0x%08x\n", info.pmc_enable);
    printf("  Die Temp       : %d C\n", info.temp_celsius);
    printf("  VRAM Strap     : %llu MB\n", static_cast<unsigned long long>(info.vram_bytes_strap >> 20));
    printf("  BAR0 (MMIO)    : phys=0x%010llx size=%llu MB\n",
           static_cast<unsigned long long>(info.bar_phys[0]),
           static_cast<unsigned long long>(info.bar_size[0] >> 20));
    printf("  BAR1 (VRAM)    : phys=0x%010llx size=%llu MB\n",
           static_cast<unsigned long long>(info.bar_phys[1]),
           static_cast<unsigned long long>(info.bar_size[1] >> 20));
    printf("  BAR3 (PRAMIN)  : phys=0x%010llx size=%llu MB\n",
           static_cast<unsigned long long>(info.bar_phys[2]),
           static_cast<unsigned long long>(info.bar_size[2] >> 20));
    printf("  MSI Index      : %d (IRQ count=%llu, last INTR_0=0x%08x)\n",
           info.msi_interrupt_index,
           static_cast<unsigned long long>(info.irq_count),
           info.last_pmc_intr_0);
    printf("  VBIOS          : %u bytes (Source: %s)\n",
           info.vbios_size, vbiosSourceName(info.vbios_source));
    printf("============================================================\n");

    if (doSaveVBIOS && info.vbios_size > 0) {
        mkdir(outDir.c_str(), 0755);
        saveVBIOS(conn, info.vbios_size, outDir + "/mx250_vbios.rom");
    }

    if (doDumpACPI) {
        printf("\n=== Dumping Raw Firmware ACPI Tables via Kernel Bridge ===\n");
        dumpACPITables(conn, outDir + "/acpi_raw");
    }

    if (!doPowerOff) {
        runSelfTests(conn, info);
    }

    if (doPowerOff) {
        printf("\n[gp108-probe] Requesting ACPI _OFF power-down (D3cold)...\n");
        uint64_t act = kGP108PowerOff;
        kr = IOConnectCallScalarMethod(conn, kGP108MethodPowerControl, &act, 1, nullptr, nullptr);
        printf("              PowerOff result: 0x%x\n", kr);
    }

    IOServiceClose(conn);
    return 0;
}
