#!/bin/zsh
set -euo pipefail
export PATH="/usr/bin:/bin:/usr/sbin:/sbin"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BACKUP_EFI="$ROOT/efi_backup/ESP/EFI"

if [ ! -f "$BACKUP_EFI/OC/config.plist" ]; then
    echo "ERROR: Backup EFI missing at $BACKUP_EFI" >&2
    exit 1
fi

PRE_MNT=$(diskutil info disk0s1 | sed -n 's/^ *Mount Point: *//p')
MOUNTED_BY_US=0
if [ -z "$PRE_MNT" ]; then
    diskutil mount disk0s1
    MOUNTED_BY_US=1
fi

MNT_PT=$(diskutil info disk0s1 | sed -n 's/^ *Mount Point: *//p')
cp "$BACKUP_EFI/OC/config.plist" "$MNT_PT/EFI/OC/config.plist"
rm -rf "$MNT_PT/EFI/OC/Kexts/GP108Bridge.kext"
sync

if [ "$MOUNTED_BY_US" -eq 1 ]; then
    diskutil unmount disk0s1
fi

echo "Restored original OpenCore EFI configuration from backup."
