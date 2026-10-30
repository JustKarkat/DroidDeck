# ShieldDeck handoff — 2026-10-30: GPU path solved, gamescope dropped, next goal is the Steam UI

Supersedes `handoff-shield-tv-2026-10-28.md` for status; read that file's §17–31
for the measurements behind everything below.

## TL;DR — what changed today

1. **Steam's CEF UI renders on the Tegra GPU.** GPU children no longer
   crash-loop (`exit_code=11` gone): `ddcefdiag` refuses `libvulkan*` dlopens
   in GPU children only, and the ANGLE build forces `DisplayEGL` for the
   GL/GLES display types. Verified: `gralloc.tegra`/`libnvrm_gpu` mapped, no
   gallium/LLVM/swiftshader, `EGL_VENDOR = "Google Inc. (NVIDIA Corporation)"`.
2. **The stutter was never the renderer.** It was device memory exhaustion:
   gamescope's software-Vulkan compositor holds a ~830–917 MB working set on a
   3 GB device, which produced ~57 major faults/s, 296 % kernel CPU and frames
   arriving in bursts. Everything externally available was ruled out (mmap and
   malloc interposition, glibc arenas, thread stacks, resolution,
   `--expose-wayland`, `LP_NUM_THREADS`, shader cache, Xwayland count,
   mangoapp, alternate ICDs, and in-gamescope `>8 MiB` allocation logging).
3. **gamescope is dropped from the UI path** (`BL_NO_GAMESCOPE=1`): Xwayland is
   hosted directly on the app compositor. Thrash gone (0 major faults/s,
   150 % CPU idle at 960x540). **No `xwayland-shell-v1` was needed** — Xwayland
   fullscreen mode maps its root window through the existing `xdg-shell` path.
4. **Scale bug found and fixed**: with no window manager Steam opens a
   1280x800 window while the session output was 960x540, cropping the right
   320 px (the language list). Session resolution is now 1280x800.

## Current device state (2026-10-30 ~15:00)

- Session running, `phase=READY`, **no gamescope**, session output 1280x800.
- Screen shows Steam's **"Shutting down Steam…"** spinner. All processes are
  alive (`Xwayland`, `droiddeck-steam`, `steam`, 5 × `steamwebhelper`).
- Persistent env:

  ```
  ENABLE_GAMESCOPE_WSI=0
  BL_WEBHELPER_ARGS=--disable-gpu-sandbox --disable-vulkan --disable-crash-reporter --enable-low-end-device-mode
  BL_NO_GAMESCOPE=1
  ```

- Session pref: `displayResolution.steam=1280x800`.

## NEXT GOAL: get into the Steam UI

The OOBE renders correctly and responds to input, then the client shows
"Shutting down Steam…". Steam's own logs (`console_log.txt`,
`bootstrap_log.txt`) show normal activity up to that point and **no shutdown
request**, which points at the OOBE restarting the client after language
selection — and with no session manager/WM, nothing brings it back the way
gamescope would.

Investigate in this order:

1. **Is it a restart or a hang?** Watch pids while it sits there:
   `ps -A -o pid,etime,comm | grep -aiE 'steam|steamwebhelper'`. A relaunch
   shows a new `steam` pid and a fresh `console_log.txt` timestamp; a hang
   shows the same pids.
2. **Does the wrapper relaunch it?** `droiddeck-steam` is the launcher script;
   check whether it loops on exit and whether its relaunch is failing (log at
   `/tmp` and `session.log`).
3. **Does the OOBE need a window manager?** Steam's Deck UI may wait for a
   focus/activation that a WM normally supplies. Gamescope provided it; the
   app compositor does not advertise a WM at X level. If this is the blocker,
   options in order of cost: pass `-noreactlogin`/`-silent` style client flags;
   run a minimal X WM inside the guest; or send the focus/activation events
   X11 clients expect from `waylandcomp`.
4. **Complete the OOBE**: the language screen responds to `dd input` — select
   a language and press A/SELECT, then watch whether it advances to sign-in.

Quick commands:

```sh
export ADB_SERIAL=192.168.0.194:5555
python3 tools/droiddeckctl state                 # phase, output size, guest pid
python3 tools/droiddeckctl screenshot /tmp/s.png # what the TV shows
python3 tools/droiddeckctl input pad a           # gamepad into the session
python3 tools/droiddeckctl input key enter       # keyboard
python3 tools/droiddeckctl guest -- sh -c '<cmd>' # inside the guest (wedges under load)
```

`dd guest` frequently times out while Steam runs; `run-as com.droiddeck.launcher
cat <path>` over adb is the reliable read path (note the guest `/tmp` quirk:
`ls`/`stat` fail with "Function not implemented" — use `cat`).

## How the no-gamescope path works (and its switches)

`droiddeck-session`, `BL_NO_GAMESCOPE=1` branch:

- `tools/x11/xwayland-host.py <n>` binds `/tmp/.X11-unix/X<n>` and execs
  `Xwayland :<n> -listenfd 3 …`. It must create the socket because Xwayland's
  own lock step needs hard links, **which the guest's /data-backed /tmp
  denies**, and `-nolock` is root-only (guest runs as uid 10018).
- Then `DISPLAY=:<n>` and the normal inner script runs (CEF wiring, Steam).
- Switches: `BL_NO_GAMESCOPE=0` back to gamescope;
  `BL_XWAYLAND_DISPLAY=<n>` (default 9); `BL_XWAYLAND_ROOTLESS=1` for rootless
  mode (untested — would need `xwayland-shell-v1`);
  `BL_XWAYLAND_GEOMETRY` is a no-op in fullscreen mode.
- `tools/x11/xprobe.c` is the checkpoint client: `xprobe :9 20` maps a window
  and logs input events to `/tmp/xprobe.log`; `xprobe :9 1 tree` dumps every
  top-level X window with geometry — this is how the 1280x800 window on a
  960x540 screen was found.

## Known issues / debt carried forward

1. **Letterboxing**: 1280x800 (16:10) on a 16:9 TV gives black side bars.
   Options: compositor stretch, or a 16:9 output with the compositor scaling a
   1280x800 surface down.
2. **Memory headroom trade-off** (handoff §31): 960x540 = plenty of headroom but
   cropped UI; 1280x800 = correct but ~25 major faults/s back. The ideal is a
   1280x800 X screen scaled into a 960x540 output, compositor-side.
3. **No focus/overlay integration**: gamescope's X atoms (focusable apps, touch
   mode, frame limiter) are gone. Steam reads some of them.
4. **`0130-diag` and the `ddcefmem` tracer are still built in** and should be
   removed once this is settled (`tools/gamescope/alloc-log.py` step in
   `build-in-arch.sh`, `PATCHES.md` note, `ddcefmem.so` in the hybris payload).
5. **Stale-asset trap** (cost real time twice): editing `tools/linuxfs/overlay`
   only reaches the device through a rebuild, and `./gradlew` can ship a stale
   copy. Use `rm -rf app/build/runtimeAssets && python3
   tools/release/runtime_inputs.py record && ./gradlew :app:assembleRelease
   --no-configuration-cache`, then verify with
   `unzip -p app/build/outputs/apk/release/app-release.apk assets/linuxfs/usr/local/bin/<file> | grep <marker>`.

## Reference: what is where

| piece | path |
|---|---|
| session modes, no-gamescope branch | `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` |
| Xwayland socket host | `tools/x11/xwayland-host.py` |
| X test client (window/input/tree) | `tools/x11/xprobe.c` |
| ANGLE force-DisplayEGL patch | `tools/angle/patches/force-egl-display.py` (+ `build-in-arch.sh`) |
| window-surface shim | `tools/hybris/eglshim/egl_shim.c` |
| GPU-child diag (Vulkan starvation, dlopen/sigaction/exit tracing) | `tools/hybris/preload/ddcefdiag.c` |
| mmap/malloc tracer | `tools/hybris/preload/ddcefmem.c` |
| gamescope patch stack | `tools/gamescope/patches/`, `PATCHES.md` |
| measurement history | `docs/handoff-shield-tv-2026-10-28.md` §17–31 |
| app compositor | `app/src/main/cpp/waylandcomp/` |

Branch `shield-tv`, remote `fork` (JustKarkat/DroidDeck). Latest commits:
`9b871c85` (no-gamescope + 1280x800 fix), `4dda7ac7` (first no-gamescope run),
`ea2890cb` (validation), `6d6c6ede` (attribution negatives).
