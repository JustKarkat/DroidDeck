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
- **STILL BROKEN (sharpened by session 62, §8–12):** the GPU child
  crash-loops anyway (6× `exit_code=11`, then survivors on
  `--use-gl=disabled`). Shim v4 instrumentation PROVED no shim EGL entry
  point is ever called (88 armed children, zero entry lines) and the dying
  child's maps show the **DEFAULT → Mesa GLX/llvmpipe** path fully loaded —
  it crashes inside Mesa init, before ANGLE's DisplayEGL could ever dlopen
  the shim. Prime suspect: the **DT16 patch did not take effect this
  session** (unverified — the in-guest check timed out), or Valve's fork
  gates the DT16 path upstream of the switch. NOT the WSI layer, NOT
  crashpad, NOT the sandbox, NOT the shim's hybris dlopen (that theory is
  dead).
- **Your first moves (details in §12):** (1) verify the DT16 patch bytes at
  libcef vaddr 0x7872bd0 in a quiet session; (2) if patched, reverse the
  code between the string match and the `mov w10,#16` for an upstream gate;
  (3) if not patched, fix the patcher ordering. `DDEGL_DLOPEN_LOCAL=1` is
  wired into shim v4 (RTLD_LOCAL A/B) but is not the current blocker.
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
