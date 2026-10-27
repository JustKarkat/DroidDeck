#!/usr/bin/env bash
# USB storage helper for Shield TV. Run steps manually after plugging a drive.
set -euo pipefail
SERIAL="${SERIAL:-192.168.0.194:5555}"
echo "Disks:"; adb -s "$SERIAL" shell "sm list-disks"
echo "Adoptable?"; adb -s "$SERIAL" shell "sm has-adoptable"
echo
echo "To adopt (wipes drive, merges into /data):"
echo "  adb -s $SERIAL shell \"sm partition disk:X,Y private\""
echo "To use as portable:"
echo "  adb -s $SERIAL shell \"sm partition disk:X,Y public\""
echo "Then: adb -s $SERIAL shell \"sm list-volumes all\""