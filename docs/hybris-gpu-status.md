# GPU for the guest — libhybris status (2026-10-25)

Short version: the Shield's GPU is reachable from the proot guest, libhybris builds and loads the
whole bionic stack, but **stock Android 11 bionic fails libhybris' TLS contract**, so `eglGetDisplay`
segfaults inside `libc.so`. The missing piece was a patched bionic (`hybris-hal`) — **it has now
been found, verified and packaged into the CI artifact** (see "hybris-hal bionic" below); what
remains is a device run with the new `HYBRIS_LD_LIBRARY_PATH` order.

## What is verified

- **Kernel side is open from the guest.** A guest process (same app UID, same `untrusted_app_27`
  SELinux domain) can open `/dev/nvhost-ctrl`, `/dev/nvhost-gpu`, `/dev/nvhost-as-gpu`, `/dev/nvmap`.
- **Vendor EGL is healthy headless.** An NDK (bionic) probe on the device reports
  `EGL 1.5 vendor=Android`, creates a pbuffer + context, `eglClear` OK, and
  `GL_RENDERER=NVIDIA Tegra`, `GL_VERSION=OpenGL ES 3.2 NVIDIA 495.00`.
- **The guest can see the Android filesystem.** `LinuxRuntime` now binds `/system/lib64`,
  `/apex/com.android.runtime` and `/vendor` into the session (commit `a821192f`; without the APEX
  bind, `libc.so`/`linker64` are dangling symlinks on Android 11).
- **libhybris builds in CI and loads the bionic tree.** New workflow `Build libhybris probe`
  (`.github/workflows/build-hybris.yml`, `tools/hybris/build-in-arch.sh`) builds libhybris +
  `eglprobe` natively in `menci/archlinuxarm:base-devel`, artifact `hybris-probe`. It loads
  libEGL, libcutils, liblog, libhidlbase, libui, libutils, libc++, libbinder, gralloc HIDL,
  `/apex/.../libc.so`, `/apex/.../libm.so`, `/apex/.../libdl.so` and more.
- **Runtime layout that works:** extract the artifact to `/usr/local/hybris`, symlink
  `/usr/lib/libhybris -> /usr/local/hybris/usr/lib/libhybris` (the wrapper hardcodes that path), and
  run with
  `LD_LIBRARY_PATH=/usr/local/hybris/usr/lib`,
  `HYBRIS_LD_LIBRARY_PATH=/usr/local/hybris/bionic:/system/lib64:/vendor/lib64:/vendor/lib64/egl:/apex/com.android.runtime/lib64/bionic`
  (the `/usr/local/hybris/bionic` entry — Halium-patched bionic from the artifact — is new and must
  come first; it supplies `libc.so`, `libm.so`, `libdl.so`, `libdl_android.so`, `ld-android.so`,
  `libc++.so`, so every bionic DT_NEEDED resolves to a TLS-contract-conforming libc).

## hybris-hal bionic: found, verified, packaged (2026-10-25)

The missing artifact — an arm64 API-30 bionic with Halium's hybris patches — is **publicly
available** and needs no AOSP platform build:

- **Source: Droidian package `android-system-gsi-30`** ([pool
  listing](https://production.repo.droidian.org/pool/main/a/android-system-gsi-30-bin/)), pinned
  `11.0.0+r46.20230226.ubports.286+git20230329184623.6015077.bookworm.production_arm64.deb`
  (sha256 `8f3cb4b0…3f4413f`, 114 MB). It is UBports' Halium-11 (Android 11) GSI system image
  (`android-rootfs.img`, ext4 inside the deb), the same production image Droidian/UBports run
  against stock Android 11 vendor partitions on Treble devices.
- **Verified locally:** its `libc.so` (from `/system/apex/com.android.runtime/lib64/bionic` inside
  the image) exports `__get_tls_hooks@@LIBC` and contains the emutls machinery — both Halium
  patches present. `readelf -sW libc.so | grep __get_tls_hooks` is non-empty, exactly the check the
  stock Shield bionic failed.
- **Patch ground truth** ([Halium/hybris-patches](https://github.com/Halium/hybris-patches) branch
  `halium-11.0`): `bionic/0004-hybris-Fix-__get_tls-and-related-functions-Android-1.patch` adds
  `libc/hybris_support.c` defining `__get_tls_hooks()` (exported via `libc.map.txt`) and routes
  bionic's TCB access through it; `bionic/0007-halium-disable-GWP-ASan-and-use-emulated-TLS-in-libc.patch`
  adds emulated TLS; `frameworks/native/0003` makes libEGL etc. use the private hook.
  [mer-hybris/hybris-patches](https://github.com/mer-hybris/hybris-patches) branch
  `hybris-sony-aosp-11` carries the same patches for Sailfish. Upstream libhybris
  [issue #559](https://github.com/libhybris/libhybris/issues/559) confirms patched bionic is the
  only proven route; the TLS save/restore alternative discussed there is still an unfinished
  experiment.
- **Packaged in CI:** `tools/hybris/build-in-arch.sh` downloads the pinned deb, extracts
  `libc.so`, `libm.so`, `libdl.so`, `libdl_android.so` (from `/system/apex/com.android.runtime/lib64/bionic`
  in the image — `/system/lib64/libc.so` there is just a symlink to it) plus `ld-android.so`,
  `libc++.so` (real files in the image's `/system/lib64`), asserts the `__get_tls_hooks@@LIBC`
  export, and ships the six libs (1.9 MB) as `usr/local/hybris/bionic/` inside the `hybris-probe`
  artifact.
- **Fallback if unpatched Shield system libs still trip on TLS:** the GSI image contains the *full*
  patched `/system/lib64` (~460 libs, Droidian model — patched system tree + Shield's `/vendor`);
  extracting more of it is mechanical. Alternative sources if the deb ever disappears: the Redmi 9
  `lancelot` Droidian Halium-11 release zip (gitlab.com/GwOS/droidian-lancelot/-/releases), or any
  Halium-11 device build's system image.

## The two crashes (both root-caused)

1. **Android 8.1 (`o`) linker + Android 11 packed relocations.** With Android 9 headers the build
   only contains `libhybris/linker/o.so`; `libEGL.so`/`libm.so` use `DT_ANDROID_RELA` (APS2), which
   `o` does not process, so `.init_array` entries stay unrelocated and the linker calls `0x69080`
   (`ElfReader::Load` → `call_function`). Fixed by building against
   `Halium/android-headers` branch **`halium-11.0`**; the `q` linker now exists and is used.
   (Upstream: `DT_ANDROID_RELR` support landed in libhybris `adc84317`; the `o` linker predates it.)
2. **Stock bionic TLS.** With `q` the crash moves into bionic itself:

   ```
   FAULT sig=11 addr=0x820 pc=0x… lr=0x…(libc++.so)
     <pc mapping>  /apex/com.android.runtime/lib64/bionic/libc.so
   ```

   `readelf -sW libc.so | grep __get_tls_hooks` is empty: stock bionic reads TLS inline
   (`mrs …, tpidr_el0`), while libhybris interposes `__get_tls_hooks()`. Halium's
   `hybris-patches` (`halium-11.0`) patch bionic for exactly this (`0004` TLS rework, `0007`
   `-femulated-tls`). Upstream confirms a patched bionic is required (libhybris issue #512).
   This cannot be fixed with env vars or by forcing a different linker at runtime.

## Vulkan status (2026-10-25, sessions 39–58)

Steam/gamescope render via **Vulkan**, so EGL was only the foundation. What it took, and where it
stands:

**Working chain (all verified on device):**
- libhybris' vulkan wrapper + `vulkanplatform_null` now build (Halium headers lack
  `vulkan/vulkan.h`; the CI container installs `vulkan-headers`, and the wrapper's retired
  `VK_NV_cuda_kernel_launch` IDLOADs are stripped). Caveat: never *link* the wrapper — its IFUNC
  resolvers run in ld.so's relocation phase and die under proot; always dlopen-in-main.
- **sysvk** (xMeM/sysvk, found via kde-yyds' android-vulkan-bridge — the existing project for
  exactly this) turns the hwvulkan HAL into a standard ICD for the guest's Mesa loader. Our
  vendored copy (`tools/hybris/sysvk/sysvk.c`) is patched for the Tegra.
- `hw_get_module("vulkan")` works, HAL `open("vk0")` works, and the HAL's driver bootstrap
  (`NvGlEglGetFunctions` → libglcore.so, bootstrapping through the bionic EGL chain) **loads and
  runs in the guest** — the full tegra gralloc/EGL/GLES chain maps via the vndksupport fallback.
- Two device quirks found and handled: `/vendor/lib64/hw` must be in `HYBRIS_LD_LIBRARY_PATH`
  (the HAL dlopens `vulkan.tegra.so` by soname), and the HAL's `open()` strcmps the device id
  `"vk0"` (anything else → `-ENOENT`).

**The remaining blocker — and how far it moved (2026-10-26, sessions 01–03):**

The decisive stock-Android test (option 2) came back **positive**: `cmd gpu vkjson` and a native
NDK probe both enumerate the real driver outside the guest — `NVIDIA Tegra X1 (rev B) (nvgpu)`,
driver 495.0, Vulkan 1.1.146, **99 extensions** including `VK_KHR_external_memory_fd`,
`VK_KHR_external_semaphore_fd` and `VK_ANDROID_external_memory_android_hardware_buffer` (no
dma_buf, no timeline_semaphore — gamescope integration will need the AHB path). The device-struct
fields are NULL **on stock too** — the stock loader reaches the driver some other way (likely
tegra-specific glue in NVIDIA's libvulkan; the hwvulkan HAL looks vestigial).

In the guest, the synthesized `/linkerconfig/ld.config.txt` (written by the probe wrapper — the
device's real one is root-only and the q linker fully supports the format) unlocked the driver
bootstrap: the HAL's `EnumerateInstanceExtensionProperties` (0x2060) now returns **7 real instance
extensions** (`VK_KHR_device_group_creation`, `VK_KHR_external_{fence,memory,semaphore}_capabilities`,
`VK_KHR_get_physical_device_properties2`, `VK_EXT_debug_report`, `VK_EXT_debug_utils`) — from zero
before. The environment problem is largely solved; what remains is the **calling protocol of the
tegra driver's function table**:

- `NvGlEglGetFunctions(1, ctx48)` in libglcore fills a 48-byte context table (the HAL's internals
  allocate 48 bytes, pass it, then use `ctx[8]`, `ctx[0x20]`, …).
- The HAL's `CreateInstance` candidate (0x242c) takes **(ctx, pCreateInfo, pAllocator, pInstance)**
  — 4 args, ctx first; calling it with 3 fails silently.
- Next probe step: call `NvGlEglGetFunctions(1, ctx)` directly, dump the 6 table slots, and identify
  `CreateInstance` / `GetInstanceProcAddr` / physical-device entrypoints by probing them with
  `NULL, "vkEnumerateInstanceExtensionProperties"` etc. under the fault handler. Then wire that
  protocol into sysvk (or drop the HAL and talk to libglcore the way NVIDIA's loader does).

**The remaining blocker (earlier analysis, kept for reference):**
The tegra HAL is *not loader-conformant*: its `GetInstanceProcAddr` requires a real instance
(dereferences `instance+8`, the hwvulkan_dispatch — `NULL` crashes), and the `hwvulkan_device_t`
ships with all three entrypoints NULL (its `ANDROID_RELA` section decodes to zero relocations; the
only constructor registers an atexit). NVIDIA's own loader uses the device-struct fields directly
for global-level queries, which is why stock Android works. Our patched sysvk answers NULL-instance
queries from the struct and fills the fields with the HAL's own implementations
(`EnumerateInstanceExtensionProperties=0x2060`, `CreateInstance=0x242c`,
`GetInstanceProcAddr=0x22f0`, firmware-stable offsets — see vkprobe.c).

With all of that in place, the driver's **`EnumerateInstanceExtensionProperties` still returns an
empty list in the guest** (`vulkaninfo` dies on its own zero-vector bug; vkprobe confirms count=0
via the struct field). The HAL's EGL bootstrap completes without logged errors, so something the
driver needs from its environment (binder service? surfaceflinger-side state? an Android-only
property?) is still missing — this is now inside NVIDIA's 2019-era driver and is the honest
frontier. Next hypotheses to test: run a *native* on-device Vulkan enumeration (bionic app, e.g.
`cmd gpu vkjson` if present) to confirm the driver enumerates on stock Android at all; strace-free
comparison of the bootstrap; or accept that this driver is out of reach for in-process Vulkan and
fall back to Option B (out-of-process GPU helper, which the working EGL path makes very feasible:
the helper owns EGL/GLES via the proven route and hands buffers to the guest).

## What is next

1. **Device run with patched bionic: PASSED (2026-10-25, session `2026-10-25-39-droiddeck-gpu-probe`).**
   The app now ships the whole probe stack (`usr/local/hybris/`: libhybris, `eglprobe`, and the
   Halium-patched bionic set, staged by `SessionFiles` and run via the new
   `usr/local/bin/droiddeck-gpu-probe` wrapper). Running it from the app session:

   ```
   stage: eglGetDisplay -> 0x1                      ← previously SIGSEGV'd here in stock bionic
   stage: EGL 1.5 vendor=Android version=1.5 Android META-EGL
   stage: pbuffer=0x… err 0x3000
   stage: context=0x… err 0x3000
   RESULT GL_RENDERER=NVIDIA Tegra
   RESULT GL_VENDOR=NVIDIA Corporation
   RESULT GL_VERSION=OpenGL ES 3.2 NVIDIA 495.00
   ```

   The full NVIDIA stack maps into the glibc probe process (`libEGL_tegra.so`, `libglcore.so`,
   `libGLESv2_tegra.so`, `gralloc.tegra.so`, `libnvidia-glvkspirv.so`). The TLS contract is
   satisfied by the patched bionic, first in `HYBRIS_LD_LIBRARY_PATH`.
2. **Known secondary issue (non-blocking): teardown fault.** After printing the results, the probe
   segfaults at cleanup: ROM `/system/lib64/libEGL.so` calls into **glibc** `libc.so.6` (+0x8a560),
   `addr=0x428` — a bionic/glibc symbol mixup on the teardown path only. Rendering, context and
   `eglMakeCurrent` all work; gamescope integration will not call `eglTerminate` (it keeps the
   context for the session lifetime). Root-cause later if it bites.
3. **Next step toward GPU for the guest:** point gamescope/zink at the hybris EGL device instead of
   lavapipe. The wrapper (`droiddeck-gpu-probe`) shows the exact env needed.
4. **Option B (out-of-process) stays the fallback** if in-process proving too brittle: a small
   bionic helper (NDK) owning EGL/GLES + gralloc handing buffers to the guest; waylandcomp already
   has an AHardwareBuffer import path (`ahb_swapchain.c`).

### Reproducing the probes on the device

```
dd run /usr/local/bin/droiddeck-gpu-probe     # starts a run-mode session under gamescope
# headless sessions suspend (no visible activity): resume as the app itself, then wait:
adb -s 192.168.0.194:5555 shell "run-as com.droiddeck.launcher sh -c \
  'am startservice --user 0 -n com.droiddeck.launcher/.session.SessionService -a com.droiddeck.launcher.RESUME_SESSION'"
adb -s 192.168.0.194:5555 shell "run-as com.droiddeck.launcher sh -c \
  'grep -E \"stage|RESULT|FAULT|vkstage|VK_\" files/logs/<session-id>/session.log'"
```

Everything up to the bionic TLS boundary — and now the boundary itself — is reproducible; the probe
is `tools/hybris/eglprobe.c` (stage prints + in-process SIGSEGV handler that prints the faulting
mapping), the wrapper is `tools/linuxfs/overlay/usr/local/bin/droiddeck-gpu-probe`, and the payload
staging is `tools/hybris/stage-payload.sh` (CI artifact → APK assets).

## Update 2026-10-26 (evening): Vulkan enumeration SOLVED

`vulkaninfo --summary` (guest Mesa loader 1.4.363 → sysvk ICD) lists
`GPU0: NVIDIA Tegra X1 (rev B) (nvgpu)`, api 1.1.178, driver 495.00
(session 2026-10-26-16). The chain, in order:

1. EGL init chain (display → pbuffer → context → MakeCurrent, `egl_warmup.h`)
   runs the `NvGlEglApiInit` sequence inside libglcore — this installs the
   dispatch master object (.bss 0x19c5640, 41 KB from `+0xf60200`) and the
   three allocator singletons (0x19c5a30/50/70). Before it, CreateInstance
   calls through NULL (master+0x140/+0x160). No shims needed.
2. `NvGlEglGetFunctions(1, &table[1])`, `table[0]`=dlopen handle,
   `table[4]`=GetProcAddressNV("vkGetInstanceProcAddr") (real driver GIPA,
   NULL-instance conformant).
3. sysvk (`tools/hybris/sysvk/sysvk.c`) exposes this as a standard ICD for the
   guest's Mesa loader; CreateInstance/DestroyInstance substitute glibc
   `VkAllocationCallbacks` (driver dereferences pAllocator).
4. Env contract: `HYBRIS_LD_LIBRARY_PATH` exported (patched bionic),
   `LD_LIBRARY_PATH` UNSET for vulkaninfo/gamescope (else the hybris vulkan
   wrapper hijacks libvulkan.so.1 and asserts in ws.c). sysvk preloads the
   EGL platform-plugin deps by absolute path so it works without
   LD_LIBRARY_PATH.

Open: device creation (`vkCreateDevice`) unexercised through sysvk; gamescope
on the Tegra is the next frontier (see handoff doc).
