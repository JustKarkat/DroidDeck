# Research packet — Shield TV / DroidDeck: Steam gamepad UI never produces its first X commit

**Audience:** an external research model (Claude Sonnet 5.5) with no access to the device or this
sandbox. Everything needed is in this file. Read §1 first; §9 has the smoking-gun code, §10 the raw
logs, §13 the questions.

Written 2026-10-25 by the working session. Device is a real NVIDIA Shield TV, ADB-reachable, running
a fork of DroidDeck (a Linux/Steam-on-Android launcher). The goal is to get **Steam's Big Picture
(gamepad) UI** on screen. Everything below has been verified on the device unless marked
`[unverified]`.

---

## 0. TL;DR

Steam starts, its UI window is created, mapped, managed, titled "Steam Big Picture Mode", with
`STEAM_GAME=769`, and gamescope's `steamcompmgr` lists it as a focus candidate with appID 769. The
focus picker matches it — **but refuses to make it the presenting focus because the window has never
produced a single completed buffer commit**:

```cpp
if ( focus )
{
    if ( window_has_commits( focus ) )
        out->focusWindow = focus;      // <-- never taken
    else
        focus->outdatedInteractiveFocus = true;   // <-- taken
}
```

With `focusWindow == nullptr`, `paint_all()` builds 0 layers, returns before presenting, gamescope
never commits its own nested output surface, the host compositor sees "0 windows open", and the
Android loading overlay stays up forever.

So the question is no longer "why is the window unmapped" (it is mapped) or "why does gamescope not
paint" (it won't paint until the window commits). The question is:

> **Why does this mapped, managed, viewable 1280×800 CEF/Steam X11 window never generate a single
> completed commit under gamescope 3.16.29 + lavapipe on this prooted aarch64 guest — and how do we
> bootstrap the first one?**

Two sub-cases that must be distinguished, because they have completely different fixes:

- **(A)** `commit_queue` is empty → Steam/CEF never drew (or its damage never reached steamcompmgr).
- **(B)** `commit_queue` has entries but none is `done` → Steam drew, gamescope never completed the
  commit (fence/present/import problem, plausibly in our `0123-shm` output fallback).

The next diagnostic (one-line change, §9.9) returns `commit_queue.size()` and
`receivedDoneCommit` and settles A vs B. §12 ranks the hypotheses for each.

---

## 1. The stack, precisely

| Layer | What it is |
|---|---|
| Hardware | NVIDIA Shield TV (2015/2017), `mdarcy`, Tegra X1, 4× Cortex-A57, **3 GB RAM**, no root, Android 11 (`RQ1A.210105.003`) |
| Host app | `com.droiddeck.launcher` (DroidDeck fork, Kotlin + C++). Runs a **Vulkan compositor** (`waylandcomp`, C++/nvgpu system driver) that owns the Android surface |
| Guest | glibc aarch64 Arch-derived rootfs under **proot** (ptrace-based, single tracer, no namespaces, no systemd), uid = app uid |
| Display | gamescope 3.16.29 (patched fork), `--backend wayland` (nested client of waylandcomp), **lavapipe** ICD (`libvulkan_lvp.so`, CPU Vulkan). Output 1280×720@60, 2 Xwayland servers |
| Steam | native aarch64 Steam client pinned to `droiddeck-pinned-v8.0`, `steamrtarm64`, CEF/Chromium **126.0.6478.183**, gamepad UI |
| Session script | `/usr/local/bin/droiddeck-session` (bash, staged from `tools/linuxfs/overlay/...` by the APK) |

Process chain:

```
SessionService (app)
 └─ libproot.so ── droiddeck-session steam (outer)
      └─ gamescope --backend wayland --expose-wayland -f -W 1280 -H 720 -w 1280 -h 720 -r 60 -e \
                  --xwayland-count 2 --default-touch-mode 4 --mangoapp -- droiddeck-session steam (inner)
           ├─ Xwayland (:0 inside, :2/:3 as seen from the Android side)
           └─ steam (steamrtarm64) ── steamwebhelper (CEF browser) ── renderers/utility
```

Relevant server displays: from inside the guest `DISPLAY=:0` (gamescope sets it); the launcher's
control API numbers the Xwayland servers `:2` and `:3`. They are the same X servers.

The Android app's own overlay (loading screen, drawer) is a Compose view **above** the compositor
surface, so the TV currently shows "Starting Steam / Waiting for Steam's first frame".

---

## 2. What already works (do not re-litigate)

1. **The output path works.** A desktop session (`mode=desktop`, runs `pcmanfm`) makes the host
   compositor report `1 window redraws | 1 windows open` and `render_scene ≈ 11 ms`. Earlier,
   `glxgears` under gamescope produced `copy N` / `present … (N)` in the host compositor. So
   gamescope→waylandcomp→screen is not broken.
2. **Gamescope renders and presents under lavapipe.** Its `0123-shm` fallback allocates/maps its
   output framebuffers (`0123-shm: allocating 1280x720 shm buffer, size=3686400 stride=5120` →
   `shm fb 1280x720 stride 5120`), and the host compositor presents.
3. **waylandcomp initialises Vulkan on Tegra** (`VK_KHR_swapchain` only; driver lacks
   `VK_EXT_external_memory_dma_buf` / `VK_EXT_image_drm_format_modifier`, which is why the shm
   fallback exists).
4. **Steam's CEF is alive.** The UI JS runs (`async CM Connect`, `SteamAppInit`, stores initialise,
   `OOBE Store: keyboards 0`), and DevTools target list used to include
   `Steam Big Picture Mode` / `MainMenu_uid2` / `SharedJSContext`.
5. **The renderer-kill bug is fixed** (this matters because it used to mask everything): Chromium
   sets its own CEF children to `oom_score_adj=300`; Android's LMKD killed the Steam UI renderer
   every 20–40 s. A session helper now re-asserts adj 0 (`droiddeck-oom-guard`, §8). Result:
   **0 `Tab Killed` for 9+ minutes**, window reaches `map: viewable`. Do not chase this again.

---

## 3. Current symptom, in one screen

- Steam window: `WM_CLASS = "steamwebhelper", "steam"`, `_NET_WM_NAME = "Steam Big Picture Mode"`,
  `WM_STATE = Normal`, `STEAM_GAME = 769`, `_NET_WM_PID = <steamwebhelper pid>`,
  `_NET_WM_WINDOW_TYPE_NORMAL`, geometry 0,0 1280×800, `_NET_ACTIVE_WINDOW` points at it.
- gamescope root atoms (read with `DISPLAY=:0 xprop -root`):

```
GAMESCOPE_FOCUSABLE_WINDOWS(CARDINAL) = 44040246, 769, 14587
GAMESCOPE_FOCUSABLE_APPS(CARDINAL)    = 769
GAMESCOPE_FOCUSED_WINDOW(CARDINAL)    =            <-- empty
GAMESCOPE_FOCUSED_APP(CARDINAL)       =            <-- empty
GAMESCOPECTRL_BASELAYER_WINDOW(CARDINAL) = 0
GAMESCOPECTRL_BASELAYER_APPID(CARDINAL)  = 413091, 769
```

- gamescope heartbeat (`0123-diag`), sampled every second, **is always `repaint=0`**:

```
[gamescope] [Error] xwm: 0123-diag: main loop iter=3600 repaint=0
[gamescope] [Error] xwm: 0123-diag: main loop iter=7200 repaint=0
```

- `0123-diag: paint_all connector=… layers=…` (placed after the `bValidContents` early return) has
  **0 hits** in every session, i.e. layers are always 0.
- `0123-diag: Commit surface=…` and `0123-diag: Present layer=…` have **0 hits** — gamescope's nested
  output surface is never committed.
- Host compositor `wayland.log` stops logging after the first minute:

```
14:55:54.818  stats  last 10 s: 1 frames on screen (0.1 fps) | 0 GPU frames from games | 0 window redraws | 0 windows open
14:55:54.818  perf   last 10 s: 536 ticks, 1 scenes, 1 on screen (copy 1, …) | render_scene 49.10/49.10 ms | … (1)
```

- The Steam client's own log is full of UI-thread stalls:

```
[2026-10-25 10:19:22] Client version: 0
[2026-10-25 10:19:22] Warning: SteamUI thread frame stalled for: 32361 ms
[2026-10-25 10:22:27] Warning: SteamUI thread frame stalled for: 10090 ms
[2026-10-25 10:23:54] Warning: SteamUI thread frame stalled for: 5204 ms
```

- Steam's DevTools endpoint binds (`127.0.0.1:41089` for that session) and accepts TCP, but returns
  an **empty HTTP reply** even through `adb forward`; `/json/list` gives nothing. The browser's
  threads are idle (`do_sys_poll`, `epoll_wait`, futex) when inspected — not spinning.

---

## 4. The decisive diagnostic output (gamescope `0124-diag`, added by us)

We patched `steamcompmgr.cpp` to log, after `pick_primary_focus_and_override()` in
`determine_and_apply_focus()`:

```
0124-diag: focus pick=<focusWindow> appID=<id> strategy=<enum> appids=<n> control=0x<focusControlWindow> candidates=<n> [<id>:<appID> …]
```

and in `paint_all()` before the `bValidContents` return:

```
0124-diag: paint_all focus=<ptr> id=0x<id> appID=<u> opacity=0x<u> layers=<n> override=<ptr>
```

Verbatim, first lines after session start (session `2026-10-25-13-steam`):

```
[gamescope] [Error] xwm: 0124-diag: focus pick=(nil) appID=0 strategy=1 appids=0 control=0x0 candidates=0 []
[gamescope] [Error] xwm: 0124-diag: paint_all focus=(nil) id=0x0 appID=0 opacity=0x0 layers=0 override=(nil)
[gamescope] [Error] xwm: 0124-diag: focus pick=(nil) appID=0 strategy=1 appids=0 control=0x0 candidates=0 []
[gamescope] [Error] xwm: 0124-diag: paint_all focus=(nil) id=0x0 appID=0 opacity=0x0 layers=0 override=(nil)
[gamescope] [Error] xwm: 0124-diag: focus pick=(nil) appID=0 strategy=1 appids=0 control=0x0 candidates=0 []
```

Later, once Steam has created its window (tail of the same log):

```
[gamescope] [Error] xwm: 0124-diag: focus pick=(nil) appID=0 strategy=1 appids=2 control=0x0 candidates=1 [0x2200036:769 ]
[gamescope] [Error] xwm: 0124-diag: focus pick=(nil) appID=0 strategy=1 appids=2 control=0x0 candidates=1 [0x2200036:769 ]
[gamescope] [Error] xwm: 0124-diag: focus pick=(nil) appID=0 strategy=1 appids=2 control=0x0 candidates=1 [0x2200036:769 ]
```

Read that carefully:

- `strategy=1` = `SteamControlled` (enum: `SingleApplication=0, SteamControlled=1, PerAppId=2,
  PerWindow=3`).
- `appids=2` = the vector parsed from `GAMESCOPECTRL_BASELAYER_APPID` is `{413091, 769}`.
- `candidates=1` = exactly one focusable window, `0x2200036`, appID **769**.
- 769 is in `appids` → the appID loop **does** match and `focus` is non-null internally.
- Yet `pick=(nil)` because of the `window_has_commits()` gate in §9.2.

So `w->outdatedInteractiveFocus = true` is being set every time. The main loop's resolver is waiting
for a commit that never comes:

```cpp
if ( w->outdatedInteractiveFocus )
{
    MakeFocusDirty();
    w->outdatedInteractiveFocus = false;
}
```

(this only runs inside the commit-handling path, i.e. when a commit arrives).

---

## 5. Why this is a deadlock

- gamescope will not make the window the presenting focus until it has a commit.
- Steam/CEF may not draw until it believes it is on screen / has a buffer to paint.
- Nothing else in the session is allowed to paint because `SteamControlled` filters candidates to
  `win_has_game_id(w) || window_is_steam(w) || isSteamStreamingClient` (glxgears has no game id, so
  it is not even a candidate — do not use it as a control in a Steam session).

A single commit should break the deadlock: the commit handler marks focus dirty, the picker then
sets `out->focusWindow`, `paint_all` gets layers, and the first frame appears.

---

## 6. What we need from you (short version)

1. **Explain the two cases** (§0 A vs B) from the gamescope/CEF source, and tell us which artefacts
   to look at (they are listed in §9.9). If you can, name the exact line in gamescope 3.16.29 or
   Xwayland that would drop the commit.
2. **Give us a way to force one commit** from a mapped Steam window using tools we actually have:
   guest rootfs has `xprop`, `glxgears`, `python3.14`, no compiler, no `xwd`/`xwininfo`/`xdotool`/
   `wmctrl`. The host is a Linux box with ADB, python3.15, `gh`, `gcc`, `curl`. There is a working
   CI to cross-build aarch64-glibc binaries (archlinuxarm container) if a tiny helper is the best
   path.
3. **Rule in/out our `0123-shm` output fallback** as the cause of case B (it forces
   `flags.bMappable = true` on every flippable texture and publishes output via `wl_shm`; see §8.3).
4. **Tell us whether gamescope 3.16 / Steam-on-Deck has a known first-commit problem** with
   `--xwayland-count 2`, or with CEF when `window_has_commits` is used, and how Deck avoids it.
5. If the answer is a patch, **specify the smallest safe patch** (function + line anchor) so we can
   apply it to `steamcompmgr.cpp` and rebuild through CI.

Longer, numbered questions are in §13.

---

## 7. Everything that has already been ruled out (do not repeat)

| Hypothesis | Status | Evidence |
|---|---|---|
| Renderer crashes / V8 OOM / proot syscall fault | **ruled out** | `Tab Killed` = SIGKILL by LMKD; fixed by `oom_score_adj` guard; 0 kills since |
| `statx` ENOSYS (kernel 4.9) breaks CEF | **ruled out** | Chromium returns ENOSYS for statx in its own sandbox by design; only coreutils `stat` trips on it |
| Minidump hunting | **pointless** | SIGKILL produces no dump; crashpad also can't ptrace under proot |
| gamescope's window unmapped (old blocker) | **fixed** | `map: viewable` once the renderer stopped being killed |
| Missing damage because `hasRepaint` is never set by *our* code | **this is the symptom** | heartbeat `repaint=0`, `paint_all` 0 hits |
| `GAMESCOPECTRL_BASELAYER_WINDOW = 0` | **not the cause** | setting it to the window id by hand did not change focus or repaint |
| `GAMESCOPECTRL_BASELAYER_APPID` never seen | **not the cause** | `appids=2` in the diag; re-setting it by hand with `xprop` also did nothing |
| `STEAM_INPUT_FOCUS` never set | **not the cause** | toggling 0→1 by hand did not change the pick result |
| Wrong virtual-connector strategy | **partially checked** | `-e` sets `SteamControlled`; forcing `SingleApplication` produced the only `repaint=1` ever seen, still 0 layers |
| glxgears as control | **invalid** | filtered out by the `SteamControlled` candidate filter (no game id) |
| In-process GPU exhausting a core | **mitigated** | dropping `--in-process-gpu` cut browser CPU from 92% to a few %; no frame change |
| Storage exhaustion | **mitigated** | freed 1.4 GB of Steam `package/tmp`; no frame change |
| CPU starvation | **mitigated, not the cause** | machine now idles at ~1.7 cores sys; still no commit |

---

## 8. Host/launcher side (for context)

### 8.1 Session scripts

`tools/linuxfs/overlay/usr/local/bin/droiddeck-session` (bash). Outer part builds gamescope args:

```bash
args=(--backend wayland --expose-wayland -f -W "$width" -H "$height" -w "$nested" -h "$height")
[ "$mode" = steam ] && [ "${BL_STEAM_UI:-}" != desktop ] && args+=(-e)
[ "$mode" = steam ] && [ "${BL_XWAYLAND_COUNT:-2}" -gt 1 ] && args+=(--xwayland-count "${BL_XWAYLAND_COUNT:-2}")
[ "$mode" = steam ] && args+=(--default-touch-mode 4)
[ "$mode" = steam ] && [ "${BL_MANGOAPP:-1}" = 1 ] && args+=(--mangoapp)
gamescope "${args[@]}" -- "$0" "$mode" "$@"
```

Inner part starts the Steam client, the control agent and (our addition, §8.2) the oom guard.

`-e` = `steamMode = true`, and in `main.cpp` it also flips
`cv_backend_virtual_connector_strategy` from `SingleApplication` to `SteamControlled`.

### 8.2 Our renderer-kill fix (do not remove)

`tools/linuxfs/overlay/usr/local/bin/droiddeck-oom-guard` (python3):

```python
#!/usr/bin/python3
# Chromium raises its own renderer/utility oom_score_adj (300/200); on this 3 GB
# device Android's LMKD then kills the Steam UI renderer every 20-40 s.
# Put the session's CEF children back at adj 0 so LMKD reaps background apps first.
import os, time

def children():
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            cmd = open('/proc/%s/cmdline' % pid, 'rb').read()
        except OSError:
            continue
        if b'steamwebhelper' in cmd and b'--type=' in cmd:
            yield pid

def main():
    idle = 0
    while True:
        found = False
        for pid in children():
            found = True
            try:
                if open('/proc/%s/oom_score_adj' % pid).read().strip() != '0':
                    open('/proc/%s/oom_score_adj' % pid, 'w').write('0')
            except OSError:
                pass
        idle = 0 if found else idle + 1
        if idle >= 60:
            return
        time.sleep(1)
main()
```

started from the session script right after `start_agent`:

```bash
[ "${BL_DESKTOP:-}" = 1 ] || start_agent
[ -x /usr/local/bin/droiddeck-oom-guard ] && /usr/local/bin/droiddeck-oom-guard &
```

It is staged by `SessionFiles.kt` (explicit file list) and attested by
`python3 tools/release/runtime_inputs.py record` before `./gradlew :app:assembleRelease`.

### 8.3 The gamescope patch series (all temporary except 0116/0122/0123)

`tools/gamescope/patches/` contains 0119–0124. The relevant ones:

- **0122-dmabuf-export-gate**: probe whether the ICD supports
  `VK_KHR_external_memory_dma_buf`; on this device it does not, so gamescope must not use the
  dmabuf path.
- **0123-shm-output-fallback**: when export is unsupported, publish every flippable texture through
  a CPU `wl_shm` buffer (`memfd_create` + `ftruncate` + `mmap`, `wl_shm_create_pool`,
  `wl_shm_pool_create_buffer`), refresh it with `FlushShm()` right before `wl_surface_attach`, and
  force `flags.bMappable = true` (LINEAR tiling) on every flippable texture so it has a CPU mapping.
  This is how gamescope's output reaches `waylandcomp`. **It only affects the *output* path, not
  per-window commit completion** — but see §12 hypothesis B3, we have not proven that separation.
- **0123-diag**: heartbeat (`main loop iter=… repaint=…`), first 3 `paint_all` /
  `CWaylandPlane::Commit` / `CWaylandPlane::Present` calls.
- **0124-diag**: the focus/commit logging described in §4. To be removed before release.

Gamescope is built in CI (GitHub Actions, archlinuxarm container) from a `patchgen` tree
(`/tmp/opencode/patchgen/{a,b}`), artifact `gamescope-patched`, and the binary is copied into
`app/src/main/assets/linuxfs/usr/local/bin/gamescope`.

### 8.4 Host compositor (`waylandcomp`)

C++ Vulkan compositor in `app/src/main/cpp/waylandcomp/src/`. It imports `wl_shm` surfaces
(`compositor.c: take_shm` → `vkp_image_create_shm` / `upload_shm`) and dmabufs
(`vk_present.c: vkp_image_from_dmabuf`). Stats line format used above comes from `wayland.log`
(`stats`/`perf`), reporting `N window redraws | N windows open`, copy counts, `render_scene` ms.

---

## 9. Gamescope source, verbatim (3.16.29 fork; paths relative to gamescope `src/`)

### 9.1 `backend.h` — strategy

```cpp
namespace VirtualConnectorStrategies {
    enum VirtualConnectorStrategy : uint32_t {
        SingleApplication,   // 0
        SteamControlled,     // 1
        PerAppId,            // 2
        PerWindow,           // 3
    };
}
```

`main.cpp`, `-e` parsing:

```cpp
case 'e':
    steamMode = true;
    if ( cv_backend_virtual_connector_strategy == VirtualConnectorStrategies::SingleApplication )
        cv_backend_virtual_connector_strategy = VirtualConnectorStrategies::SteamControlled;
    break;
```

### 9.2 `steamcompmgr.cpp` — the focus gate (THE BUG SITE)

```cpp
	if ( focus )
	{
		if ( window_has_commits( focus ) )
			out->focusWindow = focus;
		else
			focus->outdatedInteractiveFocus = true;

		// Always update X's idea of focus, but still dirty
		// the it being outdated so we can resolve that globally later.
		//
		// Only affecting X and not the WL idea of focus here,
		// we always want to think the window is focused.
		// but our real presenting focus and input focus can be elsewhere.
		if ( !globalFocus )
			out->focusWindow = focus;
	}
```

`globalFocus = true` is what the per-connector/global pick uses (line ~5226), so the commit gate
applies there. The per-Xwayland-context pick uses `globalFocus = false` and is not gated.

### 9.3 `steamcompmgr.cpp` — `window_has_commits`

```cpp
static int32_t
window_last_done_commit_index( steamcompmgr_win_t *w )
{
	int32_t lastCommit = -1;
	for ( uint32_t i = 0; i < w->commit_queue.size(); i++ )
	{
		if ( w->commit_queue[ i ]->done )
			lastCommit = i;
	}
	return lastCommit;
}

static bool
window_has_commits( steamcompmgr_win_t *w )
{
	return window_last_done_commit_index( w ) != -1;
}
```

Note: it requires `done`, not merely a queue entry. A queue with `done == false` entries is
indistinguishable from an empty queue at the focus picker (this is the A/B ambiguity).

### 9.4 `steamcompmgr.cpp` — commit resolver (what would break the deadlock)

```cpp
			if ( w->outdatedInteractiveFocus )
			{
				MakeFocusDirty();
				w->outdatedInteractiveFocus = false;
			}
```

It lives inside the per-window commit handling (`bFoundWindow == true` after iterating
`w->commit_queue`), i.e. it only runs when a commit is actually received.

### 9.5 `steamcompmgr.cpp` — focus selection start

```cpp
static bool
pick_primary_focus_and_override(
	focus_t *out,
	Window focusControlWindow,
	const std::vector<steamcompmgr_win_t*>& vecPossibleFocusWindows,
	bool globalFocus,
	const std::vector<uint32_t>& ctxFocusControlAppIDs,
	uint64_t ulVirtualFocusKey,
	gamescope::VirtualConnectorStrategy eStrategy )
{
	bool localGameFocused = false;
	steamcompmgr_win_t *focus = NULL, *override_focus = NULL;

	bool controlledFocus = eStrategy != gamescope::VirtualConnectorStrategies::SingleApplication || focusControlWindow != None || !ctxFocusControlAppIDs.empty();
	if ( controlledFocus )
	{
		if ( eStrategy == gamescope::VirtualConnectorStrategies::SteamControlled )
		{
			if ( focusControlWindow != None )
			{
				for ( steamcompmgr_win_t *focusable_window : vecPossibleFocusWindows )
				{
					if ( focusable_window->type != steamcompmgr_win_type_t::XWAYLAND )
						continue;
					if ( focusable_window->xwayland().id == focusControlWindow )
					{
						focus = focusable_window;
						localGameFocused = true;
						goto found;
					}
				}
			}

			for ( auto focusable_appid : ctxFocusControlAppIDs )
			{
				for ( steamcompmgr_win_t *focusable_window : vecPossibleFocusWindows )
				{
					if ( focusable_window->appID == focusable_appid )
					{
						focus = focusable_window;
						localGameFocused = true;
						goto found;
					}
				}
			}
		}
		else
		{
			for ( steamcompmgr_win_t *focusable_window : vecPossibleFocusWindows )
			{
				if ( focusable_window->GetVirtualConnectorKey( eStrategy ) == ulVirtualFocusKey )
				{
					focus = focusable_window;
					localGameFocused = true;
					goto found;
				}
			}
		}
found:;
	}
	...
```

This is why our diag sees a match but `out->focusWindow` stays null: the match happens, then the
commit gate drops it.

### 9.6 `steamcompmgr.cpp` — candidate construction (`xwayland_ctx_t::GetPossibleFocusWindows`)

```cpp
	for (steamcompmgr_win_t *w = this->list; w; w = w->xwayland().next)
	{
		if ( w->isSysTrayIcon || w->isOverlay || w->isExternalOverlay )
			continue;
		if ( w->oulTargetVROverlay && !cv_vr_show_forwarded_overlays )
			continue;
		if ( w->isSteamStreamingClientVideo )
			continue;

		if ( gamescope::cv_backend_virtual_connector_strategy == gamescope::VirtualConnectorStrategies::SteamControlled )
		{
			if ( !( win_has_game_id( w ) || window_is_steam( w ) || w->isSteamStreamingClient ) )
				continue;
		}

		if ( w->xwayland().a.map_state == IsViewable && w->xwayland().a.c_class == InputOutput &&
			 (w->opacity > TRANSLUCENT || w->isSteamStreamingClient ) )
		{
			vecPossibleFocusWindows.push_back( w );
		}
	}
```

### 9.7 `steamcompmgr.cpp` — window map handler (sets appID 769, map_state)

```cpp
	w->xwayland().a.map_state = IsViewable;

	XSelectInput(ctx->dpy, id, PropertyChangeMask | SubstructureNotifyMask | LeaveWindowMask | FocusChangeMask);
	XFlush(ctx->dpy);

	w->opacity = get_prop(ctx, w->xwayland().id, ctx->atoms.opacityAtom, OPAQUE);
	w->isSteamLegacyBigPicture = get_prop(ctx, w->xwayland().id, ctx->atoms.steamAtom, 0);
	...
	w->inputFocusMode = get_prop(ctx, w->xwayland().id, ctx->atoms.steamInputFocusAtom, 0);
	w->isSteamStreamingClient = get_prop(ctx, w->xwayland().id, ctx->atoms.steamStreamingClientAtom, 0);
	w->steamAppID = get_prop(ctx, w->xwayland().id, ctx->atoms.gameAtom, 0);

	if ( steamMode == true )
	{
		uint32_t appID = w->steamAppID;
		if ( appID != 0 )
			w->appID = appID;
	}
```

Root window input selection at init includes `PropertyChangeMask`, so root atom changes
(`GAMESCOPECTRL_*`) are delivered.

### 9.8 `steamcompmgr.cpp` — root atom handling (focus control)

```cpp
	if (ev->atom == ctx->atoms.gamescopeCtrlAppIDAtom )
	{
		get_prop( ctx, ctx->root, ctx->atoms.gamescopeCtrlAppIDAtom, vecFocuscontrolAppIDs );
		MakeFocusDirty();
	}
	if (ev->atom == ctx->atoms.gamescopeCtrlWindowAtom )
	{
		ctx->focusControlWindow = get_prop( ctx, ctx->root, ctx->atoms.gamescopeCtrlWindowAtom, None );
		MakeFocusDirty();
	}
```

### 9.9 Our diag patch (the exact next change)

Current, in `determine_and_apply_focus()` right after the pick:

```cpp
	// 0124-diag: which window wins the focus pick, and with what inputs (temporary)
	{
		static int s_nFocusDiag = 0;
		if ( s_nFocusDiag++ < 200 )
		{
			char szCandidates[512] = {};
			for ( steamcompmgr_win_t *pCandidate : vecPossibleFocusWindows )
				snprintf( szCandidates + strlen( szCandidates ), sizeof( szCandidates ) - strlen( szCandidates ),
					"0x%x:%u ", pCandidate->id(), pCandidate->appID );
			xwm_log.errorf( "0124-diag: focus pick=%p appID=%u strategy=%u appids=%zu control=0x%lx candidates=%zu [%s]",
				(void *)pFocus->focusWindow, pFocus->focusWindow ? pFocus->focusWindow->appID : 0u,
				(unsigned)gamescope::cv_backend_virtual_connector_strategy,
				vecFocuscontrolAppIDs.size(), root_ctx->focusControlWindow,
				vecPossibleFocusWindows.size(), szCandidates );
		}
	}
```

**The next change to make (not built yet):** add the commit state of the chosen window, and make it
visible even when the pick is nil. Suggested:

```cpp
	// in the diag above, for each candidate:
	//   window_has_commits(c), c->commit_queue.size(), c->receivedDoneCommit, c->outdatedInteractiveFocus
	// and in paint_all, log w->commit_queue.size()/receivedDoneCommit when w != nullptr
```

That single datum decides case A vs case B (§0). We could not run it before writing this packet.

### 9.10 `paint_all()` early return

```cpp
	if ( !bValidContents || GetBackend()->IsPaused() )
	{
		return;
	}
```

with `const bool bValidContents = frameInfo.layers.count() > 0;` and layers only added by
`paint_window(pFocus, w, …)` where `w = pFocus->focusWindow`. `focusWindow == nullptr` ⇒ 0 layers.

---

## 10. Raw device logs

### 10.1 CPU / memory during a session (post `--in-process-gpu` removal)

```
400%cpu  71%user   4%nice 466%sys 160%idle  14%iow   5%irq   2%sirq       <- with --in-process-gpu
400%cpu  14%user   0%nice 173%sys 195%idle  16%iow   2%irq   0%sirq       <- without
MemTotal 3016708K  MemFree ~20-60M  MemAvailable 300-500M  Swap 524284K total, 524284K used, 0 free
```

Top processes with `--in-process-gpu`: CEF browser 92%, `com.droiddeck.launcher` 73%, kswapd0 62%,
zygote64 49%, `droiddeck-steam-games probe` 32%, network service 31%, system_server 31%, renderer
5% / 273 MB. Without it, browser drops to near idle; system_server/kswapd dominate.

### 10.2 gamescope memory (why 900 MB matters, though it is not today's blocker)

`dumpsys meminfo <gamescope pid>`:

```
      Unknown   885812   885812        0        0   885812
        TOTAL   913446   907064      472        0   913446
```

`/proc/<pid>/status`: `VmRSS 905108 kB  RssAnon 891532 kB  RssFile 1632 kB  RssShmem 11944 kB`,
`VmData 1182056 kB`. `/proc/<pid>/smaps` shows ~14 anonymous `rw-p` regions of exactly 64 MB
(`0x4000000`), ~47 MB resident each, at 64 MB-aligned addresses. **Not** glibc arenas: exporting
`MALLOC_ARENA_MAX=2` and `LP_NUM_THREADS=1` changed nothing (912 MB). Not the shm buffers
(`RssShmem` 27 MB), and logged Vulkan allocations sum to <100 MB. Unresolved; likely lavapipe/llvmpipe
private allocations. Device has 3 GB and no swap left, so this matters for the UI thread's 5–32 s
stalls.

### 10.3 LMKD kill correlation (the bug we fixed)

`memwatch` (0.25 s sampler) around a kill, before the guard:

```
23:05:36 TICK avail=374 swapfree=0 renderers_rss={12764: 342}
23:05:40 GONE pid=12691 type=utility last={'VmRSS': 21}  MemFree=445 Avail=783
23:05:40 GONE pid=12694 type=utility last={'VmRSS': 22}  MemFree=445 Avail=783
23:05:40 GONE pid=12764 type=renderer last={'VmRSS': 332} oom_score=396 MemFree=445 Avail=783
23:05:40 TICK avail=816 swapfree=0 renderers_rss={}
```

Same second, Android's event log:

```
10-24 23:05:40.234 I am_proc_died: [0,11510,com.google.android.permissioncontroller,150,6]
10-24 23:05:41.109 I am_kill: [0,11977,com.google.android.tvlauncher,150,depends on provider …]
10-24 23:05:41.401 I am_proc_died: [0,11654,com.google.android.gms,900,17]
```

and `lmkd` (pid 2872) "Failed to open /proc/<pid>/oom_score_adj" chatter at 23:05:41.

Renderers carried `oom_score_adj=300`, utilities 200, gamescope/browser 0 → LMKD picks the renderer.
After the guard: `Tab Killed` counter stayed at 1230 across 9+ minutes (was one per 20–40 s), and
the window reached `map: viewable`.

### 10.4 CEF log (before the guard)

```
[2861:2861:1024/224748.708701:WARNING:sad_tab.cc(255)] Tab Killed: https://steamloopback.host/
[2861:2861:1024/224800.046630:WARNING:sad_tab.cc(255)] Tab Killed: http://error/
[2861:2861:1024/225159.879155:ERROR:network_service_instance_impl.cc(600)] Network service crashed, restarting service.
```

`grep -c "bad IPC message"` = 0. After the guard: no new `Tab Killed` lines at all.

### 10.5 Steam UI progress (post-guard)

`webhelper_js.txt`:

```
[2026-10-25 10:18:46] SteamUI: INFO: async CM Connect - 157.9ms
[2026-10-25 10:18:46] SteamUI: INFO: SteamAppInit - ControllerStore - 211.5ms
[2026-10-25 10:19:29] SteamUI: INFO: Updated Steam Version Info: {"nSteamVersion":0,"sSteamAPI":"SteamClient023",…}
[2026-10-25 10:20:27] SteamUI: INFO: OOBE Store: keyboards 0
```

So the UI JS is alive; it is the presentation that never happens.

### 10.6 DevTools

Port is bound (`/proc/net/tcp` shows `0100007F:66B4` LISTEN owned by uid 10018), TCP connect succeeds,
but the HTTP request gets an empty reply and `/json/list` returns nothing, even via
`adb forward tcp:<host> tcp:<guest>`. Thread dump of the browser process shows main thread in
`do_sys_poll`, `Chrome_IOThread` in `SyS_epoll_wait`, everything else futex — idle, not spinning.

### 10.7 Steam window properties (xprop)

```
STEAM_INPUT_FOCUS(CARDINAL) = 1
STEAM_NOTIFICATION(CARDINAL) = 0
STEAM_OVERLAY(CARDINAL) = 0
WM_STATE(WM_STATE): window state: Normal
STEAM_GAME(CARDINAL) = 769
_NET_WM_NAME(UTF8_STRING) = "Steam Big Picture Mode"
WM_NAME(UTF-8) = "Steam Big Picture Mode"
_NET_WM_SYNC_REQUEST_COUNTER(CARDINAL) = 44040247
WM_PROTOCOLS(ATOM): WM_DELETE_WINDOW, WM_TAKE_FOCUS, _NET_WM_PING, _NET_WM_SYNC_REQUEST
_NET_WM_WINDOW_TYPE(ATOM) = _NET_WM_WINDOW_TYPE_NORMAL
_NET_WM_PID(CARDINAL) = 14587
WM_CLASS(STRING) = "steamwebhelper", "steam"
```

### 10.8 The last healthy frame (host compositor), desktop session

```
stats  last 10 s: 1 frames on screen (0.1 fps) | 0 GPU frames from games | 1 window redraws | 1 windows open
perf   last 10 s: 599 ticks, 1 scenes, 1 on screen (copy 1, …) | render_scene 10.79/10.79 ms | acquire 0.45 | present 0.64
```

### 10.9 Storage (for completeness)

`/data` is a 12 GB partition. It was 98% full (268 MB free); we deleted
`~/.local/share/Steam/package/tmp` (1.4 GB stale client-update staging from Oct 23) → 1.7 GB free.
`linuxfs` is 7.1 GB (`usr` 3.8, `root` 3.6; Steam `package` 1.1 GB of cached zips, `steamrtarm64`
686 MB). Not the current blocker.

---

## 11. Environment / versions

- Kernel: `4.9.141-tegra-gb6e5605a` (no `statx`, has `memfd_create`).
- Android 11 `RQ1A.210105.003`, no root; `run-as com.droiddeck.launcher` works; `adb` over TCP
  (`192.168.0.194:5555`), drops under load and reconnects.
- gamescope: 3.16.29 fork, built in CI from `tools/gamescope/patches/0119…0124`.
- Mesa: lavapipe ICD pushed into the guest; `vk_present.c` was patched to filter unsupported device
  extensions (so `vkCreateDevice` succeeds with only `VK_KHR_swapchain`).
- Steam: pinned `droiddeck-pinned-v8.0`, native aarch64, CEF 126.0.6478.183.
- Env in the guest: `MESA_NO_ERROR=1`, `LP_NUM_THREADS=2`, `MESA_DISK_CACHE_DATABASE=1`,
  `VK_ICD_FILENAMES` → lavapipe.
- Debug env in use: `BL_WEBHELPER_ARGS=--no-zygote -AudioServiceOutOfProcess`
  (default in `tools/linuxfs/preload/webhelper.c` is
  `--no-zygote --in-process-gpu -AudioServiceOutOfProcess`).
- Steam's own browser flags include `--disable-gpu --disable-gpu-compositing
  --ignore-gpu-blocklist --no-sandbox -cef-ozone-platform=x11 --use-angle=swiftshader-webgl
  --use-gl=angle --enable-features=V4L2VideoDecode --disable-crash-reporter`.

---

## 12. Hypotheses for "no first commit", ranked

### Case A — Steam never draws

- **A1 (most likely): CEF's software compositor cannot get a shared-memory transport and silently
  renders nothing.** With `--disable-gpu` + `--disable-gpu-compositing`, CEF composites into shared
  buffers. The guest's `/dev/shm` is a bind of the app's `cache/shm` (currently 8 KB, 1 file) and
  `/proc/sys/fs/inotify/max_user_watches` is unreadable; Chromium normally uses memfd. Worth
  checking whether CEF's `XShmPutImage` path is active and whether MIT-SHM is advertised by this
  Xwayland. (We have no `xdpyinfo` in the guest.)
- **A2: Steam's UI thread is stalled so long it never reaches first paint.** `steamui.txt` shows
  5–32 s frame stalls; the machine has 3 GB RAM with swap exhausted and gamescope holds 900 MB of
  anonymous memory. Test: cut gamescope memory or throttle the UI, then watch for a commit.
- **A3: CEF waits for a "shown" signal that gamescope never sends.** Steam-on-Deck may use
  `GAMESCOPE_FOCUSED_*` / `GAMESCOPE_DISPLAY_*` atoms or the `gamescope-control` protocol to decide
  the window is on screen. If so, the commit gate is a real deadlock in this nesting and the fix is
  on the gamescope side (force first focus) or on Steam's side (ignore visibility).
- **A4: the window is drawn on the *other* Xwayland server.** With `--xwayland-count 2`, Steam may
  create its render surface on server 1 while steamcompmgr composites server 0. Our xprop on `:0`
  sees the toplevel and the diag shows it as a candidate, so this is unlikely for the toplevel, but a
  **child** render window on the other server would not be composited.

### Case B — Steam draws but the commit never completes

- **B1: `done` never set because the compositor's fence/present never signals** (lavapipe + our
  forced-linear/mappable textures + `FlushShm` copy). If the queue has entries with `done=false`,
  look at `commit->done` assignment and the `0123-shm` `OnCompositorAcquire`/`OnCompositorRelease`
  path.
- **B2: damage arrives but `add_commit` is skipped** for redirected windows on this Xwayland
  (CompositeRedirectManual + XDamage), e.g. the window's pixmap is never bound.
- **B3: our `bMappable=true` forcing changes the window texture path** — but window textures are not
  flippable output textures, so this is weaker than B1.
- **B4: `outdatedInteractiveFocus` is set once and the resolver runs on a path that also requires
  the window to be the *current* focus** (chicken-and-egg inside gamescope).

### Things to look at first (cheap, no rebuild)

1. **Get the commit state** (§9.9). Decides A vs B.
2. **Try to force a commit**: `xrefresh` (absent), `xsetroot` (absent), a synthetic
   `XClearWindow`/`XSendEvent(Expose)` from a tiny helper. We can build an aarch64-glibc helper in
   CI, or try `python3` + `ctypes` with `libX11.so` from the rootfs (it has X11 libs for
   `xprop`/`glxgears`). `XClearWindow` on the Steam window should generate damage and a commit; if
   the UI appears right after, A3 is confirmed and the workaround is trivial.
3. **Check MIT-SHM** by loading `libXext` via python/ctypes and `XShmQueryExtension`, or by looking
   for CEF log lines about shared memory.
4. **Run a non-Steam X client that composites** (e.g. `glxgears` with `STEAM_GAME=769` set via
   `xprop -id … -f STEAM_GAME 32c -set STEAM_GAME 769`) to see whether the commit pipeline works for
   a *simple* client under `SteamControlled`. This is the cleanest A/B split that needs no rebuild:
   if glxgears with a fake game id paints, the pipeline is fine and Steam's window is the problem;
   if it also never commits, steamcompmgr/damage handling is broken in this configuration.
5. **Check the `0123-shm` path's effect on window commits** by reverting to 0122-only (does
   `paint_all` get layers for a simple client?).

---

## 13. Questions for the researcher (numbered)

1. In gamescope 3.16.29, what normally produces the first `commit` for an X11 window? Walk the
   XDamage → `handle_damage` → `add_commit`/`commit_queue` path (file/function names + line
   anchors), and list the conditions under which a mapped, viewable, redirected window would never
   get an entry, or get entries that never become `done`.
2. Is `window_has_commits()` gate + `outdatedInteractiveFocus` a known source of "Steam UI never
   appears" on nested/wl backends? How does SteamOS/Deck avoid it? Is there a ConVar to bypass it
   (we saw `cv_paint_debug_pause_base_plane`; is there something like
   `gamescope_focus_allow_uncommitted`)?
3. What is the smallest safe patch to force the first focus for the Steam UI (appID 769) without
   breaking fade/held-commit semantics? E.g. in `pick_primary_focus_and_override`, treat
   `window_is_steam(focus)` as commit-valid, or set `out->focusWindow` and let `paint_window` skip
   until a commit exists.
4. Does Chromium/CEF 126 with `--disable-gpu --disable-gpu-compositing` require MIT-SHM or specific
   X extensions to present into an X11 window? What log lines would indicate failure? (Our cef_log
   shows only dbus/udev/inotify/CRL noise.)
5. Could the Steam client's `--remote-debugging-port` endpoint hanging (empty reply) and the UI
   thread stalls be the *same* root cause as no commits? What would make the browser main thread
   poll-idle yet not serve HTTP? (X connection lock? blocked on a sync reply from Xwayland?
   `_NET_WM_SYNC_REQUEST` counter waiting for a frame?)
6. Is `_NET_WM_SYNC_REQUEST` relevant? Steam's window advertises
   `_NET_WM_SYNC_REQUEST_COUNTER = 44040247` and `WM_TAKE_FOCUS`. If Steam waits for a
   `_NET_WM_SYNC_REQUEST` completion from the WM before painting, and steamcompmgr does not
   implement it for this window, that alone would deadlock first paint. Does gamescope implement
   sync counters on the nested backend? (grep for `syncRequestCounter`/`_NET_WM_SYNC_REQUEST`.)
7. Any known issue with **two Xwayland servers** (`--xwayland-count 2`) and CEF: which server does
   Steam pick, and does `steamcompmgr` composite both? What does `STEAM_MULTIPLE_XWAYLANDS=1`
   change in Steam?
8. Given that a desktop session (pcmanfm) composites fine, what is different about a CEF window
   that we should test first?
9. If the fix needs to be on the Steam side: what is the supported way to tell Steam "you are
   visible, paint now" in a nested gamescope (env vars, `-cef-*` flags, `GAMESCOPE_*` atoms)?

---

## 14. Constraints and how to verify

- No compiler in the guest; aarch64-glibc helpers must be cross-built in CI
  (`tools/gamescope/build-in-arch.sh` pattern) and pushed.
- No root on Android; `run-as com.droiddeck.launcher` for app data; `droiddeckctl guest` to run
  guest commands through the launcher's control agent (it times out under load).
- gamescope changes go through: patchgen tree → `diff -u a/… b/…` → `0123-shm-output-fallback.patch`
  → commit to fork `shield-tv` → GH Actions `Build gamescope (patched)` → artifact
  `gamescope-patched` → `tar --zstd -xf` → copy `usr/local/bin/gamescope` into
  `app/src/main/assets/linuxfs/usr/local/bin/` → `python3 tools/release/runtime_inputs.py record` →
  `./gradlew :app:assembleRelease` → `adb install -r` → start session.
- One session cycle takes ~2 min and uses ~1.6 GB of RAM (3 GB device), so batch experiments.
- `droiddeckctl focus` / `state` / `guest` frequently time out under memory pressure; read
  `files/logs/<session>/` directly with `run-as` instead.
- Logs live in `files/logs/<session>/`: `session.log` (guest stdout, includes gamescope stderr),
  `wayland.log` (host compositor), `app.log`, `events.jsonl`; Steam's own logs at
  `files/linuxfs/root/.local/share/Steam/logs/` (`steamui.txt`, `cef_log.txt`,
  `webhelper_js.txt`).

---

## 15. Appendix — exact commands used (so results are reproducible)

```sh
# device
adb connect 192.168.0.194:5555
adb -s 192.168.0.194:5555 shell 'run-as com.droiddeck.launcher sh -c "tail -40 files/logs/<session>/session.log"'

# steam window atoms (inside the guest)
droiddeckctl guest bash -c 'DISPLAY=:0 xprop -root GAMESCOPE_FOCUSED_WINDOW GAMESCOPE_FOCUSABLE_WINDOWS GAMESCOPECTRL_BASELAYER_APPID'
droiddeckctl guest bash -c 'DISPLAY=:0 xprop -id <window-id>'

# memory sampler used to catch LMKD kills
adb shell "run-as com.droiddeck.launcher sh -c 'echo <base64> | base64 -d > files/linuxfs/tmp/memwatch.py'"
droiddeckctl guest bash -c 'setsid nohup python3 /tmp/memwatch.py >/tmp/memwatch.err 2>&1 </dev/null &'

# host compositor memory breakdown (smaps_rollup does not exist on 4.9)
adb shell "run-as com.droiddeck.launcher sh -c 'cat /proc/<gamescope-pid>/smaps > files/gs.smaps'"
adb exec-out run-as com.droiddeck.launcher cat files/gs.smaps > /tmp/gs.smaps   # then awk over Rss/Anonymous

# CI
gh run list -R JustKarkat/DroidDeck --limit 1
gh run download <run-id> -R JustKarkat/DroidDeck -n gamescope-patched -D /tmp/opencode/gs
cd /tmp/opencode/gs && sha256sum -c gamescope.tzst.sha256 && tar --zstd -xf gamescope.tzst
cp usr/local/bin/gamescope <repo>/app/src/main/assets/linuxfs/usr/local/bin/gamescope
python3 tools/release/runtime_inputs.py record && ./gradlew :app:assembleRelease -q
```

Host repo state at handoff: branch `shield-tv`, fork `JustKarkat/DroidDeck`, latest gamescope
commit `12fcc61` (see `git log --oneline`), plus uncommitted launcher changes (oom guard + session
script). Gamescope patch file: `tools/gamescope/patches/0123-shm-output-fallback.patch` (contains the
0123 shm fallback + 0123/0124 diag logs).

---

## 16. One-paragraph handoff

The stack renders fine for ordinary X clients, but Steam's CEF window never produces a single
completed commit, so gamescope's focus picker (which matches its appID 769 correctly) refuses to
make it the presenting window, and everything downstream stays black. The single most valuable next
step is to log `commit_queue.size()` / `receivedDoneCommit` / `done` for that window (patch in
§9.9): empty queue means Steam isn't drawing (CEF shared-memory/visibility/Xsync problem), non-empty
with `done=false` means gamescope's commit completion is broken (likely our `0123-shm` output
fallback). A synthetic `XClearWindow` on the Steam window is the cheapest way to test whether one
forced damage event breaks the deadlock.
