# ShieldDeck handoff — CEF-on-Tegra (window-surface shim + our ANGLE), 2026-10-27 (late)

Companion to `docs/agent-playbook.md` (device access, build loops, tools — read first),
`docs/handoff-shield-tv-2026-10-26.md` (GPU history + do-not-redo), and
`docs/ExternalAgentResponses/Response102726/` (the external model's review that drove this
work). This file is the working state + exact next steps.

## TL;DR

- **Mission (user's words): hardware acceleration, method doesn't matter.** The user can SEE
  Steam's OOBE rendering on the TV — it works but is slow (SwiftShader/CPU everywhere).
- Everything needed for **CEF on the Tegra GPU now exists**: window-surface shim (built,
  works end-to-end when driven directly), our own ANGLE build with the GL backend (built,
  green), the `gles-egl` patch (live), idempotent CEF patcher (shipped).
- **One wiring bug found at the end**: our ANGLE's GL backend dlopens `libEGL.so.1` by
  generic name → dedups against Chromium's early-loaded **Mesa** libEGL → ANGLE lands on
  llvmpipe ("Google Inc. (Mesa)") instead of the shim/Tegra. Fix is IN (commit `477e4d57`):
  ANGLE CI now binary-patches its own loader string to the unique name `libddcef.so`, and
  the session script stages the shim under that name.
- **Your first job**: pick up the in-flight ANGLE CI rebuild (run **40626002076**, may be
  finished), stage its artifact, rebuild+install the APK, restart a session, and verify.

## Where everything lives

| piece | state | where |
|---|---|---|
| Window-surface shim `libddcefgl.so` | BUILT + device-verified working (default-display + pbuffer + ctx + MakeCurrent all succeed through it when driven directly) | `tools/hybris/eglshim/egl_shim.c` (own minimal EGL ABI header `egl_abi.h`); built in `build-in-arch.sh` |
| Shim v2 (dlsym-name fix + 1.5-entry guards) | **BUILT but NOT YET DEPLOYED** — the fixed build is CI run 40605875731, staged into `app/src/main/assets` by `tools/hybris/stage-payload.sh`, but the APK has NOT been rebuilt/installed since | CI `build-hybris.yml` |
| Our ANGLE (GL backend, aarch64) | BUILT (first version) + staged as APK assets; **v2 with `libddcef.so` rename = CI run 40626002076 — CHECK IT** | `tools/angle/build-in-arch.sh`, workflow `build-angle.yml` |
| `gles-egl` patch in libcef | LIVE on device (GPU process cmdline shows `--use-angle=gles-egl`); re-applied every session by `droiddeck-cef-patch` | `tools/linuxfs/overlay/usr/local/bin/droiddeck-cef-patch` |
| Session-script plumbing | DONE: replaces Valve's `libEGL.so`/`libGLESv2.so` with ours (backup `.droiddeck-orig`), stages shim as `/usr/local/hybris-cef/libddcef.so` first on LD_LIBRARY_PATH, exports HYBRIS_LD_LIBRARY_PATH, BL_WEBHELPER_ARGS logging, `BL_CEF_DIAG=1` → `DDEGL_DEBUG=1` + `--disable-software-rasterizer` | `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` (BL_TEGRA branch) |
| Guest copy lists | `usr/bin/glprobe`, `usr/local/bin/droiddeck-cef-patch`, `usr/lib/libddcefgl.so.1.0.0`, `usr/lib/angle/libEGL.so`, `usr/lib/angle/libGLESv2.so` — ALL added to `SessionFiles.kt` (learned: assets not in the copy lists never reach the guest) | `app/src/main/java/com/droiddeck/launcher/session/SessionFiles.kt` |
| glprobe (EGL matrix probe) | Works; proved Tegra EGL does desktop GL 4.6 + all 56 configs have OPENGL/ES2/ES3 bits | `tools/hybris/glprobe.c`, wired into `droiddeck-gpu-probe` wrapper |

## How to verify end to end (the loop that worked)

```sh
# 1. ANGLE artifact (run 40626002076; if not finished: gh run watch <id> -R JustKarkat/DroidDeck)
rm -rf /tmp/angle && gh run download <angle-run-id> -R JustKarkat/DroidDeck -n angle-libs -D /tmp/angle
cd /tmp/angle && sha256sum -c angle-libs.tzst.sha256 && tar --zstd -xf angle-libs.tzst
# sanity: the loader string must be patched:
strings -a libGLESv2.so | grep -c libddcef.so          # expect >= 1
strings -a libGLESv2.so | grep -c "libEGL.so.1"        # expect the dlopen site only (1)
cp libEGL.so libGLESv2.so <repo>/app/src/main/assets/linuxfs/usr/local/hybris/usr/lib/angle/

# 2. shim v2 is already staged in assets (from run 40605875731); just build+install:
cd <repo>
python3 tools/release/runtime_inputs.py record
./gradlew :app:assembleRelease -q
adb -s 192.168.0.194:5555 install -r -t app/build/outputs/apk/release/app-release.apk

# 3. session:
python3 tools/droiddeckctl --serial 192.168.0.194:5555 start steam --timeout 30

# 4. verify (device MCP preferred — the user says one is now available; else adb + guest
#    agent file protocol per playbook §6). Check the GPU process:
#    - cmdline: --use-angle=gles-egl
#    - maps: libddcefgl + angle/libGLESv2 + libglcore/libEGL_tegra present,
#      swiftshader ABSENT
#    - fds: /dev/nvmap|nvhost|nvgpu > 0
#    - CDP: droiddeckctl cdp eval --target SharedJSContext (canvas webgl renderer string
#      should say NVIDIA Tegra, not SwiftShader)
```

## Known device-side traps (all hit, all real)

1. **`SessionFiles.kt` copy lists**: APK assets are NOT auto-installed into the guest; every
   new file must be listed there. Symptom of missing: "No such file or directory" in the
   session log (`cef-patch exit 127` etc.).
2. **`dd env set` is one-shot** unless `--persistent`. Currently persistent on device:
   `BL_CEF_DIAG=1` (DDEGL_DEBUG + --disable-software-rasterizer). Keep while diagnosing,
   clear when done (`dd env clear all`). Device also has `suspendPolicy.steam=never` right
   now — restore `manual` at the end.
3. **glprobe via LD_LIBRARY_PATH**: link-time deps need
   `LD_LIBRARY_PATH=/usr/local/hybris/usr/lib` + `HYBRIS_LD_LIBRARY_PATH` exported.
4. **The old `gl-egl` value**: dead end (requests desktop-GL configs; device-verified
   SwiftShader fallback). `gles-egl` is correct for our ANGLE build.
5. **Don't run heavy tests while Steam boots** (user's advice, correct): a 4-core box under
   boot load makes probes time out. Either test in a `dd run /bin/sleep -- 3600` quiet
   session or wait for READY.
6. **Verbose GPU logs land in** `files/linuxfs/root/.local/share/Steam/logs/steamwebhelper.log`
   (NOT cef_log.txt) — that's where `ddcef-egl` shim lines and `--vmodule` output go.
7. If the GPU process still skips the shim after the rename lands: check that
   `/usr/local/hybris-cef/libddcef.so` exists in the guest and that our `libGLESv2.so` in
   steamrtarm64 contains the `libddcef.so` string (the session script replaces the pair at
   every start — watch for `== steam: ANGLE front libs replaced`).

## Open risks (honest)

- ANGLE config selection may want surface-type WINDOW configs — our Tegra config census
  says all 56 configs have PBUFFER (and WINDOW?) bits; glprobe's census printed
  OPENGL/ES2/ES3 renderable bits but not surface bits. If ANGLE says "no suitable config",
  the shim may need to synthesize a config or the census needs a WINDOW_BIT check.
- `libGL.so.1` string also exists in our libGLESv2 (DisplayGLX path). If ANGLE tries GLX
  first under ozone/x11, the shim (EGL-only) will bounce it; the gles-egl switch should pick
  DisplayEGL, but if logs show GLX attempts, add `-ozone-platform=x11 --use-angle=gles-egl`
  ordering notes or extend the shim.
- If hardware binds, expect the first frames through the shim to be slow-ish (2 MB/frame
  readback + blit); that's still GPU-rasterized UI vs full SwiftShader. Measure with the
  review's §4.1 run-delay snapshot before/after.

## Device state at handoff

- Session `2026-10-27-15-steam` was left running (OOBE on TV), then the device was left
  alone; last state IDLE. APK currently installed: commit `2e1bf973`-era payload + ANGLE v1
  libs (NO shim-v2, NO libddcef rename).
- Persistent env: `BL_CEF_DIAG=1`. `suspendPolicy.steam=never` (restore manual when done).
- Untracked repo dirs (`.CleanupShield/`, `prepared-components/`, `stores-cargo-lock/`,
  `xbox-arm64/`, `shield-probe.txt`) are pre-existing — do not delete.
- Everything committed and pushed; HEAD `477e4d57` on `shield-tv`.

## Suggested next steps in order

1. Check ANGLE run 40626002076; if green, verify the `libddcef.so` string landed in its
   `libGLESv2.so` and stage it into the APK assets (paths in the verify block above).
2. Rebuild + install APK (shim v2 + ANGLE v2 both ride in it).
3. Start a steam session; verify the GPU-process maps/fds + CDP renderer string.
4. If ANGLE calls the shim but fails at config/surface: read `ddcef-egl` lines in
   `steamwebhelper.log` (device-trap #6), iterate the shim (all source under
   `tools/hybris/eglshim/`; CI loop is fast — ~4 min via build-hybris).
5. When the renderer string says NVIDIA Tegra: screenshot the OOBE, do a quick before/after
   CPU snapshot (review §4.1), then `dd env clear all`, `suspendPolicy.steam=manual`, commit,
   push, update `docs/hybris-gpu-status.md` + this handoff.
