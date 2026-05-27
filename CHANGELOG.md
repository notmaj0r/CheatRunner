# CheatRunner — Changelog

## v0.11

### Log Quality

#### `[cheat_select]` — Log Only on Change
- `[cheat_select] [game]` log in `/api/cheats/state` handler now only fires when title, cheat path, detected version, or version match status changes — previously logged on every poll interval causing significant noise in the log buffer

#### `[cheats.mem]` — Per-Write Detail Moved to Debug
- `mprotect_rwx`, `copyin`, `int_verify ok` lines in `cr_memory.c` downgraded from `info` to `debug` — these are internal write-mechanism details that repeat 3× per write
- `begin`, `write_rc`, `ext_verify_skip`, `icache_sync` lines in `cr_cheats.c` downgraded from `info` to `debug` — 4 more lines per write; the `[cheats] write[N] … ok` summary line remains at `info` level
- Net result: a mod with 4 writes previously emitted ~28 `[cheats.mem]` info lines; now emits only the 4 summary lines plus any warnings/errors

#### Version Detection — Extended Coverage + Failure Warning
- `read_param_value_by_title_id` now also scans `/mnt/ext0|ext1/user/appmeta/{id}/param.sfo` and `…/external/…` variants — covers games installed on external drives whose appmeta was not found by the previous path set
- `running_state_get()` now also tries `/proc/{pid}/root/user/patch/{titleId}/sce_sys/param.sfo` — picks up update-version SFO via the proc namespace for PS4 BC games where the base `/app0` SFO only has the launch version
- `[warn] [game.ver] version undetected title=… pid=… tried proc=… and static paths` — emitted once when all SFO lookup paths fail for a running game; makes future diagnosis straightforward

### Dashboard Asset Separation (Phase 1)
- `src/dashboard_css.inc` — extracted from `dashboard_html.inc`; served at `/dashboard.css` (Content-Type: `text/css`)
- `src/dashboard_js.inc` — extracted from `dashboard_html.inc`; served at `/dashboard.js` (Content-Type: `application/javascript`)
- `src/dashboard_html.inc` reduced from 2964 → 172 lines; HTML shell now references `/dashboard.css` (link) and `/dashboard.js` (script defer)
- `cr_api_dashboard.c` now serves `/dashboard.css` and `/dashboard.js` from embedded `g_dashboard_css[]` / `g_dashboard_js[]` C string globals

### Launch — Complete Generation Guard
- All intermediate `set_launch_status_ex` calls inside `launch_worker_thread` replaced with `set_launch_status_ex_gen(my_gen, ...)` — stale workers can no longer overwrite newer launch state at any phase (killing_current, waiting_for_close, failed on close timeout, launching_lnc)
- `launch_title()` static function now takes a `uint64_t gen` parameter; its three internal status writes (`verifying_lnc`, `launching_system`, `verifying_system`) are all generation-guarded
- `updated_ms` field added to `launch_status_state_t` — `set_launch_status_locked()` updates it on every write; `/api/launch/status` now exposes `updatedAgeMs` and `generation`
- `launch_rc_is_async_verifiable` renamed to `launch_rc_is_submitted_or_verifiable` — name now matches behavior (rc >= 0 means submitted/async-verifiable, not just async-verifiable)

### Stability & Correctness

#### Launch Thread — Config Lock
- `launch_worker_thread` now snapshots `launch_kill_current`, `launch_wait_timeout_ms`, and `launch_kill_delay_ms` under `g_cfg_lock` in the same locked section as `launch_user_id` — previously those three fields were read without a lock, inconsistent with all other config accesses in the file

#### Launch Thread — Atomic `g_last_launch_verified_at_ms`
- `g_last_launch_verified_at_ms` changed from `volatile uint64_t` to a plain `uint64_t` protected with `__atomic_store_n`/`__atomic_load_n` (`__ATOMIC_RELEASE`/`__ATOMIC_ACQUIRE`) — ensures formal memory-ordering guarantees on writes from the launch worker and reads from HTTP handler threads

#### Launch Watchdog — Generation Guard
- `launch_status_recover_stale()` now snapshots `g_launch_status.generation` in the first locked section and re-checks it after `running_state_get()` before calling `set_launch_status_locked` — aborts recovery if a new launch started during the unlock window, preventing stale recovery from overwriting a valid in-progress launch

#### Config — Invalid File Warning
- `config_load()` counts valid `key=value` lines parsed; if the file exists but yields zero valid entries (binary garbage or completely malformed file), logs `[warn] config file exists but contains no valid key=value entries — using defaults`

#### Cheat Doc — Parse Cache
- `load_cheat_json_root_for_title_ex()` now caches the decoded JSON text (post-decrypt for SHN/MC4) keyed by `(title_id, path, mtime, kind)`; on cache hit only `stat()` + `strdup` + `cJSON_Parse` from memory are needed — eliminates repeated disk reads and MC4/SHN decryption on every `/api/cheats/state` poll interval

#### ptrace — `pt_attach_timed`
- New `pt_attach_timed(pid_t pid, int timeout_ms)` added to `pt.c`/`pt.h` — uses `WNOHANG` poll loop with 10ms steps; returns `-2` on timeout and auto-detaches so the game process is left running
- `apply_cheat_json` (cr_cheats.c) and the state check loop (cr_api.c) now call `pt_attach_timed(pid, 3000)` instead of `pt_attach` — if the game process is in an uninterruptible state, the operation fails cleanly within 3s with a `[warn] pt_attach timeout` log instead of blocking indefinitely

#### Mismatch Debug JSON — Size Cap
- Debug mismatch array cap reduced from 128 to 32 entries
- Per-entry hex fields capped at 32 bytes (64 hex chars) — buffer sizes reduced from 512 to 128 bytes each; `abs_buf`/`rel_buf` from 128 to 32 bytes — bounds max debug JSON output to ~16KB instead of unbounded

### Cheat State — Mismatch Warn Throttle
- Repeated `[warn] mismatch` log spam for already-mismatched mods (e.g. mods sharing a cave page) now throttled to once per 60 seconds per `(titleId, modIndex)` pair; subsequent detections within the window log at `debug` level instead — the mismatch state is still correctly detected and reported in the API, only the log level is suppressed

### Version Logic — Strict Equality
- `cr_version_equal_known()` added to `cr_version.c/h` — returns 1 only if both inputs normalize successfully AND are equal; `unknown/unknown → 0` (safe for exact matching)
- `cr_version_is_known()` added — returns 1 if input normalizes to at least one numeric segment
- `cheat_ver_matches()` in `cr_cheats.c` updated to use `cr_version_equal_known` for the filename equality test — prevents unparseable version strings from counting as exact matches
- `cheat_remote_match_score()` in `cr_remote_sources.c` updated to use `cr_version_equal_known` — replaced `parse_version_triplet` triplet comparison; `cr_version.h` now included; local and remote version matching use the same normalized logic
- `cr_version_from_filename()` trailing dot fix — loop consumed trailing `.` before extension (e.g. `CUSA42556_01.14.json` → `"01.14."`) causing version mismatch; now strips trailing dots before returning

### Launch — Stale Busy Recovery
- `launch_status_recover_stale()` — watchdog called on every `/launch` and `/api/launch/status` request: if launch has been busy past threshold (configurable wait + grace + 15s, clamped 45–120s), auto-recovers to `ready` (game running) or `timeout` (game not detected)
- Generation counter added to `launch_status_state_t` and `launch_worker_request_t` — each new launch increments `generation`; late-completing workers only write final status if their generation still matches
- `launch_begin_ex()` — new atomic function that increments generation and records `started_ms` at the moment a launch is dispatched
- `set_launch_status_ex_gen()` — generation-guarded status write; silently dropped if a newer launch has started
- `/api/launch/status` now includes `ageMs` (ms since launch started) and `stale` (busy and aged past 60s) fields
- `/api/health` now includes `"launch"` key in the `busy` object and a top-level `"launch"` sub-object with `busy`, `phase`, `titleId`, `ageMs`, `stale` — allows external tooling to detect stuck launches
- `Refresh` button is no longer disabled while a launch is in progress — allows recovery from stuck launch states without reloading the page
- `refreshCheatRunner` is now error-tolerant: each sub-task (launch status, game state, games list, logs, cheat modal) runs in its own try/catch; a partial failure shows a warning toast instead of silently aborting
- Launch poll now detects `stale` / `phase === "timeout"` response from backend recovery and shows a "Launch timed out — check if the game started manually" warning instead of silently hanging
- `refreshLaunchStatus` shows a toast when the backend auto-recovers a timed-out launch (transition to `phase === "timeout"`)

### Version Detection
- Running game version now read from live `/app0/sce_sys/param.sfo` first — fixes wrong version for patched/updated games (e.g. CUSA00900 update 01.09 was detected as 01.00)
- Static path scan reordered: `/user/patch/<titleId>/sce_sys/param.sfo` checked before `/user/app/` paths so the update version is preferred
- Added `/proc/{pid}/root/app0/sce_sys/param.sfo` as primary version source — accesses the running game's sandbox filesystem namespace on PS5/FreeBSD, fixing "unknown" game version for games where `/app0` is not directly accessible
- Fixed SFO key lookup order for `contentVersion`: now tries `APP_VER` first (actual game version e.g. "01.14") before `VERSION` (which is the SFO schema/spec version, not the game version)
- Added `applicationVersion` as a JSON key alias when reading param.json — PS5 stores game version under this key, not `contentVersion` or `appVersion`

### API Module Split (Phase 1)
- `src/cr_api_launch.c` — new file containing `handle_launch` and `handle_api_launch_status`, extracted from `cr_api.c`
- `src/cr_api_config.c` — new file containing `handle_api_config` and `handle_api_config_set`, extracted from `cr_api.c`
- `cr_version_from_filename()` added to `cr_version.c`/`cr_version.h` — extracts version token after first `_` in a cheat filename; replaces four private static helpers that were duplicated in `cr_cheats.c`
- `launch_phase_is_busy()` / `launch_phase_is_terminal()` — new inline helpers in `cr_launch.h` for phase classification; eliminates scattered string comparisons

### Cheat Selection — Centralized Version Normalization
- New `cr_version_normalize` / `cr_version_equal` / `cr_version_compare` (in `cr_version.c`) — shared by all version comparisons; handles leading 'v', leading zeros per segment, trailing `.0` segments (`01.09.00` → `1.9`)
- Candidate classification: every cheat file candidate is now classified as `exact`, `wrong_version`, `generic` (no version in filename), or `unknown` (game version not detected)
- Version-aware auto-selection: if game version is known, exact matches win; generics (no version) are allowed as fallback; wrong-version files are **blocked from auto-load** (require manual force)
- Manual cheat file selection: `POST /api/cheats/select?titleId=…&path=…[&force=1]` — allows choosing any candidate; wrong-version requires `force=1`; selection persisted to `/data/cheatrunner/cheat_selections.json`; `POST /api/cheats/select/auto?titleId=…` resets to auto

### Cheat State API
- `/api/cheats/state` now includes `detectedVersion`, `selectedCheatVersion`, `versionMatch` fields
- New `selectionMode` field: `exact` / `generic` / `wrong_version` / `manual` / `none`
- New `candidates` array: per-file `path`, `filename`, `format`, `version`, `versionNormalized`, `match`, `selected`, `score`
- Per-mod `canToggle`, `nextAction` (`enable`/`disable`/`none`), `nextOn`, `blockReason` fields
- MIXED state backend now reports `visualState="mixed"`, `canDisable=true`, `nextAction="disable"` — no longer silently treated as OFF

### Compatibility
- `/data/etaHEN/cheats` added as search path for cross-loader cheat file compatibility
- `/data/elf-arsenal/cheats` added as search path for elf-arsenal cheat file compatibility

### Cheat Engine — Stability
- Fixed double `pt_attach` race on fast consecutive mod applications
- Fixed "refusing enable: no cheat file" error after game stop during re-enable flow
- Fixed `bytes mismatch before OFF` blocking valid disables — `expected_reliable` guard skips verification for MC4/SHN mods without explicit `expected` bytes
- Fixed mod enabled-state not being cleaned up on game exit (`mod_enabled_clear_for_pid`)
- Added idempotent re-enable detection for JSON cheats — if memory already matches ON state (e.g. after CheatRunner restart), re-applies without error
- Fixed cave/hook write ordering: enable applies caves first then hooks; disable applies hooks first then caves
- Fixed cave page classification — short writes (< 16 bytes) on a known-cave page are now correctly promoted to cave type

### Crash Guard
- Crash guard now tracks up to 8 simultaneously active mods (previously only tracked the last one applied)
- Crash suspects are now automatically cleared when the game survives the full watch window

### Notifications
- PS5 on-screen toast notification on every cheat toggle: `CheatRunner: <name> ON/OFF`

### Cheat Sources
- Remote cheat source download from configured repositories
- Auto-download missing cheats on game launch
- Source cache with configurable TTL

### Game List
- SQLite-backed game list via PS5 `app.db` scanner (falls back to folder scan if unavailable)
- Title name lookup — resolves title IDs to game names with cache

### Dashboard
- Web UI served on configurable HTTP port
- Notification center with internal history
- Activity tracking — records last cheat used per title
- Game launch integration from dashboard
- Launch flow fixed: `sceLncUtilLaunchApp` positive return codes (e.g. 8216, 16408) now treated as submitted/verifiable — `sceSystemServiceLaunchApp` is no longer called when LncUtil already submitted successfully
- Launch preflight: if the requested title is already running (per game monitor), CheatRunner reports it as running and skips kill/relaunch entirely
- "Refresh Games" button renamed to "Refresh" — now refreshes full dashboard state (launch status, game list, running state, logs) and reloads the open cheat modal if one is active
- Manual cheat file selection and Reset-to-auto now reload the full cheat document (`/api/cheats`) before re-rendering — fixes stale mod names shown after switching cheat file
- `reloadCheatMenuForCurrentSelection` helper centralizes cheat doc + state reload used by manual selection, auto-reset, and the Refresh button
- Post-launch refresh now calls `refreshGlobalState` and `refreshGames(force)`, and reloads the open cheat modal so version and mod data are up to date after the game starts
- Version comparison now uses `cr_version_normalize` server-side — no false mismatches (e.g. `01.09` vs `01.09.00`)
- Toggle button uses `data-next` attribute instead of CSS class inference — more reliable after failed or partial toggles
- Removed optimistic ON/OFF CSS flip before backend confirmation — button stays pending until refresh
- Fixed ON/OFF button stuck in loading state after toggle — `applyingMods` is now cleared before the final re-render so the button always unlocks automatically
- Toggle button `data-next` now uses backend-provided `nextOn` field instead of CSS class inference; invalid `data-next` shows `blockReason` toast instead of sending a wrong request
- Candidate selector panel in cheat modal: shows all available cheat files with match badges (EXACT / GENERIC / WRONG VER), highlights selected file, allows switching via "Select" / "Force…" buttons, "Reset to Auto-select" clears manual override
- Meta line shows `✔ match` / `⚠ MISMATCH` for cheat vs game version
- `normalizeNextOn` frontend helper — safe coercion of backend `nextOn` to `1`, `0`, or `null`; fixes bug where `undefined` was coerced to `'0'` (disable) instead of blocking the action
- `data-next` attribute now set from `normalizeNextOn(row.nextOn)` — empty string when backend provides no valid action; click handler blocks and shows `blockReason` toast instead of sending a wrong request
- Mixed cheat state no longer treated as OFF — gets its own `is-mixed` class, amber row highlight, amber switch fill, and "PARTIAL PATCH" chip; does not auto-enable unless backend explicitly provides `nextOn=1`
- ON/OFF toggle redesigned as a smooth sliding switch: knob slides with `cubic-bezier(.2,.8,.2,1)`, ON state has green glow, applying state shows a track shimmer sweep (replaced dot/ball spinner), error state is orange-red, mixed state is amber
- Global UI motion polish: buttons gain `translateY(-1px)` hover lift and `scale(0.97)` active press; game tiles lift on hover; cards have smooth border/shadow transitions; all interactions use consistent fast easing
- Candidate selector rows redesigned as cards with left-border accent (green for selected, amber for wrong-version), hover lift, and format/version sub-line below the filename
- Background grid opacity reduced (`0.18 → 0.11`) and circuit lines reduced (`0.75 → 0.48`) — less visual noise, content easier to read
- Global `:focus-visible` outline added for keyboard navigation
- `@media (prefers-reduced-motion)` expanded to cover buttons, cards, game tiles, cheat rows, candidate rows, and support links

---

## v0.1

- Initial release
- ptrace-based memory write engine (`pt_copyin` / `pt_copyout`)
- Cheat format support: JSON, SHN/XML, MC4 (AES-256-CBC encrypted XML)
- Basic cheat enable / disable via HTTP API
- Game process detection and monitoring
