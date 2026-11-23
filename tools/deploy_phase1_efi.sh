#!/bin/zsh
set -euo pipefail
export PATH="/usr/bin:/bin:/usr/sbin:/sbin"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGED_EFI="$ROOT/efi_staged/EFI"

if [ ! -f "$STAGED_EFI/OC/config.plist" ] || [ ! -d "$STAGED_EFI/OC/Kexts/GP108Bridge.kext" ]; then
    echo "ERROR: Staged EFI is incomplete at $STAGED_EFI" >&2
    exit 1
fi

if ! diskutil info disk0 | grep -q "Device Location: *Internal"; then
    echo "ERROR: disk0 is not the internal NVMe drive; aborting for safety!" >&2
    exit 1
fi

PRE_MNT=$(diskutil info disk0s1 | sed -n 's/^ *Mount Point: *//p')
MOUNTED_BY_US=0
if [ -z "$PRE_MNT" ]; then
    diskutil mount disk0s1
    MOUNTED_BY_US=1
fi

MNT_PT=$(diskutil info disk0s1 | sed -n 's/^ *Mount Point: *//p')
if [ -z "$MNT_PT" ] || [ ! -d "$MNT_PT/EFI/OC" ]; then
    echo "ERROR: Failed to locate mounted ESP at $MNT_PT" >&2
    exit 1
fi

echo "[1/3] Preserving original config.plist on ESP as config.backup.plist..."
if [ ! -f "$MNT_PT/EFI/OC/config.backup.plist" ]; then
    cp "$MNT_PT/EFI/OC/config.plist" "$MNT_PT/EFI/OC/config.backup.plist"
fi

echo "[2/3] Installing GP108Bridge.kext and updated config.plist to ESP..."
rm -rf "$MNT_PT/EFI/OC/Kexts/GP108Bridge.kext"
cp -R "$STAGED_EFI/OC/Kexts/GP108Bridge.kext" "$MNT_PT/EFI/OC/Kexts/GP108Bridge.kext"
cp "$STAGED_EFI/OC/config.plist" "$MNT_PT/EFI/OC/config.plist"
sync

if [ "$MOUNTED_BY_US" -eq 1 ]; then
    echo "[3/3] Unmounting ESP..."
    diskutil unmount disk0s1
fi

echo "Done! Phase 1 OpenCore config + GP108Bridge.kext deployed to disk0s1."
