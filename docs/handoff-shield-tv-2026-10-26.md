# ShieldDeck handoff — Shield TV GPU (libhybris/Vulkan), 2026-10-26

Companion to `docs/agent-playbook.md` (device access, build loop, logs, traps — read it first)
and `docs/hybris-gpu-status.md` (full evidence trail). This file is the working state + next steps.

## TL;DR

- **EGL/GLES on the Tegra from the guest: DONE and shipped in the app.** The probe prints
  `RESULT GL_RENDERER=NVIDIA Tegra` (session 2026-10-25-39). Do not redo: bionic TLS, q linker
  APS2, IFUNC, sphal/config — all solved, see status doc.
- **Vulkan (what Steam/gamescope actually render with): one bug away from enumeration.**
  The driver's `CreateInstance` calls libglcore singleton allocator objects whose function
  pointers are NULL in our process (NVIDIA's loader installs them on stock). The probe has
  logging/allocating shims for them ready and installed in the APK — **but a probe-ordering bug
  killed the process before the shims were ever exercised on device** (session 2026-10-26-10).
  Fix the ordering (one line), re-run, read the shim logs, wire real allocators, rewrite sysvk.

## Where things stand (all committed, HEAD `62bc6152`, pushed, CI green)

- App ships `usr/local/hybris/` = libhybris + `eglprobe` + `vkprobe` + Halium-patched bionic +
  **patched sysvk ICD** (`tools/hybris/sysvk/sysvk.c`, registered at
  `/usr/local/hybris/usr/share/vulkan/icd.d/sysvk_icd.json` for the guest's Mesa loader).
- The probe wrapper `droiddeck-gpu-probe` synthesizes `/linkerconfig/ld.config.txt` in the guest
  (the device's is root-only; the q linker needs it for the sphal namespace — without it every
  Vulkan path saw the loader's stub HAL).
- **Driver table protocol decoded** (this is the load-bearing discovery):
  `libglcore.so!NvGlEglGetFunctions(1, &table[1])` with `table[0]` = dlopen handle fills a
  48-byte table whose slot 1 is a `vkGetProcAddressNV(name)` getter. The driver requires
  `table[4] = GetProcAddressNV("vkGetInstanceProcAddr")` to be stored by the consumer.
  `gipa(NULL, …)` on the REAL driver gipa is conformant (the HAL wrapper at `base+0x22f0` is not —
  it dereferences the instance). `vkEnumerateInstanceExtensionProperties` via GetProcAddressNV
  works with NULL instance → 7 real extensions in the guest.
- `cmd gpu vkjson` on stock: driver is healthy (NVIDIA Tegra X1, 99 extensions, has
  `VK_KHR_external_memory_fd` + `VK_KHR_external_semaphore_fd` +
  `VK_ANDROID_external_memory_android_hardware_buffer`; **no** dma_buf, **no** timeline_semaphore
  — the gamescope integration must use the AHB path).

## The frontier: CreateInstance

- The tegra driver **requires a non-NULL `VkAllocationCallbacks`** (pfnAllocation): NULL falls
  back to an unset process-global inside libglcore (`+0x185b880`) → call through NULL. The probe
  passes glibc-backed callbacks now.
- Even with callbacks, CreateInstance calls **singleton allocator objects**:
  libglcore GOT `0x185b880/88/98` → `.bss 0x19c5a30/50/70` — function pointers NULL in our
  process. Nothing in libglcore's text writes them directly (checked: no adrp refs to those
  pages); NVIDIA's loader installs them on stock by some path we haven't traced.
- Session 2026-10-26-09: one singleton invoked as `(heap, rodata, 8, heap)` — not a plain
  alloc(size). Session 2026-10-26-10: `NvGlEglGetFunctions(2, …)` fills nothing (dead end);
  blind-calling the HAL's ensure helper `base+0x2160(0,NULL,0)` faults at NULL+0x30 (needs real
  args — and it ran *before* the allocator patching, aborting the process before the shims were
  ever exercised). **The always-allocate arg-dumping shims (commit `62bc6152`) are in the
  installed APK but untested on device.**

## Next steps (in order)

1. **Fix the ordering bug in `tools/hybris/vkprobe.c` main() gpa path**: move
   `patch_libglcore_allocators()` to right after `tegra_table_init()` (before the variant(2) and
   ensure blocks, which can fault). CI → stage (`tools/hybris/stage-payload.sh`) → build →
   install → `dd run /usr/local/bin/droiddeck-gpu-probe`.
2. **Read the shim logs** (`shimA/B/C(...)` + hexdumps): identify each singleton's calling
   convention, implement real glibc allocators to replace the shims.
3. If CreateInstance + `vkEnumeratePhysicalDevices` succeed in the probe → **rewrite sysvk** on
   the table protocol: init via NvGlEglGetFunctions (store table[4]!), install the real allocator
   singletons at ICD-open, answer global-level queries from GetProcAddressNV, instance-level from
   the real gipa. Then `vulkaninfo --summary` should list the Tegra (the wrapper already exports
   `VK_DRIVER_FILES` for the sysvk json).
4. **Gamescope on the Tegra**: `dd env set 'VK_DRIVER_FILES=/usr/local/hybris/usr/share/vulkan/icd.d/sysvk_icd.json'`
   (seeds next session) + select the ICD as graphics driver; gamescope's zink/CEF path comes
   after. Buffer handoff must go through AHB (no dma_buf ext). Watch for the loader's
   `VK_EXT_physical_device_drm` warning — gamescope checks it.
5. If the singleton story fights back: fallback plan unchanged — Option B, out-of-process EGL
   helper (the proven EGL path) handing buffers to the guest.

## Do NOT redo (all verified dead ends or solved)

Stock-bionic TLS (patched bionic shipped); o-linker/APS2 (halium-11.0 headers); linking the
hybris vulkan wrapper (IFUNC resolvers die in ld.so relocation — dlopen in main, always);
Android-loader discovery (stub HAL without sphal → synthesized ld.config.txt solves it);
`NvGlEglGetFunctions(2, …)` variant (empty); blind `base+0x2160(0,NULL,0)` (faults; needs args
derived from its real callers first); hwvulkan device-struct fields (NULL on stock too — the
loader never uses them); `open("vulkan")` id (must be `"vk0"`).

## Cheat sheet — tegra HAL / libglcore offsets (Shield firmware 33.01, driver 495.00)

| what | where |
|---|---|
| hwvulkan device struct | HAL vaddr `0x4448` (ships all-NULL) |
| HAL real EnumerateInstanceExtensionProperties | HAL `+0x2060` |
| HAL CreateInstance wrapper (takes table first) | HAL `+0x242c` |
| HAL GetInstanceProcAddr wrapper (needs instance) | HAL `+0x22f0` |
| HAL ensure/bootstrap helper (args unknown) | HAL `+0x2160` |
| libglcore exports | `NvGlEglGetFunctions`, `NvGlEglApiInit` |
| driver table | 48 B; `[0]`=dlopen handle, `[1]`=GetProcAddressNV, `[4]`=GIPA (consumer-stored) |
| singleton allocator objects | GOT `0x185b880/88/98` → bss `0x19c5a30/50/70` (NULL in guest) |

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
adb -s 192.168.0.194:5555 shell "run-as com.droiddeck.launcher sh -c 'grep -aE \"stage|RESULT|vkstage|shim|FAULT|vulkaninfo\" files/logs/<session-id>/session.log'"
```

Device state at handoff: idle, no session, `suspendPolicy.steam` = `manual`, latest APK
(commit `62bc6152` payload) installed. Untracked repo dirs (`.CleanupShield/`, `prepared-components/`,
`shield-probe.txt`, `stores-cargo-lock/`, `xbox-arm64/`) are pre-existing — do not delete.
