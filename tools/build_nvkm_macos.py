#!/usr/bin/env python3
import concurrent.futures
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NOUVEAU_ROOT = os.path.join(ROOT, "third_party/nouveau_src/drivers/gpu/drm/nouveau")
UAPI_INC = os.path.join(ROOT, "third_party/nouveau_src/include/uapi")
SHIM_ROOT = os.path.join(ROOT, "nvkm_macos")
OBJ_DIR = os.path.join(ROOT, "build/nvkm_objs")
LIB_OUT = os.path.join(ROOT, "build/libnvkm_gp108.a")

MACOS_SRCS = [
    os.path.join(SHIM_ROOT, "macos_os.c"),
    os.path.join(SHIM_ROOT, "macos_rbtree.c"),
    os.path.join(SHIM_ROOT, "nouveau_nvif_macos.c"),
    os.path.join(SHIM_ROOT, "device_gp108.c"),
    os.path.join(SHIM_ROOT, "gp108_telemetry.c"),
    os.path.join(SHIM_ROOT, "gp108_nvk_bridge.c"),
]

NVIF_SRCS = [
    "nvif/object.c",
    "nvif/client.c",
    "nvif/device.c",
    "nvif/driver.c",
    "nvif/event.c",
    "nvif/fifo.c",
    "nvif/mem.c",
    "nvif/mmu.c",
    "nvif/timer.c",
    "nvif/vmm.c",
    "nvif/chan.c",
    "nvif/chan506f.c",
    "nvif/chan906f.c",
    "nvif/chanc36f.c",
    "nvif/user.c",
    "nvif/userc361.c",
]

NVKM_SRCS = [
    # core
    "nvkm/core/client.c",
    "nvkm/core/engine.c",
    "nvkm/core/enum.c",
    "nvkm/core/event.c",
    "nvkm/core/firmware.c",
    "nvkm/core/gpuobj.c",
    "nvkm/core/intr.c",
    "nvkm/core/ioctl.c",
    "nvkm/core/memory.c",
    "nvkm/core/mm.c",
    "nvkm/core/object.c",
    "nvkm/core/oproxy.c",
    "nvkm/core/option.c",
    "nvkm/core/ramht.c",
    "nvkm/core/subdev.c",
    "nvkm/core/uevent.c",
    # nvfw
    "nvkm/nvfw/fw.c",
    "nvkm/nvfw/hs.c",
    "nvkm/nvfw/ls.c",
    "nvkm/nvfw/acr.c",
    "nvkm/nvfw/flcn.c",
    # falcon
    "nvkm/falcon/base.c",
    "nvkm/falcon/cmdq.c",
    "nvkm/falcon/fw.c",
    "nvkm/falcon/msgq.c",
    "nvkm/falcon/qmgr.c",
    "nvkm/falcon/v1.c",
    "nvkm/falcon/gm200.c",
    "nvkm/falcon/gp102.c",
    # acr
    "nvkm/subdev/acr/base.c",
    "nvkm/subdev/acr/lsfw.c",
    "nvkm/subdev/acr/gm200.c",
    "nvkm/subdev/acr/gp102.c",
    "nvkm/subdev/acr/gp108.c",
    # bar
    "nvkm/subdev/bar/base.c",
    "nvkm/subdev/bar/nv50.c",
    "nvkm/subdev/bar/g84.c",
    "nvkm/subdev/bar/gf100.c",
    "nvkm/subdev/bar/gk20a.c",
    "nvkm/subdev/bar/gm107.c",
    # bios
    "nvkm/subdev/bios/base.c",
    "nvkm/subdev/bios/bit.c",
    "nvkm/subdev/bios/boost.c",
    "nvkm/subdev/bios/conn.c",
    "nvkm/subdev/bios/cstep.c",
    "nvkm/subdev/bios/dcb.c",
    "nvkm/subdev/bios/disp.c",
    "nvkm/subdev/bios/dp.c",
    "nvkm/subdev/bios/extdev.c",
    "nvkm/subdev/bios/fan.c",
    "nvkm/subdev/bios/gpio.c",
    "nvkm/subdev/bios/i2c.c",
    "nvkm/subdev/bios/iccsense.c",
    "nvkm/subdev/bios/image.c",
    "nvkm/subdev/bios/init.c",
    "nvkm/subdev/bios/mxm.c",
    "nvkm/subdev/bios/npde.c",
    "nvkm/subdev/bios/pcir.c",
    "nvkm/subdev/bios/perf.c",
    "nvkm/subdev/bios/pll.c",
    "nvkm/subdev/bios/pmu.c",
    "nvkm/subdev/bios/power_budget.c",
    "nvkm/subdev/bios/ramcfg.c",
    "nvkm/subdev/bios/rammap.c",
    "nvkm/subdev/bios/shadow.c",
    "nvkm/subdev/bios/shadowacpi.c",
    "nvkm/subdev/bios/shadowof.c",
    "nvkm/subdev/bios/shadowpci.c",
    "nvkm/subdev/bios/shadowramin.c",
    "nvkm/subdev/bios/shadowrom.c",
    "nvkm/subdev/bios/timing.c",
    "nvkm/subdev/bios/therm.c",
    "nvkm/subdev/bios/vmap.c",
    "nvkm/subdev/bios/volt.c",
    "nvkm/subdev/bios/vpstate.c",
    "nvkm/subdev/bios/xpio.c",
    "nvkm/subdev/bios/M0203.c",
    "nvkm/subdev/bios/M0205.c",
    "nvkm/subdev/bios/M0209.c",
    "nvkm/subdev/bios/P0260.c",
    # bus
    "nvkm/subdev/bus/base.c",
    "nvkm/subdev/bus/hwsq.c",
    "nvkm/subdev/bus/nv04.c",
    "nvkm/subdev/bus/nv31.c",
    "nvkm/subdev/bus/nv50.c",
    "nvkm/subdev/bus/g94.c",
    "nvkm/subdev/bus/gf100.c",
    # clk pll helpers used by devinit
    "nvkm/subdev/clk/pllnv04.c",
    "nvkm/subdev/clk/pllgt215.c",
    # devinit
    "nvkm/subdev/devinit/base.c",
    "nvkm/subdev/devinit/nv04.c",
    "nvkm/subdev/devinit/nv50.c",
    "nvkm/subdev/devinit/g84.c",
    "nvkm/subdev/devinit/gf100.c",
    "nvkm/subdev/devinit/gm107.c",
    "nvkm/subdev/devinit/gm200.c",
    # fault
    "nvkm/subdev/fault/base.c",
    "nvkm/subdev/fault/user.c",
    "nvkm/subdev/fault/gp100.c",
    # fb
    "nvkm/subdev/fb/base.c",
    "nvkm/subdev/fb/nv50.c",
    "nvkm/subdev/fb/gf100.c",
    "nvkm/subdev/fb/gk104.c",
    "nvkm/subdev/fb/gm107.c",
    "nvkm/subdev/fb/gm200.c",
    "nvkm/subdev/fb/gp100.c",
    "nvkm/subdev/fb/gp102.c",
    "nvkm/subdev/fb/ram.c",
    "nvkm/subdev/fb/ramgf100.c",
    "nvkm/subdev/fb/ramgf108.c",
    "nvkm/subdev/fb/ramgk104.c",
    "nvkm/subdev/fb/ramgm107.c",
    "nvkm/subdev/fb/ramgm200.c",
    "nvkm/subdev/fb/ramgp100.c",
    "nvkm/subdev/fb/ramgp102.c",
    "nvkm/subdev/fb/sddr2.c",
    "nvkm/subdev/fb/sddr3.c",
    "nvkm/subdev/fb/gddr3.c",
    "nvkm/subdev/fb/gddr5.c",
    # fuse
    "nvkm/subdev/fuse/base.c",
    "nvkm/subdev/fuse/gm107.c",
    # instmem
    "nvkm/subdev/instmem/base.c",
    "nvkm/subdev/instmem/nv04.c",
    "nvkm/subdev/instmem/nv50.c",
    # ltc
    "nvkm/subdev/ltc/base.c",
    "nvkm/subdev/ltc/gf100.c",
    "nvkm/subdev/ltc/gk104.c",
    "nvkm/subdev/ltc/gm107.c",
    "nvkm/subdev/ltc/gm200.c",
    "nvkm/subdev/ltc/gp100.c",
    "nvkm/subdev/ltc/gp102.c",
    # mc
    "nvkm/subdev/mc/base.c",
    "nvkm/subdev/mc/nv04.c",
    "nvkm/subdev/mc/nv17.c",
    "nvkm/subdev/mc/nv50.c",
    "nvkm/subdev/mc/g84.c",
    "nvkm/subdev/mc/g98.c",
    "nvkm/subdev/mc/gt215.c",
    "nvkm/subdev/mc/gf100.c",
    "nvkm/subdev/mc/gk104.c",
    "nvkm/subdev/mc/gk20a.c",
    "nvkm/subdev/mc/gp100.c",
    # mmu
    "nvkm/subdev/mmu/base.c",
    "nvkm/subdev/mmu/nv04.c",
    "nvkm/subdev/mmu/nv50.c",
    "nvkm/subdev/mmu/gf100.c",
    "nvkm/subdev/mmu/gk104.c",
    "nvkm/subdev/mmu/gm200.c",
    "nvkm/subdev/mmu/gp100.c",
    "nvkm/subdev/mmu/mem.c",
    "nvkm/subdev/mmu/memgf100.c",
    "nvkm/subdev/mmu/vmm.c",
    "nvkm/subdev/mmu/vmmnv04.c",
    "nvkm/subdev/mmu/vmmnv50.c",
    "nvkm/subdev/mmu/vmmgf100.c",
    "nvkm/subdev/mmu/vmmgk104.c",
    "nvkm/subdev/mmu/vmmgm200.c",
    "nvkm/subdev/mmu/vmmgp100.c",
    "nvkm/subdev/mmu/umem.c",
    "nvkm/subdev/mmu/ummu.c",
    "nvkm/subdev/mmu/uvmm.c",
    # pci
    "nvkm/subdev/pci/agp.c",
    "nvkm/subdev/pci/base.c",
    "nvkm/subdev/pci/pcie.c",
    "nvkm/subdev/pci/g84.c",
    "nvkm/subdev/pci/g94.c",
    "nvkm/subdev/pci/gf100.c",
    "nvkm/subdev/pci/gk104.c",
    "nvkm/subdev/pci/gp100.c",
    # pmu
    "nvkm/subdev/pmu/base.c",
    "nvkm/subdev/pmu/memx.c",
    "nvkm/subdev/pmu/gt215.c",
    "nvkm/subdev/pmu/gf100.c",
    "nvkm/subdev/pmu/gk104.c",
    "nvkm/subdev/pmu/gk20a.c",
    "nvkm/subdev/pmu/gm107.c",
    "nvkm/subdev/pmu/gm200.c",
    "nvkm/subdev/pmu/gp102.c",
    # privring
    "nvkm/subdev/privring/gf100.c",
    "nvkm/subdev/privring/gk104.c",
    "nvkm/subdev/privring/gm200.c",
    # timer
    "nvkm/subdev/timer/base.c",
    "nvkm/subdev/timer/nv04.c",
    "nvkm/subdev/timer/gk20a.c",
    # top
    "nvkm/subdev/top/base.c",
    "nvkm/subdev/top/gk104.c",
    # engines
    "nvkm/engine/falcon.c",
    "nvkm/engine/device/acpi.c",
    "nvkm/engine/device/ctrl.c",
    "nvkm/engine/device/pci.c",
    "nvkm/engine/device/user.c",
    # ce
    "nvkm/engine/ce/gt215.c",
    "nvkm/engine/ce/gf100.c",
    "nvkm/engine/ce/gk104.c",
    "nvkm/engine/ce/gm107.c",
    "nvkm/engine/ce/gm200.c",
    "nvkm/engine/ce/gp100.c",
    "nvkm/engine/ce/gp102.c",
    # dma
    "nvkm/engine/dma/base.c",
    "nvkm/engine/dma/nv04.c",
    "nvkm/engine/dma/nv50.c",
    "nvkm/engine/dma/gf100.c",
    "nvkm/engine/dma/gf119.c",
    "nvkm/engine/dma/user.c",
    "nvkm/engine/dma/usernv04.c",
    "nvkm/engine/dma/usernv50.c",
    "nvkm/engine/dma/usergf100.c",
    "nvkm/engine/dma/usergf119.c",
    # fifo
    "nvkm/engine/fifo/base.c",
    "nvkm/engine/fifo/cgrp.c",
    "nvkm/engine/fifo/chan.c",
    "nvkm/engine/fifo/chid.c",
    "nvkm/engine/fifo/runl.c",
    "nvkm/engine/fifo/runq.c",
    "nvkm/engine/fifo/nv04.c",
    "nvkm/engine/fifo/nv50.c",
    "nvkm/engine/fifo/g84.c",
    "nvkm/engine/fifo/gf100.c",
    "nvkm/engine/fifo/gk104.c",
    "nvkm/engine/fifo/gk110.c",
    "nvkm/engine/fifo/gk208.c",
    "nvkm/engine/fifo/gm107.c",
    "nvkm/engine/fifo/gm200.c",
    "nvkm/engine/fifo/gp100.c",
    "nvkm/engine/fifo/ucgrp.c",
    "nvkm/engine/fifo/uchan.c",
    # gr
    "nvkm/engine/gr/base.c",
    "nvkm/engine/gr/gf100.c",
    "nvkm/engine/gr/gf104.c",
    "nvkm/engine/gr/gf108.c",
    "nvkm/engine/gr/gf110.c",
    "nvkm/engine/gr/gf117.c",
    "nvkm/engine/gr/gf119.c",
    "nvkm/engine/gr/gk104.c",
    "nvkm/engine/gr/gk110.c",
    "nvkm/engine/gr/gk110b.c",
    "nvkm/engine/gr/gk208.c",
    "nvkm/engine/gr/gk20a.c",
    "nvkm/engine/gr/gm107.c",
    "nvkm/engine/gr/gm200.c",
    "nvkm/engine/gr/gp100.c",
    "nvkm/engine/gr/gp102.c",
    "nvkm/engine/gr/gp104.c",
    "nvkm/engine/gr/gp107.c",
    "nvkm/engine/gr/gp108.c",
    "nvkm/engine/gr/ctxgf100.c",
    "nvkm/engine/gr/ctxgf104.c",
    "nvkm/engine/gr/ctxgf108.c",
    "nvkm/engine/gr/ctxgf110.c",
    "nvkm/engine/gr/ctxgf117.c",
    "nvkm/engine/gr/ctxgf119.c",
    "nvkm/engine/gr/ctxgk104.c",
    "nvkm/engine/gr/ctxgk110.c",
    "nvkm/engine/gr/ctxgk110b.c",
    "nvkm/engine/gr/ctxgk208.c",
    "nvkm/engine/gr/ctxgk20a.c",
    "nvkm/engine/gr/ctxgm107.c",
    "nvkm/engine/gr/ctxgm200.c",
    "nvkm/engine/gr/ctxgp100.c",
    "nvkm/engine/gr/ctxgp102.c",
    "nvkm/engine/gr/ctxgp104.c",
    "nvkm/engine/gr/ctxgp107.c",
    # nvdec
    "nvkm/engine/nvdec/base.c",
    "nvkm/engine/nvdec/gm107.c",
    # sec2
    "nvkm/engine/sec2/base.c",
    "nvkm/engine/sec2/gp102.c",
    "nvkm/engine/sec2/gp108.c",
    # sw + vga helpers
    "nvkm/engine/disp/vga.c",
    "nvkm/engine/sw/base.c",
    "nvkm/engine/sw/nv50.c",
    "nvkm/engine/sw/gf100.c",
    "nvkm/engine/sw/chan.c",
    "nvkm/engine/sw/nvsw.c",
]

CFLAGS = [
    "-std=gnu11",
    "-O2",
    "-g",
    "-fPIC",
    "-fno-strict-aliasing",
    "-Wno-unused-parameter",
    "-Wno-sign-compare",
    "-Wno-missing-field-initializers",
    "-Wno-gnu-variable-sized-type-not-at-end",
    "-Wno-gnu-zero-variadic-macro-arguments",
    "-Wno-pointer-arith",
    "-Wno-visibility",
    f"-I{SHIM_ROOT}/include",
    f"-I{NOUVEAU_ROOT}/include",
    f"-I{NOUVEAU_ROOT}/include/nvkm",
    f"-I{NOUVEAU_ROOT}/nvkm",
    f"-I{NOUVEAU_ROOT}/nvkm/subdev/gsp",
    f"-I{NOUVEAU_ROOT}",
    f"-I{UAPI_INC}",
]


def compile_one(item):
    src_path, obj_path = item
    os.makedirs(os.path.dirname(obj_path), exist_ok=True)
    # Recompile if src or os.h is newer
    os_h_mtime = os.path.getmtime(os.path.join(SHIM_ROOT, "include/nvif/os.h"))
    if os.path.exists(obj_path):
        obj_m = os.path.getmtime(obj_path)
        if obj_m >= os.path.getmtime(src_path) and obj_m >= os_h_mtime:
            return (src_path, 0, "")
    cmd = ["clang"] + CFLAGS + ["-c", src_path, "-o", obj_path]
    res = subprocess.run(cmd, capture_output=True, text=True)
    return (src_path, res.returncode, res.stderr)


def main():
    os.makedirs(OBJ_DIR, exist_ok=True)
    work = []
    objs = []
    for p in MACOS_SRCS:
        rel = "macos_" + os.path.basename(p)[:-2] + ".o"
        o = os.path.join(OBJ_DIR, rel)
        work.append((p, o))
        objs.append(o)
    for rel_c in NVIF_SRCS + NVKM_SRCS:
        src = os.path.join(NOUVEAU_ROOT, rel_c)
        o = os.path.join(OBJ_DIR, rel_c[:-2].replace("/", "_") + ".o")
        work.append((src, o))
        objs.append(o)

    print(f"[build_nvkm] Compiling {len(work)} source files...")
    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        for src_path, rc, err in ex.map(compile_one, work):
            if rc != 0:
                errors.append((src_path, err))

    if errors:
        print(f"[build_nvkm] {len(errors)} file(s) failed to compile:")
        for src_path, err in errors[:12]:
            print(f"--- {src_path} ---")
            print(err)
        sys.exit(1)

    if os.path.exists(LIB_OUT):
        os.remove(LIB_OUT)
    subprocess.run(["libtool", "-static", "-o", LIB_OUT] + objs, check=True)
    print(f"[build_nvkm] Built {LIB_OUT} ({os.path.getsize(LIB_OUT)} bytes) successfully!")


if __name__ == "__main__":
    main()
