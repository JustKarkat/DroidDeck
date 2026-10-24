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

## Status (no-ADB pass, untested on hardware)

`shield-tv` now boots past every Adreno gate without a device in hand:

- Tegra detected as `Family.TEGRA` → `UNTESTED` (warns, never blocks install/launch).
- Compositor uses the system Nvidia Vulkan driver (no Turnip unpack on Tegra).
- Guest ICD prefers NVK → lavapipe/swrast → freedreno, so stock kernels land on software rendering instead of a dead freedreno ICD.
- NVK sandbox binds wired for custom kernels exposing `/dev/dri`.
- Leanback launcher + optional touchscreen so one APK covers phones and TV.
- New test in `DriverPairsTest.tegraIsDetectedAndWarnsInsteadOfBlocking`.

Expect on stock Shield: Steam boots under Zink-on-lavapipe (slow), games mostly unplayable until NVK + `/dev/dri` exists. A57 has no TSO/LSE — use a FEX STABILITY preset.

## Phase 0 probe — 2026-10-22 (read-only adb, shield-probe.txt)

Device: SHIELD Android TV, fingerprint `NVIDIA/mdarcy/…RQ1A.210105.003/7825230_4387.0822`, firmware built 2026-01-30. **VERIFIED**

| Question | Answer | Status |
|---|---|---|
| RAM | MemTotal 2.88 GiB (3 GB model), 512 MB swap | VERIFIED |
| Storage | /data 12 GB, 7.3 GB free | VERIFIED |
| OS | Android 11 / SDK 30, Shield Experience 9.x | VERIFIED |
| SoC | Tegra X1/X1+ family; 4× A57 (0xd07 only, no A53 shown); **no LSE atomics, no TSO** → FEX STABILITY/LL-SC preset required | VERIFIED |
| Kernel | **4.9.141-tegra** (brief's "3.10.96" is stale; doc's "4.9 downstream" confirmed) | VERIFIED |
| SELinux | Enforcing; core GPU nodes `/dev/nvmap`, `nvhost-as-gpu`, `nvhost-ctrl-gpu`, `nvhost-gpu`, `nvhost-tsg-gpu` are DAC world-RW and **open OK from shell domain while Enforcing**; `/dev/nvhost-ctxsw-gpu` (profiling) and `nvhost-power-gpu` root-only | VERIFIED |
| App-domain access | Regular apps render via `vulkan.tegra.so`, so untrusted_app almost certainly has gpu_device; final proof = first app-session log | UNVERIFIED |
| Vulkan driver | `vulkan.tegra.so` + libnvrm*/libnvos stack; feature flag `vulkan.version=1.1.0`, level=1, compute | VERIFIED |
| Vulkan limits | storageBuffer8BitAccess, timelineSemaphore, descriptorIndexing, maxMemoryAllocation | UNVERIFIED — needs a Vulkan Hardware Capability Viewer report from the box |
| /dev/dri | **absent** → NVK/DRM impossible on stock kernel | VERIFIED |
| CUDA | `libcuda.so` present in /vendor (Android-side; not needed for graphics) | VERIFIED |
| Bootloader | `verifiedbootstate=green` (locked), no `su` | VERIFIED |
| 2019 vs 2017 | mdarcy/darcy fingerprint ⇒ 2019 family; X1 vs X1+ (T210/T214) | UNVERIFIED (codename only) |

**Consequences for DroidDeck layers:** DXVK 1.10.x (needs Vulkan 1.1) structurally fits; DXVK 2.x (1.3) and VKD3D-Proton do not, on either the bionic driver or L4T r32.7 glibc blobs (UNVERIFIED per release notes, flag consistent with feature=1.1). NVK class (1.3) needs `/dev/dri` → custom kernel or native L4T boot.

**Route call: continue route A (in-app Tegra support), which is what this branch already sketched.** In-app path order: (1) install current APK — verify Tegra detection avoids all Adreno gates and the UI boots; (2) software rendering (lavapipe/zink) end-to-end with Steam; (3) user-supplied L4T r32.7 glibc Vulkan blobs loaded in the runtime against the world-RW nodes above — no root needed if app-domain access holds; (4) only if blob ABI mismatches: Venus-style forwarding (big effort) or native L4T boot (unlock wipes data — needs explicit user opt-in). Route C remains the faster ceiling (all 3 GB to the guest, no bionic host) but is a different product and requires the unlock step.

## Realignment — 2026-10-22 (user directive)

> "once you get this line you are working, lets realign and figure out how this helps us get a working build of droid deck (with steam and ability to play games using the onboard CPU & GPU). Just want to make sure we aren't chasing any unrelated paths."

Goal: a working shield-tv build — Steam launches, games run. Scope call:

- **Phase 1 (now): CPU-only via lavapipe.** Single blocker is gamescope dying at `vkAllocateMemory -2` → flat-texture assert, seconds after picking llvmpipe. Everything on-goal: reproducing/fixing that alloc, Steam boot, measuring FPS/RAM. Unrelated paths (host compositing experiments, Adreno-only tuning, new features) are out.
- **Phase 2 (deferred, documented): onboard GPU.** Stock 4.9 kernel has no `/dev/dri` → NVK impossible. `vulkan.tegra.so` is bionic-linked; using it from the glibc guest needs a bridge that is real R&D, not this milestone. Unlocked bootloader + custom kernel (nouveau/NVK) would unlock it but wipes data / needs explicit user opt-in. The fork stays driver-portable so Adreno devices get full GPU natively.
- Exit criterion for Phase 1: Steam UI up in DroidDeck on the Shield with lavapipe and a measured FPS/RAM number for one old game.

Session-crash facts so far (all VERIFIED on device): Guest environment is healthy — no rlimits, no cgroup limit, lvp heap 2.88 GiB free-budget, maxAllocationCount unlimited. Minimal python probes up to the full DEDICATED+Export-DMA_BUF+WSI chain all succeed; only gamescope's context fails. Prime suspect from source reading: `findMemoryType()` returns -1 (`memoryTypeBits==0` on the replication image) and gamescope casts it to `uint32_t`, so the alloc asks for an invalid typeBits → -2.

Bind this into future compaction summaries alongside the quoted directive above.
