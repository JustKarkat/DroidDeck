# ShieldDeck — GPU blockers review doc (2026-10-27)

**Purpose:** a complete, self-contained briefing for an external reviewer (model) that has
access to this repository (`github.com/JustKarkat/DroidDeck`, branch `shield-tv`, HEAD
`99aa7dc4` + unpushed local work) but **no access to the device**. Everything needed to reason
about the blockers is in this file; repo pointers are given for deeper reading.

## 1. What this project is

ShieldDeck runs a desktop-style Steam session on a 2015 NVIDIA Shield TV:

- Host: Android 11 app (`com.droiddeck.launcher`), **no root**, 3 GB RAM, 4× Cortex-A57,
  Wayland compositor (`waylandcomp`) in the app renders the session into an AHardwareBuffer.
- Guest: glibc Arch rootfs under **proot** (nested ptrace unavailable — no strace/gdbserver).
- Window manager / compositor: **gamescope 3.16.29** (patched, see `tools/gamescope/patches/`),
  nested Wayland backend, hosting Xwayland + Steam (gamepadui, CEF 126).
- GPU access from the guest via **libhybris** + Halium-patched bionic
  (`tools/hybris/build-in-arch.sh`); the vendor drivers are Android blobs
  (`/vendor/lib64/libglcore.so`, `libEGL_tegra.so`, `vulkan.tegra.so`).

## 2. What is proven and working (do not re-derive)

1. **EGL/GLES on the Tegra from the glibc guest works fully** — eglprobe prints
   `GL_RENDERER=NVIDIA Tegra`, GLES 3.2, real GL rendering (sessions since 2026-10-25).
   Notably `eglQueryString(EGL_CLIENT_APIS)` returns **"OpenGL_ES OpenGL"** — the Tegra EGL
   *advertises a desktop-OpenGL client API*. We have never attempted an `EGL_OPENGL_API`
   context on it. (Untested hypothesis — see B1.)
2. **Vulkan enumeration works** through a custom ICD (`tools/hybris/sysvk/sysvk.c`) on the
   "driver table protocol": `NvGlEglGetFunctions(1, &table[1])`, consumer stores
   `table[4] = GetProcAddressNV("vkGetInstanceProcAddr")`, real driver GIPA is
   NULL-instance-conformant. `vulkaninfo --summary` lists
   `NVIDIA Tegra X1 (rev B) (nvgpu)`, api 1.1.178, driver 495.00.
3. **The Tegra Vulkan driver (495.00) cannot render.** Verified via its own format tables
   (ctypes probes): B8G8R8A8 supports *no* usage bits; A8B8G8R8/R8G8B8A8 only
   TRANSFER_SRC/DST. No COLOR_ATTACHMENT, no STORAGE, no SAMPLED on any 8-bit format; no
   timeline semaphores (and `vkWaitSemaphores`/`vkGetSemaphoreCounterValue`/
   `vkWaitForFences`/`vkGetFenceStatus`/`vkGetSemaphoreFdKHR`/`vkGetMemoryFdKHR`/fence-fd
   resolve to **NULL entry points**). 99 extensions are advertised but unusable for rendering.
   → gamescope compositing on the Tegra is **impossible**; DXVK/vkd3d are **impossible**.
4. **Env contract** (violating any of these breaks things in confusing ways):
   - `HYBRIS_LD_LIBRARY_PATH=/usr/local/hybris/bionic:/system/lib64:/vendor/lib64:/vendor/lib64/hw:/vendor/lib64/egl:/apex/com.android.runtime/lib64/bionic`
     must be set for any process that touches the driver (patched bionic first).
   - `LD_LIBRARY_PATH` must **not** contain the hybris dir for processes using the guest's
     Mesa Vulkan loader (the hybris libvulkan wrapper would hijack `libvulkan.so.1`).
   - Any process using the driver needs its driver state installed: running the minimal EGL
     sequence (display→pbuffer→context→MakeCurrent, `tools/hybris/egl_warmup.h`) initializes
     libglcore's master dispatch object + allocator singletons; without it CreateInstance and
     friends call through NULL.
5. **Steam UI renders today** (user confirms OOBE visible) with gamescope on lavapipe and
   Steam's CEF GPU process out-of-process. It is *slow* — see B2.

## 3. Blockers

### B1 (top priority): Steam's CEF cannot bind the Tegra GLES — status: patched, unverified

**Goal:** Steam's UI (CEF 126 in `steamwebhelper`) rendering on the Tegra GPU instead of
SwiftShader/CPU. This is the last big consumer of CPU that could move to hardware.

**Facts:**

- Valve's `steamwebhelper` force-launches its GPU process with
  `--use-gl=angle --use-angle=swiftshader-webgl` — Steam's own fork hardcodes software WebGL
  regardless of client flags. Found by reading `/proc/<gpu-pid>/cmdline`.
- libcef.so contains the ANGLE extension strings `EGL_ANGLE_platform_angle_opengl`,
  `EGL_ANGLE_platform_angle_device_type_egl_angle` (the `gl-egl` path), and dlopen strings
  `libGL.so`, `libGL.so.1`, `libGLX_nvidia.so.0`. So **the ANGLE OpenGL backend ships** in
  Valve's CEF.
- We binary-patched the guest's `libcef.so`
  (`/root/.local/share/Steam/steamrtarm64/libcef.so`): the single occurrence of the string
  `swiftshader-webgl` (17 bytes) was replaced in place with `gl-egl` + NUL padding (strcmp
  stops at NUL). Backup kept at `libcef.so.droiddeck-bak`. After the patch the GPU process
  command line shows `--use-angle=gl-egl` (verified via /proc/<pid>/cmdline).
- The guest session exports `LD_LIBRARY_PATH=/usr/local/hybris-egl` (symlink farm of every
  hybris wrapper lib **except** libvulkan) + `HYBRIS_LD_LIBRARY_PATH` for the inner steam
  branch of `tools/linuxfs/overlay/usr/local/bin/droiddeck-session`, so a dlopen of
  `libEGL.so.1` from the GPU process resolves to the **Tegra EGL** through hybris.
- GPU process CPU is high (~40%) after the patch — consistent with either SwiftShader still
  running, or hardware init plus software fallback paths. **We cannot yet tell which renderer
  ANGLE-gl actually bound.**

**The open questions for the reviewer:**

1. Does upstream ANGLE's OpenGL backend (`--use-angle=gl-egl`) support **GLES** drivers, or
   is it desktop-GL only? If it dlopens `libGL.so.1`/`libGLX` (as the strings suggest) it may
   never touch libEGL at all, in which case `gl-egl` cannot reach the Tegra. What exactly does
   DisplayEGL/DisplayGLX load and which EGL APIs does it request?
2. What version/features does the ANGLE GL backend gate on (desktop GL 3.x? GLES3?), and can
   a GLES 3.2 driver satisfy them?
3. If ANGLE-gl is desktop-only: what is the cheapest way to give Chromium a GLES-backed
   desktop-GL — e.g. a `libGL.so.1` shim (gl4es translates GL 2.1 → GLES2; ANGLE may gate on
   a higher version), or a purpose-built ANGLE build? Note CEF statically links ANGLE, so the
   *system* libEGL/libGL it dlopens is the only replaceable layer.
4. **Cheap decisive experiment we could not complete** (device-side, see B3): create an
   `EGL_OPENGL_API` context on the Tegra EGL via hybris and print `glGetString(GL_VERSION)`.
   The Tegra EGL advertises the OpenGL client API (fact 2.1); if a desktop-GL context actually
   works, ANGLE-gl-egl should bind natively and B1 collapses to "make it stick". A 20-line
   probe based on `tools/hybris/eglprobe.c` answers this.
5. How do we **persist** the libcef patch across Steam client updates (currently a one-shot
   binary patch in the guest rootfs)? Candidate: re-apply from the session script after
   Steam self-update (string may change between versions).
6. Verification channel: CEF logs nothing about ANGLE init by default. What Chromium/CEF
   switches make GPU-process GL init failures visible in `--log-file` (e.g.
   `--enable-logging=stderr --v=1`, `GpuControl` debug)?

### B2: CPU starvation — three heavy processes on four A57 cores

Measured (session 2026-10-27-05, `ps` by CPU):

```
50.6%  gamescope (lavapipe compositor, 960x540)
39.5%  steamwebhelper --type=gpu-process  (ANGLE target unknown — B1)
18.6%  com.droiddeck.launcher (host app — waylandcomp/AHB path, see below)
16.7%  steamwebhelper --type=renderer
```

The user confirms the OOBE renders but is slow. The host app itself burning ~20% CPU is
unexplained — candidates: the app-side compositor loop, CDP polling (`droiddeck-steam-compat`
waits on SharedJSContext), or SurfaceFlinger-side work attributed to the app.

Reviewer questions:

1. What would you profile first on a 4-core ARM device where compositor + GPU-process +
   app-compositor split the CPU? (No perf/ptrace inside proot; the *host* Android side can
   use `debuggerd`/simpleperf normally.)
2. Known SwiftShader/llvmpipe tuning knobs worth trying: `LP_NUM_THREADS` (currently 2),
   SwiftShader thread count for Chromium (`--num-raster-threads`, already 1 in renderer),
   `--disable-gpu-process-crash-limit`? Output resolution is 960x540 already.

### B3: No device-side debugging inside the guest

proot forbids nested ptrace, so strace/gdbserver cannot attach to any guest process. We have
an in-process SIGSEGV logger in **gamescope** (patch `0127-diag`, prints pc/lr + maps), which
is how all the driver NULL-PFN crashes were localized. There is **no equivalent for
steamwebhelper** — a GPU-process crash only shows as `exit_code=11` in cef_log.

Ideas to evaluate:

- `LD_PRELOAD` a tiny fault-handler .so into steamwebhelper (glibc process, preloading
  works — the guest already ships `/etc/ld.so.preload` shims `libblsession/libfakeinput/
  libblfastpath`). A 40-line sigaction handler writing pc/lr + maps to stderr would make
  GPU-process crashes debuggable.
- Chromium switches for GPU-process crash diagnostics (see B1.6).

### B4: The games layer (accepted dead-end for local Vulkan; open for GL)

- Vulkan games: impossible (B-fact 2.3). No timeline semaphores → even old DXVK is out.
- GL games: no Tegra driver in Mesa; a GL→GLES shim (gl4es) inside the guest would give
  GL 2.1-class games a hardware path through hybris, but that is a per-game-quality project.
- Streaming (Moonlight/Steam Link/GeForce NOW) works well on this hardware and is the
  pragmatic "performance and quality" answer for actual games — product decision, flagged
  for the owner.

### B5: Cleanup debt in gamescope patches (low priority)

`0117–0121-diag` logging patches (per-frame `vk_log.infof`, bare-alloc probes) and the
`0127-diag` fault logger are temporary bring-up aids, still compiled in. They cost per-frame
log formatting on the hot path. The playbook says remove before release. Not touched while
the CEF story is in flux.

### B6: libhybris wrapper vs system sonames (minor, structural)

The guest's `/usr/lib/libEGL.so.1` (glvnd→Mesa) and the hybris `libEGL.so.1` share a soname;
which one a process gets depends on `LD_LIBRARY_PATH` order. sysvk deliberately excludes
libvulkan from its preload list for this reason. Any new glue (ANGLE, GL shims) must respect
the same split: GL/EGL can be hybris; libvulkan must never be.

## 4. Repo map (branch `shield-tv`)

- `docs/agent-playbook.md` — device access, build loops, session tools (`tools/droiddeckctl`),
  adb quirks. **Read first** if device access is later granted.
- `docs/handoff-shield-tv-2026-10-26.md` — full GPU history, offsets cheat sheet
  (libglcore/HAL vaddrs), build & probe loops, do-not-redo list. Updated 2026-10-27 evening
  with the "Tegra Vulkan is a stub" verdict.
- `docs/hybris-gpu-status.md` — evidence trail.
- `tools/hybris/` — `eglprobe.c`, `vkprobe.c` (driver-table probe + fault logger),
  `sysvk/sysvk.c` (the ICD: EGL-init chain → driver table → allocator-substituting wrappers →
  NULL-entry-point stubs), `egl_warmup.h`, `build-in-arch.sh` (CI), `stage-payload.sh`.
- `tools/gamescope/patches/` — 0002…0127; `0127-tegra-vk11-no-timeline.patch` = Vulkan-1.1
  accept + fence-free completion + CTimeline degradation + per-extension gating + WSI-pNext
  gating + in-process fault logger. Built by `.github/workflows/build-gamescope.yml` (fork CI),
  artifact staged into the APK.
- `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` — session script; the `BL_TEGRA`
  branch has the CEF flag choices and the `/usr/local/hybris-egl` + `HYBRIS_LD_LIBRARY_PATH`
  export. `BL_CEF_GLES=0` restores software CEF.
- `tools/linuxfs/overlay/usr/local/bin/droiddeck-gpu-probe` — probe wrapper (synthesizes
  `/linkerconfig/ld.config.txt` for the q linker; runs eglprobe + vkprobe + vulkaninfo).
- App side: `app/src/main/java/com/droiddeck/launcher/…` (`SessionService.kt` seeds
  `BL_TEGRA=1`, `LP_NUM_THREADS=2`; `BwrapSpawner.kt` injects default ICDs unless overridden).

## 5. Device state at pause

- Session `2026-10-27-05-steam` was left running (OOBE visible on the TV).
- `libcef.so` patched (gl-egl), backup `libcef.so.droiddeck-bak` beside it.
- Debug env on the device: cleared; `suspendPolicy.steam=manual` restored.
- All repo work pushed; the blocker doc + this file are the only unpushed additions (will be
  pushed with the doc).

## 6. Ranked asks for the reviewer

1. **B1.1–B1.4**: pin down whether ANGLE-gl (gl-egl) can run on a GLES 3.2 driver — and if
   not, design the cheapest bridge (libGL shim vs ANGLE rebuild vs Chromium switches). The
   EGL_OPENGL_API-context probe design is wanted either way.
2. **B1.5–B1.6**: persistence + observability plan for the CEF GPU process.
3. **B2**: a triage plan for the 3-way CPU contention (what to measure first, what to tune).
4. Sanity-check the sysvk/0127 design for landmines we haven't hit yet (allocator
   substitution, stub list, fence-free completion).
