# ShieldDeck — Nvidia Shield TV port

Fork of [DroidDeck](https://github.com/Droid-Deck/DroidDeck) (`upstream/main`) targeting Shield TV
(Tegra X1 `T210` / X1+ `T214`, Maxwell GM20B, Android 11 / Shield Experience 9, Leanback).

## Why it's not a rebuild

DroidDeck assumes Adreno + Turnip + Zink:

| Assumption (DroidDeck `main`) | Shield TV reality |
|---|---|
| `GpuInfo.kt`, `DeviceSupport.kt`: `/sys/class/kgsl/kgsl-3d0`, `vulkan.adreno.so` | No kgsl. GPU behind `/dev/nvhost-*`, `/dev/nvmap`, `vulkan.tegra.so` |
| `TurnipDriver.java` bundled `turnip-*.tzst`, `chooseByDevice()` on Adreno gen | Turnip never loads on Maxwell. Needs NVK (`libvulkan_nouveau.so`) or passthrough, else `llvmpipe` fallback |
| `LinuxVulkanDriverManager.java` `LIB_NAME = "libvulkan_freedreno.so"` | Must accept `libvulkan_nouveau.so` / `libvulkan_lvp.so` |
| `LinuxRuntime.java:vulkanIcd()` prefers `*freedreno*.json` | Must prefer `*nouveau*`, then `*lvp*`/`*swrast*` |
| `BwrapSpawner.kt:gpu()` gates on `/dev/kgsl-3d0` + binds `libvulkan_freedreno.so` | Gate on `/dev/dri/renderD*` or `/dev/nvhost-*`; bind nouveau + `libdisplay-info`, write `nouveau_icd.json` |
| `SessionService.kt:addClientEnvironment()`: `MESA_LOADER_DRIVER_OVERRIDE=zink`, `GALLIUM_DRIVER=zink`, `VK_ICD_FILENAMES=<freedreno>` | On NVK keep Zink-on-NVK; on stock kernel (no `/dev/dri`) drop the freedreno ICD and let `llvmpipe`/`zink+llvmpipe` run (slow, but boots) |
| `GpuStatsComponent.kt`, `GpuClockPin.kt`, `LinuxRuntime binds`: kgsl sysfs nodes | Guard with `File.exists()`; add tegra nodes (`/sys/class/devfreq/*.tegra*`, `nvhost` stats) later |
| `AndroidManifest.xml`: touch portrait, `sensorLandscape`, no Leanback | Add `android.software.leanback`, `LEANBACK_LAUNCHER` intent, `Banner`, D-pad focus handling |

## Kernel situation

- Linked `SHIELD-OS/kernel_nvidia_shield` is 3.10 nv-tegra (2015-era, foster). Modern Shield Experience 9 runs 4.9 downstream — neither has nouveau/`/dev/dri`.
- HW-accelerated path needs either a custom kernel with `CONFIG_DRM_NOUVEAU` + `/dev/dri/renderD128` (cf. XDA L4T-Ubuntu thread, Jetson r34.1 docs) or a new `nvtools`-style shim passing the host Nvidia Vulkan driver into the guest (same role as `adrenotools/`).
- Lazy order: (1) probe stock box, (2) boot on `llvmpipe`, (3) NVK behind custom kernel, (4) shim only if NVK stalls.

## Branch layout

- `main` tracks `upstream/main` (DroidDeck). Never commit here.
- `shield-tv` is the port. Rebase onto upstream when it moves: `git fetch upstream && git rebase upstream/main`.

## Next step

Run `tools/shield-probe.sh` against the Shield over adb and paste the output into an issue. Everything after that depends on what the box reports (`/dev/dri` present or not, Vulkan driver name, `ro.board.platform`).
