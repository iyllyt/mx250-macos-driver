#!/bin/zsh
set -uo pipefail

export PATH="/usr/bin:/bin:/usr/sbin:/sbin"

OUT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HW_DIR="$OUT_DIR/hw_dump"
EFI_BACKUP_DIR="$OUT_DIR/efi_backup"

mkdir -p "$HW_DIR" "$EFI_BACKUP_DIR"

echo "[1/4] Activating Xcode Command Line Tools..."
xcode-select --switch /Library/Developer/CommandLineTools || true

echo "[2/4] Exporting raw ACPI tables (DSDT/SSDT) and DeviceTree from IORegistry..."
ioreg -rd1 -lw0 -c AppleACPIPlatformExpert > "$HW_DIR/acpi_platform_expert_root.txt"
ioreg -lw0 -p IOACPIPlane > "$HW_DIR/acpi_plane_root.txt"
ioreg -lw0 -p IODeviceTree > "$HW_DIR/devicetree_root.txt"

echo "[3/4] Backing up OpenCore EFI partition (read-only mount)..."
PRE_MNT=$(diskutil info disk0s1 | sed -n 's/^ *Mount Point: *//p')
MOUNTED_BY_US=0
if [ -z "$PRE_MNT" ]; then
    diskutil mount readOnly disk0s1
    MOUNTED_BY_US=1
fi

MNT_PT=$(diskutil info disk0s1 | sed -n 's/^ *Mount Point: *//p')
echo "EFI mounted at: $MNT_PT"

if [ -n "$MNT_PT" ] && [ -d "$MNT_PT" ]; then
    ls -laR "$MNT_PT" > "$HW_DIR/efi_tree.txt"
    rm -rf "$EFI_BACKUP_DIR/ESP"
    mkdir -p "$EFI_BACKUP_DIR/ESP"
    cp -R "$MNT_PT"/ "$EFI_BACKUP_DIR/ESP/"
fi

if [ "$MOUNTED_BY_US" -eq 1 ]; then
    diskutil unmount disk0s1 || true
fi

echo "[4/4] Setting ownership to maple:staff..."
chown -R maple:staff "$OUT_DIR"

echo "Done! Phase 0 root backup and ACPI dump completed successfully."
