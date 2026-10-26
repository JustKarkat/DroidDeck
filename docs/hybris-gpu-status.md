# GPU for the guest — libhybris status (2026-10-25)

Short version: the Shield's GPU is reachable from the proot guest, libhybris builds and loads the
whole bionic stack, but **stock Android 11 bionic fails libhybris' TLS contract**, so `eglGetDisplay`
segfaults inside `libc.so`. The missing piece is a patched bionic (`hybris-hal`), not a flag.

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
  `HYBRIS_LD_LIBRARY_PATH=/system/lib64:/vendor/lib64:/vendor/lib64/egl:/apex/com.android.runtime/lib64/bionic`.

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

## What is needed next

- **Option A (in-process, the upstream-supported route):** obtain or build an arm64 API-30 bionic
  with `halium-11.0` hybris patches (`libc.so`, `libm.so`, `libdl.so`, `libc++.so`, … — the
  "hybris-hal" set), place it in the guest, and put it **first** in `HYBRIS_LD_LIBRARY_PATH`.
  Building AOSP 11 bionic is a full platform build; lifting the libs from a public Halium-11 /
  Droidian / UBports arm64 image (`/usr/libexec/droid-hybris/system/lib64`) is the cheaper source.
- **Option B (out-of-process):** a small bionic helper (NDK) that owns EGL/GLES + gralloc and
  hands buffers to the guest. No ROM patching, but needs a buffer/IPC protocol; waylandcomp already
  has an AHardwareBuffer import path (`ahb_swapchain.c`).

Everything up to the bionic TLS boundary is in place and reproducible; the probe is
`tools/hybris/eglprobe.c` (stage prints + in-process SIGSEGV handler that prints the faulting
mapping), the runner is the same `Build libhybris probe` workflow.
