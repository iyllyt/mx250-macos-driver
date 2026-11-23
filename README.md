# mx250-macos-driver

**Native Vulkan 1.3 Compute, 3D Graphics & PRIME Render Offload Driver for NVIDIA GeForce MX250 (`GP108M`) on macOS Sequoia (`x86_64`)**

Bring discrete **NVIDIA Pascal (`GP108` / GeForce MX250, 2 GB GDDR5)** GPUs to life on modern macOS (`Darwin 24.x` / macOS Sequoia 15.x `x86_64`) — **without disabling SIP root volume sealing** and while keeping the integrated Intel UHD GPU cleanly driving `WindowServer`.

<p align="center">
  <img src="docs/images/mx250_macos_desktop_window.png" width="48%" alt="MX250 PRIME Desktop 3D Demo (91.67 FPS)" />
  <img src="docs/images/mx250_vulkan_3d_offscreen.png" width="24%" alt="MX250 Offscreen 3D Lit Cube" />
  <img src="docs/images/mx250_vulkan_compute_mandelbrot.png" width="24%" alt="MX250 SM61 Compute Shader" />
</p>

---

## Architecture Highlights

Instead of porting millions of lines of kernel GPU code into XNU (where any GPU fault causes a kernel panic), **98% of the driver stack runs in macOS userspace** atop a minimal ~800-LOC IOKit kernel hardware bridge:

```
┌──────────────────────────────────────────────────────────────────────────┐
│ Vulkan 1.3 Apps (llama.cpp, wgpu, Blender, Emulators, mx250-demo)        │
├──────────────────────────────────────────────────────────────────────────┤
│ Global macOS Vulkan Loader (/usr/local/lib/libvulkan.1.dylib)            │
├──────────────────────────────────────────────────────────────────────────┤
│ Mesa 26.3.1 NVK + NAK (Rust SM61 SASS Compiler) + NIL (BlockLinear)      │
│   ├── nvkmd_macos: Direct userspace GPFIFO / GP100 49-bit VMM backend    │
│   └── VK_EXT_metal_surface: Hardware Copy Engine (0xc1b5) Untile +       │
│       Zero-Copy MTLBuffer -> Intel iGPU CAMetalLayer PRIME Offload       │
├──────────────────────────────────────────────────────────────────────────┤
│ Linux 6.19 Nouveau NVKM + NVIF Userspace Port (libnvkm_gp108.a)          │
│   └── ACR / SEC2 Falcon Signed Firmware Secure Boot + Golden Context     │
├─────────────────────────── Mach / IOKit UserClient ──────────────────────┤
│ GP108Bridge.kext (XNU Kernel Space)                                      │
│   └── BAR0/1/3 MMIO, 40-bit Contiguous/Scatter-Gather DMA, MSI IRQs,     │
│       PCI Config & ACPI Power (_ON / _OFF / _ROM)                        │
└──────────────────────────────────────────────────────────────────────────┘
```

## Live Hardware Benchmarks (macOS Sequoia 15.8.3, Core i5-10210U + MX250 2GB)

| Metric | Measured Result | Details |
| :--- | :--- | :--- |
| **Chip & PCIe Link** | `GP108-A1` (`10de:1d13`) | PCIe Gen3 `x4` (`8.0 GT/s`), `2048 MiB` GDDR5 |
| **ACR / SEC2 Secure Boot** | **`68.34 ms`** | Official `nvidia/gp108` signed Falcon WPR boot + 3D Golden Context |
| **16 MiB Bidirectional DMA** | **`0 / 4,194,304` word errors** | 100% bit-exact via `PASCAL_DMA_COPY_B` (`0xc1b5`) |
| **Vulkan Conformance Target** | **Vulkan `1.3.363`** | 182 device extensions including `VK_KHR_swapchain` & `VK_EXT_metal_surface` |
| **NAK `SM61` Compute Shader** | **`0.72 ms` execution** | `256×256` (65,536 threads, 1.57M complex iters) — **100.00% bit-exact** vs CPU IEEE-754 |
| **Offscreen 3D + BlockLinear Untile** | **`2.46 ms`** | `512×512` Dynamic Rendering + `D32_SFLOAT` Z-buffer + hardware untile |
| **Desktop Windowed PRIME 3D** | **`91.67 FPS` (`960×640`)** | MX250 renders 3D -> PCIe DMA -> Intel UHD `CAMetalLayer` zero-copy present |

---

## Included Utilities

### 1. `nvidia-smi` (CLI Hardware & Process Monitor)
Real-time die temperature (`NV_THERM_TSENSOR`), PCIe link state, GDDR5 VRAM usage, hardware fence `GPU-Util %`, active Vulkan `Processes:` table, `-l` live watch mode, and manual `D0 / D3off` power switching:
```sh
nvidia-smi              # Snapshot of temp, VRAM, GPU-Util %, and active GPU processes
nvidia-smi -l 1         # Continuous 1s live refresh
nvidia-smi --vulkan     # Verify global Vulkan 1.3 ICD discovery
nvidia-smi --power-off  # Put MX250 into D3cold (0W battery saving mode)
nvidia-smi --power-on   # Wake MX250 back to D0
```

### 2. `MX250 Monitor.app` (macOS Menu Bar Status Monitor)
Featherweight native Cocoa status bar item (`0.0%` steady-state CPU, `LSUIElement` Dock-less) with strict `D3off` sleep protection (never touches `BAR0` when the dGPU is powered off), active process list, 1-click `mx250-demo` launcher, and 1-click `D0 / D3off` power toggle.

### 3. `mx250-demo` (Interactive PRIME Render Offload 3D Window)
```sh
mx250-demo
```

---

## Repository Layout

* `kext/` — `GP108Bridge.kext` IOKit PCIe / MMIO / 40-bit DMA / MSI interrupt / ACPI bridge
* `include/` — Shared kernel-userspace ABI (`GP108BridgeShared.h`) & lock-free telemetry page (`GP108TelemetryShared.h`)
* `nvkm_macos/` — Darwin userspace Linux kernel compatibility layer (`macos_os.c`, `gp108_nvk_bridge.c`, `gp108_telemetry.c`)
* `mesa_macos/` — Native macOS `nvkmd` backend (`nvkmd_macos.c`), NIR query helpers (`nvkcl_macos.c`), and cubin stub
* `patches/` — Reproducible unified patches against upstream **Mesa `26.3.1`** and **Linux `6.19` Nouveau**
* `firmware/` — Redistributable signed `nvidia/gp108` Falcon microcode from `linux-firmware`
* `tools/` — `nvidia-smi`, `MX250 Monitor.app`, `mx250-demo`, Vulkan 1.3 ICD loader generator, hardware self-tests, and `.pkg` builder

---

## Quick Install (Prebuilt Release)

1. Add `GP108Bridge.kext` to your OpenCore `EFI/OC/Kexts/` and ensure `-wegnoegpu` (or SSDT `_OFF` boot-disable on `RP05`) is removed so macOS enumerates the PCIe `3D Controller` (`10de:1d13`).
2. Download and install **`NVIDIA-MX250-Driver-1.1.0.pkg`** from [**Releases**](../../releases).
3. Run `nvidia-smi --vulkan` and `mx250-demo` in Terminal!

## License

* Original macOS bridge, `nvkmd_macos`, loader, and Cocoa/CLI tools (`kext/`, `include/`, `mesa_macos/`, `tools/`): **MIT License**
* Linux `nouveau/nvkm` adaptation (`nvkm_macos/`, `patches/`): **MIT / GPL-2.0** (matching upstream Linux DRM Nouveau dual/MIT headers)
* `firmware/nvidia/gp108/`: Redistributed under the upstream `linux-firmware` NVIDIA binary redistribution license (`LICENCE.nvidia`).
