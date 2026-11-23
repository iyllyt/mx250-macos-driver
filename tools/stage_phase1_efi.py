#!/usr/bin/env python3
import copy
import os
import plistlib
import shutil
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BACKUP_EFI = os.path.join(ROOT, "efi_backup/ESP/EFI")
STAGED_EFI = os.path.join(ROOT, "efi_staged/EFI")
BUILT_KEXT = os.path.join(ROOT, "build/GP108Bridge.kext")

def main():
    if not os.path.isdir(BACKUP_EFI):
        raise RuntimeError(f"Missing backup EFI at {BACKUP_EFI}")
    if not os.path.isdir(BUILT_KEXT):
        raise RuntimeError(f"Missing built kext at {BUILT_KEXT}")

    if os.path.exists(STAGED_EFI):
        shutil.rmtree(STAGED_EFI)
    os.makedirs(os.path.dirname(STAGED_EFI), exist_ok=True)
    shutil.copytree(BACKUP_EFI, STAGED_EFI)

    dst_kext = os.path.join(STAGED_EFI, "OC/Kexts/GP108Bridge.kext")
    if os.path.exists(dst_kext):
        shutil.rmtree(dst_kext)
    shutil.copytree(BUILT_KEXT, dst_kext)

    cfg_path = os.path.join(STAGED_EFI, "OC/config.plist")
    with open(cfg_path, "rb") as f:
        cfg = plistlib.load(f)

    # 1. Disable SSDT-dGPU-Off.aml so MX250 stays powered ON for PCI enumeration & BAR assignment
    disabled_ssdt = False
    for entry in cfg["ACPI"]["Add"]:
        if entry.get("Path") == "SSDT-dGPU-Off.aml":
            entry["Enabled"] = False
            disabled_ssdt = True
            print("[OK] Disabled ACPI -> Add -> SSDT-dGPU-Off.aml")

    if not disabled_ssdt:
        raise RuntimeError("Could not find SSDT-dGPU-Off.aml in ACPI -> Add")

    # 2. Clone an existing Kernel -> Add entry template to guarantee 100% OpenCore 1.0.8 schema match
    kernel_add = cfg["Kernel"]["Add"]
    kernel_add[:] = [e for e in kernel_add if e.get("BundlePath") != "GP108Bridge.kext"]
    template = copy.deepcopy(kernel_add[-1])
    template["Arch"] = "x86_64"
    template["BundlePath"] = "GP108Bridge.kext"
    template["Comment"] = "NVIDIA MX250 (GP108) PCIe/DMA/ACPI Micro-Bridge"
    template["Enabled"] = True
    template["ExecutablePath"] = "Contents/MacOS/GP108Bridge"
    template["MaxKernel"] = ""
    template["MinKernel"] = ""
    template["PlistPath"] = "Contents/Info.plist"
    kernel_add.append(template)
    print("[OK] Added Kernel -> Add -> GP108Bridge.kext (using exact OpenCore 1.0.8 schema)")

    # 3. Remove -wegnoegpu from boot-args so WhateverGreen does not strip PXSX@0 from RP05;
    #    GP108Bridge.kext itself masks IOName/class-code from WindowServer after claiming PXSX@0.
    nvram_uuid = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
    boot_args = cfg["NVRAM"]["Add"][nvram_uuid].get("boot-args", "")
    tokens = [t for t in boot_args.split() if t != "-wegnoegpu"]
    new_boot_args = " ".join(tokens)
    cfg["NVRAM"]["Add"][nvram_uuid]["boot-args"] = new_boot_args
    print(f"[OK] Updated boot-args: '{boot_args}' -> '{new_boot_args}'")

    with open(cfg_path, "wb") as f:
        plistlib.dump(cfg, f, sort_keys=False)

    subprocess.run(["plutil", "-lint", cfg_path], check=True)
    print(f"[OK] Validated staged config.plist at {cfg_path}")

if __name__ == "__main__":
    main()
