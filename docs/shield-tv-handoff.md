# ShieldDeck handoff — Shield TV (192.168.0.194:5555, network adb)

**Goal (from compaction, verbatim):** a working build of droid deck (with steam and ability to play games using the onboard CPU & GPU). Do not chase unrelated paths.

## Where we are

- **Phase 1 (now): CPU-only via lavapipe.** Single blocker: gamescope 3.16.29 dies ~1.4 s after start with `vkAllocateMemory failed (VkResult: -2)` then assert at `rendervulkan.cpp:3178` (`vulkan_create_flat_texture`). Fix this → Steam boots under lavapipe → measure FPS/RAM.
- **Phase 2 (deferred):** onboard GPU (NVK). Stock 4.9 kernel has no `/dev/dri` → NVK impossible without bootloader unlock/custom kernel (user opt-in only; wipes data). Fork stays driver-portable (Adreno native).

## Verified facts (on device / on code)

- Guest env healthy: no rlimit/cgroup limit; lvp heap budget 2.88 GiB; memTypeBits==0x1 seen; type0 = DEVICE_LOCAL.
- Our `vkprobe3.py` replicated gamescope's full DEDICATED+Export-DMA_BUF+WSI alloc chain (1920×1080 B8G8R8A8, usage 0xC, `typeBits=0x1`) → **succeeds**. So the failure is device-context-specific (extensions/features/device flags), not lvp health.
- lvp dynsym exports ONLY: `vk_icdGetPhysicalDeviceProcAddr` @0xd42a0, `vk_icdNegotiateLoaderDeviceInterfaceVersion` @0xd40a0, `vk_icdGetInstanceProcAddr` @0xed0e0 (no direct `vkAllocateMemory`). Loader `libvulkan.so.1` also stripped. → ICD pointer must be caught at runtime via `vk_icdGetInstanceProcAddr` return trap.
- lvp base under gdbserver (ASLR disabled) is deterministic: `0x2004560000` (from `info proc mappings` at the SIGABRT stop in watch3c.out).
- SIGABRT lands in libc `raise` at pc `0x200141a848`; `__assert_fail` unresolved (lazy solib).
- Steam client partially installed: Proton Experimental (ARM64) fully present at `root/.local/share/Steam/steamapps/common/Proton Experimental (ARM64)` (~2 GB). Client itself missing `webkit` part (died with curl:23 when /data hit 100%).

## Disk state

`/data/user/0` was 100% (75 MB free → curl:23 on webkit download). Freed by deleting app-owned dead weight:
`usr/share/{locale,ibus,gir-1.0,man,vim,wallpapers}`, `boot/*` (kernels unused under proot). Now ~836 MB free (93%). **Permanent until r10; record if reverting.**
Safe to delete more if needed: `root/.local/share/Steam/steamapps/common/Proton Experimental` (~2 GB) — but client re-download costs ~2.5 GB churn; prefer finishing current client install.

Do NOT delete user files under `/sdcard/Download` (AuroraStore APK, Downloader/, com.gamepass/, com.mojang.minecraftpe/) or my small leftovers there (~20 MB total — not worth touching).

## Disk math for finishing Steam install

Webkit package is ~500-700 MB peak (download + unzip). ~836 MB free is enough for one package at a time (each part is rm'd after unzip; client tree mostly present). Retry flow: `droiddeck-steam-install` re-downloads all 17 parts if stamp missing — no resume. Need ~1.5 GB headroom to be comfortable.

## Build & deploy cycle (used today)

```
python3 tools/release/runtime_inputs.py record   # hashes overlay files
./gradlew :app:assembleRelease -q
adb -s 192.168.0.194:5555 install -r app/build/outputs/apk/release/app-release.apk
```

Session script patched in overlay: after gamescope rc≠0 → `start_agent --no-focus; sleep 900; exit $bl_rc` (terminal keep-alive, no fall-through into steam bootstrap). Installed APK reflects this patch.

Installed APK is current (built 21:02, installed Success). Do NOT reinstall unless overlay changes.

## Session mechanics

- `python3 tools/droiddeckctl --serial 192.168.0.194:5555 start steam` → session stages, steam-check, gamescope crashes at ~1.4 s, keep-alive sleeps 900 s.
- `droiddeckctl state` → machine JSON (session id, phase, running, guestPid).
- `droiddeckctl guest <cmd> [args...]` → literal argv (NO shell syntax); wrap complex things in `/bin/sh -c '...'`.
- `pkill -9 -f '^/usr/local/bin/gdbserver'` to kill old gdbserver (anchored pattern avoids killing self).
- Push files to guest rootfs via: `cat file | adb -s ... shell "run-as com.droiddeck.launcher sh -c 'cat > files/linuxfs/usr/local/bin/<name> && chmod 755 ...'"` — **CAUTION 2026-10-23: this does NOT survive session starts** for anything SessionFiles stages from APK assets every start (gamescope included, `SessionFiles.kt:117-124`); earlier note that staging "only overwrites APK assets" was wrong. Such files must ship inside the APK asset dir.
- Session logs: `files/logs/<session-id>/session.log` (app files dir, readable via run-as cat).

## GDB harness (in progress — next move)

Host gdb (x86_64 CachyOS, `--enable-targets=all` → aarch64-capable) connects via `adb forward tcp:<port> tcp:<port>` → guest `gdbserver --multi 0.0.0.0:<port>` (Arch ARM gdb-18.1, at guest `/usr/local/bin/gdbserver`, pushed 21:09).

Key facts:
- One-shot gdbserver serves ONE client; `run` restart works under `--multi` + `target extended-remote`.
- Ports tested OK: 4913 (full success, watched SIGABRT stop). Connect timeouts earlier were stale-forward artifacts: remove+re-add forward fixes.
- The connect works reliably when done cleanly in ONE call: kill old server → spawn `gdbserver --multi` → `forward --remove-all` → `forward tcp:P tcp:P` → run gdb batch.

Guest files (aarch64, from Arch ARM gdb-18.1 pkg at /tmp/opencode/gdbpkg/):
- `gdbserver` → guest `/usr/local/bin/gdbserver` (pushed 21:09, runs fine)
- `gdb` → guest `/usr/local/bin/gdb` (pushed 21:21) — BUT missing libs: `libxxhash.so.0`, `libguile-3.0.so.1`, `libsource-highlight.so.4` (+ guile chain: libgc etc.). In-guest gdb likely dead-end under proot (ptrace conflict). Host-gdb route is working instead.

Host-side sysroot (byte-identical copies from device):
- `/tmp/opencode/hsgb/sysroot/usr/local/bin/gamescope` (stripped PIE)
- `/tmp/opencode/hsgb/sysroot/usr/lib/libvulkan_lvp.so` (stripped, dynsym: only 3 ICD entrypoints)
- `/tmp/opencode/hsgb/sysroot/usr/lib/{libc.so.6,libm.so.6,ld-linux-aarch64.so.1}`
- `sysroot/lib/ld-linux-aarch64.so.1` → symlink to `../usr/lib/ld-linux-aarch64.so.1` (gdb solib-tracking)

## ROOT CAUSE FOUND (confirmed via gdb)

**gamescope `rendervulkan.cpp:2330` casts `findMemoryType()`'s `int32_t` return (-1 on failure) directly to `uint32_t`, producing `memoryTypeIndex = 0xFFFFFFFF`.** `vkAllocateMemory` then returns -2 (`VK_ERROR_OUT_OF_DEVICE_MEMORY`), `BInit` returns false, and `assert(bRes)` at `rendervulkan.cpp:3170` fires SIGABRT.

GDB evidence: SIGABRT at `0x200156a848` (libc `raise` → `__assert_fail`), corrupt 2-frame backtrace, x6/x7 contain assertion-message fragments. `dlopen` breakpoint never resolves (stripped binary), so `IcdHook`/`DevHook` never fire — `vk_icdGetInstanceProcAddr` lookups all happen before the hook is installed.

**Fix** (applied to `/tmp/opencode/rendervulkan.cpp`):
1. Line 2327-2332: Added `if (memTypeIndex == ~0u) { vk_log.errorf(...); return false; }` — matches the existing pattern at lines 1046-1050 and 4770-4774 (the only call site missing it).
2. Line 3176-3179: Changed `assert(bRes)` to `if (!texture->BInit(...)) return nullptr;` — callers already null-check (`steamcompmgr.cpp:3314` checks `tex != nullptr`), so gamescope continues without the hacky blank texture.

**Prerequisite done on device:** `libvulkan_lvp.so` + `lvp_icd.json` pushed to guest rootfs (`linuxfs/usr/lib/` and `linuxfs/usr/share/vulkan/icd.d/`). `LinuxRuntime.vulkanIcd()` selects lvp over freedreno (name contains "lvp"), and `SessionService.kt:523` sets `VK_ICD_FILENAMES` to the manifest path.

## Next move (CI build → local APK swap → test)

There is no local ARM64 toolchain on this x86_64 host, and none is needed: the gamescope binary
is produced by `.github/workflows/build-gamescope.yml` (runs on `ubuntu-24.04-arm`, builds inside
`menci/archlinuxarm:base-devel` with `tools/gamescope/build-in-arch.sh`). Everything the fix
needed is a patch file next to the others:

- 2026-10-23: `tools/gamescope/patches/0116-vulkan-texture-alloc-failure.patch` added
  (commit e5dc28f on `shield-tv`), verified to apply cleanly to upstream 3.16.29
  (`patch -p1 --dry-run`). Source diff is exactly the two edits; the working copy
  `/tmp/opencode/rendervulkan.cpp` diffs against a pristine 3.16.29 `src/rendervulkan.cpp`
  with only those edits.

Build flow used (fork — no push access to `Droid-Deck/DroidDeck`):

```
gh repo fork Droid-Deck/DroidDeck --clone=false        # fork: JustKarkat/DroidDeck
git remote add fork https://github.com/JustKarkat/DroidDeck.git
git push fork shield-tv                                 # touches tools/gamescope/** → CI
gh api -X PUT repos/JustKarkat/DroidDeck/actions/permissions -F enabled=true -F allowed_actions=all   # forks start with Actions off
gh api -X POST repos/JustKarkat/DroidDeck/actions/workflows/build-gamescope.yml/dispatches -F ref=shield-tv
gh run watch <run-id> --repo JustKarkat/DroidDeck       # then: gh run download <run-id> -n gamescope-patched -R JustKarkat/DroidDeck
```

Install path (gamescope is **not** push-survivable): `SessionFiles.kt:117-124` restages
`usr/local/bin/gamescope` from the APK assets at *every* session start, unconditionally
(staged-then-rename). A bare push to the guest rootfs is clobbered on the next start. So:

1. Unpack the artifact (`gamescope.tzst` → `usr/local/bin/gamescope`), sha256 check.
2. Overwrite `app/src/main/assets/linuxfs/usr/local/bin/gamescope` (git-ignored asset; gradle
   packages it as-is — `stageRuntimeAssets` does no download). Old sha was
   6e1eb25824455fae1f58c07fc3b2f4e86cd9b5d6bc1339fb18207723af86079a.
3. Rebuild + install (`./gradlew :app:assembleRelease`, `adb install -r`), then
   `python3 tools/droiddeckctl --serial 192.168.0.194:5555 start steam --wait --timeout 120`.
4. Verify Steam UI up under lavapipe + measure FPS/RAM → Phase 1 exit criterion met.

Upstream/legalization for later (not needed to unblock): publish the CI artifact as a release in
`Droid-Deck/DroidDeck-Components`, bump `tools/gamescope/release.env` (tag + sha), so release
builds pick the fixed binary automatically.

## Important context

- `runtime_inputs.py record` was run before the present build and does NOT need re-running for lvp/gdbserver/gdb file pushes (only for `tools/linuxfs/overlay` script changes).
- Device session currently (as of ~21:34 CDT 2026-10-22): session `2026-10-22-13-steam`, phase STARTING_STEAM, running=True — keep-alive window expires ~15 min after crash. May need fresh `start steam` now.
- `droiddeckctl` binary at repo root; session scripts staged from `tools/linuxfs/overlay/` via `stageSessionScripts` gradle task into APK assets.
- GPL-3.0 obligation for DroidDeck-derived work; no flashing/bootloader/bundling NVIDIA blobs without explicit user confirmation.
- Docs: `docs/shield-tv.md` holds Phase-0 probe notes + "Realignment — 2026-10-22" section with the user directive quoted verbatim.
