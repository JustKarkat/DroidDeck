# Shield TV — USB adoptable/partition expansion attempt (handoff)

## RESOLVED 2026-10-23 20:57 — drive mounted, FIXED

Resolution path (verified):
1. Wedge root-caused from vold log: at every boot since the failed `sm partition private`,
   vold auto-adopted the leftover `android_expand` partition (sda2) with its found key,
   failed the rw mount (EROFS), leaked a read-only fallback mount at
   `/mnt/expand/<uuid>`, and that mount held `dm-2` busy until the next power cycle —
   so every later format/partition attempt failed with EBUSY ("Device or resource
   busy", `DM_DEV_REMOVE failed`, exit 255 with no message).
2. `adb reboot` never took effect (hung; uptime kept counting — verify with
   `cat /proc/uptime` before assuming). A HARD power cycle (unplug power, replug)
   broke the wedge: fresh boot came up with NO stale dm-2 mount and disk re-scanned.
3. Immediately after cold boot, `sm forget all`, then
   `sm partition disk:8,0 public` → rc=0.
4. Result: `public:8,1` (FAT32, UUID 09A3-1C22) MOUNTED rw at `/storage/09A3-1C22`
   (~30 GB free). Shell rw verified (probe.txt write/read/delete).
   vold log noise about "NVIDIA_SHIELD shared folder: Operation not permitted" and
   brief bad_removal broadcasts is harmless; volume ends MOUNTED with speed class 8.

Staging recipe for the ShieldDeck goal (verified 2026-10-23 21:05):
- Host (or adb shell) can read/write the volume root directly: /storage/09A3-1C22/.
  Use it to HOLD big tarballs (adb push steam.tar.gz /storage/09A3-1C22/).
- Stream USB → app data WITHOUT materializing on /data (peak-space halved):
  adb -s <serial> shell 'cat /storage/09A3-1C22/x.tar' | \
  adb -s <serial> shell run-as com.droiddeck.launcher dd of=files/linuxfs/x.tar
  (then extract inside the guest). NO shell redirection under run-as — /system/bin/sh
  redirection into app files is SELinux-denied (Permission denied); exec'd binaries
  (dd/touch) write fine. Relative paths in run-as are flaky; prefer absolute or keep cwd.
- The app itself still has zero access to the volume (scoped storage; run-as reads
  denied). To let the launcher use the USB natively it must call
  Context.getExternalFilesDirs() — small code change, only needed for in-app staging.
- Rooting decision (user offered): unlock+Magisk on mdarcy IS possible; root enables
  ext4-on-USB bind-mounted into the proot rootfs ( moves steamapps/Proton OFF /data
  for real). Costs: full data wipe (deck setup rebuilt), DV + AI-upscaling lost
  forever, documented mdarcy recovery quirk raises un-brick risk. Docs require user
  opt-in for unlock; suggested deferring until after Phase 1 proof since unlock is
  also needed for Phase 2 NVK anyway.

Key tips for next time on THIS device:
- Cold power cycle, not `adb reboot`, when the volume state table wedges.
- Run the `sm partition` immediately after cold boot, before vold re-attempts the
  leftover private volume adopt (window ~30–60 s).
- Verify state with `cat /proc/uptime` (fresh boot = <120 s), `sm list-volumes all`,
  `cat /proc/mounts | grep -E 'expand|dm-2'`.
- `deviceHasKeyguard=false` — the "secure keyguard" theory in the original doc was
  wrong; the "delaying scan due to secure keyguard" vold message resolved, not blocked.
- If the drive must be internal-style storage (adoptable/private) someday, the
  rw-bricks-back-EROFS mystery on dm-crypt ext4 remains unsolved; public FAT32 is
  what works.



Device: SHIELD Android TV (mdarcy), 192.168.0.194:5555
Internal /data: mmcblk0p32, 12 GB. USB drive: ~32 GB, disk:8,0 (was sda1..sda5, now sda1,sda2).

## What worked
- Minecraft PE folder deleted (~954 MB freed).
- Firmware/modules trimmed; /data now ~7.9 GB used, 3.5 GB free (70%).
- USB drive detected and adoptable storage supported (`sm has-adoptable` = true).

## What did NOT work
Goal: use the USB drive as additional storage (adoptable/private preferred).

Commands tried:
1. `sm partition disk:8,0 private` (backgrounded; eventually completed).
   - Created partitions sda1/sda2 and a volume `private:8,2`, but it came up `unmountable` with "filesystem check: Device or resource busy".
   - `sm format private:8,2` + `sm mount private:8,2` reported success but volume stayed `unmountable` (new UUID each format).
   - After reboot, the adopted volume entry disappeared; `sm list-volumes` only shows internal `private` + `emulated;0`.
2. `sm partition disk:8,0 public` (backgrounded) — device went offline during/after; on return only sda1/sda2 remained, no public volume mounted. `sm forget all` cleared stale entries but did not help mount.

Current state after reboot:
- `sm list-volumes all`: `private mounted null`, `emulated;0 mounted null` only.
- USB block device still `sda` with `sda1`, `sda2`.
- /data unchanged (12 GB, 3.5 GB free).
- No USB volume mounted under /storage.

## Likely issue
Adoptable/private volumes on this Shield (Android 11 / Shield Experience 9) appear to need a user lock-screen credential to bind the encryption key, or vold's filesystem-check is racing. Portable/public partitioning crashed vold/offline, so the drive may be left half-partitioned.

## Suggested next steps for next agent
- Re-run from a clean slate: `sm forget all`, then `sm partition disk:8,0 public` (portable) and wait for completion without rebooting; check `sm list-volumes all` for a `public` volume and mount path under /storage.
- If adoptable is still desired, try `sm set-force-adoptable on` first, then partition private, and ensure a PIN/password is set on the Shield before adopting (encryption key binding).
- Alternative: skip sm entirely and manually format sda1 as exfat/fat32 via `mkfs` in a root shell (requires root, which this device lacks) — not viable without unlock.
- Alternative: use the drive via DroidDeck's proot by mounting it manually with `mount` (needs root) — not viable.
- Safest fallback: leave drive unpartitioned/unused and rely on the 3.5 GB free + trimmed rootfs; or move large static trees (Proton 2 GB) off via user-level file copies if a mount point becomes available.

## Notes
- Device has no root (`su` absent, bootloader locked/verifiedboot green).
- Network ADB address 192.168.0.194:5555; may need re-enable after reboots.
- Do not delete Proton/steamrt without explicit user consent (breaks Steam).