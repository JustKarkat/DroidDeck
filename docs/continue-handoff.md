# Shield TV — handoff checkpoint (2026-10-23, evening)

## Goal
Boot Steam (CPU-only lavapipe) on Shield TV (192.168.0.194:5555, network adb), fix gamescope crash, then download+play a game and publish.

## Phase-1 LADDER (where we are now)
1. ~~gamescope vkAllocateMemory -2 crash~~ **FIXED** (0122: gate DMA_BUF export on ICD caps; committed aa13b2a+15:44Z patch `tools/gamescope/patches/0122-dmabuf-export-gate.patch`). Gamescope now boots and runs clean under lvp.
2. ~~Steam client SIGILL 132~~ **FIXED**: Valve raised native ARM64 client to ARMv8.1 (LSE) after build 1776387948, Apr 15 2026 (steam-for-linux#13288). Cortex-A57 = no LSE. Fix = pin the install:
   - `droiddeck-steam-install` now installs client **1776387948** from SteamTracking's archived manifest (commit f58aaa973df1f21e1208f6d65b448db677ec31e8). Valve's CDN still serves all 17 zips. Auto-repair (pin-catch) if installed > pin.
   - Valve ARM64 zips ship ELFs with **no exec bits** in these manifests (good files needed chmod). Installer now exec-bits every ELF/.sh under steamrtarm64/linuxarm64.
   - Steam.cfg with BootStrapperInhibitAll=Enable in the steam root ***suppresses*** the client self-update — verified "Suppressing Steam update" in bootstrap log (Fake beta channel `droiddeck-pinned-v8.0` in `package/beta` + `-clientbeta` arg as belt: client falls back to the stable channel on a 404 for the fake channel, so Steam.cfg is the load-bearing part). Unsupported-ish config path: if it fails later, the fallback is blocking client-update hosts in /etc/hosts.
3. ~~zink GL fail (`glXChooseVisual failed`)~~ **FIXED**: Tegra no longer gets the zink env override (`SessionService.kt` — `DeviceSupport.tegra()` guard around MESA_LOADER_DRIVER_OVERRIDE/GALLIUM_DRIVER/LIBGL_KOPPER_DRI2).
4. **Current blocker**: panorama/gamepad UI window never paints a first frame. CEF children (renderer, network service) crash repeatedly; webhelper browser process uses in-process GPU with ANGLE-on-lvp → watchdog kills renderers after whatever-this-is. Now: `-cef-disable-gpu` (software raster, BL_TEGRA=1 signal) + LP_NUM_THREADS=2 (llvmpipe thread-pool memory) + fresh `/tmp/dumps` (perm 777) so breakpad gives us actual crash dumps instead of silence. Latest run: the client is ALIVE (audio login sound played per user), UI still hasn't painted.

## Facts of the run so far
- Client tree pinned on device: all parts April 15 build (steam/steamclient.so/steamwebhelper sizes match 1776387948, mtime 2026-04-15 12:55). BootStrapperInhibitAll verified suppressing the update UI ("uninstalled manifest" cycle didn't re-download).
- MemAvailable at night: 360-480 MB free of 3 GB with session + Android running. gamescope RSS ~920MB under llvmpipe (biggest single consumer). CEF ~600MB. It is *plausible* the renderers die from memory pressure; not confirmed. Crash-dump dir should show it next run.
- Panorama init messages: "XRRGetOutputInfo() is not available" — benign (Steam has a workaround: "initialized with override: 1 real: (nil)")
- "steam-runtime-launcher-service: not found" — the guest rootfs install dropped it or it isn't in PATH ("possible problem, disabling"). Affects run-through with the runtime-launch env for games. Park until Steam UI works.
- `-clientbeta droiddeck-pinned-v8.0` is logged as `droiddeckpinnedv80` (Sanitization) — deliberate.
- dmesg blocked; /proc/vmstat blocked; oom evidence can only come from the crashpad dumps (now captureable).

## What succeeded and shipped today (all pushed to JustKarkat/DroidDeck branch shield-tv)
- 0122 gamescope DMA_BUF gate (aa13b2a) — gamescope crash fixed.
- aa13b2a: LSE atomics disabled in guest glibc tunables + libdecor cairo-only.
- pin-catch auto-repair + exec bits + Steam.cfg in droiddeck-steam-install.
- BL_TEGRA=1 + LP_NUM_THREADS=2 in SessionService guests env (guarded on DeviceSupport.tegra()) to the session script rewrite.
- `-cef-disable-gpu` on Tegra in droiddeck-session.
- Loading log overlay (loadingLogEnabled setting), tested.
- CI run 40007876024 green; APK installed. Session `2026-10-23-63-steam` currently mid-flight: client ALIVE, UI unpainted, "login sound" heard by user.

## Next moves
1. Get the UI: the run in flight uses `-cef-disable-gpu` + LP_NUM_THREADS=2. Take a screenshot once it stops. If no frame, use the crashpad dumps (should now exist) — `/tmp/dumps/*.dmp` — pull one and decode (host breakpad tools or `minidump-stackwalk`; build a local aarch64 sysroot from the client binaries if symbols needed).
2. If loops of renderer death continue: try `--js-flags`/`--single-process`? Wait no — try adding `--renderer-process-limit=1` to the webhelper args via the steam client? Valve's arg plumbing doesn't pass graphics switches through easily; the droiddeck-session's steamwebhelper.sh is a fixed client script — inspect `steamrtarm64/steamwebhelper.sh` contents for an EXTRA_ARGS hook.
3. If the first frame eventually comes (slowness only): Phase-1 verification = screenshot + FPS not needed for a UI-only state. Then install a small game droiddeck-side, launch through droiddeck-proton-arm64, and measure FPS (mangohud overlay already active).
4. When Steam UI works end-to-end: revert diag (remove 0117-0122 patches from gamescope build? — they log every texture alloc: 0119/0120 diags should get REMOVED before release builds; keep 0122 gate). Delete `.CleanupShield` helper dir? — check what it is; keep local copies for later.
5. Disk-headroom note: /data had 2.3GB free before tonight's two ~500-670MB self-update churn events (which staged a voucher in package/steam_client_*). If Steam.cfg keeps future self-update from resuming, the disk frees on the next forced reinstall once; disk economics may still be the binding constraint for phase-1: monitor.
6. User added a **new logo file "ShieldDeck.png"** (user-comment). It's for the ShieldDeck port rebranding. Not yet integrated — pick it up when doing the branding/assets pass (leanback banner, app icon). Path unknown — ask user / find by name.

## Operational notes (carried forward)
- Guest exec only works while a session is "kept alive for probing". Force-stop the app to reset: `adb shell am force-stop com.droiddeck.launcher`.
- NEVER write empty values to `gamescope_*` env vars (upstream convar strlen(NULL) segfault, exit 139).
- Push files to guest rootfs via: `cat file | adb -s ... shell "run-as com.droiddeck.launcher sh -c 'cat > path && chmod ...'"` — **CAUTION: does NOT survive session starts** for anything SessionFiles stages from APK assets every start (gamescope, session scripts). Script/binary pushes to `$steam_root` (steamrtarm64) DO survive.
- Session logs: `files/logs/<session-id>/session.log` (app files dir, readable via run-as cat).
- Steam client logs live in the GUEST: `files/linuxfs/root/.local/share/Steam/logs/` (bootstrap_log.txt, steamui_system.txt, steamui_html.txt, cef_log.txt).
- Build & deploy cycle: `python3 tools/release/runtime_inputs.py record` then `./gradlew :app:assembleRelease -q`, `adb -s 192.168.0.194:5555 install -r app/build/outputs/apk/release/app-release.apk`, `python3 tools/droiddeckctl --serial 192.168.0.194:5555 start steam`.
- runtime_inputs.py record is required ONLY for `tools/linuxfs/overlay` changes (and binaries).

## Crash (now historical, kept for reference)
See git history for the 0117-0121 diag patch trail. Root cause was gamescope's `findMemoryType()` returning -1 for the flippable/unassociated allocation on llvmpipe, which the 0122 gate now sidesteps by not requesting DMA_BUF export at all when the ICD reports 0 export features for it. Symptom chain: `vkAllocateMemory failed (VkResult: -2)` → `Assertion 'm_texture' failed` at `steamcompmgr.cpp:2369` (`MouseCursor::getTexture`). The 0116 texture-alloc patch fixed the original cursor site; failure moved to the output image (bFlippable→bExportable→DMA_BUF export chain).

## Probe matrix (in-process, 0120/0121 diag patches)
At the pre-texture alloc site in `CVulkanTexture::BInit` (gamescope process):
- `none` (bare 2304): OK
- `wsi` (WSI pNext): OK
- `ded` (dedicated): OK
- `exp` (VkExportMemoryAllocateInfo, DMA_BUF): **-2**
- `full`: -2
- 0121 `opaque` (handleTypes=OPAQUE_FD): **OK**
- 0121 `freshdev` (fresh VkDevice, same instance, DMA_BUF export): **-2**
- 0121 `freshinst` (fresh VkInstance+device, DMA_BUF export): **-2**

Identical DMA_BUF export request (handleTypes=2) in a Python process on the same device: **OK**. Same physical device, same ICD (`llvmpipe (LLVM 22.1.8, 128 bits)`), same caps (probe-ext.py reports features=0/export=0/import=0/compat=0 for BOTH OPAQUE_FD and DMA_BUF).

Rootfs facts:
- `/dev/udmabuf` does NOT exist; `/dev/dri` does NOT exist; kernel 4.9.141-tegra. Per mesa 26.2.5 source, `llvmpipe_dmabuf_alloc` requires `screen->udmabuf_fd` (open `/dev/udmabuf`) → should always fail on this device.
- Yet python DMA_BUF export succeeds → resolves to the OPAQUE (memfd) path. gamescope's identical call resolves to the DMA_BUF (udmabuf) path → -2.

## Debugging recipes
- gdbserver: `setsid /bin/sh /tmp/g19-gdb.sh &`, `adb forward tcp:4920 tcp:4920`, host gdb with `set architecture aarch64` + `file gs-out/usr/local/bin/gamescope`. Use `catch syscall`, not `break` (libc symbols don't resolve remotely without sysroot). Host sysroot in `/tmp/opencode/hsgb/sysroot`. lvp base under gdbserver deterministic: `0x2004560000`.
- Pull host-side: `gh run download <run-id> -n gamescope-patched -R JustKarkat/DroidDeck`; gamescope release asset path + sha in the build logs; APK asset overwrite workflow documented in docs/shield-tv-handoff.md ("Next move (CI build → local APK swap → test)").
- Disk math / Steam client install size: client ~1.5 GB churn per (re)install; keep ~2 GB free on /data before repairs.
- `/tmp/opencode/probe-ext.py`, `/tmp/opencode/probe9.py`, `/tmp/opencode/trace-gs.log`, `/tmp/opencode/trace-py.log` (python replication + syscall traces); `/tmp/opencode/mesa-src/mesa-26.2.5/` (mesa source).
