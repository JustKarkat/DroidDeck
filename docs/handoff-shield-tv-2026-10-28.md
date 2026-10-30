# ShieldDeck handoff — CEF-on-Tegra, the chain is proven, the GPU process still exits, 2026-10-28

## ⚡ START HERE — current state after 4 update rounds (read before anything below)

The sections below are in chronological order and **some early conclusions
were later disproven**. Trust §8–12 (latest) over earlier text. Current facts:

- **Commit `7c3c9295`** (pushed). CI is **UNBLOCKED** (owner fixed
  `allowed_actions` on 2026-10-28); `gh workflow run "Build ANGLE (GL backend)"`
  works, ~45 min, artifact `angle-libs` — already downloaded and staged into
  the APK assets (sizes match the locally built ones).
- **SOLVED:** the GPU-process crash loop's *first* cause was the Gamescope WSI
  Vulkan layer → `ENABLE_GAMESCOPE_WSI=0` (persistent `dd env`). Also solved:
  Valve's fork silently maps its backend string to **DisplayType 14 =
  SwiftShader**; `droiddeck-cef-patch` now also rewrites that `mov w10,#14`
  to **#16 = ANGLE_OPENGLES_EGL** (verified fresh/patched/re-run states).
- **STILL BROKEN (sharpened by sessions 62 + the review pass, §8–16):** the
  GPU child crash-loops anyway (6× `exit_code=11`, then survivors on
  `--use-gl=disabled`). Established: DT16 IS applied and stored (bytes
  verified; the string-match gate disassembles as runtime strlen+memcmp —
  no hardcoded length). The child dies in **Mesa/llvmpipe init** after
  silently falling through to DEFAULT — meaning ANGLE's DT16 display
  attempt fails before any shim call. Prime suspect: ANGLE front-end
  attrib validation on Chromium's full DT16 attrib list
  (`{0x3209,0x348e}`/`{0x3209,0x345e}`, workers `0x320e/0x320d/0x3489`).
- **Your first move (details in §14.5/§16):** replay the exact DT16 attrib
  list in the guest ctypes probe and read `eglGetError` — that names the
  rejecting validation. Then per §16: LD_PRELOAD the crash logger, read
  the minidump exception stream, consider Option A (DEFAULT=EGL in our
  ANGLE build). Patcher + shim now self-verify (exit 5 + per-child
  `cef DT insn=` lines in `/tmp/ddcef-egl.log`).
- **Dead ends — do NOT re-derive:** client-extension query is fine (ANGLE
  answers correctly; earlier "ANGLE rejects it" was a probe bug —
  EGL_EXTENSIONS is 0x3055, not 0x3053); `--disable-gpu-sandbox` reaching the
  child is unrelated to the crash; the WSI layer is fixed, not the whole story;
  the shim/ANGLE/hybris stack are individually good.
- Device: session may be RUNNING (stable llvmpipe baseline). Persistent env:
  `BL_WEBHELPER_ARGS=--disable-gpu-sandbox --disable-vulkan
  --disable-crash-reporter`, `ENABLE_GAMESCOPE_WSI=0`. The device libcef
  already carries both patches (string + DT16) — the cef-patch re-applies
  per session and is idempotent. `eglfront.so` wrapper exists but is
  env-gated OFF (`BL_CEF_EGLFRONT=1`) — it destabilizes the GPU process.

## TL;DR (original, morning)

- **ANGLE → shim → Tegra is proven directly.** Driven correctly in a quiet
  guest session, our ANGLE `libEGL.so` loads the shim and reports
  `EGL_VENDOR = "Google Inc. (NVIDIA Corporation)"`, for window and pbuffer
  surfaces (`eglCreateWindowSurface` on a real X11 XID + `eglMakeCurrent` +
  `eglSwapBuffers` all succeed). The shim, the ANGLE build and the hybris stack
  are individually confirmed good.
- **CEF still does not use it.** The GPU process receives `--use-angle=gles-egl`,
  loads the shim, then exits `exit_code=11` six times before calling any `egl*`
  entry point and settles on `--use-gl=disabled`.
- **The tell is one log line.** `[ddcef-egl] shim loaded` in
  `steamwebhelper.log` means the shim was dlopened; without the follow-up
  `[ddcef-egl] real lib loaded` / `eglGetDisplay` lines ANGLE loaded it as a
  dependency but never drove it.
- **Backend choice is order-sensitive.** Same binary, same env, same session:
  a probe that calls `eglGetPlatformDisplayEXT` first gets **NVIDIA**; one that
  initialises `eglGetDisplay(NULL)` (GLX) first gets **Mesa** and never loads
  `libddcef.so`. That instability is the reason to make CEF request the platform
  display + EGL device type explicitly rather than rely on `--use-angle`.

## Where everything lives

| piece | state | where |
|---|---|---|
| Window-surface shim `libddcefgl.so.1.0.0` | **works, device-verified** through ANGLE (window + pbuffer + ctx + MakeCurrent + SwapBuffers) | `tools/hybris/eglshim/egl_shim.c`, built by `build-in-arch.sh` |
| Our ANGLE (GL backend, aarch64) | **staged in the APK**, `libddcef.so` loader string present, DisplayEGL+DisplayGLX compiled in | CI run 40626002076 (`angle-libs`), staged by `tools/angle/stage-libs.sh` |
| `gles-egl` patch in libcef | applied every session (`swiftshader-webgl`=0, `gles-egl`=2 in libcef.so) | `tools/linuxfs/overlay/usr/local/bin/droiddeck-cef-patch` |
| Session-script ARM wiring | fixed this session (see below) | `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` |
| Guest copy lists | `usr/lib/angle/{libEGL,libGLESv2}.so` added to `SessionFiles.kt` | `app/src/main/java/.../session/SessionFiles.kt` |

## Reproduce the proof (5 min, quiet session)

`dd run /bin/sleep -- 400` so the agent is not starved, then in the guest:

```sh
export LD_LIBRARY_PATH=/usr/local/hybris-cef:/usr/local/hybris-egl
export HYBRIS_LD_LIBRARY_PATH=/usr/local/hybris/bionic:/system/lib64:/vendor/lib64:/vendor/lib64/hw:/vendor/lib64/egl:/apex/com.android.runtime/lib64/bionic
export DDEGL_DEBUG=1
```

Then a ctypes probe that goes **straight to `eglGetPlatformDisplayEXT`** — that
is what selects ANGLE's DisplayEGL and therefore the shim:

```
eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE = 0x3202, NULL,
    { EGL_PLATFORM_ANGLE_TYPE_ANGLE          = 0x3203,
      EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE = 0x320E,
      EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE   = 0x3209,
      EGL_PLATFORM_ANGLE_DEVICE_TYPE_EGL_ANGLE = 0x348E,
      EGL_NONE = 0x3038 })
-> eglInitialize -> eglQueryString(dpy, EGL_VENDOR = 0x3053)
```

Expected, and what was measured:

```
[ddcef-egl] shim loaded (pid ...)
[ddcef-egl] real lib loaded
VENDOR = b'Google Inc. (NVIDIA Corporation)'
```

Two traps that cost real time here:
- **Do not touch `eglGetDisplay(NULL)` first.** Initialising the default (GLX)
  display pins the process to Mesa for the rest of its life and `libddcef.so` is
  never loaded — same binary, same env, different answer.
- **Get the attribute ids right.** `0x3207` is D3D9 and `0x348D` is not the
  device type; both return `EGL_BAD_ATTRIBUTE (0x3004)`, which looks exactly
  like "the shim is not called".

## Wiring fixes that landed (all in `droiddeck-session`)

1. **The `BL_TEGRA` branch dropped the ANGLE selectors.** It replaced `_bl_cef`
   with only `-cef-force-gpu -disable-gpu-sandbox`, so the GPU process got no
   `--use-angle` and Chromium defaulted to Vulkan — which SIGSEGVs in
   Gamescope-WSI on lavapipe. Now:
   `_bl_cef=(-cef-force-gpu -disable-gpu-sandbox -cef-use-gl=angle -cef-use-angle=$BL_CEF_ANGLE)`.
2. **`BL_WEBHELPER_ARGS` is the channel that reaches the GPU child.** The
   `-cef-*` client flags only reach the Steam client. The preload
   (`tools/linuxfs/preload/webhelper.c`, wired from `net.c`) rewrites
   steamwebhelper's argv, which is how `--vmodule` already got through. The
   plain `--use-gl=angle --use-angle=…` forms are now appended there too.
   Verified: the GPU process argv contains `--use-angle=gles-egl`.
3. **Do not stage the shim as `libEGL.so.1`.** `libcef.so` dlopens that name to
   load ANGLE's *front* lib; a shim there shadows our ANGLE, so CEF drives the
   raw null-platform hybris EGL, which rejects X11 windows → the SIGSEGV.
   `hybris-cef/` holds only `libddcef.so`.
4. **`tools/angle/stage-libs.sh` (new).** `tools/hybris/stage-payload.sh`
   `rm -rf`s `usr/local/hybris`, which had silently deleted `usr/lib/angle`
   between builds. The new script re-stages it and refuses any artifact lacking
   the `libddcef.so` loader string.

## Open risks (honest, in priority order)

1. **Make CEF take the platform-display path.** `--use-angle=gles-egl` picks the
   ANGLE front backend but CEF still enters through the default display, so the
   shim is dlopened and never driven (and the backend is order-sensitive, see
   above). Options, cheapest first:
   - ~~`BL_CEF_ANGLE=gl-egl`~~ — **tried this session, it does not work.** The
     patcher keys on its own `gles-egl` string and exits 3 on any other value,
     so the session fell back to `-cef-disable-gpu` (verified in session
     2026-10-28-12-steam). To test `gl-egl` you must also make
     `droiddeck-cef-patch` write it;
   - check whether this CEF honours an ANGLE feature override /
     `ANGLE_DEFAULT_PLATFORM` value that can select the EGL device type (the env
     name exists in the binary; `swiftshader` and `metal` are known values —
     enumerate what parses);
   - extend `droiddeck-cef-patch` to rewrite libcef's ANGLE platform-init to
     request `EGL_PLATFORM_ANGLE_DEVICE_TYPE_EGL_ANGLE`, using the same-length
     string-patch technique already proven for `swiftshader-webgl` -> `gles-egl`.
2. **The 6 SIGSEGVs hide the real failure.** `--disable-software-rasterizer`
   (from `BL_CEF_DIAG=1`) turns a soft fallback into a hard exit, which is why
   the crash shows up instead of a silent SwiftShader fallback. When testing a
   fix, consider clearing `BL_CEF_DIAG` so you see whether it *works* rather than
   whether it *fails cleanly*.
3. **`--use-gl=disabled` on the survivor** is the post-crash fallback, not a
   separate bug.
4. Expect first hardware frames to be slow-ish (2 MB/frame readback + blit at
   960x540); that is still GPU rasterisation vs full SwiftShader.
5. The guest agent wedges under Steam load; `dd guest` and direct `adb` reads of
   `files/linuxfs/...` are more reliable than the agent file protocol during
   boot. Prefer a `dd run /bin/sleep` session for probes.

## Device state at handoff

- No session running (`am force-stop`ed). `suspendPolicy.steam` restored to
  `manual`. Debug env cleared (`BL_CEF_DIAG` removed).
- APK installed: the 01:11 build (ANGLE v2 + shim v2 + the three wiring fixes).
- **Disk was full** (510 MB free, install failed). Reclaimed by deleting 66 old
  session log dirs (759 MB → 100 MB) and the cached Steam update zips in
  `root/.local/share/Steam/package/*.zip.vz.*` (1.17 GB → 83 MB). Both are
  regenerable; Steam re-downloads the client packages if it needs them. Watch
  free space before every install.
- Untracked dirs (`.CleanupShield/`, `prepared-components/`,
  `stores-cargo-lock/`, `xbox-arm64/`, `shield-probe.txt`) are pre-existing — do
  not delete.

## Suggested next steps

1. Reproduce the proof above in a quiet session (5 min) so you trust the stack
   before changing anything.
2. Try `BL_CEF_ANGLE=gl-egl` (`dd env set 'BL_CEF_ANGLE=gl-egl'`) — one session,
   no rebuild, and it exercises the other compiled-in front backend.
3. If neither front backend passes the device type, extend `droiddeck-cef-patch`
   to rewrite ANGLE's platform-init path in libcef to request
   `EGL_PLATFORM_ANGLE_DEVICE_TYPE_EGL_ANGLE` (same-length string patch pattern
   already proven with swiftshader-webgl → gles-egl).
4. When the renderer string says NVIDIA Tegra: screenshot the OOBE, take the
   before/after CPU snapshot, clear debug env, restore `suspendPolicy.steam`
   to manual, commit, and update `docs/hybris-gpu-status.md`.

---

# Update 2026-10-28 (later): GPU crash loop SOLVED; the EGL-selection gap is now precisely scoped

## 1. The GPU process crash loop is root-caused and fixed

Every `exit_code=11` GPU-process crash (the "6× before any egl* call" mystery
from the morning) is the **Gamescope WSI Vulkan layer**. The GPU process
initializes Vulkan before GL; gamescope injects its WSI layer via
`ENABLE_GAMESCOPE_WSI=1`, and the layer + lavapipe init SIGSEGVs the process
(`[Gamescope WSI] Application info` lines immediately precede every exit in
steamwebhelper.log).

**Fix (verified, sessions 44/45/50):** `ENABLE_GAMESCOPE_WSI=0` in the session
env → zero WSI lines, GPU processes stable (one first-attempt crashpad
`NOTREACHED` crash, then a survivor — see below). It is currently set
persistent via `dd env`; it should move into the session script's BL_TEGRA
branch (Tegra Vulkan is a renderless stub, so the layer buys nothing here).

The 10-26 handoff's "-disable-gpu-sandbox fixes it" was a coincidental
correlation, not the mechanism.

## 2. Crash forensics tooling (all committed, reusable)

- The shim (`libddcefgl`) now embeds a fault logger (`crashlog_embed.c`):
  arms sigaction interposers in `--type=gpu-process` processes only, dumps
  PC/LR/SP + maps + stack to `/tmp/crashlog.txt`; interposes
  `exit/_exit/_Exit/abort`; runs a 20 ms `/proc/self/maps` poller writing
  `/tmp/ddmaps-<pid>.txt`; interposes kill/tgkill/raise/pthread_kill.
  Trap learned the hard way: in the GPU process the shim is LAST in the link
  map, so `dlsym(RTLD_NEXT, …)` returns NULL — resolve libc symbols via
  `dlopen("libc.so.6", RTLD_NOLOAD)` instead.
- Valve's crashpad writes real minidumps to `/tmp/dumps/completed/*.dmp`.
  They lack a module list, but the exception PC + the maps snapshot resolves
  the crashing module (this is how libcef+0x5381868 was identified).
- The shim logs to `/tmp/ddcef-egl.log` (file, not stderr — the GPU child's
  stderr IS captured in steamwebhelper.log, but only via
  `--enable-logging=stderr`, which does reach the child; the file log is the
  reliable channel).

## 3. The EGL-selection gap: precisely scoped, one experiment short

With the crash loop gone, the GPU process survives but ANGLE lands on
Mesa GLX/llvmpipe instead of the shim/Tegra. Root cause chain (verified
against Chromium 126.0.6478 + ANGLE chromium/6478 sources):

1. Chromium builds the ANGLE display-init list in
   `ui/gl/init/gl_display_initializer.cc::GetEGLInitDisplays`. The
   `gles-egl` request only adds `ANGLE_OPENGLES_EGL` when
   `supports_angle_opengl_egl` is true — which is
   `client_ext.b_EGL_ANGLE_platform_angle_device_type_egl_angle`.
2. If nothing matches, the list is empty and Chromium silently requests the
   `DEFAULT` display type — ANGLE's default on X11 = DisplayGLX → Mesa
   llvmpipe. No error is logged anywhere.
3. `client_ext` comes from `eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS)`.
   **EGL_EXTENSIONS is 0x3055** (0x3053 is EGL_VENDOR — earlier probes that
   "proved ANGLE rejects the query" were querying the wrong name).
4. `tools/angle/eglfront.c` is a wrapper front-lib (built, staged as
   `usr/lib/angle/eglfront.so`) that forwards everything to the real ANGLE
   front and answers the client-extension query itself. With it served
   (intercept on `eglGetProcAddress("eglQueryString")` too — Chromium binds
   through eglGetProcAddress, not dlsym), the GPU child gets PAST display
   selection but GL contexts then fail with
   `SharedContextState::InitializeGL failure max_vertex_attribs : 0` —
   ANGLE's DisplayEGL still never calls the shim (no
   `eglGetPlatformDisplay`/`load_real` lines, no Tegra libs in maps).

**Remaining unknown:** why ANGLE's display creation doesn't reach
DisplayEGL's dlopen of `libddcef.so` even with client_ext served. Next
probe: in a guest python session, call our ANGLE's
`eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, native, attribs)` with
Chromium's exact attrib list (TYPE=OPENGLES, DEVICE_TYPE=EGL, plus the
feature-control/debug-layers attribs Chromium appends) and read the error —
that names the rejecting validation. The wrapper is env-gated off
(`BL_CEF_EGLFRONT=1` to A/B it); the raw ANGLE front is the default.

## 4. Other findings this session

- Hiding **Mesa's** `usr/lib/libEGL.so.1.1.0` (instead of the hybris one)
  kills Xwayland (`libepoxy` can't find libEGL.so.1 → abort → gamescope
  "Xwayland startup failed"). Restored. Don't hide guest-system EGL.
- The GPU child's stderr DOES reach steamwebhelper.log (shim fprintf lines
  appear); Chromium LOG() lines from the child appear too
  (`--enable-logging=stderr` propagates — it was past the 400-char cmdline
  truncation in earlier reads).
- `--disable-gpu-sandbox` and `--use-angle` DO propagate to the GPU child
  via the browser argv (BL_WEBHELPER_ARGS); `--disable-vulkan` does not
  (not in the child's switch list).
- CI is still blocked: repo setting `allowed_actions=local_only` makes every
  GitHub Actions workflow startup_fail (build-hybris/build-angle included).
  The shim/wrapper here were built locally with host clang +
  `/tmp/arm64root` (docker-exported archlinuxarm rootfs) as sysroot.

## 5. Device state at this update

- Session 50 running (stable, llvmpipe). Persistent env: `BL_WEBHELPER_ARGS=--disable-gpu-sandbox --disable-vulkan`, `ENABLE_GAMESCOPE_WSI=0`.
- APK: shim v3 (file logging + embedded crash logger), eglfront.so staged but inactive.
- An orphaned GPU process from session 31 (pid ~27124) may linger until reboot; harmless.
- `angle.hidden/`, `libEGL.so.1.0.0.hidden` leftovers from the bisect are still in
  `usr/local/hybris/usr/lib/` — safe to delete.

## 6. Update 2 (evening, after §3): the client-ext theory is DEAD; the drop is in Valve's fork

Follow-up experiments corrected §3's conclusion:

- **ANGLE 6478 answers `eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS)`
  correctly** (408-char string with `EGL_ANGLE_platform_angle_opengl` and
  `EGL_ANGLE_platform_angle_device_type_egl_angle`). Earlier probes that
  "showed ANGLE rejecting the query" used name 0x3053 (EGL_VENDOR) by
  mistake. The raw-front baseline therefore has a GOOD client_ext in the GPU
  child — the wrapper was never needed for that.
- A pid-tagged wrapper run confirmed **the GPU child queries client
  extensions and receives the correct string** — and still ends up with an
  init-displays list of `[DEFAULT]` (the wrapper logged only
  `eglGetDisplay(X11Display*)` calls — the DEFAULT display type — never an
  ANGLE-platform `eglGetPlatformDisplay`).
- Therefore the gles-egl drop happens in **Valve's fork of the selection
  code**, upstream of display creation. Upstream's `GetEGLInitDisplays`
  (gl_display_initializer.cc) would have added `ANGLE_OPENGLES_EGL`; Valve's
  libcef apparently only maps its own value (`swiftshader-webgl`) — which is
  exactly why their stock build always renders SwiftShader.
- The probe (`/tmp/angleprobe2.py` in the guest, preserved in this handoff)
  proved the whole downstream works: with
  `{TYPE=OPENGLES, DEVICE_TYPE=EGL, DEBUG_LAYERS=0}` via
  `eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, ...)`, ANGLE creates a
  DisplayEGL and calls its dlopen of `libddcef.so` (the shim). In a probe
  process without hybris-cef on LD_LIBRARY_PATH that dlopen fails with
  error 12289 — in the GPU child (which has hybris-cef first) it would
  succeed. **The only missing link is making Valve's selection code request
  that display type.**
- The `eglfront.so` wrapper is **destabilizing** (GPU crash loops or
  max_vertex_attribs=0 contexts in every wrapper session) — it stays
  env-gated off (`BL_CEF_EGLFRONT=1`); the raw ANGLE front is the default.

### Next session's first move

Reverse Valve's selection code in libcef: find the code consuming the
patched string site (`swiftshader-webgl` → `gles-egl`, ~2 occurrences; the
Valve fork's copy of the display-list builder), and determine what
value/display-type Valve's branch maps to an EGL-GLES display. Options:
(a) find a Valve value that maps to `ANGLE_OPENGLES_EGL` and patch to that;
(b) patch the branch's comparison bytes so the gles-egl path is taken;
(c) reimplement the wrapper WITHOUT the stability regression (the raw
forwarders are suspect — try dlopen-by-soname instead of RTLD_GLOBAL).
The REA MCP tooling can decompile libcef around the string xrefs.

## 7. Update 3 (night): Valve's selection REVERSED — the DT16 fix is in; a separate early GPU-child crash is the new frontier

Decompiled Valve's selection chain around the patched string (local
objdump + ADRP/ADD xref scanning; no full-decompiler needed):

- The Valve string site (2nd `gles-egl`, ex-`swiftshader-webgl`, at
  libcef+0x20dec0e) has 4 code xrefs: the ANGLE-implementation parser
  (→ value 2), a feature/flag recorder, and **the display-list builder**
  (libcef+0x7872ba0): on match it stores `mov w10, #0xe` = **DisplayType 14**.
- The DisplayType switch (`GetDisplayFromType`, dispatch at libcef+0x77dca98,
  byte table at 0x20dead8, base 0x77dcab8): **DT14 → the SwiftShader-device
  path** (attribs DEVICE_TYPE=0x3209 + 0x3487). **DT16 → the real
  ANGLE_OPENGLES_EGL case** (0x77dcc28: attribs 0x3209 + 0x348e, worker
  type 0x320e). So patching the string alone still lands on SwiftShader —
  the value had to change too.
- **Fix applied and verified:** 4 bytes at libcef vaddr 0x7872bd0
  (`mov w10, #14` → `mov w10, #16`; 12-byte idempotent pattern), now baked
  into `droiddeck-cef-patch` (handles fresh/already-patched/re-run states).

**Result so far:** with DT16 the GPU child still crash-loops (6×
exit_code=11 at startup, same pre-EGL signature: main thread in a raw
rt_sigtimedwait in libcef+0x5381868, or libc+0x91c6c with x0=0x28; fault
address always <mapping>+0x1b70). Key negatives:
- The crash is NOT the WSI layer (still disabled), NOT crashpad
  (`--disable-crash-reporter` doesn't change it), NOT the sandbox flag.
- The children DID load the shim (SDL3) and ARMED the fault logger, but the
  shim's `load_real` never ran — **no shim EGL function is ever called**, so
  the crash is between display selection and ANGLE's first shim call.
- The first child of the burst dies before the shim even arms (no maps
  snapshot) — a very early death that then poisons the relaunch loop.

### Next session's first moves

1. Instrument the shim's `eglGetDisplay`/`eglGetPlatformDisplay` to log
   BEFORE `load_real` (current code logs after `pthread_once` — a crash
   inside `load_real`'s `dlopen(hybris EGL, RTLD_GLOBAL)` would be
   invisible). That splits "ANGLE never calls the shim" from "the shim's
   bionic-stack load crashes the child".
2. If the crash is inside `load_real`: try `RTLD_LOCAL`, or preload the
   hybris/bionic stack pieces before Steam, or compare the GPU child's
   loaded-library set at crash time vs the working probe process.
3. The `dt14→dt16` patch is independent of the crash — keep it.
4. CI works again (owner changed `allowed_actions`); the ANGLE workflow runs
   (40779394546). Rebuild-hybris can rebuild the shim properly once needed.

---

# Update 4 (night, session 62): shim v4 instrumentation ran — the child dies on the DEFAULT/Mesa-GLX path with the shim mapped but never called

## 8. What shipped this session

- **Shim v4** (`tools/hybris/eglshim/egl_shim.c`), built locally and staged
  into the APK + installed on the device:
  - `eglGetDisplay`/`eglGetPlatformDisplay` now log their **entry** BEFORE
    `pthread_once(load_real)`.
  - `eglGetProcAddress` logs its first call.
  - `load_real` logs `RTLD_NOW|GLOBAL` vs `RTLD_NOW|LOCAL` and the
    `dlerror()` text on failure.
  - **`DDEGL_DLOPEN_LOCAL=1`** switches the hybris dlopen to `RTLD_LOCAL`
    (the §7 move-2 A/B, no rebuild needed — set it via `dd env`).
- Local cross-compile recipe that works (host clang 23, no cross-gcc
  needed): `clang --target=aarch64-linux-gnu --sysroot=/tmp/arm64root
  -fuse-ld=lld -O2 -shared -fPIC -o libddcefgl.so.1.0.0 egl_shim.c
  /tmp/arm64root/usr/lib/libX11.so.6` — the sysroot has no `libX11.so` dev
  symlink, so link the `.so.6` directly. Export surface identical to v3
  (52 symbols). APK build requires `python3 tools/release/runtime_inputs.py
  record` after touching any staged asset (the `verifyRuntimeInputs` guard).

## 9. Session 62 evidence (APK with shim v4, fresh steam session)

The v4 markers answer the §7 move-1 question decisively:

1. **No shim EGL entry point was EVER called in any GPU child.** Zero
   `eglGetDisplay(...) entry` / `eglGetPlatformDisplay(...) entry` /
   `eglGetProcAddress first call` / `load_real: dlopen ... starting` lines
   in the whole session, across **88 GPU children** that all armed the fault
   logger with `--use-angle=gles-egl`. "ANGLE never calls the shim" is now
   proven, not just suspected. The crash is upstream of any EGL call.
2. **The 6× `exit_code=11` burst reproduced** (18:49:03–18:49:15), a
   survivor settled on `--use-gl=disabled` (argv keeps
   `--use-angle=gles-egl` too), and GPU children kept spawning/dying all
   session long (88 armed) — the loop is not just the initial 6.
3. **`/tmp/crashlog.txt` does not exist** despite armed loggers + interposed
   `exit/_exit/_Exit/abort/kill/tgkill/raise/pthread_kill`. So the death
   bypasses every libc path we cover: either a **raw syscall**
   (`exit_group(11)`/`tgkill` via `syscall()`, not libc), or a **synchronous
   SIGSEGV delivered to a thread that has SIGSEGV blocked** (the kernel then
   forces the default action — no handler runs). The 20 ms maps poller DID
   run in the dead children (`/tmp/ddmaps-<pid>.txt` exists), so the shim's
   threads were alive until the end.
4. **Dead child 15887's maps snapshot (`/tmp/ddmaps-15887.txt`, committed
   under `docs/evidence/`) is the smoking gun**: at ~20 ms before death the
   child had mapped **the full Mesa/GLX stack** — `libgallium-26.2.4`,
   **two LLVMs** (`libLLVM.so.22.1` AND `libLLVM.so.23.1`),
   `libGLX_mesa.so`, glvnd `libGLdispatch/libGLX/libGL.so.1.7.0/libEGL.so`,
   **`libvulkan_lvp` (lavapipe)** — i.e. it went down the **DEFAULT display
   type → DisplayGLX → Mesa** path. At the same time:
   - our shim (`libddcefgl.so.1.0.0`) is mapped (SDL3 eager DT_NEEDED),
   - our ANGLE front (`usr/local/hybris/usr/lib/angle/libEGL.so` +
     `libGLESv2.so`) IS mapped,
   - Valve's runtime `steamrtarm64/libEGL.so` + `libGLESv2.so` are ALSO
     mapped (both ANGLEs resident; ours shadows theirs via
     `LD_LIBRARY_PATH=/usr/local/hybris-egl` …),
   - **`hybris-cef` mappings = 0** → ANGLE's DisplayEGL never dlopened
     `libddcef.so` (the shim's loader name) — consistent with (1),
   - swiftshader libs = 0 (it is NOT landing on SwiftShader; it crashes in
     Mesa/GLX/llvmpipe init instead).

## 10. Interpretation + the one open question

The child behaves exactly like a process whose display-type selection
returned **DEFAULT**: Valve's ANGLE front initializes, gets an empty/init
list that matches nothing, silently falls to DEFAULT (DisplayGLX → Mesa),
and dies inside Mesa/llvmpipe init — before ANGLE's DisplayEGL (and thus
the shim) is ever touched. That is the §6 "init-displays list is
`[DEFAULT]`" behavior, now seen from inside the dying child.

**Which means the DT16 patch's effect is the open question**: either (a)
the patch did not actually apply in session 62 (the idempotent patcher is
supposed to run every session — the verification run inside the busy guest
TIMED OUT, so it is unverified), or (b) the patch applied but Valve's
selection still lands on DEFAULT (e.g. the patched `mov w10,#16` feeds a
code path that is gated by something else upstream — the
`supports_angle_opengl_egl` client-ext gate, or Valve's fork ignoring
value 2's mapping). Distinguishing (a) from (b) is next session's FIRST
move: run `droiddeck-cef-patch` in a quiet session (`dd run /bin/sleep -- 400`
first — the agent wedges under Steam load) and read the bytes at libcef
vaddr 0x7872bd0 directly, then re-run one session and compare.

Note (1)+(4) also kill a lingering theory: the crash is NOT inside
`load_real`'s hybris dlopen (RTLD_LOCAL A/B unneeded for now — keep the
switch for later), and NOT crashpad/sandbox/WSI.

## 11. Tooling quirks learned this session

- Guest `/tmp` `ls`/`stat` fail with `Function not implemented` (proot
  quirk) while `cat` works — never trust "No such file" from `ls`; use
  `cat <exact path>`.
- `dd guest` and the agent content-provider wedge under Steam load
  (state polls time out at 30 s); `run-as com.droiddeck.launcher`
  reads `files/logs/<session>/` directly over adb and always works.
  `steamwebhelper.log` is inside the guest at
  `/root/.local/share/Steam/logs/steamwebhelper.log` (reaches it via
  `dd guest cat`, also wedgy under load).
- Shim v4's `shim_init` cmdline logging has a buffer bug: log lines can end
  in garbage after `LD_LIBRARY_PATH=...` (the 512-byte cmdline read is not
  reliably terminated). Cosmetic, but fix next shim build.
- The GPU child's argv now carries BOTH `--use-angle=gles-egl` (our
  BL_WEBHELPER_ARGS wiring) and — on the survivor — `--use-gl=disabled`
  (the post-crash fallback). Utility children get `--use-gl=angle
  --use-angle=gles-egl`.
- `ddmaps-1765.txt` (survivor) references a DIFFERENT APK install dir than
  the dead children's (`~~9Bfr55…` vs `~~FEnaf…`) — either a stale file from
  a pre-reinstall session or the launcher still binding the old APK dir;
  unresolved, noted for later.

## 12. Next moves (in order)

1. Verify the DT16 patch bytes in the guest (quiet session first).
2. If bytes are patched: the drop is in Valve's fork upstream of the switch
   — decompile the code path BETWEEN the string match and the
   `mov w10,#16` (libcef+0x7872ba0 region) for an upstream gate
   (client-ext flag plumbing is prime suspect; §7 already proved the client
   string itself is served correctly).
3. If bytes are NOT patched: fix the patcher's session ordering/idempotency
   and re-test before touching anything else.
4. Keep `DDEGL_DLOPEN_LOCAL` in the back pocket; not the current blocker.
5. Consider catching the death path: add a `SIGSEGV`-blocked-state audit or
   a raw-syscall exit hook (ptrace/`prctl` SECCOMP on exit_group) if the
   no-crashlog mystery needs closing once the display-type fix lands.

---

# Update 5 (night): browser-Claude review processed — DT16 patch VERIFIED APPLIED, H2 ruled out, H1 stands; patcher + shim hardened

The external review (repo access, no device; delivered as
`~/Downloads/handoff-shielddeck-gpu-review-2026-10-28.md`) made four
headline corrections and proposed a work order. Verified on device and
implemented the same night:

## 13. Review corrections — all four accepted

1. **`exit_code=11` is the raw waitpid status → SIGSEGV, not `exit(11)`.**
   (`exit(11)` would report 2816.) The §10 "raw exit_group(11)" theory is
   withdrawn.
2. **The shim's libc interposers can never fire in the GPU child** — libc
   maps before the shim, so every libc-resolved call (`exit`, `sigaction`,
   …) binds to libc, and Valve's `crashhandler.so` then replaces our
   constructor-installed handler. "No crashlog.txt" says nothing about the
   death. Review's fixes (LD_PRELOAD the logger; re-check sigaction in the
   poller) are the right closure path — not yet implemented.
3. **Both ANGLE pairs in the dead child's maps are OUR build** —
   `droiddeck-session` `cp -L`s our `libEGL.so`/`libGLESv2.so` over the
   `steamrtarm64/` ones. There is no "Valve's ANGLE + ours"; §9 bullet 4's
   reading is corrected.
4. **`hybris-cef` mappings = 0 is NOT evidence**: `libddcef.so` is a
   symlink to the same inode as the already-mapped shim, so ANGLE's
   `dlopen("libddcef.so")` returns the existing handle with no new
   mapping. Corrected.

Also confirmed from the dead child's `/proc/self/status`
(`ddstat-15887.txt`): SIGSEGV was **not blocked** (SigBlk = SIGUSR1+SIGCHLD
only) and was in SigCgt — the blocked-signal theory is dead too; the
handler was overwritten (review §0.2 explains how).

## 14. The decisive checks, executed on device (19:30–19:45)

1. **DT16 patch IS applied in the session-62 libcef**: bytes at file offset
   `0x7862bd0` are `0a 02 80 52` (`mov w10,#16`) followed by the intact
   `stur` — file mtime 18:47, just before session 62. "Patch didn't apply"
   is DEAD.
2. **Session-62's real GPU children are pids 1647/1679/1726/1735/1748/1756
   (dead) + 1765 (survivor)** — the earlier §8/§9 attribution to pids
   15762+/15887 was wrong: guest `/tmp/ddcef-egl.log` is APPEND-ONLY across
   sessions and mixes several boots (browsers at pid 1616, 15762, 16046).
   All six dead children's maps contain the Mesa/GLX/LLVM stack, WITH DT16
   applied.
3. **H2 (length-gated match broken) is RULED OUT by disassembly** of
   libcef 0x7872b60–0x7872bd8: the match gate is a runtime
   `strlen`-compare + `memcmp` against the patched literal (no hardcoded
   `cmp #17`), and on match it executes `mov w10,#16`. The string match
   succeeds and DT16 IS stored.
4. Therefore **H1 stands as the blocker**: DT16 is selected, ANGLE's
   DisplayEGL attempt fails *silently* (front-end attrib validation or
   display init — both happen before any shim call), and Chromium's
   init-display loop falls through to DEFAULT → DisplayGLX → Mesa → dies
   in llvmpipe init. The official build compiles VLOG out, so the absence
   of `gl_*`/`gpu_init` child lines in steamwebhelper.log is expected, not
   a signal.
5. **DT16 case attribs enumerated** (disassembly at 0x77dcc28): repeated
   `{DEVICE_TYPE=0x3209, X}` pairs with X ∈ {0x348e, 0x345e} and worker
   types {0x320e, 0x320d, 0x3489} — several display attempts in sequence.
   **Next experiment: replay this exact list in the guest ctypes probe and
   read `eglGetError` after `eglGetPlatformDisplay` + `eglInitialize`** to
   name the rejecting validation.

## 15. Hardening implemented (from the review)

- `droiddeck-cef-patch`: DT site now found by **pattern search** (no
  hardcoded offset); exactly one site must exist or it **exits 5** (the
  session then falls back to `-cef-disable-gpu` instead of claiming
  hardware on a string-only patch); **post-write verification** re-reads
  the file and prints sha256 + the 12 DT bytes; dead no-op removed.
- Shim v5 (`crashlog_embed.c`): the GPU-child constructor now
  `dl_iterate_phdr`s for libcef and logs the **live instruction word** at
  vaddr 0x7872bd0 (`#16 PATCHED ok` / `#14 UNPATCHED` / `SITE MOVED?`) —
  runtime ground truth per child, also catches a child mapping a different
  libcef than the one patched.
- `droiddeck-session`: Valve's original `libEGL.so` is now backed up as
  `.droiddeck-orig` like `libGLESv2.so` was; the stale "ANGLE 6478 rejects
  that query" comments are corrected (handoff §6 disproved it).
- Shim v5 staged into the APK, rebuilt, installed.

## 16. Review items still open (next session)

1. Replay the DT16 attrib list in the guest probe (§14.5) — THE next move.
2. Move the crash logger into `LD_PRELOAD` (first in the chain) so the
   interposers are real; re-check `sigaction(SIGSEGV)` in the 20 ms poller
   and reinstall if displaced.
3. Read crashpad minidumps' **exception stream** (not the waiting main
   thread) and resolve it against `ddmaps` to name the crashing module.
4. `LD_DEBUG=libs` on one GPU child to learn who loads the second ANGLE
   pair (`usr/local/hybris/usr/lib/angle/`).
5. Review Option A (make DEFAULT = EGL in our own ANGLE build,
   `angle_use_x11=false` or patch the default-platform path) — start the
   ANGLE CI build if §16.1 shows the attrib validation is hostile and not
   cheaply patchable in libcef.
6. `readelf -d` on `libvulkan_lvp`/`libgallium` for the **two-LLVM
   mismatch** (lvp links 22.1, gallium 23.1 — a stale-rootfs crash source
   in its own right).

---

# Update 6 (night, sessions 63–69): the child's SIGSEGV is MASKED by Mesa's LLVM; the DT16 chain works end-to-end outside the child; Vulkan/lvp init is the new prime suspect

## 17. What was found

1. **The whole EGL chain works with the GPU child's exact environment.**
   Guest probe `/tmp/dt16probe.py` (committed as
   `tools/hybris/dt16probe.py`): with the child's
   `LD_LIBRARY_PATH=/usr/local/hybris-cef:/usr/local/hybris-egl` +
   `HYBRIS_LD_LIBRARY_PATH`, `eglGetPlatformDisplay(PAA, X11dpy,
   {TYPE=0x320E OPENGLES, DEVICE_TYPE=0x3209, 0x348E EGL_ANGLE})` →
   `eglInitialize` → **`EGL_VENDOR = "Google Inc. (NVIDIA Corporation)"`**.
   Also passes with `DEBUG_LAYERS=0` appended, with DEVICE_TYPE 0x345E
   (ANGLE's SW variant, still routes to Tegra here), and with NULL native.
   Only `{TYPE=0x3489}` is rejected (EGL_BAD_ATTRIBUTE, 0x3004) — that is
   the one DT16-case variant that never works. Without the child's env the
   SAME probe fails at exactly `Could not dlopen native EGL: libddcef.so`
   (ANGLE error 12289) — the seam is real and env-dependent, and inside
   the child nothing even attempts that dlopen.

2. **ddcefdiag.so (LD_PRELOAD, first in the link map) made interposition
   real.** The shim is LAST in the GPU child's link map (SDL3's
   DT_NEEDED), so its libc interposers can never fire — the review's
   correction. `tools/hybris/preload/ddcefdiag.c` is preloaded via
   `/etc/ld.so.preload` (appended by `droiddeck-session` in the BL_TEGRA
   branch, staged from the hybris payload): GPU-children-only it traces
   every `dlopen` (name/flags/result/dlerror), interposes
   `sigprocmask`/`pthread_sigmask`, rides in `crashlog_embed.c`'s fault
   logger, and checks the live libcef DT16 instruction word. Non-GPU
   processes are silent.

3. **THE "no crashlog" MYSTERY IS SOLVED: the child blocks SIGSEGV.**
   The tracer caught `pthread_sigmask(SIG_BLOCK)` calls that add SIGSEGV
   to the blocked set in every GPU child — resolved against
   `ddmaps-31738` to **`libLLVM.so.23.1` + 0xb7dffc (Mesa's LLVM, JIT
   init)**. The per-thread `ddstat` scan (upgraded: now dumps every
   thread's `Name:` + `SigBlk:`) shows the child's main threads AND Mesa's
   `llvmpipe-0/1` threads all carry SIGSEGV blocked
   (`SigBlk=fffffffe3ffbfaff` — recheck of session-62's 0x20400 also shows
   bit 10 = SIGSEGV; §13's "SIGSEGV not blocked" was a decoding error).
   A synchronous fault on a blocked thread forces the default action — no
   handler, no crashlog, `exit_code=11`. This explains every observation.

4. **The child enters Vulkan/llvmpipe BEFORE any GL work.** The dlopen
   trace per GPU child: `steamrtarm64/libGLESv2.so`+`libEGL.so` (our
   ANGLE front, absolute-path load) → `libvulkan.so` →
   **`/usr/lib/libvulkan_lvp.so` ×4** (ICD scan) → VR layer → X11/GL/GLX/
   Mesa. No `libddcef.so` attempt, no failed dlopen: the child dies in the
   Vulkan/GPU-info phase with SIGSEGV masked — long before ANGLE's
   DT16 display could ever be attempted. With lvp (LLVM 22.1) AND gallium
   (LLVM 23.1) mapped into one process (the review's two-LLVM mismatch),
   a fault there is expected.

## 18. Attempted fixes + current state

- `VK_ICD_FILENAMES=/VK_DRIVER_FILES=/dev/null` (persistent `dd env`) —
  **insufficient**: the loader treats /dev/null as an unparsable manifest
  and lvp still loads. Replaced with a **valid-but-empty ICD JSON**
  (`usr/local/hybris/empty-icd.json`:
  `{"file_format_version":"1.0.0","ICD":{"library_path":"","api_version":"1.1.0"}}`)
  — explicit `VK_DRIVER_FILES` is exclusive, so this should yield zero
  Vulkan drivers in every guest process. Verification cycle in flight at
  handoff time.
- If the crash loop stops with no ICDs, the GPU child should proceed into
  GL display init and hit the DT16 path that the probe proves works —
  shim entry lines (`eglGetPlatformDisplay(...) entry` in
  `/tmp/ddcef-egl.log`) are the success signal, then
  `real lib loaded` + `EGL_VENDOR ... NVIDIA`.
- If ANGLE then fails at attrib validation INSIDE the child (entry lines
  but no `real lib loaded`), the difference vs the working probe is
  sandbox/exposure — next lever is the review's Option A (make DEFAULT=EGL
  in our ANGLE build) or `--disable-gpu-process-for-...` style fallbacks.
- Fallback if the empty-ICD env can't beat the loader: rename
  `usr/share/vulkan/icd.d/lvp_icd*.json` in the guest rootfs (reversible;
  watch for gamescope/zink dependence — the llvmpipe GLX path does not
  need lvp).

## 19. Infra notes for the next session

- The overlay (`droiddeck-session`) and any new guest binaries MUST go
  through the APK (`stageSessionScripts` copies the overlay at build time;
  `SessionFiles.kt` hybrisPayload stages guest files). Ad hoc `adb`-pushed
  copies are overwritten at every session start by asset staging.
- Rebuild loop: edit → `install` into
  `app/src/main/assets/linuxfs/usr/local/hybris/usr/lib/` →
  `python3 tools/release/runtime_inputs.py record` → `./gradlew
  :app:assembleRelease` → `adb install -r` → `am force-stop` +
  `dd start steam --wait`.
- Cross-compile: `clang --target=aarch64-linux-gnu
  --sysroot=/tmp/arm64root -fuse-ld=lld -O2 -shared -fPIC -I
  tools/hybris/eglshim tools/hybris/preload/ddcefdiag.c` (diag lib) and
  the §8 recipe for the shim.
- Persistent env at handoff: `ENABLE_GAMESCOPE_WSI=0`,
  `BL_WEBHELPER_ARGS=--disable-gpu-sandbox --disable-vulkan
  --disable-crash-reporter`, `VK_ICD_FILENAMES=`/`VK_DRIVER_FILES=` →
  `/usr/local/hybris/empty-icd.json`.

---

# Update 7 (external review #2): GPU-child Vulkan starvation FIXED the crash loop; force-DisplayEGL ANGLE build is in CI

## 20. Crash loop: fixed

`ddcefdiag` v3 (`tools/hybris/preload/ddcefdiag.c`, commit `af30ba61`)
refuses every `libvulkan*` `dlopen` **in `--type=gpu-process` children
only** (`DDEDIAG_NOVK=0` restores stock behavior). Session 72 verified:
both `libvulkan.so` and `libvulkan.so.1` were blocked, the child survived
the lvp/LLVM phase that used to kill it, produced **zero
`exit_code=11`** lines, proceeded into GL init and exited cleanly. Steam's
bootstrap and gamescope are separate processes and kept their Vulkan — no
wedge (the earlier global ICD starvation, §18, did wedge
`steam.starting`; that env was reverted).

## 21. Remaining gap and the force-DisplayEGL fix (in CI)

With the crash gone the child still takes the DEFAULT display:
`{OPENGL, HARDWARE, X11}` → `CreateGLXDisplay` → Mesa. Only
`DEVICE_TYPE_EGL` reaches `DisplayEGL`, so even a correct
`ANGLE_OPENGLES_EGL` request without that device type goes to GLX — and
the child's DT16 request demonstrably never reaches ANGLE's platform
display path.

**Patch:** `tools/angle/patches/force-egl-display.py`, run from
`tools/angle/build-in-arch.sh` right after the AstcDecompressor fixup.
It edits `src/libANGLE/Display.cpp` (chromium/6478) so both the OPENGL and
OPENGLES cases construct `rx::DisplayEGL` on Linux **before** the
GBM/device-type/GLX branches — Valve's display-type selection stops
mattering, and the DEFAULT display itself routes to our shim → hybris →
Tegra. Verified against the real chromium/6478 source: all four anchors
match with the expected counts (1/2/1/1) and the script is idempotent.
Runtime switches: `DDANGLE_NO_FORCE_EGL=1` (stock selection) and
`DDANGLE_LOG=1` (prints requested display/device/platform type and whether
an impl was built, to the child's stderr → steamwebhelper.log).

Branch `fix/angle-force-egl-display`, commit `32a24440`; CI run
`40982345640` (push on `tools/angle/**`). Next: download `angle-libs`,
`tools/angle/stage-libs.sh`, `runtime_inputs.py record`, rebuild install,
then one session with both `DDANGLE_LOG=1` and `DDEDIAG_NOVK` defaults.

Success signals, in order: `[ddangle] CreateDisplay ... impl!=0` in
steamwebhelper.log → shim `eglGetDisplay/eglGetPlatformDisplay entry` in
`/tmp/ddcef-egl.log` → `real lib loaded` → `EGL_VENDOR = "Google Inc.
(NVIDIA Corporation)"`.

## 22. RESULT — CEF renders on the Tegra GPU (2026-10-29)

CI run `40982345640` green (45m36s), artifact verified to contain the
patch (`DDANGLE_NO_FORCE_EGL` + `[ddangle]` strings in `libGLESv2.so`),
staged via `tools/angle/stage-libs.sh`, APK rebuilt and installed.
Two consecutive sessions (2026-10-29-01 and the follow-up) verified:

- `[ddangle] CreateDisplay type=0x320d device=0x320a platform=0x31d5`
  → `impl=non-null`: the DEFAULT request (OPENGL + HARDWARE + X11) now
  builds **DisplayEGL** instead of GLX.
- The shim's `load_real: dlopen /usr/local/hybris/usr/lib/libEGL.so.1`
  returned a valid handle and `real lib loaded` — the hybris/bionic EGL
  stack loads **inside the sandboxed GPU child**.
- GPU-child maps: `gralloc.tegra`, `libnvrm_gpu`, `libnvos`,
  `libnvrm_*`, `libddcefgl` present; **`libgallium` = 0, `libLLVM` = 0,
  `libGLX_mesa` = 0, `swiftshader` = 0**. The Mesa/llvmpipe path is gone
  from the CEF process entirely.
- `exit_code=11` count for the session: **0** (GPU-child Vulkan
  starvation from §20 holds it).
- The Steam OOBE renders and responds to gamepad input (screenshot:
  `docs/evidence/steam-oobe-tegra-gpu-2026-10-29.png`), text crisp.

**Both halves of the fix are required:** §20 (stop the child dying in
lavapipe) and §21 (make ANGLE pick DisplayEGL). Either alone leaves the
child on Mesa or dead.

### Notes / remaining work

- Presentation does not go through the shim's pbuffer+readback path (no
  `CreateWindowSurface`/`SwapBuffers` lines): ANGLE resolves most entry
  points through `eglGetProcAddress`, which our shim forwards to the real
  hybris lib. CEF presents via its own Ozone path. If we later want swap
  timing/damage control, make `eglGetProcAddress` return OUR wrappers for
  the surface/swap/query entry points too.
- One transient ghost-text artifact was seen on the first OOBE frame,
  gone afterwards — worth re-checking under motion (scrolling) before
  calling presentation perfect.
- `DDANGLE_LOG` and `DDEDIAG_NOVK` are debug switches (unset by default).
- Merge `fix/angle-force-egl-display` → `shield-tv` once accepted.

---

# Update 8 (2026-10-29 evening): the bursty/stuttering frames are NOT the renderer — measured as device memory exhaustion

## 23. What the stutter actually is

The symptom ("nothing, then a burst of frames, sporadically") reproduces with
**and without** the Tegra path, which matches the report that it predates
it. Measured, not guessed:

- **Compositor is fast.** gamescope's own `wayland.log` reports
  `render_scene 1.51/9.37 ms` (mean/max) while ticking at 60 Hz
  (`599 ticks` per 10 s). Composition is not the bottleneck.
- **The Steam window is the limiter**: the same log line pairs
  `36 frames on screen (3.6 fps)` with `36 window redraws` — gamescope
  draws exactly one frame per X11 client repaint. `0 GPU frames from games`
  and `copy NN, zero-copy 0` (every scene is a copy).
- **SurfaceFlinger confirms it end-to-end**: the session SurfaceView shows
  ~1 frame/s under load (p50 ≈ 460 ms, p90 ≈ 2.2 s), i.e. what the user
  sees is what is measured.
- **The device is memory-starved**, which is why everything stalls and then
  catches up: 25 MB free of 3 GB, **swap 100 % full (511/511 MB)**,
  `pgmajfault` +172 in 3 s (~57 major faults/s), CPU ~300 % of 400 % in
  **kernel** time with user time only ~90 %.
- **gamescope-wl is the hog**: VmRSS 947 MB of which **RssAnon 908 MB**
  (private, not shared buffers), VmData 1.18 GB. Sampled over a minute it
  is flat (945 → 942 → 941 MB): a large baseline allocation, not a leak.
- The pressure is so severe that `adb shell input keyevent` (a Java
  process launch) took ~11 s per call, which also invalidated earlier
  input-rate-based measurements.

**Ruled out this round:** `--disable-gpu-vsync --disable-frame-rate-limit`
(no change), `--renderer-process-limit=1 --js-flags=--max-old-space-size=192`
(saved ~40 MB, no visible change), `BL_MANGOAPP=0` + `BL_XWAYLAND_COUNT=1`
(no measurable change), force-stopping background Android apps (GMS
restarts itself; net zero).

## 24. Shim v6 (kept — correctness, not the stutter fix)

While chasing this, `eglGetProcAddress` was found to hand back the **real**
hybris symbols for most entry points, which bypassed the shim's window
surface emulation entirely (`CreateWindowSurface`/`SwapBuffers` never
logged; `MakeCurrent` was surfaceless: `rd=nil dr=nil`). v6 returns the
shim's own wrappers for the surface/swap/query/context entry points and
replaces the per-pixel RGBA→BGRA swizzle with a `GL_BGRA` readback plus
row-wise flip (`memcpy` per row instead of `w*h` iterations). Behaviour is
unchanged today because CEF still runs surfaceless, but the present path is
now ours whenever a window surface is requested.

## 25. Next moves for the stutter

1. **Give the device headroom.** 3 GB with gamescope's ~900 MB baseline
   leaves nothing for CEF + the guest. Options, cheapest first: run the
   session without gamescope (direct X11 / `BL_STEAM_UI=desktop`) to
   recover ~900 MB and confirm the theory in one session; or find the
   gamescope allocation knob (`--expose-wayland` / output size / Vulkan
   device) that brings 908 MB down.
2. **Ask what the allocation is**: gamescope's RssAnon is Vulkan/lavapipe
   side. Try `MESA_...`, or a gamescope build with its GL renderer.
3. Only after memory is healthy does frame pacing mean anything — anything
   measured while swap is full is measuring reclaim, not rendering.

---

# Update 9: where gamescope's ~950 MB actually goes (measured), and the two real ways out

## 26. The allocation, characterized

Sampling gamescope's RSS from session start (`ps -A -o rss,comm`):

```
 243 → 338 → 465 → 598 → 714 → 851 → 936 MB  (≈60 s), then flat at ~930 MB
```

It is **not** preallocated, **not** a per-frame leak (it plateaus), and
**not** affected by any of: output/nested resolution (640×540…960×540),
`--expose-wayland` (now switchable: `BL_EXPOSE_WAYLAND=0`), `LP_NUM_THREADS=1`,
`MESA_SHADER_CACHE_DISABLE=true`, `--xwayland-count 1`, mangoapp off.
`/proc/<pid>/status` shows `RssAnon 908 MB`, `RssShmem 35 MB`,
`VmData 1.18 GB` — private, not shared buffers.

The address space is 13 × 46.0 MB + 23 × 18.0 MB `rw-p` anonymous blocks
(~1.0 GB). The 0123 shm output path is **not** it: its buffers are
960×540 (`size=2073600`), and all `memfd:gamescope-shm` maps together are
~14 MB. What remains ís lavapipe's Vulkan image memory, which the device
reports as a single 2 GB heap (`memoryHeap[0] size=2147483648`,
`memoryTypes=1` from the 0117 diag line), i.e. plain anonymous mmap.

So: **gamescope's software-Vulkan compositor settles on a ~950 MB working
set on this device, and a 3 GB SHIELD cannot hold that plus the Steam
client + CEF + the guest's tooling.** Everything else measured (frame
bursts, 1 fps present cadence, 11 s `input keyevent`) is downstream of the
resulting reclaim thrash.

## 27. The two real ways out

**A. Don't run gamescope for the UI (≈950 MB reclaimed).** The app's own
compositor (`app/src/main/cpp/waylandcomp`) implements `xdg-shell` but
**not** `xwayland_shell`, so Steam's X11 CEF cannot attach to it directly
today; gamescope is what supplies Xwayland. Making this real means either
(a) implementing `xwayland_shell` in waylandcomp and dropping gamescope, or
(b) running CEF with `--ozone-platform=wayland`, which makes the Deck UI a
native Wayland client and needs no Xwayland at all. (b) is a one-session
experiment with the existing compositor. Cost: loses gamescope's
scaling/focus/frame-limiter behaviour, which the app reimplements.

**B. Bound gamescope's Vulkan pool (target ≈700–800 MB back).** We already
carry 20+ patches on gamescope 3.16.29 (`tools/gamescope/patches`, built by
`.github/workflows/build-gamescope.yml`), including 0116/0117–0121 from the
earlier lavapipe allocation bisect and 0123's shm output fallback. The
remaining work is to find which object is allocated 13 × 46 MB + 23 × 18 MB
during startup and either free it after the first composite or reuse one
ring. This is the surgical fix and it lives entirely in code we own.

**Not available:** growing zram (no root — `adbd cannot run as root in
production builds`), a real Vulkan driver for Tegra (renderless stub;
`libsysvk` was the dead end), and shrinking the pool by resolution or
Mesa knobs (all tested above, no effect).
