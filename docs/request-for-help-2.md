# Request for help #2 — Shield TV: game's last mile is gamescope's output leg vs lavapipe's non-exportable memory (pick + spec the fix)

Written 2026-10-24 ~00:45 CDT. See `docs/request-for-help.md` (#1) for context. **Research only.** Device still ADB `192.168.0.194:5555` (`run-as com.droiddeck.launcher` works).

## New verified facts this session (supersede #1's priorities)

1. **Renderer crash loop was a red herring for the top wall.** Real wall, verified twice (steam + desktop session):
   `[gamescope] [Info] vulkan: 0122-fix: dma-buf export supported=0 (features=4)` — the pushed lavapipe `libvulkan_lvp.so` (14 MB, api_version 1.4.0) reports `exportableTypes=0` / `supported=0` for `VK_KHR_external_memory_dma_buf`.
   Consequences verified in logs of every session since 0122: `wlserver: [types/wlr_drm.c:211] Failed to get DRM FD from renderer`, app compositor stats stay `0 frames on screen (0.0 fps) | 0 windows open`, app phase never leaves STARTING_*.
2. ALSO seen: `memoryTypes=1 memoryHeaps=1`, `memoryType[0] propertyFlags=0xf` (one type, all flags incl. DEVICE_LOCAL). `physical device does not support DRM format modifiers`, no `VK_EXT_physical_device_drm`. `0122-fix` already gates the export request, so gamescope stays alive but has nothing to present.
3. **Steam's gamepad-UI (BPM) window creation worked once** (created 1280×800, WasHidden 0, SteamApp Init + OOBE loaded) — the unmapped-window churn is downstream of the renderer exit loop, and that loop may be CPU thrash (WebUITransport 11.6 s on 2nd pass).
4. Desktop mode = KDE Plasma Session (`startplasma-wayland`, `kwin_wayland`, `plasmashell` — Plasma 6, `mode=lxqt` label). **It also boots a nested gamescope** (`gamescope version 3.16.29+ gcc 16.1.1` in on-screen overlay, `fps=0`), and KWin with an internal Xwayland run fine. Steam desktop login window maps and stays viewable on the first X server. So **every** session type's last mile is gamescope's own output leg — an alternative shell path does not exist on this build.
5. Rootfs also ships `libvulkan_freedreno.so` (irrelevant on Tegra) and a generic `libvulkan.so` loader. Mesa version string of the lvp build was not extractable by `strings` — **know how?** see Q6.
6. Storage: device partition is 12 GB and was full (desktop install died `ENOSPC`); I deleted `steamapps/common/Proton Experimental (ARM64)` (~2 GB, `appmanifest_4427310.acf`) to make room — needs redownload before any Windows-game test.
7. The app-side compositor (C++ `waylandcomp`, srcs in `app/src/main/cpp/waylandcomp/src/`) imports dmabuf into its own VkImages (`vk_present.c: vkp_image_from_dmabuf`) and **already supports wl_shm surfaces** (`compositor.c: take_shm` → `vkp_image_create_shm`/`upload_shm`). Zero-copy AHB layer spike doc lives next to it (`ZERO_COPY_SPIKE.md`).

## The decision to research

Two candidate root fixes for the last mile on a CPU-only device:

- **(A) Patch lavapipe (Mesa lvp) so allocations with `externalMemoryFeatures=export` actually allocate from `/dev/dma_heap/system` (Android 11 Shield has dma_heap) and export that fd.** Everything downstream (gamescope, app compositor, zero-copy blit) then works unchanged; keeps the gamescope product shape; helps every CPU-only Android vendor. Suspicion: kernel 4.9.148 on the Shield targets Android 9-style ION first — check whether `/dev/dma_heap/system` truly exists on this build (`RQ1A.210105.003` = Android 11). If only ION exists (`/dev/ion`), adapt the patch to ION's legacy `ION_IOC_ALLOC` (ion_droid `heelix-idea`).
- **(B) Patch gamescope's nested wayland backend to publish its output surface via `wl_shm`** (CPU copy from its llvmpipe textures into a wl_shm_pool buffer). The app compositor already handles that. Slower (one full memcpy per frame) but driver-agnostic and small blast radius; llvmpipe allocations are host memory so the copy is a `vdpsync`/map+memcpy, no extra GPU work.

Also as a third (less preferred): **(C) substitute a different ICD that has true DMA_BUF export** — e.g. an aarch64 build of SwiftShader Vulkan (does Chromium's swiftshader ship external-memory-dma-buf?), or Mesa lvp built with the Mozzila `*vk-sw-device*` WSI work (Mesa's `VkSwDevice` gained `linux-dmabuf` shmem802 feedback support in 24.x?). If a prebuilt aarch64 ICD with export exists, that's the lazy unblock: push it like we pushed lvp and pick it via `LinuxRuntime.vulkanIcd()` name-match.

## Questions (research, don't modify anything)

1. (A) feasibility: in current Mesa lavapipe, where does `lvp_GetPhysicalDeviceMemoryProperties` / `lvp_AllocateMemory` get its backing memory, and what does it actually return today for `vkAllocateMemory` with `VkExportMemoryAllocateInfo(DMA_BUF)` in the pNext chain (`lvp_swapchain`, ram?). Does lvp claim `VK_KHR_external_memory_fd` / `externalMemoryFeatures` honestly because of a `sw_device` allocator that has no heap fd - implying a ~300-line patch is needed in `src/gallium/frontends/lavapipe` — which functions?
2. (A) kernel check: does Android 11's Tegra 4.9 kernel on the SHIELD expose `/dev/dma_heap/system`? If not, is there a known-good ION fallback pattern Mesa/Shield uses? (Check nvidia's 4.9 kernel ION API surface on this mdarcy build.)
3. (B) exact places: in ** gamescope 3.16.29's `src/backends/wayland_backend.cpp` (or whatever file the `xdg_backend`/nested wl backend lives in), which routine creates the PRESENTED output buffer (the one attached to `wl_surface`) — does it call `CVulkanDevice::AllocMemory` with DMA_BUF export and then `zwp_linux_dmabuf`? What would a wl_shm fallback patch need to touch (buffer pool, mmap of the vkImage via `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT` + `vkMapMemory`, format conversion jdx?) — outline the patch structure with function names + line anchors if you have the source.
4. (C) does any prebuilt aarch64 Linux ICD with real `VK_EXT_external_memory_dma_buf` exists (SwiftShader, Zink-llvmpipe-on-GL, or Mesa `libgallium_hardware.so`? — specifically Zink on lvp-GL with gbm-free... anything known, please list with URLs to binaries or build instructions (e.g., Arch linux `mesa` package build inside `menci/archlinuxarm:base-devel` like `tools/gamescope/build-in-arch.sh` does).
5. The app-side import leg — whose driver do the imported dma-bufs land IN on the Shield (the soaring `compositor.start driver=null` in `files/driver-setup.txt` is worrying: "driver=null") — investigate `vk_loader.c`/`SessionService.kt` (`SessionFiles.kt:117-124`) to see what the app picks on Shield and whether it'd even have a driver able to import `/dev/dma_heap` fds. This might kill option (A) — if the *host* has no driver that can import, (B) or (C) wins.
6. How to get the exact Mesa version of a stripped `libvulkan_lvp.so` push (which symbol/string holds it) so we know which lvp codebase the patch would apply to. The file was stolen by 14,768,576 bytes; `strings` only showed `MESA_VK_*` and LLVM symbols — maybe `getenv("MESA_...")` for `MESA_VERSION`? Also tell me what to grep in the guest (`lvp_...` debug markers) to find the precise mesa commit ("Mesa 24.x" vs `main` from `droiddeck-components` release).
7. Quick win sanity check: on upstream Mesa, is there a `LIBGL_ALWAYS_SOFTWARE`-style env that makes **Mesa's zink-WGL` go via `vk_sw_device` plus `linux-dmabuf` with the lowest-effort change, vs the lvp path? (Is there a sh середине `MESA_VK_WSI_PRESENT_MODE=shm` or similar actually supported?)

## How to answer

Drop notes to `docs/request-for-help-2-replies.md`. If you have concrete patch outlines (function names + upstream 3.16.29/Mesa commit anchors), say so plainly — I'll verify applying with `patch --dry-run` before any CI run. If you get binaries for (C), give sha256.

## Constraints (same as #1)

- GPL-3.0 obligation (DroidDeck-derived).
- No flashing/bootloader/NVIDIA-blob bundling.
- Device is production-Keys `RQ1A.210105.003` so no adb root; only `pm` + `run-as` exec paths.
- CI/gamescope build flow is the fork one from #1 (`JustKarkat/DroidDeck`, workflow `build-gamescope.yml`, archlinuxarm builder) — any (A) Mesa patch needs its own archlinuxarm CI recipe similar to that (a Mesa build with only lavapipe + swrast would be ~20-30 min on ubuntu-24.04-arm).
