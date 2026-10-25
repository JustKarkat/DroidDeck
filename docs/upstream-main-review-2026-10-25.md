# Upstream `main` review for the Shield TV port

Read-only analysis. No changes pulled or applied. Base of `shield-tv` is
`f635e924`; upstream `main` is `8cf2ffe2`, 1528 commits ahead. No upstream
commit mentions Tegra/Shield/lavapipe directly, so usefulness is indirect.

## Rebase risk

- gamescope base moved from the p3/p4 era to `gamescope-3.16.29-p5`
  (`33fe6edd` + patches through 0112). Shield-tv patches 0116–0123 must be
  rebased onto the newer base.
- Files shield-tv edited all changed upstream (`SessionActivity.kt`,
  `PadBridge.java`, `SessionFiles.kt`, `SessionPrefs.kt`,
  `droiddeck-session`), so expect conflicts.

## Promising commits

### Software WSI / gamescope / dmabuf (directly relevant to first-frame blocker)
- `49b6a073` Present games from the KDE Steam client through software WSI
- `6eda278d` Backport upstream gamescope fix for games left behind Steam's loading screen
- `93352da1` Pace gamescope's fixed-refresh frame limiter per window by time
- `d60f683b` Release cursor SHM buffers during long sessions
- `64273927` Refactor compositor: move zwp_linux_dmabuf_v1 into wl_dmabuf.c (dmabuf handling relevant to the missing-fence blocker)

### Steam client builds / ARM64
- `2eb18f55` Document which Steam client builds Valve's arm64 manifests carry
- `7aeb7779` Fall back to Valve's stable Steam client when the beta keeps failing
- `c866bb90` Share installed games between the arm64 and x86-64 Steam clients

### Controller / input (Shield TV's virtual-device problem)
- `e032f251` Ignore `uinput-*` system virtual input devices in controller detection (may simplify/replace shield-tv's analog-stick gamepad filter)
- `eb0eecda` Let a connected controller keep the first player slot
- `92dbd1cd` / `750780b7` Restore Factorio ARM64 controller input through readv

### Memory / session resilience (Shield: ~2.9 GB RAM, 512 MB swap)
- `2be08bc6` Adjustable RAM and VRAM caps for the container
- `a4eee5c2` Page session out to zram before suspending
- `4e00b403` / `53ac21b4` Start session even when agent env can't be prepared
- `4cbda4e5` Release microphone before freezing so the SoC can sleep

### CPU pinning
- `866551e7` / `c7bcd827` Stop pinning Steam to one core on Snapdragon 8 Gen 2 (worth testing on Tegra's 4 A57s)

## Not recommended to cherry-pick blindly
Adreno-specific commits (Turnip/kgsl/GPU stats) do not apply to Tegra and may
conflict. Rebase should be done as a whole, then re-apply shield-tv patches
0116–0123 and re-test the first-frame blocker.