# Shield TV (SteamDeck) — handoff, 2026-10-25

Supersedes `docs/continue-handoff.md` (2026-10-23). Everything below is what this session
actually verified from device logs, CI runs or source — stated as such; open questions are
flagged.

---

## 1. Where the code is

- Repo: `/home/joshuashearussell/Documents/Experiments/ShieldDeck`, fork `fork` =
  `https://github.com/JustKarkat/DroidDeck.git`, branch **`shield-tv`**.
- HEAD: **`7025be2`** — pushed, CI green (workflow `Build gamescope (patched)`, artifact
  `gamescope-patched`, ~3 min).
- Only CI workflow that ever runs on this branch is the gamescope builder. The APK is built
  locally (`./gradlew :app:assembleRelease`).

### Commits added this session

| Commit | What |
|---|---|
| `bac4f6d` | 0123: replace `CreateShmBuffer()` (mkostemp in `$XDG_RUNTIME_DIR`) with raw `memfd_create` + `ftruncate` + `mmap`, with `errno` logging. The mkostemp path was failing on the prooted guest. |
| `73e656e` | 0123: force `flags.bMappable = true` (LINEAR tiling) on **every flippable texture** when `!g_bDmaBufExportSupported`. Output images created for the nested-Wayland backend had no CPU mapping, so the shm fallback had `rowPitch()==0` and `mmap(0)` returned EINVAL. |
| `191e607`…`7025be2` | Temporary 0123-diag instrumentation: log first calls of `CWaylandPlane::Commit/Present`, `CWaylandConnector::Present`, `paint_all` entry, plus a steamcompmgr main-loop heartbeat incl. `hasRepaint`. **Remove before release.** |
| `ee45414` (contains the compositor fix, swept in by `git add -u`) | `app/src/main/cpp/waylandcomp/src/vk_present.c`: filter the device-extension list to what the driver actually supports before `vkCreateDevice`; `VK_KHR_swapchain` stays required, everything else optional. Plus two diag prints in `compositor.c` (`xdg_toplevel created`, `shm import failed`). |

`git status` is clean; untracked dirs (`.CleanupShield/`, `xbox-arm64/`, `prepared-components/`,
`docs/request-for-help*.md`, `docs/ExternalAgentResponses/`, `stores-cargo-lock/`,
`shield-probe.txt`, `ShieldDeck.png`) are pre-existing, leave them.

---

## 2. What works now (verified)

1. **Gamescope renders and presents** under lavapipe on the Shield — `0123-shm` allocates and
   maps its output fbs, e.g. `0123-shm: allocating 1280x720 shm buffer, size=3686400 stride=5120`
   → `shm fb 1280x720 stride 5120`.
2. **The DroidDeck compositor (waylandcomp) initializes Vulkan on Tegra.** Before the
   `vk_present.c` fix it died with `present: vkCreateDevice failed` because it passed
   `VK_EXT_external_memory_dma_buf` and `VK_EXT_image_drm_format_modifier` (absent on the
   nvgpu system driver) to `vkCreateDevice`. After the fix:
   `screen output 1920x1080, 3 buffers, vsync`, and the stats line reports frames on screen
   (`… 7 frames on screen (0.7 fps) | … (copy 7 …) | present 0.68/1.00 ms (7)`).
3. **End-to-end commit pipeline works for a plain nested client.** `droiddeckctl run glxgears`
   under gamescope: the X11 window shows `map: viewable`, wmState `normal` in
   `droiddeckctl focus`, and the nested app compositor receives and copies frames
   (`copy N`, `present … (N)`). So the chain gamescope→waylandcomp→screen surface is not
   fundamentally broken.
4. **steamcompmgr's main loop is alive**: heartbeat logs `main loop iter=…` continuously
   (~60/s, reached iter 90000) with `repaint=0` in steady state.
5. The old "7 failed CI runs then green" history from the previous session checks out: those
   were force-push iterations of 0123; `0c577cd` and the final commits are green.

---

## 3. Current blocker: Steam never produces its first frame

### Symptoms (all from the latest `steam` sessions, e.g. `2026-10-24-15-steam`)

- Steam's panorama window exists on Xwayland `:2` (`class: steam`, 1280x800, pid = steam) but
  stays **`map: unmapped`** in `droiddeckctl focus`. No mapped window ⇒ no commit ⇒
  `paint_all()` hits `if ( !bValidContents ) return;` (0 layers) ⇒ the output surface never
  commits ⇒ waylandcomp reports `0 windows open` ⇒ the app's "Starting Steam" overlay stays up
  forever. (The 0123-diag `paint_all` line sits *after* that early return, so its absence means
  "no layers", not "function never called".)
- `~/.local/share/Steam/logs/steamui.txt`: `Warning: SteamUI thread frame stalled for: 5–28 s`
  repeatedly, every few seconds.
- `cef_log.txt`: `sad_tab.cc: Tab Killed`, `Network service crashed, restarting service`, and
  per-renderer `crash_reporting enabled for process: renderer`.
- `webhelper_js.txt`: `SteamUI: INFO: setting locale: en` re-appears every ~20–40 s ⇒ the web
  helper's renderers are respawning in a loop.
- `/tmp/dumps` (guest: `files/linuxfs/tmp/dumps`) contains only `settings.dat`; **no minidumps**
  for the deaths ⇒ the processes are not dying by signal; that pattern fits Android's LMKD /
  clean kill rather than segfaults.
- Device memory is the constraint: 3 GB total, `MemAvailable` 400–700 MB; `gamescope` alone is
  ~950–970 MB RSS; logcat `ActivityManager` shows continuous `Process … has died` and
  `Force removing ActivityRecord{… com.droiddeck.launcher/.SessionActivity …}: app died`.

### Experiment run: Chromium renderer diet (negative)

`BL_WEBHELPER_ARGS="--no-zygote --in-process-gpu -AudioServiceOutOfProcess
--renderer-process-limit=1 --js-flags=--max-old-space-size=256"` was applied for session
`2026-10-24-20-steam`; `/proc/<webhelper pid>/cmdline` confirms the flags reached the browser
process. Result: **no change** — still `Waiting for Steam's first frame`, wmhelper windows
`unmapped`, `droiddeck-steam-compat: waiting for the client (Runtime.addBinding: Target
crashed)` continues. `am kill-all` afterwards freed nothing meaningful (MemAvailable ~380 MB
with the session up vs ~700 MB idle) and did not unblock it.

Notes:
- Steam's gamepadui is **CEF-rendered** (`steamui_html.txt`: `BrowserReady`,
  `GetDesiredSteamUIWindows`), so the webhelper renderer crash loop is directly the reason the
  panorama window never maps — not merely an OOM side-effect.
- `crashpad-handler` is running with `--database=/tmp/dumps`, yet still **no minidumps** appear
  for the renderer deaths. `steamwebhelper.log` shows `ERROR:check.cc(376)] Check failed: false.
  NOTREACHED log messages are omitted in official builds.` — a deliberate Chromium CHECK abort,
  which is a plausible source of the restarts (not a segfault).
- Bug in `tools/linuxfs/preload/webhelper.c`: tokens from `BL_WEBHELPER_ARGS` that start with a
  single `-` are dropped, so the built-in default `-AudioServiceOutOfProcess` never takes
  effect either. Only `--flags`, `+Feature` and `-Feature` tokens survive.

### Not yet proven

- Why gamescope sits at ~950 MB (single largest consumer; worth its own look).
- Why glxgears stopped being copied after ~5–7 frames in waylandcomp stats even though the
  app ran at ~350 fps (screenshots stayed static). Possibly the same repaint-gating semantics,
  not confirmed.
- Which Chromium CHECK is firing (needs a debug build of the webhelper, or binary-swap of
  `steamwebhelper`; the client files are Valve's, so this may only be diagnosable by bisecting
  args/features).

---

## 4. Runbook

Device: `192.168.0.194:5555` (network ADB; if `device offline`/`No route to host`, `adb connect`
again — the user may need to approve network debugging on the TV).

### Build/deploy loop (gamescope change)

```sh
# 1. edit /tmp/opencode/patchgen/b/src/... (a/ = pristine), then regenerate:
cd /tmp/opencode/patchgen
{ for f in src/backend.h src/Backends/WaylandBackend.cpp src/rendervulkan.cpp \
          src/rendervulkan.hpp src/steamcompmgr.cpp; do
    diff -u a/$f b/$f --label a/$f --label b/$f || true; done; } \
  > ~/Documents/Experiments/ShieldDeck/tools/gamescope/patches/0123-shm-output-fallback.patch
# 2. commit + push fork shield-tv, wait ~3 min, then:
gh run list -R JustKarkat/DroidDeck --limit 1
gh run download <run-id> -R JustKarkat/DroidDeck -n gamescope-patched -D /tmp/opencode/gs
cd /tmp/opencode/gs && tar --zstd -xf gamescope.tzst
cp usr/local/bin/gamescope \
   ~/Documents/Experiments/ShieldDeck/app/src/main/assets/linuxfs/usr/local/bin/gamescope
# 3. refresh the runtime input manifest (REQUIRED, else verifyRuntimeInputs fails):
cd ~/Documents/Experiments/ShieldDeck
python3 tools/release/runtime_inputs.py record
./gradlew :app:assembleRelease -q
adb -s 192.168.0.194:5555 install -r app/build/outputs/apk/release/app-release.apk
adb shell am force-stop com.droiddeck.launcher
python3 tools/droiddeckctl --serial 192.168.0.194:5555 start steam --timeout 30
```

### Reading logs

```sh
adb -s 192.168.0.194:5555 shell 'run-as com.droiddeck.launcher cat files/logs/<session>/session.log'
# also: wayland.log (app compositor), events.jsonl, app.log
# guest Steam logs: files/linuxfs/root/.local/share/Steam/logs/ (session.log, steamui*.txt, cef_log.txt, webhelper*.txt)
```

`droiddeckctl state`, `focus` (X windows with map state), `screenshot <path>`, `guest <argv>`
(run inside the session; use `guest bash -c "…"`; `--stdin` passes a *path string*, so push
scripts with base64).

### Live env overrides (no rebuild)

`droiddeckctl env set KEY=VALUE` seeds the next session (shown as `pending` in `env list`).
The session script also sources `/tmp/droiddeck-diag.env` inside the guest if present (write
`files/linuxfs/tmp/droiddeck-diag.env` via `run-as`; it survives a restart).

`BL_WEBHELPER_ARGS` is the supported Chrome-argument hook: `tools/linuxfs/preload/webhelper.c`
rewrites `steamwebhelper` argv from that variable (supports `+Feature` / `-Feature` tokens);
default is `--no-zygote --in-process-gpu -AudioServiceOutOfProcess`, `stock` disables the
rewrite. Caveat: the parser only appends tokens starting with `--`, so the default's
`-AudioServiceOutOfProcess` is dropped; single-dash flags must be written as `--`-prefixed or
as `-Feature` entries to have any effect.

### gdbserver

`gdbserver` exists in the guest (`/usr/local/bin/gdbserver`); `gdbserver --attach 127.0.0.1:4920 <pid>`
+ `adb forward tcp:4920 tcp:4920` works, but **the CI binary is stripped**
(`strip --strip-unneeded` at the end of `tools/gamescope/build-in-arch.sh`) so you get no
backtraces. To debug: drop the strip (and add `-g`) there, rebuild, and point gdb at the
matching binary plus `/tmp/opencode/hsgb/sysroot`.

---

## 5. Next steps (ranked)

1. **Attack the Chromium CHECK abort — this is the blocker.** Steam's gamepadui is CEF-rendered,
   and the webhelper renderers restart because of a deliberate `CHECK(false)`
   (`steamwebhelper.log`: `check.cc(376)`), not an OOM kill. Cheap probes via the env hook,
   one feature at a time, watching for that line to stop:
   - `BL_WEBHELPER_ARGS="--no-zygote --in-process-gpu --disable-features=NetworkService"`
   - …`-VaapiVideoDecoder`, `-VizDisplayCompositor`, `-UseChromeOSDirectVideoDecoder`
   - `--disable-dev-shm-usage`, `--disable-gpu-memory-buffer-video-frames`
   The hook already works (verified in `/proc/<pid>/cmdline`); note `webhelper.c` drops
   single-dash tokens, so only `--flags` / `+Feature` / `-Feature` reach the process.
2. **Chromium renderer diet** (no effect so far, keep while bisecting):
   `--renderer-process-limit=1 --js-flags=--max-old-space-size=256`.
3. **Cut gamescope RSS (~950 MB).** Check whether it is llvmpipe texture caching, the 3 output
   images, or per-frame fb recreation; `LP_NUM_THREADS=2` is already set. Consider dropping the
   look-ahead image count on the nested backend.
4. **Verify the Steam E2E result** with `droiddeckctl focus` + screenshot, then:
   - remove 0117–0121 diag patches and the `0123-diag` logging (keep 0116/0122/0123),
   - add the missing `PATCHES.md` entries for 0116 and 0122,
   - remove the unused `copyImageToBuffer` helper added by 0123 (dead code; `FlushShm` memcpys).
5. Only after the UI is up: install/launch a small game through `droiddeck-proton-arm64` and
   measure FPS.

---

## 6. UI note (not code)

The on-screen session-log overlay was temporarily disabled for clean screenshots by editing
`/data/data/com.droiddeck.launcher/shared_prefs/session.xml`
(`<boolean name="loadingLog" value="false" />`); it has been **restored to `true`** and verified
back on screen. The in-app toggle is Settings → Diagnostics → loading log. If it ever
disappears again, check that pref first.

---

## 7. Traps that cost time

- `diff -u` exits 1 when files differ — never chain the patch regen with `&&`; use `;` (or
  append `|| true` inside the loop).
- `git add -u` swept the compositor fix into the gamescope diag commit (`ee45414`). After
  committing, `git show --stat` to confirm what actually went in.
- If the gradle build fails, a following `adb install` silently installs the **previous** APK —
  always check the APK mtime.
- `droiddeckctl start steam`'s first state call often times out (`ADB_FAILED`, 30 s) while the
  session is fine; check `files/logs/` for the new session dir instead of retrying blindly.
- `adb` drops occasionally; `adb connect` recovers. The user may have to approve network
  debugging on the TV.
- `WAYLAND_DEBUG=1` set through the diag env produced no output in `session.log` (gamescope's
  libwayland is likely built without debug logging) — don't rely on it.
- Don't build guest binaries with the NDK: the guest rootfs is glibc, NDK produces bionic
  binaries. The guest has python3.14 (no compiler); push scripts with base64 over
  `droiddeckctl guest bash -c`.

---

## 8. Follow-up session 2026-10-24 22:30–23:55 — the renderer kill is root-caused

Supersedes §3's "current blocker" paragraph. Everything here is from device logs this session.

### 8.1 On-screen log

The overlay was off because `session.xml` had **no `loadingLog` key at all** (not `false`): the pref
had been rewritten without it and `SessionPrefs.loadingLogEnabled` defaults to false. Writing
`<boolean name="loadingLog" value="true" />` into
`files/shared_prefs/session.xml` with the app stopped and then starting a session restored it
(verified on screen). The in-app toggle is still Settings → Diagnostics → loading log.

### 8.2 Steam renderer deaths = LMKD, not a crash, not proot

Verified with a 0.25 s in-guest sampler (`/tmp/memwatch.py`, same script as request-for-help-3 §4.1)
and `/proc/<pid>/oom_score_adj`:

- Chromium sets its own CEF children to **`oom_score_adj=300`** (renderers) and **200**
  (network/storage utility). gamescope, the browser process and the launcher are `0`.
- Android's **lmkd** picks by `oom_score_adj`, so under memory pressure it kills the Steam UI
  renderer first. Evidence: each renderer death is within the same second as an `am_proc_died`
  storm for unrelated apps (`com.google.android.gms` adj 900, tvlauncher 150, inputmethod 200 …),
  and `lowmemorykiller` logs at the same timestamps. Deaths happened with 300–575 MB
  `MemAvailable` and renderer RSS 38–199 MB, i.e. never the biggest process — that is exactly what
  adj-first selection predicts. gamescope is 900 MB and was never touched.
- `cef_log.txt` order is `Network service crashed` → `sad_tab.cc Tab Killed` for every tab: the
  browser's normal recovery after its utility process (adj 200) is killed; it is not a separate
  crash. `bad IPC message` count is 0. There are no minidumps (SIGKILL cannot produce one), and
  vmodule (`--vmodule=render_process_host_impl=1,…`) added no reason text.
- The kernel cannot dump dmesg (no root) and lmkd only reports registered apps, but the adj
  correlation is decisive on its own.

**Fix (in tree):** `tools/linuxfs/overlay/usr/local/bin/droiddeck-oom-guard`, a python3 loop that
puts every `steamwebhelper … --type=*` child back at adj 0 once a second; started from
`droiddeck-session` after `start_agent`; staged by `SessionFiles.kt` (`files` list). The session is
the foreground experience, so LMKD now reaps background Android apps first.

Verified on sessions `2026-10-24-24-steam` and `-25-steam`: **0 `Tab Killed` in 3.5 min**
(baseline ~1 per 20–40 s), Steam's real window reached `map: viewable` on `:2` (first time), and
CDP `targets` showed `Steam Big Picture Mode` / `MainMenu_uid2` / `SharedJSContext` at
`steamloopback.host/routes/oobe/1` instead of only `http://error/` pages.

### 8.3 Next blocker (unchanged phase, new symptom)

The app still reports `firstFrame: false`, `phase: STARTING_STEAM`, `steamUi: null`, so the
loading overlay stays up, and gamescope still logs `0123-diag: main loop iter=… repaint=0` with the
`paint_all` diag never reached (0 hits). The app compositor (`wayland.log`) stops at
`0 windows open` / ~0.5 fps. So the window is mapped but steamcompmgr never gets damage/valid
contents for it. Next: check damage delivery for the now-mapped `:2` window (and whether Steam is
actually painting — the page was still `about:blank` at the time), before touching the compositor.

### 8.4 Notes

- Proot's fast path returns `ENOSYS` for `statx` on this 4.9 kernel (coreutils `stat` fails;
  `ls`/write work). Chromium's sandbox already treats `statx` ENOSYS as normal, so it is not the
  renderer killer — see request-for-help-3 and its reply. `python3 -c 'os.stat'` works for probes.
- `fgets`/`strace` inside the guest: proot already ptrace-attaches every tracee, so a second tracer
  gets no events. Use a sampler, not strace.
- Keep the sampler cheap: the first guard version forked `pgrep`+`cat` every second inside proot,
  which is visible in logcat and makes a thrashing device worse; the python version scans
  `/proc` in-process.
- `droiddeckctl stop`/`focus` can time out (ADB_FAILED) when the device is swapping; the session is
  usually fine, check `files/logs/<session>/`. `am force-stop com.droiddeck.launcher` is the
  reliable fallback.

### 8.5 Post-reboot measurements (2026-10-25 10:00–10:50, sessions -02…-05)

- The oom-guard fix survives reboots: every session since has 0 `Tab Killed` and all CEF
  children at adj 0.
- Storage: `/data` was at 98% (268 MB free). Removed `Steam/package/tmp` (stale client-update
  staging from Oct 23, 1.4 GB) → 1.7 GB free. Biggest remaining: `linuxfs/usr` 3.8 GB,
  `linuxfs/root` 3.6 GB (Steam `package` 1.1 GB of cached zips, `steamrtarm64` 686 MB), logs 116 MB.
- Steam's window is a normal managed toplevel: `WM_STATE Normal`, `_NET_WM_NAME "Steam Big Picture
  Mode"`, `STEAM_GAME=769`, `_NET_ACTIVE_WINDOW` points at it, `GAMESCOPE_FOCUSABLE_WINDOWS` lists
  it — but `GAMESCOPE_FOCUSED_WINDOW` and `GAMESCOPECTRL_BASELAYER_WINDOW` are **empty/0**, and
  steamcompmgr still logs `repaint=0` with 0 `paint_all`/`Commit surface` hits. So no damage ever
  arrives from the Steam window.
- The cause looks like starvation, not a compositor bug: `top -b -n2` during the session showed
  ~4 cores saturated — CEF browser 92%, `com.droiddeck.launcher` 73%, `kswapd0` 62%, `zygote64`
  49%, `droiddeck-steam-games probe` 32%, network service 31%, system_server 31% — with swap
  524/524 MB used and ~20 MB free. The Steam CDP port is bound (41089) but returns an empty reply
  through `adb forward`, i.e. the browser main thread is too starved to serve DevTools.
- gamescope is ~890 MB of anonymous memory in ~14 × 64 MB chunks. Not glibc arenas: exporting
  `MALLOC_ARENA_MAX=2` and `LP_NUM_THREADS=1` in `/tmp/droiddeck-diag.env` changed nothing
  (912 MB). Not the 0123 shm buffers (RssShmem only 27 MB), and the logged Vulkan allocations add
  up to <100 MB. Unresolved.
- Next cheap experiments: (a) drop `--in-process-gpu` (the browser's SwiftShader GPU thread burns a
  full core in-process), (b) desktop-mode Steam as a control for "does CEF paint on this device at
  all", (c) find what holds the 64 MB anon chunks in gamescope.
- The temporary `/tmp/droiddeck-diag.env` written for the arena test was removed again.

### 8.6 Continuing the first-frame hunt (11:00–11:40)

- Dropping `--in-process-gpu` is a real CPU win on this device: during a session the CEF browser went
  from 92% of a core (in-process SwiftShader GPU) to a few %, and the machine went from ~4 cores
  saturated to ~1.7 cores of sys time. Still no frame. Current pending debug env:
  `BL_WEBHELPER_ARGS=--no-zygote -AudioServiceOutOfProcess` (no `--in-process-gpu`).
- The output path itself is fine: a desktop session (`2026-10-25-07-desktop`) shows the app
  compositor with `1 window redraws | 1 windows open` and `render_scene` ~11 ms. It is the Steam
  session that never produces a frame.
- In the Steam session the UI window is mapped, managed (`WM_STATE Normal`, `STEAM_GAME=769`) and
  listed by `GAMESCOPE_FOCUSABLE_WINDOWS`, but `GAMESCOPE_FOCUSED_WINDOW` is empty and
  `GAMESCOPECTRL_BASELAYER_WINDOW` is 0, so `steamcompmgr` builds zero layers and `paint_all`
  returns at `if (!bValidContents)`. `repaint=0` forever; toggling `STEAM_INPUT_FOCUS`, re-setting
  `GAMESCOPECTRL_BASELAYER_APPID` (769) or `BASELAYER_WINDOW` did not change it.
- Running glxgears *inside* the Steam session is not a valid control: with the default
  `SteamControlled` strategy it has no appID and is never focused.
- Forcing `--virtual-connector-strategy SingleApplication` (via `BL_GAMESCOPE_ARGS`) produced the
  only `repaint=1` seen in any session, but still 0 `paint_all` — the layer list stayed empty.
- Next step is a gamescope diag print (temporary, like 0119/0120/0123) immediately before
  `const bool bValidContents = frameInfo.layers.count() > 0;` in `paint_all`, logging
  `pFocus->focusWindow` id/appID/opacity and `layers.count()`. That tells us whether focus is null
  or the layer build skips the window. Requires the normal CI cycle
  (patchgen → diff → push → `gamescope-patched` artifact → assets → `runtime_inputs.py record`).
- The `BL_GAMESCOPE_ARGS` experiment was cleared again. `BL_WEBHELPER_ARGS` keeps the
  no-`--in-process-gpu` value.

### 8.7 SOLVED — Steam's gamepad UI renders (2026-10-25 15:58)

Two independent bugs were blocking the first frame; both are fixed and verified on the TV.

**Bug 1 (renderer kills).** Chromium sets its own CEF children to `oom_score_adj=300`; Android's
LMKD killed the Steam UI renderer every 20–40 s. Fixed by `droiddeck-oom-guard` (§8.2).

**Bug 2 (commits never complete).** With Steam's window mapped and drawing, `steamcompmgr` still
withheld focus. New diag (`0124`) showed the commit path:

```
0124-diag: commit surf=… -> win=… id=0x2a00036 appID=769 q=1 done=0
0124-diag: commit fence kind=memfence fd=-1 knownReady=0
0124-diag: focus pick=(nil) … candidates=1 [0x2a00036:769(q1,d0,o1)]
```

The window's own commit never became `done`, so `window_has_commits()` was false and
`pick_primary_focus_and_override()` set `outdatedInteractiveFocus` and left `focusWindow == null` —
`paint_all` then built 0 layers and returned. The fence comes from
`CVulkanTexture::memoryFence()` → `vkGetMemoryFdKHR(DMA_BUF)`, which **lavapipe cannot do** (the
same limitation that drove 0122/0123): it returns `-1`. `commit_t::SetFence(-1, …)` then registers
fd -1 with epoll, so `OnPollIn` never fires, `Signal()` is never called, and the commit is never
marked done.

**Fix (gamescope `0125`, commit `bee66bc`)**: in `update_wayland_res()`, when the texture has no
dma-buf fence (`fence < 0`) treat the commit as ready:

```cpp
fence = newCommit->vulkanTex->memoryFence();
if ( fence < 0 )
    bKnownReady = true;   // CPU-uploaded shm buffer; fd -1 would never signal
```

After deploying: `focus pick=0x… appID=769`, `paint_all … layers=2`, `firstFrame: true`,
`phase: READY`, host compositor `12 window redraws | 1 windows open (1.2 fps)`, and the TV shows
Steam's OOBE controller screen. `Tab Killed` stayed at 1230.

**Still to do**
- Remove the `0124-diag` logging (paint_all focus, focus pick, commit surf, commit fence) and the
  0119–0121 diag patches before release; keep 0116/0122/0123/0125.
- Performance: the UI runs at ~1 fps (`render_scene` up to 361 ms, `present` 31 ms). Next wins are
  the CPU/memory items in §8.5/§8.6 — gamescope's ~890 MB anonymous footprint and the browser's
  software compositing.
- `droiddeckctl state` still reports `steamUi`/`steamUrl` null at `READY`; only `firstFrame` flips.
- Launcher change committed as `8a30622` (oom guard + staging + session hook).
