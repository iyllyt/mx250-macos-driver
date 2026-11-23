#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

#include "../include/GP108BridgeShared.h"

struct BiosPmuImage {
    uint32_t init_addr_pmu;
    uint32_t args_addr_pmu;
    uint32_t boot_addr;
    uint32_t boot_addr_pmu;
    uint32_t boot_size;
    uint32_t code_addr;
    uint32_t code_addr_pmu;
    uint32_t code_size;
    uint32_t data_addr;
    uint32_t data_addr_pmu;
    uint32_t data_size;
};

static inline uint8_t  rd08(const std::vector<uint8_t> &b, uint32_t off) { return (off < b.size()) ? b[off] : 0; }
static inline uint16_t rd16(const std::vector<uint8_t> &b, uint32_t off) {
    return (off + 2 <= b.size()) ? uint16_t(b[off] | (b[off + 1] << 8)) : 0;
}
static inline uint32_t rd32(const std::vector<uint8_t> &b, uint32_t off) {
    return (off + 4 <= b.size()) ? uint32_t(b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24)) : 0;
}

static bool findBitEntry(const std::vector<uint8_t> &rom, char id, uint8_t &ver, uint16_t &len, uint16_t &ptr) {
    const uint8_t sig[6] = { 0xFF, 0xB8, 'B', 'I', 'T', 0x00 };
    for (size_t i = 0; i + 12 < rom.size(); i++) {
        if (memcmp(&rom[i], sig, 6) == 0) {
            uint8_t hdrSz = rom[i + 8];
            uint8_t tokSz = rom[i + 9];
            uint8_t cnt   = rom[i + 10];
            size_t off = i + hdrSz;
            for (uint8_t k = 0; k < cnt && off + tokSz <= rom.size(); k++, off += tokSz) {
                if (rom[off] == uint8_t(id)) {
                    ver = rom[off + 1];
                    len = rd16(rom, uint32_t(off + 2));
                    ptr = rd16(rom, uint32_t(off + 4));
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

static bool parseBiosPmu(const std::vector<uint8_t> &rom, uint8_t targetType, BiosPmuImage &out) {
    uint8_t ver = 0, hdr = 0, len = 0, cnt = 0;
    uint16_t bitLen = 0, bitPtr = 0;
    if (!findBitEntry(rom, 'p', ver, bitLen, bitPtr) || ver != 2 || bitLen < 4) {
        return false;
    }
    uint32_t tbl = rd32(rom, bitPtr);
    if (!tbl) return false;
    ver = rd08(rom, tbl + 0);
    hdr = rd08(rom, tbl + 1);
    len = rd08(rom, tbl + 2);
    cnt = rd08(rom, tbl + 3);

    for (uint8_t i = 0; i < cnt; i++) {
        uint32_t ent = tbl + hdr + i * len;
        uint8_t type = rd08(rom, ent + 0);
        uint32_t data = rd32(rom, ent + 2);
        if (type == targetType && data != 0) {
            memset(&out, 0, sizeof(out));
            out.init_addr_pmu = rd32(rom, data + 0x08);
            out.args_addr_pmu = rd32(rom, data + 0x0c);
            out.boot_addr     = data + 0x30;
            out.boot_addr_pmu = rd32(rom, data + 0x10) + rd32(rom, data + 0x18);
            out.boot_size     = rd32(rom, data + 0x1c) - rd32(rom, data + 0x18);
            out.code_addr     = out.boot_addr + out.boot_size;
            out.code_addr_pmu = out.boot_addr_pmu + out.boot_size;
            out.code_size     = rd32(rom, data + 0x20);
            out.data_addr     = data + 0x30 + rd32(rom, data + 0x24);
            out.data_addr_pmu = rd32(rom, data + 0x28);
            out.data_size     = rd32(rom, data + 0x2c);
            return true;
        }
    }
    return false;
}

class GP108MMIO {
public:
    volatile uint32_t *bar0;
    explicit GP108MMIO(volatile uint32_t *b) : bar0(b) {}

    inline uint32_t rd(uint32_t addr) const { return bar0[addr / 4]; }
    inline void wr(uint32_t addr, uint32_t val) const { bar0[addr / 4] = val; }
    inline void mask(uint32_t addr, uint32_t m, uint32_t v) const {
        uint32_t cur = rd(addr);
        wr(addr, (cur & ~m) | v);
    }

    bool pmuWaitMemScrub() const {
        wr(0x10a040, rd(0x10a040));
        for (int i = 0; i < 200; i++) {
            if ((rd(0x10a10c) & 0x6U) == 0) return true;
            usleep(100);
        }
        return false;
    }

    bool pmuResetEngine() const {
        mask(0x10a3c0, 0x1U, 0x1U);
        usleep(20);
        mask(0x10a3c0, 0x1U, 0x0U);
        return pmuWaitMemScrub();
    }

    bool pmuReset() const {
        /* gp102_pmu_flcn: disable -> enable */
        mask(0x10a048, 0x3U, 0x0U);
        wr(0x10a014, 0xFFFFFFFFU);
        if (!pmuResetEngine()) return false;
        if (!pmuResetEngine()) return false;
        wr(0x10a084, rd(0x000000));
        return true;
    }

    void pmuLoadCode(const std::vector<uint8_t> &rom, uint32_t pmuAddr, uint32_t imgOff, uint32_t len, bool sec) const {
        wr(0x10a180, 0x01000000U | (sec ? 0x10000000U : 0U) | pmuAddr);
        uint32_t i = 0;
        for (; i < len; i += 4) {
            if ((i & 0xFFU) == 0) {
                wr(0x10a188, (pmuAddr + i) >> 8);
            }
            wr(0x10a184, rd32(rom, imgOff + i));
        }
        while (i & 0xFFU) {
            wr(0x10a184, 0);
            i += 4;
        }
    }

    void pmuLoadData(const std::vector<uint8_t> &rom, uint32_t pmuAddr, uint32_t imgOff, uint32_t len) const {
        wr(0x10a1c0, 0x01000000U | pmuAddr);
        for (uint32_t i = 0; i < len; i += 4) {
            wr(0x10a1c4, rd32(rom, imgOff + i));
        }
    }

    uint32_t pmuArgs(uint32_t argp, uint32_t argi) const {
        wr(0x10a1c0, argp);
        uint32_t ptr = rd(0x10a1c4);
        wr(0x10a1c0, ptr + argi);
        return rd(0x10a1c4);
    }

    void pmuExec(uint32_t initAddr) const {
        wr(0x10a104, initAddr);
        wr(0x10a10c, 0x00000000U);
        wr(0x10a100, 0x00000002U);
    }
};

int main() {
    FILE *fp = fopen("/Users/maple/projects/temp/mx250-macos/hw_dump/mx250_vbios.rom", "rb");
    if (!fp) { perror("open vbios"); return 1; }
    fseek(fp, 0, SEEK_END);
    size_t sz = size_t(ftell(fp));
    fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> rom(sz);
    fread(rom.data(), 1, sz, fp);
    fclose(fp);

    uint8_t bitIVer = 0;
    uint16_t bitILen = 0, bitIPtr = 0;
    if (!findBitEntry(rom, 'I', bitIVer, bitILen, bitIPtr) || bitIVer != 1 || bitILen < 0x1c) {
        fprintf(stderr, "Missing BIT 'I' v1 table\n");
        return 1;
    }

    BiosPmuImage devinitImg{}, preosImg{};
    if (!parseBiosPmu(rom, 0x04, devinitImg)) {
        fprintf(stderr, "Missing PMU type 0x04 (DEVINIT) image in VBIOS\n");
        return 1;
    }
    printf("[VBIOS] Parsed PMU DEVINIT (0x04): boot=0x%x(%uB) code=0x%x(%uB) data=0x%x(%uB) init=0x%x args=0x%x\n",
           devinitImg.boot_addr_pmu, devinitImg.boot_size,
           devinitImg.code_addr_pmu, devinitImg.code_size,
           devinitImg.data_addr_pmu, devinitImg.data_size,
           devinitImg.init_addr_pmu, devinitImg.args_addr_pmu);

    bool hasPreos = parseBiosPmu(rom, 0x01, preosImg);
    printf("[VBIOS] Has PMU PRE_OS  (0x01): %s\n", hasPreos ? "YES" : "NO");

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(kGP108BridgeServiceName));
    if (!svc) { fprintf(stderr, "GP108Bridge not found\n"); return 1; }
    io_connect_t conn = IO_OBJECT_NULL;
    if (IOServiceOpen(svc, mach_task_self(), 0, &conn) != KERN_SUCCESS) return 1;
    IOObjectRelease(svc);

    mach_vm_address_t bar0VA = 0, bar3VA = 0;
    mach_vm_size_t bar0Sz = 0, bar3Sz = 0;
    if (IOConnectMapMemory64(conn, kGP108MapBAR0, mach_task_self(), &bar0VA, &bar0Sz, kIOMapAnywhere) != KERN_SUCCESS) {
        fprintf(stderr, "Failed to map BAR0\n");
        return 1;
    }
    GP108MMIO gpu(reinterpret_cast<volatile uint32_t *>(bar0VA));

    /* Step 1: Ensure all engines in PMC_ENABLE are enabled */
    gpu.wr(0x000200, 0xFFFFFFFFU);
    usleep(2000);

    uint32_t postFlagBefore = gpu.rd(0x02240c);
    bool needsPost = ((postFlagBefore & 0x2U) == 0);
    printf("[GPU] Before POST: 0x02240c = 0x%08x (needs_post=%s), 0x100ce0 = 0x%08x\n",
           postFlagBefore, needsPost ? "YES" : "NO (already POSTed!)", gpu.rd(0x100ce0));

    if (needsPost) {
        printf("[GPU] Resetting PMU Falcon and uploading signed VBIOS DEVINIT microcode...\n");
        if (!gpu.pmuReset()) {
            fprintf(stderr, "PMU Falcon reset timeout!\n");
            return 1;
        }
        gpu.pmuLoadCode(rom, devinitImg.boot_addr_pmu, devinitImg.boot_addr, devinitImg.boot_size, false);
        gpu.pmuLoadCode(rom, devinitImg.code_addr_pmu, devinitImg.code_addr, devinitImg.code_size, true);
        gpu.pmuLoadData(rom, devinitImg.data_addr_pmu, devinitImg.data_addr, devinitImg.data_size);

        /* Upload init script opcodes & boot scripts */
        uint32_t tblPmu = gpu.pmuArgs(devinitImg.args_addr_pmu + 0x08, 0x08);
        uint32_t tblImg = rd16(rom, bitIPtr + 0x14);
        uint32_t tblLen = rd16(rom, bitIPtr + 0x16);
        gpu.pmuLoadData(rom, tblPmu, tblImg, tblLen);

        uint32_t scrPmu = gpu.pmuArgs(devinitImg.args_addr_pmu + 0x08, 0x10);
        uint32_t scrImg = rd16(rom, bitIPtr + 0x18);
        uint32_t scrLen = rd16(rom, bitIPtr + 0x1a);
        gpu.pmuLoadData(rom, scrPmu, scrImg, scrLen);

        printf("[GPU] Executing signed DEVINIT on PMU (tables=%uB, scripts=%uB)...\n", tblLen, scrLen);
        gpu.wr(0x10a040, 0x00005000U);
        gpu.pmuExec(devinitImg.init_addr_pmu);

        bool done = false;
        int elapsedMs = 0;
        for (int ms = 0; ms < 2500; ms += 5) {
            usleep(5000);
            if (gpu.rd(0x10a040) & 0x00002000U) {
                done = true;
                elapsedMs = ms + 5;
                break;
            }
        }
        printf("[GPU] DEVINIT result: %s in %d ms (mbox0=0x%08x, cpuctl=0x%08x)\n",
               done ? "SUCCESS" : "TIMEOUT", elapsedMs, gpu.rd(0x10a040), gpu.rd(0x10a100));
        if (!done) return 1;

        if (hasPreos) {
            printf("[GPU] Executing signed PRE_OS on PMU...\n");
            if (gpu.pmuReset()) {
                gpu.pmuLoadCode(rom, preosImg.boot_addr_pmu, preosImg.boot_addr, preosImg.boot_size, false);
                gpu.pmuLoadCode(rom, preosImg.code_addr_pmu, preosImg.code_addr, preosImg.code_size, true);
                gpu.pmuLoadData(rom, preosImg.data_addr_pmu, preosImg.data_addr, preosImg.data_size);
                gpu.pmuExec(preosImg.init_addr_pmu);
                usleep(20000);
            }
        }
    }

    /* Re-enable all engines in PMC_ENABLE after DEVINIT */
    gpu.wr(0x000200, 0xFFFFFFFFU);
    usleep(2000);

    /* Read gp102_fb_vidmem_size(0x100ce0) */
    uint32_t ce0 = gpu.rd(0x100ce0);
    uint32_t lmag = (ce0 & 0x000003F0U) >> 4;
    uint32_t lsca = (ce0 & 0x0000000FU);
    uint64_t vramBytes = uint64_t(lmag) << (lsca + 20);
    if (ce0 & 0x40000000U) vramBytes = vramBytes / 16 * 15;

    uint32_t ts = gpu.rd(0x020460);
    int tempC = ((ts & 0x1FFF8U) >> 3) >> 5;
    bool vprReq = [&]() {
        gpu.wr(0x100cd0, 0x2U);
        return (gpu.rd(0x100cd0) & 0x10U) != 0;
    }();

    printf("\n============================================================\n");
    printf(" POST & GDDR5 Memory Training Complete!\n");
    printf("============================================================\n");
    printf("  POST Status Reg (0x02240c) : 0x%08x (POST bit=%u)\n", gpu.rd(0x02240c), (gpu.rd(0x02240c) >> 1) & 1U);
    printf("  GDDR5 VRAM Reg  (0x100ce0) : 0x%08x -> %llu MB Dedicated GDDR5!\n",
           ce0, static_cast<unsigned long long>(vramBytes >> 20));
    printf("  MMU Local Range (0x10f20c) : 0x%08x\n", gpu.rd(0x10f20c));
    printf("  VPR Scrub Needed(0x100cd0) : %s\n", vprReq ? "YES" : "NO");
    printf("  GPU Die Temp    (0x020460) : %d C\n", tempC);

    /* Test reading & writing actual GDDR5 VRAM via BAR0 PRAMIN window (0x700000, window reg 0x001700) */
    if (vramBytes >= (64ULL << 20)) {
        /* Point BAR0 PRAMIN (1MB window @ 0x700000) to VRAM physical offset 16 MB (addr >> 16) */
        uint64_t testVramPhys = 16ULL * 1024ULL * 1024ULL;
        gpu.wr(0x001700, uint32_t(testVramPhys >> 16));
        (void)gpu.rd(0x000000);

        volatile uint32_t *pramin = reinterpret_cast<volatile uint32_t *>(bar0VA + 0x700000);
        bool ok = true;
        for (uint32_t i = 0; i < 1024; i++) {
            pramin[i] = 0xA5A50000U ^ (i * 0x01010101U);
        }
        /* Flush FB write buffer */
        (void)gpu.rd(0x000000);
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t got = pramin[i];
            uint32_t exp = 0xA5A50000U ^ (i * 0x01010101U);
            if (got != exp) {
                printf("  [VRAM FAIL] word %u: got 0x%08x expected 0x%08x\n", i, got, exp);
                ok = false;
                break;
            }
        }
        if (ok) {
            printf("  [VRAM PASS] Verified 4KB read/write pattern in live GDDR5 VRAM @ +16MB!\n");
        }
    }
    printf("============================================================\n");

    (void)bar3VA; (void)bar3Sz;
    IOConnectUnmapMemory64(conn, kGP108MapBAR0, mach_task_self(), bar0VA);
    IOServiceClose(conn);
    return 0;
}
