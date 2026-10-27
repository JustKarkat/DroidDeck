# Shield TV cleanup — 2026-10-23

Device: SHIELD Android TV (mdarcy), 192.168.0.194:5555
Partition: /data (mmcblk0p32) 12 GB.

## Before
- /data: 10.0 GB used, 1.5 GB free (87%)

## Actions taken (safe, reversible-ish)
1. `pm trim-caches 2000000000 internal` — reclaimed ~0.3 GB of app caches.
2. Removed guest rootfs linux-firmware except nvidia/ (982 MB → 153 MB). Guest (proot) cannot load kernel modules/firmware; host has its own. Reclaimed ~0.8 GB.
3. Removed guest rootfs /usr/lib/modules (192 MB) — not usable under proot.
4. Removed leftover /data/local/tmp/probe9.py.
5. `sm fstrim`.

## After
- /data: 8.6 GB used, 2.8 GB free (76%)

## Still large (intentionally kept)
- Proton Experimental (ARM64) ~2.0 GB — needed for Steam; deleting saves space but costs re-download churn. Delete only if space is critical.
- steamrtarm64 ~663 MB — Steam runtime, needed.
- Minecraft PE download folder ~954 MB under /sdcard/Download/com.mojang.minecraftpe — user data; not touched.
- App APKs total ~4.2 GB across installed apps.

## Optional further cleanup (user decision)
- Uninstall rarely-used streaming apps (pluto, viki, hulu, etc.) via `pm uninstall <pkg>`.
- Delete Minecraft PE download folder if no longer needed.
- Delete Proton Experimental if Steam install is being abandoned.

## USB as additional storage
No USB drive was connected during this run. Adoptable storage is supported (`sm has-adoptable` = true).

When a drive is plugged in:
```sh
adb -s 192.168.0.194:5555 shell "sm list-disks"          # note disk id, e.g. disk:8,0
adb -s 192.168.0.194:5555 shell "sm partition disk:8,0 private"   # adoptable (merges into /data)
# or: sm partition disk:8,0 public   # portable, mounts under /storage/<uuid>
adb -s 192.168.0.194:5555 shell "sm list-volumes all"
```
Adoptable (private) expands internal storage; portable keeps it separate. Adopting wipes the drive. For DroidDeck's container, portable is safer (point linuxfs at it) but adoptable is simplest.

After adopting, move large static trees (e.g. firmware, steamrt) only if needed; DroidDeck paths are hardcoded under files/linuxfs, so moving requires symlinks inside the container.
