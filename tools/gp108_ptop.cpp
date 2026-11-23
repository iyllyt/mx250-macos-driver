#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <unistd.h>
#include "../include/GP108BridgeShared.h"

int main() {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(kGP108BridgeServiceName));
    if (!svc) { fprintf(stderr, "No GP108Bridge\n"); return 1; }
    io_connect_t conn = IO_OBJECT_NULL;
    if (IOServiceOpen(svc, mach_task_self(), 0, &conn) != KERN_SUCCESS) return 1;
    IOObjectRelease(svc);

    mach_vm_address_t bar0 = 0; mach_vm_size_t sz = 0;
    if (IOConnectMapMemory64(conn, kGP108MapBAR0, mach_task_self(), &bar0, &sz, kIOMapAnywhere) != KERN_SUCCESS) return 1;
    auto *mmio = reinterpret_cast<volatile uint32_t *>(bar0);

    printf("PMC_BOOT_0        (0x000000) = 0x%08x\n", mmio[0x000000/4]);
    printf("PMC_ENABLE        (0x000200) = 0x%08x\n", mmio[0x000200/4]);
    printf("PMC_DEVICE_ENABLE (0x000600) = 0x%08x\n", mmio[0x000600/4]);
    printf("PGC6_SCRATCH_05   (0x0015f4) = 0x%08x\n", mmio[0x0015f4/4]);
    printf("PEXTDEV_BOOT_0    (0x101000) = 0x%08x\n", mmio[0x101000/4]);
    printf("PBF_21C04         (0x021c04) = 0x%08x\n", mmio[0x021c04/4]);

    /* Enable all sub-engines in PMC_ENABLE (gp100_mc_init: wr32(0x200, 0xffffffff)) */
    uint32_t old_en = mmio[0x000200/4];
    mmio[0x000200/4] = 0xFFFFFFFFU;
    usleep(2000);
    printf("\n--- After PMC_ENABLE = 0xFFFFFFFF (was 0x%08x -> now 0x%08x) ---\n",
           old_en, mmio[0x000200/4]);
    printf("PTHERM_TSENSOR    (0x020460) = 0x%08x -> %d C\n",
           mmio[0x020460/4], (int)(((mmio[0x020460/4] & 0x1fff8) >> 3) >> 5));
    printf("PFB_CSTATUS       (0x100ce4) = 0x%08x\n", mmio[0x100ce4/4]);
    printf("PFB_MMU_LOCAL_MEM (0x10f20c) = 0x%08x\n", mmio[0x10f20c/4]);
    printf("PMU_FALCON_HWCFG  (0x10a108) = 0x%08x\n", mmio[0x10a108/4]);
    printf("SEC2_FALCON_HWCFG (0x087108) = 0x%08x\n", mmio[0x087108/4]);
    printf("CE2_FALCON_HWCFG  (0x104108) = 0x%08x\n", mmio[0x104108/4]);

    printf("\n--- Silicon PTOP Engine Table (0x022400..0x0225ff) ---\n");
    for (int i = 0; i < 64; i++) {
        uint32_t v = mmio[(0x022400 + i * 4) / 4];
        if (v != 0) printf("  PTOP[%02d] = 0x%08x\n", i, v);
    }

    IOConnectUnmapMemory64(conn, kGP108MapBAR0, mach_task_self(), bar0);
    IOServiceClose(conn);
    return 0;
}
