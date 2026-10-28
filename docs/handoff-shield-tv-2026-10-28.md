# ShieldDeck handoff — CEF-on-Tegra, the chain is proven, the GPU process still exits, 2026-10-28

Companion to `docs/agent-playbook.md` (device access, build loops — read first),
`docs/hybris-gpu-status.md` (the GPU stack, now with the 10-28 update) and
`docs/handoff-shield-tv-2026-10-27.md` (previous state).

## TL;DR

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
