# ShieldDeck handoff — Shield TV GPU (libhybris/Vulkan), 2026-10-26 (evening)

Companion to `docs/agent-playbook.md` (device access, build loop, logs, traps — read it first)
and `docs/hybris-gpu-status.md` (full evidence trail). This file is the working state + next steps.

## TL;DR

- **EGL/GLES on the Tegra from the guest: DONE and shipped** (`RESULT GL_RENDERER=NVIDIA Tegra`).
- **Vulkan enumeration: DONE.** The guest's real Vulkan stack (Mesa loader 1.4.363) lists the
  hardware through the rewritten sysvk ICD (session 2026-10-26-16, `vulkaninfo --summary`):
  `GPU0: NVIDIA Tegra X1 (rev B) (nvgpu)`, api 1.1.178, driver 495.00, INTEGRATED_GPU.
- **Next: gamescope on the Tegra** (`dd env set 'VK_DRIVER_FILES=…sysvk_icd.json'`), then real
  rendering; buffer handoff must go through AHB (no dma_buf ext on the driver).

## What happened this session (all committed, pushed, CI green; HEAD `7df5804d`)

1. **vkprobe ordering bug fixed** (`1f13bd15`): the allocator-shim patch now runs before the
   CreateInstance probe; the dead `NvGlEglGetFunctions(2,…)` variant and the blind HAL
   `ensure(0,NULL,0)` block were deleted. First run with the shims (session -11) showed
   CreateInstance still calling a NULL: `singleton(0x19c5a70)` then
   `master(libglcore .bss 0x19c5640)->fn_0x160`.
2. **Disassembly findings** (libglcore 33.01/495.00, offsets in the cheat sheet): the "singleton
   allocators" are three pointers of a bigger init story; the master dispatch object (41 KB,
   allocated by `libglcore+0xf60200`, filled from `+0xf60250`) is installed by the
   `NvGlEglApiInit` chain — the same chain EGL initialization runs.
3. **The fix is EGL, not shims** (`f38acf97`): vkprobe's gpa path now runs the minimal EGL
   sequence first (display → pbuffer → context → MakeCurrent, shared in
   `tools/hybris/egl_warmup.h`). After that the three bss slots hold the REAL NVIDIA allocators
   and CreateInstance works: session -12 printed
   `RESULT VK_TABLE_DEVICE=NVIDIA Tegra X1 (rev B) (nvgpu)`. No shim ever fires; the shim
   patcher now only logs the real values and fills still-NULL slots.
4. **sysvk rewritten on the driver table protocol** (`7d2356ac`, `62ad013a`): at ICD-open it
   runs the EGL chain (installs driver state), opens the HAL device, then
   `NvGlEglGetFunctions(1,…)` + `table[4] = GetProcAddressNV("vkGetInstanceProcAddr")`.
   Global queries go through the real GIPA (NULL-instance conformant); CreateInstance and
   DestroyInstance are wrapped to substitute glibc `VkAllocationCallbacks` when the loader
   passes NULL (the driver dereferences pAllocator unconditionally). Device create/destroy pass
   through unwrapped (allocator requirement only proven at instance level).
5. **Two env traps found and fixed** (`776943f5`, `7df5804d`):
   - `LD_LIBRARY_PATH=/usr/local/hybris/usr/lib` must NOT reach vulkaninfo/gamescope — they
     would load the hybris vulkan wrapper instead of the Mesa loader (ws.c assert). The ICD's
     deps resolve via its own DT_RUNPATH.
   - Without LD_LIBRARY_PATH the hybris EGL platform plugin chain can't resolve, so sysvk
     preloads it leaf-first by absolute path (common → sync → hardware → ui → gralloc →
     platformcommon → eglplatformcommon; deliberately NOT libEGL/libGLES — guest Mesa owns
     those sonames) and recreates `/usr/lib/libhybris` if missing.
   - Without `HYBRIS_LD_LIBRARY_PATH` (patched bionic first) everything segfaults in
     eglGetDisplay — that one must stay exported for any process that touches the driver.

## Next steps (in order)

1. **Gamescope on the Tegra**: seed `dd env set 'VK_DRIVER_FILES=/usr/local/hybris/usr/share/vulkan/icd.d/sysvk_icd.json'`
   (no LD_LIBRARY_PATH!) + select the ICD as graphics driver. Watch for the loader's
   `VK_EXT_physical_device_drm` warning — gamescope checks it (handoff 2026-10-25 said it may
   be missing; verify against 495.00). gamescope's zink/CEF path comes after.
2. **Device-level sanity**: `vulkaninfo` summary works, but device creation is unexercised.
   Quick check: `vkcube`/`vulkaninfo --json` device block, then a tiny vkCreateDevice probe
   through sysvk (or run vkprobe's loader path). If vkCreateDevice faults on a NULL allocator,
   wrap it in sysvk (needs a pd→instance registry — see the shortcut comment in sysvk.c).
3. **Buffer handoff**: AHB path only (`VK_ANDROID_external_memory_android_hardware_buffer`).
4. If anything fights back at device level: fallback plan unchanged — Option B, out-of-process
   EGL helper handing buffers to the guest.

## Do NOT redo (all verified dead ends or solved)

Everything from the previous handoff (below) plus: shims for the allocator singletons
(EGL init installs the real ones; `tools/hybris/vkprobe.c` patcher remains as logger only);
driving the hwvulkan HAL's CreateInstance wrapper directly (needs driver-internal state);
`NvGlEglGetFunctions(2,…)` (empty); blind `base+0x2160(0,NULL,0)` (faults at NULL+0x30);
LD_LIBRARY_PATH for vulkaninfo/gamescope (hybris wrapper hijack).

Previous handoff list, still valid: stock-bionic TLS (patched bionic shipped); o-linker/APS2
(halium-11.0 headers); linking the hybris vulkan wrapper (IFUNC resolvers die in ld.so
relocation — dlopen in main, always); Android-loader discovery (stub HAL without sphal →
synthesized ld.config.txt solves it); `open("vulkan")` id (must be `"vk0"`).

## Cheat sheet — tegra HAL / libglcore offsets (Shield firmware 33.01, driver 495.00)

| what | where |
|---|---|
| hwvulkan device struct | HAL vaddr `0x4448` (ships all-NULL) |
| HAL real EnumerateInstanceExtensionProperties | HAL `+0x2060` |
| HAL ensure/bootstrap helper (args unknown) | HAL `+0x2160` |
| libglcore exports | `NvGlEglApiInit`, `NvGlEglGetFunctions` |
| `NvGlEglGetFunctions` | dispatcher, modes 0–4; mode 1 → vkGetProcAddressNV |
| driver table | 48 B; `[0]`=dlopen handle, `[1]`=GetProcAddressNV, `[4]`=GIPA (consumer-stored) |
| master dispatch object | .bss anchor `0x19c5640`, 41 KB (0xa190), `->fn` used at +0x140/+0x160 |
| allocator singletons | GOT `0x185b880/88/98` → bss `0x19c5a30/50/70` — **installed by EGL init**, NULL only before |
| `NvGlEglApiInit` chain | fill fn-ptr table → `{_nv004glsi,_nv006glsi}` pair → core init `0xf63de0` → master obj `0xf60200` |

## Build & probe loop

```sh
# CI: Build libhybris probe auto-runs on tools/hybris/** pushes (fork = JustKarkat/DroidDeck)
RID=$(gh run list -R JustKarkat/DroidDeck --limit 1 --json databaseId -q '.[0].databaseId')
gh run watch $RID -R JustKarkat/DroidDeck --exit-status
rm -rf /tmp/hp && gh run download $RID -R JustKarkat/DroidDeck -n hybris-probe -D /tmp/hp
tools/hybris/stage-payload.sh /tmp/hp/hybris.tzst
python3 tools/release/runtime_inputs.py record
./gradlew :app:assembleRelease -q
# reconnect loop FIRST — adb drops constantly on this device
adb -s 192.168.0.194:5555 install -r -t app/build/outputs/apk/release/app-release.apk
# probe (headless): set suspend policy never, run, read log, restore manual after
python3 tools/droiddeckctl --serial 192.168.0.194:5555 prefs set session suspendPolicy.steam never --type string
python3 tools/droiddeckctl --serial 192.168.0.194:5555 run /usr/local/bin/droiddeck-gpu-probe
adb -s 192.168.0.194:5555 shell "run-as com.droiddeck.launcher sh -c 'grep -aE \"stage|RESULT|vkstage|shim|FAULT|vulkaninfo|sysvk\" files/logs/<session-id>/session.log'"
# vulkaninfo's full output is ALSO at files/linuxfs/tmp/vulkaninfo.out in the guest rootfs
```

Live-guest experiments are much faster than build loops: start a desktop session and drive the
in-guest agent (playbook §6) — e.g. `VK_LOADER_DEBUG=driver vulkaninfo` with
`HYBRIS_LD_LIBRARY_PATH` exported and `LD_LIBRARY_PATH` unset.

Device state at handoff: idle, no session, `suspendPolicy.steam` = `manual`, latest APK
(commit `7df5804d` payload) installed. Untracked repo dirs (`.CleanupShield/`,
`prepared-components/`, `shield-probe.txt`, `stores-cargo-lock/`, `xbox-arm64/`) are
pre-existing — do not delete.

## Update 2026-10-27 (night): gamescope-on-Tegra verdict + CEF GPU path

**gamescope compositing on the Tegra is NOT possible — the driver's Vulkan is a
transfer-only stub.** Proven in the driver's own format tables (session
2026-10-26-32 + ctypes probes): B8G8R8A8 supports NOTHING, A8B8G8R8 only
TRANSFER_SRC/DST; no COLOR_ATTACHMENT, no STORAGE, no SAMPLED anywhere. The
99 extensions are advertised but unusable for rendering. The 0127 patch chain
that got us to the proof (all committed, keep — they also un-break any future
ICD): apiVersion-1.1 accept, fence-free completion (QueueWaitIdle drain),
CTimeline → nullptr, memory-type relaxation, per-ext enable gating
(external_memory_fd without dma_buf), WSI-pNext gating. Also: sysvk stubs
known-missing entry points (GetFenceFdKHR etc.) so callers get
VK_ERROR_FEATURE_NOT_PRESENT instead of NULL-call crashes. Gameoscope must
stay on lavapipe.

**CEF GPU path (session script)**: Tegra CEF now runs
`-cef-force-gpu -cef-use-gl=angle -cef-use-angle=gles -disable-gpu-sandbox`
instead of `-cef-disable-gpu`. Findings:
- The GPU process SIGSEGVs under Chromium's sandbox with the hybris EGL;
  after 4 crashes CEF silently falls back to SwiftShader.
  `-disable-gpu-sandbox` fixes it (0 crashes).
- Chromium has NO ANGLE GLES backend on Linux: `--use-angle=gles` lands on
  SwiftShader-vulkan. `--use-gl=egl` (native EGL) never started a GPU process
  in CEF 126. So CEF is currently SwiftShader-rendered but out-of-process
  (no more UI-thread wedge) and READY + OOBE render proven (sessions
  2026-10-26-34/-36).
- The real-hardware CEF lever left: an ANGLE build with a working Linux GLES
  backend (upstream doesn't ship one), pointed at the hybris EGL — the
  session script already stages `/usr/local/hybris-egl` (hybris sonames minus
  libvulkan) on LD_LIBRARY_PATH inside the steam branch, plus
  HYBRIS_LD_LIBRARY_PATH. `BL_CEF_GLES=0` restores software CEF.
- Debug env on device: cleared (was: BL_VK_DRIVER/gamescope_diag_skip/etc.).
  suspendPolicy restored to manual.

Vulkan games: DXVK/vkd3d require timeline semaphores the driver doesn't
export — old DXVK or GL-only games via... there is no GL driver for Tegra in
Mesa. Realistic device story: GLES-heavy Android-side rendering (proven), UI
via SwiftShader/CEF, streaming clients for real games.
