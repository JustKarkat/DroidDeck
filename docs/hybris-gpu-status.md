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

## What is next

1. **Device run with patched bionic (ready to go).** Pull the new `hybris-probe` artifact from the
   `Build libhybris probe` workflow, extract over `/usr/local/hybris` in the guest (bionic set lands
   in `/usr/local/hybris/bionic`), and re-run `eglprobe` with the updated `HYBRIS_LD_LIBRARY_PATH`
   above. Expected: `eglGetDisplay` gets past bionic init; the TLS segfault inside `libc.so` is the
   thing to watch disappear (probe prints the faulting mapping if it doesn't).
2. **If vendor/system libs still fault on TLS** (they read `tpidr_el0` inline and are unpatched):
   escalate to the Droidian model — serve the *whole* GSI `/system/lib64` in
   `HYBRIS_LD_LIBRARY_PATH` (keep the Shield's `/vendor/lib64`), i.e. patched system tree + Shield
   vendor blobs. The image already contains it; only packaging changes.
3. **Option B (out-of-process) stays the fallback** if in-process proving too brittle: a small
   bionic helper (NDK) owning EGL/GLES + gralloc handing buffers to the guest; waylandcomp already
   has an AHardwareBuffer import path (`ahb_swapchain.c`).

Everything up to the bionic TLS boundary is in place and reproducible; the probe is
`tools/hybris/eglprobe.c` (stage prints + in-process SIGSEGV handler that prints the faulting
mapping), the runner is the same `Build libhybris probe` workflow.
