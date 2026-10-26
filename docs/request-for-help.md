# Request for help — Shield TV (Tegra X1) Phase 1: Steam panorama never survives webhelper renderer crash

Written 2026-10-23 ~23:45 CDT. **Please research only; I am actively bisecting on device.** If you find something actionable, write findings to `docs/request-for-help-replies.md` instead of touching code/logs.

## What I'm building (context)

DroidDeck (Android launcher, repo `ShieldDeck`, branch `shield-tv`) runs Steam inside a proot Linux guest (`linuxfs/`) driven by a patched gamescope compositor, in a wayland-composited app window. Current device: SHIELD Android TV (Tegra X1, 3 GB, Android, no root). Phase 1 goal = **steam renders on lavapipe (llvmpipe / Mesa lvp)** — the box has no usable hardware Vulkan path (no NVK / no freedreno; Tegra).

Key code touchpoints:
- `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` — guest session script; steam launch flags around lines 640–700.
- `tools/gamescope/patches/0116…0122` — gamescope 3.16.29 patch series for lvp alloc failures.
- `SessionService.kt:523` — sets `VK_ICD_FILENAMES` to lvp manifest; `LinuxRuntime.vulkanIcd()` prefers "lvp".
- Centrally-controlled env: `BL_TEGRA=1` on this device (verified: `-cef-disable-gpu` and `rm -rf /tmp/dumps; mkdir -m 777 /tmp/dumps` are in effect in the running session).

## What is already verified working (do NOT re-suggest)

- lvp ICD (`libvulkan_lvp.so` + `lvp_icd.json`) staged into guest rootfs; gamescope picks lvp.
- gamescope `vkAllocateMemory` returns VK_SUCCESS everywhere (0119/0121 diag probes: "bare alloc size=... -> 0"), textures allocate (2048, 3686400, 8294400, 3563520… all 0).
- Android app boots, session starts, gamescope + TWO Xwayland servers (`:2`, `:3`) + steam + steamwebhelper all run.
- steamwebhelper **does** create the gamepad-UI panorama windows once: log shows `SP BPM_uid0: Created window: size: 1280,800`, `SP BPM_uid0-'Steam Big ': WasHidden 0`, SharedJSContext created MainMenu/QuickAccess popups, SteamApp Init completed (websocket open, stores loaded, OOBE store).

## The blocker (what I need help figuring out)

Root/lp session `2026-10-23-68-steam` (guest, started 23:39:47):

1. **Gamepad-UI renderer keeps dying and Steam relaunches it forever.** SteamApp Init completes at 23:40:06–16; at 23:40:53 "Applying safe browser shutdown workaround" on all SPs → new renderer at 23:40:53/23:41:22/23:41:39/23:41:45, each dying in ~15–45 s. Client-side waiters report `Runtime.addBinding: Target crashed`, `Runtime.evaluate: Target crashed`, `no SharedJSContext yet`. Second-init webhelper took WebUITransport 11,642 ms — box thrashing.
2. **No crash dumps.** breakpad says `10/23 23:39:52 /tmp/dumps: insufficient permissions - delete and recreate` twice — despite `BL_TEGRA` path doing `rm -rf /tmp/dumps; mkdir -m 777 /tmp/dumps` just before steam start (session.log line 250 shows flags; line 261 the perms error). Suspect wrong order (the steamsysinfo query runs before steam?) or the mkdir targets the real /tmp vs the chroot rootfs /tmp. So we have zero signal on why renderers die.
3. **No windows reach gamescope's compositor.** The app-side compositor reports `0 on screen`, `0 windows open` the entire session; gamescope only "Updating mode for xwayland server" logs. So even the frames webhelper paints don't surface. Suspect the webhelper browser window targets the wrong X server (two Xwaylands: `:2` and `:3`), or the CEF output window isn't visible/never paints so gamescope's Xwayland peer never sees a mapped window.
4. webhelper also logs `XOpenIM() failed, LANG = C.UTF-8` and `Error: XRRGetOutputInfo() is not available` (with a DroidDeck's own workaround shim printing "Workaround: initialized with override: 1 real: (nil)"). `XRRGetOutputInfo()` may be a red herring from the login-screen's xrandr probe; it appears in the desktop-mode session (#67) where the login window DID render fine.

## Some flags / environment of the running steam baseline

```
-gamepadui -steamdeck -steamos3 -cef-enable-debugging -devtools-port 43270
-language english -clientbeta droiddeck-pinned-v8.0 -no-cef-sandbox
-cef-ozone-platform=x11 -cef-disable-gpu
```
webhelper browser cmdline additionally: `--no-sandbox --password-store=basic --no-zygote --in-process-gpu --disable-gpu --disable-gpu-compositing --ignore-gpu-blocklist` (CEF children use `--use-angle=swiftshader-webgl --use-gl=angle`).

Device specifics: Tegra X1, aarch64 (Cortex-A57), 3 GB RAM, Android 9?, unit is SHA (production) so no adb root (run-as DOES work — perf-debuggable? build is a local release `0.4.0 local dirty · shield-tv at aa13b2a`). Memory: MemTotal 3.0 GB, MemFree 53 MB, MemAvailable 477 MB during the crash loop — tight but not OOM-killed.

## Questions to research (in priority order)

1. Known causes of Steam-client CEF **renderer** child crash loops on aarch64 / armv8 software rendering of the SteamOS gamepad UI ("SharedJSContext", `Target crashed` from CDP). Anything like a V8/JIT page-permission issue under Android/SELinux? (Same CPU family SIGILLed on LSE builds before — see the `clientbeta droiddeck-pinned-v8.0` pin comment in droiddeck-session ~line 680.)
2. Why the `-cef-disable-gpu` path still needs CEF to raster: is SwiftShader raster in webhelper known to SIGSEGV on Tegra X1 without `--use-gl=angle --use-angle=swiftshader-webgl` tweaks (`vulkan` vs `swiftshader-webgl`)? Which combination works reliably software-only on aarch64 Linux in CEF?
3. chromium `--no-zygote` / `--in-process-gpu` interactions with Steam's webhelper on Linux — any known renderer-exit patterns?
4. Why breakpad would still say `/tmp/dumps: insufficient permissions` after a fresh `mkdir -m 777` — is there a second copy of the check inside steamwebhelper itself / a different mount namespace? How to get a real CEF crash dump when `--disable-crash-reporter` is set (breakpad's minidump vs crashpad) and `crashpad-handler-pid=0`?
5. On dual-Xwayland gamescope (xwayland server :2 and :3), which one is the "app/keyboard/pointer" server and which is the "game" server, and what makes steam/steamwebhelper pick? Any known gamescope switch/env to force steam client windows onto a specific Xwayland? (`droiddeck-session` runs `-cef-ozone-platform=x11`.)
6. Alternative cheaper Phase-1 marker if panorama stays broken: Steam desktop mode (UI-from-steamwebhelper but desktop style) DOES paint login window successfully in session #67 — worth pivoting Phase 1 acceptance to desktop-mode Steam + GPU/FPS measurement, and treating gamepadui as Phase 2?
7. RRO/RandR: on a gamescope Xwayland, `XRRGetOutputInfo()` not available — is there a cheap way to give Xwayland a synthetic RandR output (gamescope has `--rt`/res? there's a upstream fix; check whether DroidDeck's shim in `tools/linuxfs` or gamescope patches already implements `XRRGetOutputInfo` and just fails on `real: (nil)`).

## Evidence excerpts

Steam args (session 68):
```
== steam args: -gamepadui -steamdeck -steamos3 -cef-enable-debugging -devtools-port 43270 -language english -clientbeta droiddeck-pinned-v8.0 -no-cef-sandbox -cef-ozone-platform=x11 -cef-disable-gpu
```

Renderer churn (webhelper.txt, guest log dir `logs/webhelper.txt`):
```
[2026-10-23 23:39:57] Browser - launching child process with: .../steamwebhelper --type=renderer ...
[2026-10-23 23:40:53] SP BPM_uid0-'Steam Big ': Applying safe browser shutdown workaround
[2026-10-23 23:40:53] MainMenu_uid2-...: Applying safe browser shutdown workaround
[2026-10-23 23:40:53] QuickAccess_uid2-...: Applying safe browser shutdown workaround
[2026-10-23 23:40:53] Browser - launching child process with: .../steamwebhelper --type=renderer ... --force-device-scale-factor=1.33 ...
[2026-10-23 23:41:22] ... --type=renderer ...
[2026-10-23 23:41:39] ... --type=renderer ...
[2026-10-23 23:41:45] ... --type=renderer ...
```

cef_log.txt sad tabs:
```
[18560:18560:1023/234054.173206:WARNING:sad_tab.cc(255)] Tab Killed: https://steamloopback.host/
[18560:18582:1023/234147.332814:ERROR:network_service_instance_impl.cc(600)] Network service crashed, restarting service.
```

breakpad/dumps:
```
10/23 23:39:52 /tmp/dumps: insufficient permissions - delete and recreate
10/23 23:39:52 minidumps folder is set to /tmp/dumps
```

gamescope vulkan (healthy, for contrast):
```
[gamescope] [Info] vulkan: 0119-diag: post-createDevice bare alloc size=3686400 -> 0
[gamescope] [Info] vulkan: 0119-diag: upload buffer alloc size=8294400 -> 0
[gamescope] [Info] vulkan: 0119-diag: pre-texture bare 2304 -> 0 (about to alloc size=3563520)
```

panorama started fine once (webhelper_js.txt, first pass):
```
[2026-10-23 23:40:00] SteamUI: INFO: Storing new config params ?...&IN_GAMESCOPE=true
[2026-10-23 23:40:05] SteamUI: INFO: Connection status: connected
[2026-10-23 23:40:06] SteamUI: INFO: async LoginStore - 430.4ms
[2026-10-23 23:40:16] SteamUI: INFO: OOBE Store: keyboards 0
```

compositor side (wayland.log, every 10 s, same numbers throughout):
```
stats  last 10 s: 0 frames on screen (0.0 fps) | 0 GPU frames from games | 4 window redraws | 0 windows open
```

## How to access devices/logs (repro commands, no pushing changes)

```sh
adb connect 192.168.0.194:5555
cd ShieldDeck
python3 tools/droiddeckctl --serial 192.168.0.194:5555 state
# pull a session dir (adb pull can't read app-private; use run-as tar):
adb -s 192.168.0.194:5555 shell "run-as com.droiddeck.launcher tar -c -C files/logs 2026-10-23-68-steam" | tar -x -C /tmp/opencode
# steam logs inside guest rootfs:
adb -s 192.168.0.194:5555 shell "run-as com.droiddeck.launcher tar -c -C files/linuxfs/root/.local/share/Steam/logs webhelper.txt webhelper_js.txt webhelper_gpu.txt cef_log.txt console_log.txt" | tar -x -C /tmp/opencode
```
`droiddeckctl logs latest` fails with `Permission denied` pointing at the private logDir path — that's a broken ctl route on this device, use the run-as tar above.
(README note: `tools/droiddeckctl` is a Python CLI; `docs/agent-control.md` explains commands; debugging tools (`guest`/`run`/`cdp`) are gated by an on-device toggle which is OFF — do not ask me to re-enable it for research.)

## Working session inventory to help you (snapshot of device state)

- App: `com.droiddeck.launcher` 0.4.0 installed; session `2026-10-23-68-steam`; guest pids (ps) showed `gamescope 18355`, `Xwayland 18371 (X server :2?)`, `Xwayland 18372`, `gamescopereaper 18377`, `steam 18516`, `steamwebhelper 18560` plus renderer/utility children under 19034/19035/19036.
- Steam pinned client build date "Apr 15 2026 19:50:09", steamRT aarch64 (steamrtarm64).
- `-cef-force-gpu -cef-use-gl=angle -cef-use-angle=vulkan` is the desktop-perf path (BL_TEGRA=0 path; currently NOT used on Shield).

## What I've tried / ruled out already

- Vulkan allocator and textures on lvp: fine; no findMemoryType -1 anymore (0116/0122-fixed via `LinuxRuntime.vulkanIcd` selection + patches).
- `STEAM_GAMESCOPE_NIS_SUPPORTED=1`, `STEAM_GAMESCOPE_FANCY_SCALING_SUPPORT=1`, `-steamos-white=`, nullptr etc. not relevant — flags unchanged from sessions where desktop UI worked (so no flag drift on the desktop path).
- OOM/low-memory killing: MemAvailable 477 MB, no kernel OOM in dmesg, and the child deaths happen at 15–45 s cadence and memory stays steady — not an LMK kill.
- `steam-runtime-launcher-service` was disabled early ("exec: not found") and is on all sessions — not new.
- Desktop login window (session #67, mode `BL_STEAM_UI=desktop`) renders and stays up fully — so it's gamepadui-specific, not a general CEF-dies-at-startup problem.

## Repro loop for a bisect

```sh
cd ShieldDeck
python3 tools/droiddeckctl --serial 192.168.0.194:5555 stop   # if needed
python3 tools/droiddeckctl --serial 192.168.0.194:5555 start steam --wait --timeout 180
# then either:
#   repeat poll state, or restart and pull logs, or use run-as tar trick above
```

Sandboxing note: nothing is pushed after the `2026-10-22 19:46` build (`app/src/main/assets/linuxfs/usr/local/bin/gamescope` sha256 `2ef3f54492a64a7edb7436f99b937e6d525c1849fac5b4bdeb6f41560180f4a9`). If a restored gamescope patch-step is needed, `droiddeckctl override` is disabled (debug toggle OFF) so a rebuild + `adb install -r` APK (`./gradlew :app:assembleRelease`) is the install path (SessionFiles restages via APK assets each session start).
