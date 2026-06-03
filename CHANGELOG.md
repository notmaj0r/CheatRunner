# CheatRunner — Changelog

## v0.12

### Dashboard — Motion & Polish

- **Animated view switching.** Switching between **All / ★ Favorites / Recent / PS5 / PS4 / Apps** now plays a staggered fade-and-slide entrance on the game tiles instead of an instant, flat swap — the grid visibly re-flows when you change tabs, search, sort, or toggle a favorite. The same entrance plays once on first load.
- **Active-tab accent.** The selected tab draws an animated brand-gradient underline; tabs lift on hover and depress on click for tactile feedback. Clicking the already-active tab is a no-op (no needless re-render).
- **Empty states fade in** rather than popping.
- Motion is scoped so background polls (status/launch refresh) don't re-trigger it, the entrance class self-clears so tile hover-lift keeps working, and everything is fully disabled under `prefers-reduced-motion`.

### Dashboard — UI Refresh (Icons, Buttons, Loading, Focus)

- **Crisp SVG icons.** The gear (Settings), heart (Support), and Favorites-tab star are now inline SVG instead of emoji/HTML entities, so they render identically and theme-colored across the PS5 browser and phones (matching the existing Discord/X icons). Game tiles get a ▶ icon on **Play** and a crosshair on **Cheats**; the favorite star is a filled/outline SVG pair that reflects state.
- **Primary-button sheen.** Play/primary buttons get a light sweep on hover for a more tactile feel; secondary buttons stay flat for clear hierarchy.
- **Controller & keyboard focus.** All buttons, tabs, inputs, the favorite star, and game tiles now show a clear brand-colored `:focus-visible` ring — D-pad/keyboard navigation is finally visible on console. (Mouse clicks stay ring-free.)
- **Loading skeletons.** The game grid shows shimmering placeholder tiles on first load instead of a blank area, replaced in place once titles arrive.
- **Live game pill.** When a game is running, the header pill shows a pulsing dot.
- **Games-in-view counter** above the grid count-ups as you filter/search.
- **Typography & color depth.** Inactive tabs use a cooler neutral so the crimson active state reads as "selected"; display headings tightened. All new motion respects `prefers-reduced-motion`.

### Dashboard — Favorites, Recents & First-Run Onboarding

- **Favorites** — every game tile now has a star button; favorited games are pinned to the top of the grid and get a dedicated **★ Favorites** tab.
- **Recent** — a **Recent** tab lists games you've recently launched or opened cheats for (most-recent first, capped at 12). Recorded automatically on launch and on opening a game's cheats.
- **Synced server-side** — favorites and recents are stored on the console (`/data/cheatrunner/favorites.json` via the new `/api/favorites` endpoints) instead of per-browser, so they're identical on every phone/PC that opens the dashboard. State updates optimistically and is pushed in the background.
- **First-run onboarding** — on first visit, a short guided overlay runs a system check (privileges / cheat engine / memory patching from `/api/state`), lets you pick a starting profile (Safe / Max Compatibility / Debug presets), and points you to where cheats come from. Shown once (localStorage `cr_onboarded`); dismissible via Skip or Esc.
- Context-aware empty states for the Favorites/Recent tabs.

### Dashboard — Settings Redesign & UX Improvements

- **Settings is now a top-level panel.** Added a **gear button** in the header that opens a dedicated Settings overlay. Previously settings were buried as a tab inside a specific game's cheat-trainer modal — you no longer have to open a game to change global config.
- **One-click presets** (`/api/config/preset`): **Safe** (recommended defaults), **Max Compatibility** (allow unverified/unsafe applies for stubborn MC4/SHN cheats), and **Debug** (verbose logging + diagnostics). Each applies a coherent bundle of related settings at once.
- **Plain-language labels + guidance.** Every option now has a human label and a one-line description instead of raw engine jargon. Recommended options show a green **Recommended** badge.
- **Risk signposting.** Dangerous toggles (force-unsafe applies, apply-unverified) are grouped under "Advanced (can crash games)", tagged with a red **Advanced** badge, and require a confirmation before being turned on.
- **Modified indicator.** Settings that differ from their default show a **Modified** badge.
- **Search** box filters settings live; empty groups hide themselves.
- **Reset to defaults** (`/api/config/reset`) — restores all settings (the live web port is preserved so you stay connected).
- **Theme picker** — the `theme` setting is now wired end-to-end with three dark schemes (Dark, Crimson, Midnight); applied on load and persisted.
- **Report Issue** button copies a diagnostics bundle to the clipboard and opens the Discord, turning manual log-gathering into one click.
- Per-setting saves now show a toast confirmation. New config fields exposed to the UI (`allow_unsafe_mc4_apply`, `allow_unsafe_shn_apply`, `cheat_log_candidates`, `cheat_mark_crash_suspect`, `cheat_apply_one_at_a_time`) with matching `set` handlers.
- Mobile: settings rows stack and controls enlarge on narrow screens.

### Networking — Fix `bind() failed: 48` (port stuck / dashboard disconnects)

- **Root cause: the "kill previous instance" step ran before privilege escalation.** On startup CheatRunner SIGKILLs any prior `CheatRunner.elf` so it can take the HTTP port — but that loop ran in `main()` *before* `jb_escalate_pid`/`cr_priv_init`. A previously-running instance has already escalated itself (uid 0 + a privileged authid), so the un-escalated newcomer's `kill()` was rejected with `EPERM`; the old process kept listening on port 9999, and the new instance's `bind()` then failed with `EADDRINUSE` (errno 48) on every retry — forever. (`SO_REUSEADDR`, which was already set, only reclaims dead `TIME_WAIT` sockets, not a live listener.) This showed up as an endless `[HTTP] bind() failed: 48` flood and, for users, as the dashboard "disconnecting" when a stale instance was left holding the port. Fix: the kill-old-instance loop now runs **after** privilege escalation, so the SIGKILL actually lands; it also logs if a target refuses to die.
- **Bounded, legible bind retries.** The HTTP bind loop no longer spams an identical failure line + toast every 3 s indefinitely. It retries ~5×, then emits a single clear message — for `EADDRINUSE`, *"port N still in use — is a previous CheatRunner running? Reboot the PS5 or close it."* — and backs off to a quieter 10 s cadence while still retrying, so it recovers automatically if the port frees up.

### Stability Audit — ptrace Serialization, State-Read & Cache Hardening

- **Critical: serialized the ptrace credential swap.** `sys_ptrace` in `pt.c` temporarily elevates CheatRunner's process-wide ucred (authid + caps) around each ptrace syscall, then restores it. This ran unserialized while ptrace is invoked from three threads — the cheat-apply thread (`apply_cheat_json`), the game-monitor thread (`patch_restore_all_for_pid`), and HTTP handler threads (dashboard state read, dev endpoints). Concurrent entry let one thread save another's *already-elevated* caps as its baseline and restore the process to a permanently-privileged state, while the other thread's restore dropped privileges mid-syscall (intermittent `pt_attach`/ptrace EPERM failures). Fix: a dedicated `g_ptrace_ucred_lock` now wraps the save→elevate→syscall→restore sequence; also restores `authid` on the partial-failure path (previously left elevated if the caps-set failed)
- **High: game-monitor no longer issues a competing `pt_attach` during an apply.** On game-stop the monitor called `patch_restore_all_for_pid`, which `pt_attach`es the pid. If the game genuinely exited mid-apply, this raced the apply thread's attach and stalled the 500 ms monitor poll for the full 2 s attach timeout (and failed anyway — pid already traced). Fix: the restore is skipped while `g_cheat_applying` is set; backups are still cleared and the next poll re-evaluates once the apply releases. Patched bytes in a dead process are gone with it, so nothing is lost
- **High: dashboard state-read defaults to relative address resolution.** The passive cheat-state poll resolved unverified MC4/SHN addresses via the legacy magnitude heuristic by default, which for large offsets reads the *raw* offset first — on titles where that maps to PS5 GPU/MMIO the ptrace read page-faults and never returns, freezing the game just from opening the cheat page. Fix: the state read now defaults to RELATIVE (`base + offset`, the correct target for these cheats) and treats the deprecated `legacy` value (and any unknown config) as relative; explicit `absolute`/`block` are still honored. Mirrors the relative-first ordering already used by the apply path
- **Medium: address-cache no longer silently truncates.** `addr_cache_save_locked` sized its JSON buffer at 800 B/entry, but a worst-case entry (escaped key + 128-byte orig hex + scaffolding) needs ~1060 B, so the bounded `snprintf` truncated mid-entry → unparseable JSON → the entire learned-address cache was dropped on the next load (forcing re-probing, which is itself the main freeze-exposure path). Fix: buffer sized to 1200 B/entry
- **High (latent): Tier-2 x86 address probe is now relative-first.** The instruction-heuristic probe in `cheat_resolve_write_addr_ex` read *both* address candidates unconditionally, including the raw absolute offset (the GPU/MMIO freeze vector). It is unreachable from the apply and state paths today (both force `auto_detect=0` when there is no reliable baseline), but was a loaded gun for any future caller. Fix: it now reads the relative candidate first and returns immediately when that looks like code, only reading the absolute candidate if the relative one is ruled out — mirroring Tier-1 and the reliable-probe branch
- **Medium: codecave write rolls back on verify failure.** `write_via_codecave` replaces the target page with a `MAP_FIXED` anonymous mapping, writes the original bytes back, then the patch. It freed the saved original *before* verifying, so a failed verify returned −1 while leaving a half-applied patch in the anonymous mapping — which the game could execute and crash. Fix: the original page contents are kept until after the verify and copied back (page re-marked RX) on failure before returning
- **Medium: bounded kinfo_proc offset reads.** `find_pid_by_name` / `find_pid_for_app_id` read fixed offsets into each sysctl process record (`ki_pid` at +72, `ki_tdname` at +447) without checking the record was long enough; a short/malformed final record could read past it. Fix: records shorter than the accessed offset are skipped
- **Low: manual cheat-file selection saved atomically.** `manual_sel_save_locked` used `fopen("w")`+`fwrite`, which can corrupt the selections file on power loss. Fix: switched to `write_file_atomic` (temp + rename), matching every other persisted file
- **Low: `g_ver_warn_last` race fixed.** The version-undetected log-dedup global is touched by `read_running_state` from multiple threads (monitor, apply, HTTP); added a mutex around its read/update (cosmetic correctness — prevents a data race on the dedup state)

### MC4/SHN — Poisoned addr_cache No Longer Bricks Every Cheat

- **Fixed**: a stale/poisoned `addr_cache` entry could make *every* mod of a cheat fail with `entry[N] bytes mismatch before ON`, even on titles that previously worked (reproduced on PPSA30803 — all 7 mods failed off cached entries)
- Root cause: on an `addr_cache` hit the cached `orig_bytes` become the validation baseline (`expected_reliable=1`). With `cheat_address_auto_detect=0`, `cheat_resolve_write_addr_ex` early-returns `rel_addr` as `OK_VERIFIED` **without probing**, so `resolve_st` is never `UNRESOLVED` and the existing stale-cache recovery path is bypassed. Validation then compares live memory against the poisoned cached bytes and hard-fails. (Cave entries slipped through via the lenient `cave_overwrite` branch; short hook entries had no escape.)
- Fix: when the pre-ON validation mismatch baseline came from the `addr_cache`, CheatRunner no longer fails the apply — it clears the poisoned cache entry and proceeds as an unverified write at the resolved (`base+offset`) address. Self-heals a bad cache instead of bricking the cheat, and is reached regardless of the `auto_detect` setting
- Instant recovery without rebuilding: delete `/data/cheatrunner/addr_cache.json` and relaunch — the cache rebuilds clean

### MC4/SHN — Cheat Pages Left RWX After Write (Fixes Freeze-After-Apply)

- **Fixed**: applying a code-cave MC4/SHN cheat could freeze the game shortly after, even when the cheat wrote successfully to the correct addresses (reproduced on PPSA20560 with ASLR off — `base=0x400000`, so addressing was provably exact, yet it still froze)
- Root cause: `write_process_memory_forced` set the target page `RWX` for the write, then **restored it to `R-X`** (stripping WRITE). That's fine for a hook in a pure code page, but MC4/SHN code caves sit well past `.text` (e.g. `base+0x3725da0`) in pages the **game itself writes to**. Removing write from such a page makes the game's next store there page-fault → freeze on resume
- Fix: both `write_process_memory_forced` and `write_via_codecave` now leave the page `PROT_READ|PROT_WRITE|PROT_EXEC` after writing. This preserves both execution of the cave and the game's own writes to that page. More-permissive only (adds WRITE) — cheats/patches that worked under R-X are unaffected
- `base=0x400000` in the logs is ASLR being off for that launch (not a wrong base) — it confirmed the freeze was page-protection, not addressing

### MC4/SHN — Code-Cave & Master-Code Defaults Enabled

- Enabled `cheat_codecave_fallback` and `cheat_master_code_fixup` by default (both were off):
  - **codecave_fallback** — when a cheat write doesn't land in real writable memory, CheatRunner `pt_mmap`s an executable page at that address (the cave) and retries, instead of failing the cheat. Needed for MC4/SHN cheats whose code-cave region (e.g. a shared `0x37xxxxx` cave table) isn't backed by the eboot
  - **master_code_fixup** — for MC-dependent mods it scans the live master-code region for the dependent's bytes and corrects the offset
- Both are **additive**: codecave_fallback only engages when a write fails/doesn't verify; master_code_fixup only for mods whose name marks them MC-dependent. Cheats that already applied cleanly are unaffected
- Note: this does **not** change CheatRunner's "no reliable baseline" safety gate. CheatRunner still refuses to apply unverified MC4/SHN by default (shown as `BASELINE_UNKNOWN`) unless `allow_legacy_mc4_without_expected` / `allow_legacy_shn_without_expected` is set — that remains an explicit user opt-in because force-applying an unverified cheat at a guessed address can crash the game

### Shutdown / Reload — Robust SIGKILL

- **Fixed**: "Shutdown Payload" / reload could appear to do nothing — the new CheatRunner sent from the PC would never come online, or the old one seemed to keep running
- Two root causes, both addressed with a bounded SIGKILL-then-verify approach:
  - **New instance hung at boot**: `main()` kills any previous CheatRunner by name in a loop before starting. A process that has been SIGKILL'd but not yet reaped by its loader lingers as a **zombie** that still appears in the process list, so `kill()` keeps "succeeding" on it and the old unbounded `while (find_pid_by_name(...) > 0) { kill; sleep(1); }` spun **forever** — the new instance never reached the HTTP server. Now bounded (≤6 s of 100 ms retries) and then proceeds: a dead/zombie process holds no socket, so the bind still succeeds
  - **Self-shutdown had no fallback**: the delayed shutdown thread (and both fallback paths) called `kill(getpid(), SIGKILL)` and, if that was somehow refused/deferred, simply returned — leaving CheatRunner alive. Added `_exit(0)` immediately after each self-SIGKILL so the process always terminates
- Net effect: shutdown reliably kills the running instance, and a freshly-sent CheatRunner.elf reliably takes over even if the previous one is mid-teardown or unreaped

### Patches — Fixed Mid-Apply Failure from Concurrent ptrace (Bloodborne 60 FPS et al.)

- **Fixed**: applying a multi-line patch could fail partway through with `mprotect_failed page=0x… rc=-1` → `cave read orig failed` → `write_failed`, then roll back (observed on CUSA00900 "60 FPS (With Deltatime)" at line 47). The backup phase had just read every line's address successfully, so the page was valid — it became inaccessible *during* the write loop
- Root cause: the patch-apply, single-patch-restore, and game-stop restore paths were the only ptrace users that held **no shared lock and set no busy-flag**. Every other path (cheat apply, dashboard state poll) coordinates through `g_cheat_apply_lock` + `g_cheat_applying`. So while a patch apply held the game under ptrace, the periodic dashboard state poll (every ~500 ms) called `pt_attach` on the same pid; that second attach fails (pid already traced) and its cleanup issues `PT_DETACH`, tearing down the patch thread's ptrace session. The next `pt_copyin`/`kernel_mprotect` in the patch loop then failed — whichever line happened to be executing when the poll fired
- Fix: `g_cheat_apply_lock` is now the single global ptrace-session gate (exposed via `cr_cheats.h`). The patch paths participate in it exactly like the cheat path:
  - `patch_apply_entry_ex` and `patch_restore_entry` (user actions) take the lock **blocking** across attach→write→detach and set `g_cheat_applying` so the dashboard poll backs off
  - `patch_restore_all_for_pid` (game-monitor thread, on game stop) takes it **non-blocking** (trylock) so the 500 ms poll never stalls and can't deadlock — deferring the restore one tick if ptrace is busy
- Net effect: no two threads can ever hold the game pid under ptrace simultaneously, so a patch apply can no longer be torn down by a concurrent state read. Cheat apply was already correct and is unchanged

### Patches — Large Patches No Longer Time Out (Batched ptrace Credential Elevation)

- **Fixed**: with the tear-down race resolved, large patches (e.g. Bloodborne "60 FPS (With Deltatime)", ~130 lines) now run to completion instead of failing early — but the apply was so slow it exceeded the HTTP request timeout (observed progressing past line 100 then "request timed out")
- Root cause: every `pt_copyin`/`pt_copyout` routes through `sys_ptrace`, which raises this process's ucred (≈4 kernel credential operations) and restores it **per call**. A 130-line patch performs a write + readback-verify per line — thousands of ptrace ops, hence thousands of kernel credential operations, which on PS5 kernel-RW is slow enough to blow the request timeout
- Fix: added `pt_batch_begin()` / `pt_batch_end()` to the ptrace layer. They raise the credentials **once** and hold the ucred lock; every `pt_*` call on the same thread until `pt_batch_end()` then skips the per-call swap and issues only the bare syscall (begin/end nest via a depth counter and are balanced). This is safe because the patch paths hold the global ptrace gate, so no other thread does ptrace while a batch is open
- The patch-apply, single-patch-restore, and game-stop bulk-restore loops are now wrapped in a batch — cutting roughly `2 × lines × 4` redundant kernel credential operations per apply. The cheat-apply path (few writes) is unchanged
- No behavior change to what gets written or verified — only the per-op credential overhead is eliminated

### MC4/SHN — Cache-Backed Cheats Can Now Be Re-Enabled After Disable

- **Fixed**: an MC4/SHN cheat with a learned addr_cache baseline could not be turned back on after being turned off — re-enable failed with `entry[N] bytes mismatch before ON at 0x…`, and the only recovery was relaunching the game (observed on PPSA30803 mod 0 "Untouchable")
- Root cause: on enable, the validator compared current memory against `must = exp_bytes` (the *original* game bytes captured in the addr_cache at first enable). But a disabled cheat legitimately holds its `ValueOff` bytes, and for MC4/SHN cheats ValueOff frequently differs from the captured original. So after a disable cycle `cur_bytes == off_bytes != exp_bytes` → "mismatch before ON" and the apply was rejected
- The bug was instruction-dependent: 5-byte `E9` JMP hooks happened to pass through the existing `hook_redirect` branch (`on_bytes[0]==0xE9 && cur_bytes[0]==0xE9`), so cheats like mod 1 "Infi Health" re-enabled by luck; 6-byte/non-`E9` hooks (mod 0) had no escape and failed every time. Unverified mods (no cache baseline) were unaffected because they skip the strict check
- Fix in `apply_cheat_json`: the pre-ON validation now accepts **either** the captured original bytes **or** the cheat's own `off_bytes` (ValueOff) as a valid baseline — a cheat sitting in its disabled state is a correct precondition for enabling, for any hook width or instruction type. This mirrors the ValueOff-acceptance logic that already existed in the address-resolution stale-cache recovery path

### Config — Hot-Reload on File Change

- CheatRunner now detects external edits to `/data/cheatrunner/config.ini` and reloads the config automatically, without requiring a payload restart
- Implementation: `config_check_reload()` is called once per game-monitor tick (~500 ms); it calls `stat()` on the config file and compares `st_mtime` against the last-seen value; reload only fires when the mtime changes
- `config_load()` now captures the file's mtime immediately after a successful read, so the first monitor tick never triggers a spurious reload at boot
- Reload is logged: `[info] [config] config file changed on disk, reloading` followed by the standard `[info] [config] config loaded` line
- Practical use: edit the config over FTP/SSH, changes apply within ~500 ms with no restart

### Cheats — Source: HEN-PPSA-Cheats

- Added **HEN-PPSA-Cheats** (`RDX-Sci01/HEN-PPSA-Cheats`, branch `main`, path `cheats`) as the fourth default remote cheat source, enabled by default
- Existing installations with a saved sources config are unaffected; the new source appears automatically on fresh installs or after resetting sources

### Cheats — 1-Second Delay Between "Disable All" Calls

- `GET /api/cheats/disable-all` now waits 1 second between each successive cheat disable call — prevents rapid back-to-back memory writes that could destabilise the game when multiple cheats are active simultaneously
- The delay is inserted only between calls, not before the first or after the last; mods that are already disabled are skipped without delay

### Dashboard — Refresh Button Rescans Cheat and Patch Directories

- The **Refresh** button now calls `/api/patches/rescan` on every click, invalidating the patch index so any XML files dropped manually into the patches directories are picked up immediately without a restart
- The cheat menu reload (`reloadCheatMenuForCurrentSelection`) now runs unconditionally when a title ID is known — previously it only fired when the cheat modal was already open; new cheat files dropped into the cheats directory are now reflected after a Refresh regardless of modal state

### Cheats — PARTIAL PATCH No Longer Shown for Disabled MC4/SHN Hook-Codecave Cheats

- Fixed: after disabling a `hook_codecave` MC4/SHN cheat that has no explicit `expected` bytes, the state probe could show `MIXED` ("PARTIAL PATCH" chip in the UI) instead of `BASELINE_UNKNOWN` (displayed as OFF), permanently blocking re-enable
- Root cause: for a two-entry cheat (cave + hook), the cave entry's ValueOff bytes (zeros) matched current memory → `off_val_matches++`; the hook entry's ValueOff bytes did not match → `baseline_unknown++`; the `BASELINE_UNKNOWN` decision branch required `off_val_matches == 0`, so the mixed `(off_val_matches=1, baseline_unknown=1)` combination fell through to `MIXED` — even though `on_matches == 0` and `mismatch == 0` confirm the cheat is not active
- Fix: removed `&& off_val_matches == 0` from the `BASELINE_UNKNOWN` branch in `cr_api.c`; the invariant is `on_matches == 0 && mismatch == 0` — if no entry matches its ON bytes and no reliable-baseline bytes mismatched, the cheat is not active regardless of how many entries soft-match their ValueOff; `MIXED` is now only reachable when `on_matches > 0 && on_matches < total` (a genuinely partial apply)

### Patch Engine — Line Limit Raised from 64 to 640 (Critical: 60 FPS Patches Now Apply Fully)

- **Critical**: `PATCH_MAX_LINES = 64` silently truncated any patch with more than 64 lines — the "60 FPS (With Deltatime)" patch for Bloodborne has 130 lines; the frame-interval write at address `0x0243487e` (around line 90) was never applied, leaving the game at 30fps despite the patch reporting success; other affected patches: "60 FPS" patches in other games (up to 283 lines), "Enable Dev Menu" (371 lines), "Restore Debug Camera" (569 lines)
- Fix: `PATCH_MAX_LINES` raised from 64 → 640 in `cr_patch_parser.h` — covers the largest patch in the current repository (569 lines) with margin; the parser loop already stopped cleanly at the cap, so no structural change was needed
- `PATCH_BACKUP_STORE_MAX` reduced from 64 → 16 in `cr_patch_parser.c` — the static `g_patch_backups` array size is proportional to `PATCH_MAX_LINES`; keeping 64 backup slots at 640 lines each would have grown the BSS section to ~8.9 MB; 16 slots cover all realistic simultaneous-patch scenarios at ~2.8 MB
- `PATCHES_ALL_MAX` in `cr_api_patches.c` changed from `PATCH_MAX_ENTRIES × PATCHES_MAX_SOURCES` (768) to a fixed 128 — bounds the temporary heap allocation for the merged-entry list; with larger `patch_entry_t` (640-line array) the old formula would have required ~272 MB per API request; 128 slots cover 64 entries × 2 source files, more than any game in the current patch repository
- Added `[warn] [patches] line_limit_hit entry='…' file=… cap=… dropped=…` log line when a PatchList has more lines than the cap — truncation is no longer silent

### MC4/SHN — Multiple Mastercode Group Support

- Fixed: `find_master_code_mod` always returned the first mastercode in the file, so mods belonging to a second mastercode group (e.g. mod[9] "Unlimited Health" depending on mod[5] "Speed Mastercode") would auto-enable the wrong mastercode (group 0's) before applying — `mc_scan_dep_addr` then scanned the wrong payload and wrote to a wrong address
- Renamed to `find_master_code_mod_for(mods, target_mod_idx)`: instead of returning the first mastercode, it returns the **nearest preceding** mastercode relative to the target mod index; for single-group files the result is identical; for multi-group files each group's mods correctly auto-enable their own mastercode
- Both callers updated: the auto-enable path (`apply_cheat_json` pre-lock) and the MC fixup setup

### MC4/SHN — State Now Shows OFF_UNVERIFIED Instead of BASELINE_UNKNOWN After Restart

- Fixed: after a CheatRunner restart, all MC4/SHN cheats that had been disabled showed `BASELINE_UNKNOWN` (displayed as OFF but with uncertain label) because the in-memory session tracking (`g_mods_disabled`) was cleared — the state handler had no way to distinguish "never applied" from "applied and disabled" without that tracking
- Fix: in the per-entry state loop, when `off_reliable == 0` (no explicit expected bytes), current memory is now compared against `ValueOff` bytes; if they match the entry is counted as `off_val_matches` (soft OFF) rather than `baseline_unknown`; when all entries match ValueOff, state reports `OFF_UNVERIFIED` instead of `BASELINE_UNKNOWN` — this survives restarts because the comparison reads live game memory, not ephemeral session state
- Similarly, if ALL entries match `on_bytes`, the state already correctly reports `CHEAT_STATE_ON` (memory read is authoritative); the session tracking is now only needed for the mixed/ambiguous case
- State decision tree updated with `off_val_matches` counter alongside the existing `baseline_unknown`, `off_matches`, and `mismatch` counters

### MC4/SHN — Conflict Map No Longer Produces False Positives Between Mastercode Groups

- Fixed: `conflict_map_build` compared all mod pairs by raw offset without any awareness of mastercode groups; two mods from different groups (e.g. "Infinite Stamina" under mastercode A and "Speed 2x" under mastercode B) that happened to share a raw offset were flagged as conflicting, blocking the user from enabling them simultaneously even though at runtime the auto-enable system treats them as independent
- Fix: before the O(n²) pair loop, each mod is assigned a `group_id` = index of its nearest preceding mastercode (−1 = before any mastercode, −2 = IS a mastercode); pairs where both mods have defined but different group IDs are skipped
- Additionally: mastercode vs its own dependents (mod_group[j] == i when mod_group[i] == −2) are no longer flagged as conflicting — enabling a dependent requires the mastercode to be active, not the other way around

### MC4/SHN — Crash Suspects Auto-Expire After 7 Days

- Fixed: crash suspects loaded from disk (`crash_suspects.json`) had no TTL and accumulated permanently — a cheat flagged after a bad address resolve (which may have since been fixed) blocked re-enabling indefinitely until the user manually clicked "Clear Crash Flags"
- Fix: `crash_suspects_load()` now computes the current time and discards any suspect whose `ts` timestamp is more than 7 days old; a warn log reports how many were discarded on startup

### MC4/SHN — `mc_scan_dep_addr` Skips Uniform Off-Bytes

- Fixed: when a dependent cheat's `ValueOff` bytes are all identical (all zeros, all NOPs, etc.) the scan loop matched the first occurrence of that byte in the MC code cave — a meaningless result that caused the cheat write to land at a random offset inside the mastercode payload
- Fix: before scanning, the function checks if all bytes in `dep_off` are identical; if so, it skips the scan and falls through directly to the low-byte formula fallback, which is more reliable than a false scan match

### MC4/SHN — Section Field Now Resolves Dynlib Module Base

- Changed: entries with `section > 0` previously logged a "not yet mapped to module" warning and silently used the eboot base — the cheat wrote to the wrong address for any entry targeting a dynamically loaded library
- Fix: `section_num` is now passed to `kernel_dynlib_mapbase_addr(pid, section_num)` to attempt resolving the module base for that entry; if the call returns a valid base it is used as `entry_base` instead of `mod_base`; if the call fails (module not loaded at that handle index) the code falls back to eboot base and logs a warn explaining the failure
- The section base is per-entry (not per-mod), allowing mixed entries in the same mod

### MC4/SHN — Address Cache No Longer Poisoned by ValueOff Bytes on Stale Recovery

- Fixed: when the addr_cache held stale expected bytes (game binary updated) and the recovery path detected that current memory matched `ValueOff` bytes, it updated `exp_bytes = off_bytes` and re-stored them in the cache — subsequent applies treated off_bytes as a reliable baseline; after a game update the new original bytes != off_bytes, causing false MISMATCH and blocking the cheat
- Fix: the `match_off` recovery branch no longer updates `exp_bytes`/`exp_len` or calls `addr_cache_set`; it uses the cached address (confirmed correct by the off_bytes match) and proceeds in unverified mode, which is safe with the default `allow_legacy_mc4_without_expected=1`

### Mod Session Tracking — Increased Array Cap from 64 to 256

- Fixed: `g_mods_disabled` and `g_mods_enabled_arr` were capped at 64 entries; when more than 64 mods across all titles had been toggled in a session, the 65th and beyond were silently dropped from session tracking — `BASELINE_UNKNOWN` could not be promoted to `ON_UNVERIFIED`/`OFF_UNVERIFIED` for those mods, and conflict auto-disable could fail silently
- `MOD_DISABLED_MAX` raised from 64 → 256 in `cr_cheats.h`

### Format Parser — Warn Instead of Silently Dropping Oversized Cheat Elements

- Fixed: `shn_xml_to_json` silently skipped `<Cheat>` elements whose header exceeded 2048 bytes and `<Cheatline>` elements exceeding 4096 bytes — the user had no indication a cheat was missing from the mod list
- Fix: both skip paths now emit `[warn] [cheat_fmt]` log lines with the element size and the limit, making truncation visible in the log panel

### Bug Fix — "Disable All Cheats" Button Stuck on "Disabling…" After Re-Enable

- Fixed: clicking "Disable All Cheats" successfully disabled all active mods, but subsequently re-enabling any cheat caused the button to reappear stuck — showing "Disabling…" and permanently disabled
- Root cause: the success path of the `disableAllCheatsBtn` click handler set `btn.disabled = true` and `btn.textContent = 'Disabling…'` at click time, then called `renderCheatMenu()` which hid the button (`display:none`) because `activeCount` dropped to zero; the DOM `disabled` and `textContent` properties were never reset on success — only the `catch` path restored them; when a cheat was later re-enabled, `renderCheatMenu()` showed the button again (`display:inline-flex`) but the stale `disabled=true` / `textContent='Disabling…'` state was still in the DOM
- Fix: `btn.disabled = false` and `btn.textContent = 'Disable All Cheats'` added to the success path immediately after `renderCheatMenu()`, matching the existing reset already present in the `catch` path

### Bug Fix — MC Scan Fallback Address Missing `mod_base`

- Fixed: in `mc_scan_dep_addr()`, the fallback path (reached when the dependent cheat's off-bytes are not found within the live MC region) returned a raw offset `(mc_base_off & ~0xFF) | (dep_raw_off & 0xFF)` without adding `mod_base` — the caller assigned this value directly to `addr` (an absolute process address), so the write landed at a small, wrong address (e.g. `0x8b6bXX` instead of `mod_base + 0x8b6bXX`)
- Impact: when MC fixup (`cheat_master_code_fixup=1`) was active and the byte-scan in the MC region failed to locate the dependent cheat's off-bytes, the cheat write targeted the wrong memory location; depending on what was mapped there, this corrupted unrelated memory or was silently remapped via the codecave path — in either case the intended cheat code was never reached, and the corrupted region could cause a game freeze when executed
- Fix: `mod_base` is now derived as `mc_addr - (intptr_t)mc_base_off` (since `mc_addr` is already the absolute process address of the MC region) and added to the combined offset before returning — producing the same absolute address that `cheat_resolve_write_addr_ex` would have computed for the same offset via its relative path

### Title Lookup — PlayStation Store Fallback

- New `cr_store_lookup.c` module: when the local appdb and param.sfo fail to resolve a game name, CheatRunner now fetches the PlayStation Store product page (`store.playstation.com/en-us/product/<contentId>`) and extracts the title from the JSON-LD `<script id="mfe-jsonld-tags">` block
- Integrated into `handle_appdb_lookup` as a last resort before falling back to the raw title ID string — only fires when `title_lookup_enabled=1` (default) and the contentId is available from the SFO
- Result is cached to disk in the existing title-name cache directory so subsequent requests are instant; `source` field in `/appdb/lookup` response reports `"ps_store"` when this path resolves the name
- Timeout controlled by existing `title_lookup_timeout_ms` config key (default 3000 ms)

### Eboot Dump — Force Re-Dump and Cache Delete

- `GET /api/cr/eboot?titleId=X&force=1` — new `force` parameter bypasses the cached `eboot.dec` file and always performs a fresh memory dump from the running game; useful after a game update changes the binary layout
- `GET /api/cr/eboot/delete?titleId=X` — new endpoint deletes the cached `eboot.dec` and `eboot.base` files for the given title, freeing storage without requiring FTP access; returns `{"ok":true}` even if the files were already absent

### Cheats — Disable All Active Mods

- `GET /api/cheats/disable-all?titleId=X` — new endpoint that reverts all currently active cheats for the running game in a single call: iterates every mod in the cheat file and calls `apply_cheat_json(i, 0)` for any mod not already in the disabled list; returns `{"ok":true,"disabled":<count>}` — count is the number of mods whose off-bytes were successfully written back; safe to call when no cheats are active (returns `disabled:0`)

### Patch Engine — Global Enable / Disable

- `GET /api/patches/global` — returns `{"ok":true,"enabled":true|false}` reflecting whether any live XML patch directory exists
- `GET /api/patches/global/set?on=0|1` — enables or disables all three XML patch directories at once (`xml_prospero`, `xml`, `elf-arsenal/patches/xml`) by renaming each live directory to `<dir>.off` (off) or back (on); invalidates the patch index so the next lookup rescans; returns `{"ok":true,"enabled":<new state>}`
- New `patch_global_enabled()` / `patch_global_set(int on)` functions in `cr_patch_parser.c`

### Bug Fix — `handle_api_state` cheatFormat Always Reported as "json"

- Fixed: when a running game had an SHN or MC4 cheat file, `GET /api/state` always reported `"cheatFormat":"json"` — the function built the response body twice, and the first build used a hardcoded `"json"` string instead of the actual format; the second build (which used the correct `cheat_fmt` variable) immediately overwrote the first, so the bug was masked in responses but the wasted first snprintf ran on every call
- Fix: pre-compute a `cheat_format_field` string (`"\"shn\""`, `"\"mc4\""`, `"\"json\""`, or `"null"`) once before the if-block and use it in a single snprintf; the redundant second build and the `fixed[4096]` stack buffer are removed

### Cheats — MC4/SHN Baseline Check Respects allow_legacy Flag Unconditionally

- Fixed: `allow_legacy_mc4_without_expected=1` (the default) had no effect when the address resolution result was `AMBIGUOUS` — the validate_original check required `is_legacy_unverified=true` AND `allow_legacy=1`, but `AMBIGUOUS` is not in the `is_legacy_unverified` set, so `!(false && true)=true` → "no reliable baseline" error fired even with `allow_legacy=1`
- Fix: condition changed from `if (!allow_unsafe && !(is_legacy_unverified && allow_legacy_noex))` to `if (!allow_unsafe && !allow_legacy_noex)` — `allow_legacy` now unconditionally bypasses the baseline check regardless of resolve status; the `is_legacy_unverified` flag is no longer load-bearing for the allow path
- This is safe because `allow_legacy_mc4_without_expected=1` is the user's explicit consent to write without verified baseline; the resolve status type does not change that permission
- Combined with the cave_null_target fix below, all MC4/SHN cheats with `allow_legacy=1` (default) now proceed through both guards: cave null is bypassed with a warn log, and baseline check is bypassed regardless of whether the resolver flagged the address as "unverified" or "ambiguous"

### Cheats — MC4/SHN Cave Null Target Respects allow_legacy Flag

- Fixed: `cave_null_target` guard blocked cave writes (len≥16) to null-byte addresses even when `allow_legacy_mc4_without_expected=1` (or the SHN equivalent) was set — the guard ran before the `allow_legacy` check at line 1553, so the user's explicit permission to proceed without expected bytes was never consulted
- Root cause: `allow_legacy_mc4_without_expected=1` (the default) is designed to allow writes without a baseline; `cave_null_target` was a secondary guard added later that unconditionally blocked those same writes before they reached the `allow_legacy` path
- Fix: `cave_null_target` now checks `allow_legacy_mc4` / `allow_legacy_shn` / `allow_unsafe` before blocking — if any is set, it logs `cave_null_target_bypassed` at warn level and proceeds; the hard block only fires when all three are disabled (i.e. the user has not accepted unverified writes)
- Affected games: PS5-native (PPSA\*) titles with MC4 cheat files whose cave offsets resolve to null-byte addresses under both absolute and relative interpretation — a common condition when the game hasn't fully populated those memory regions at apply time

### Logging — klogsrv Welcome Banner

- On startup, CheatRunner writes an ASCII art banner to `/dev/klog` (klogsrv stream) before any other log output — visible in any klog viewer or TCP client connected to port 3232
- Banner is written only to klogsrv; it does not appear in the in-memory log ring or via `printf` so it does not pollute the dashboard log panel
- Implemented as `cr_log_klog_banner()` in `cr_log.c`, called first in `main()` before privilege setup and version log lines

### Logging — klogsrv Integration

- CheatRunner now forwards every log line to klogsrv via `sendsyslog` (PS5 syscall `0x259`) — the same mechanism used by kstuff.elf, elfldr.elf, and all other PS5 payloads to write into the kernel log that klogsrv (TCP port 3232) streams to clients
- Each line is formatted as `<118>[CheatRunner] [level] [tag] message\n` — the `<118>` syslog priority prefix (facility=kernel, severity=info) is required for messages to appear in the klog stream; without it the syscall is ignored
- Both `cr_log()` and `log_msg()` call `klog_send()` after their existing `printf` + `log_push` paths; the log level filter still applies
- Zero configuration and zero overhead when klogsrv is absent — the syscall fails silently if no one is reading `/dev/klog`

### Shutdown — SIGKILL Self-Termination

- `cheatrunner_request_shutdown()` now terminates the process with `kill(getpid(), SIGKILL)` instead of `exit(0)` — all three shutdown code paths (delayed thread, malloc-fail fast path, thread-create-fail fast path) use SIGKILL
- `exit(0)` runs atexit handlers and flushes stdio buffers, both of which can block indefinitely if any thread is stuck in a kernel call (e.g. a hung ptrace or vmem query); SIGKILL is unconditional and cannot be caught, blocked, or delayed
- The graceful cleanup sequence (set `g_shutdown_requested`, stop game monitor, close HTTP socket, save activity, save notifications) is preserved and runs before SIGKILL — only the final termination mechanism changed
- Kills the CheatRunner process externally with `kill(pid, SIGKILL)` Thx to elf-arsenal.

### Dashboard — Thumbnail Cache Corruption on Concurrent Requests

- Fixed: when the dashboard loaded with many games, concurrent icon requests for the same title could corrupt the cached thumbnail — two HTTP handler threads both found the cache file missing, both read the source PNG, and both called `write_file_atomic` on the same destination; `write_file_atomic` uses `path.tmp` as its temp file name, so the second thread's `open(O_CREAT|O_TRUNC)` truncated the first thread's in-progress write; the two writes then interleaved bytes into the same temp file; the last `rename()` landed a corrupted PNG in the cache, causing the icon to appear broken on every subsequent request
- Fix: `write_file_atomic` now appends a per-call atomic sequence number to the temp file name (`path.N.tmp`), guaranteeing each concurrent write uses a distinct temp file; both renames succeed, the last one wins with valid content (both threads wrote the same source bytes), and no data is interleaved
- The fix also covers concurrent writes to the same CheatRunner internal JSON files (addr_cache, crash_suspects) which share the same `write_file_atomic` code path

### Cheats — Pre-Write Log Missing for X86-Probe Addresses

- Fixed: `pre_write_bytes` diagnostic log was not emitted when the x86 instruction heuristic probe found a clear winner (`CR_ADDR_RESOLVE_OK_X86_PROBE`) — only the three `UNVERIFIED_*` statuses were in the `is_legacy_unv` check; the baseline guard at the same site was already updated in v0.12 to include `CR_ADDR_RESOLVE_OK_X86_PROBE`, but the pre-write logging check was a separate variable that was missed
- Impact: when the x86 probe picked an address (absolute or relative) and wrote to it, the `[info] [cheats.mem] pre_write_bytes addr=… cur=[…]` line was never emitted — the only diagnostic evidence of what bytes existed at the target address before the write was silently absent; this made it impossible to tell from logs alone whether the probe chose the correct candidate
- The null-byte guard (`pre_write_hint_null`) similarly did not fire for x86-probe results, so misdirected writes to data/BSS memory were not flagged
- Fix: `CR_ADDR_RESOLVE_OK_X86_PROBE` added to `is_legacy_unv` in the pre-write logging block, consistent with the baseline guard check immediately below it

### Patch Engine — Firmware 12.70 Write Fix (PS4 BC)

- **Critical**: On firmware 12.70, `kernel_mprotect` fails for PS4 BC game process code pages (e.g. Driveclub / CUSA00093 address `0x1187caf`) — Sony tightened hypervisor-level protection for certain vnode-backed pages in PS4 BC mode, preventing direct vm_map modification even with kernel-level access; `write_process_memory_forced` returned `-4` immediately and the patch was never written
- Fix: when `kernel_mprotect(RWX)` fails in `write_process_memory_forced`, CheatRunner now falls back to `write_via_codecave` — which uses `pt_mmap(MAP_FIXED | MAP_ANONYMOUS)` in the game process's own context to replace the vnode-backed page with a fresh anonymous page, writes the patch bytes, then calls `kernel_mprotect(RX)` on the newly-anonymous page (which succeeds); the fallback path logs `[info] [cheats.mem] mprotect_failed page=… rc=… — codecave fallback`
- `write_via_codecave` maximum write size increased from 128 → 256 bytes to match `PATCH_LINE_MAX_BYTES`; previously any patch line over 128 bytes would silently fail the codecave path
- Both the apply path and the rollback path (`do_rollback`) call `write_process_memory_forced`, so rollback on 12.70 now also uses the codecave fallback instead of also failing with `rollback_failed addr=… rc=-4`
- The `canPatchGameMemory: true` priv check is not affected — it tests mprotect on a different region and still passes correctly

### HTTP Server — Sleep Mode Reconnect

- Fixed: after the PS5 enters and exits sleep mode, CheatRunner's HTTP server stopped accepting connections and required re-sending the ELF — the browser appeared permanently disconnected
- **Root cause**: when the network interface drops during sleep, `accept()` returns a non-transient error (e.g. `EBADF`); the error handler did `sleep(1); continue`, retrying `accept()` on the same dead socket forever — the outer `while` loop that closes the broken socket and creates a fresh one was never reached
- Fix: non-transient `accept()` errors now `break` out of the inner loop; the existing outer loop closes the dead socket, waits 1 second, creates a new socket, and rebinds — the outer loop's bind-retry path (3-second sleep on `bind()` failure) handles the case where the network interface is not yet back when the reconnect attempt fires
- `g_listen_ip` and the `Listening on: IP:PORT` log line are now updated on every successful bind, not only on first boot — previously the dashboard displayed a stale IP after reconnect, and the new address was never logged if the IP changed after a DHCP renewal

### Crash Diagnosis — Persistent Crash Suspects

- Crash suspects are now saved to `/data/cheatrunner/crash_suspects.json` on disk and reloaded on startup — previously suspects were lost on CheatRunner restart, allowing unknowingly re-enabling cheats that caused a crash
- `crash_suspects_save()` is called whenever suspects are added (game crash detected), auto-cleared (mod survived watch window), or manually cleared via the UI button
- On startup, up to 32 previously recorded crash suspects are restored; a log line reports how many were loaded

### Crash Diagnosis — Pre-Write Byte Logging

- In `legacy_unverified` mode (MC4/SHN files without `expected` bytes), CheatRunner now always logs the bytes currently at the target address immediately before writing — previously this required enabling `cheat_log_candidates` and debug mode
- New `[info] [cheats.mem] pre_write_bytes addr=… len=… format=… cur=[…]` log line emitted per write entry in unverified mode; aids in diagnosing wrong-address issues and version mismatches without requiring config changes
- New `[warn] [cheats.mem] pre_write_hint_null` warning when the target address starts with three or more null bytes — indicates the address may point to data or uninitialized memory rather than executable code; suggests checking `cheat_mc4_unverified_fallback` / `cheat_shn_unverified_fallback` config

### Cheat Engine — Cave Overwrite and JMP Redirect

- **Cave overwrite**: When enabling a cheat whose code cave (`len ≥ 16`) already contains bytes from a different active mod (e.g. switching between mutually exclusive Speed Normal / Speed 3x / Speed 5x cheats that share a cave address), CheatRunner now allows the overwrite instead of blocking with "bytes mismatch before ON" — the cave is free-form writable space and can safely be replaced
- **JMP redirect**: When enabling a cheat whose hook entry (`on_bytes[0] == 0xE9`) targets an address that already contains a JMP (`cur_bytes[0] == 0xE9`) pointing to a different destination, the new JMP is written over the old one — redirecting a hook to a different cave is always safe and enables switching between cave-based cheats without a game restart
- **Cave disable foreign**: When disabling a cheat whose cave entry doesn't match the expected ON bytes (another mod overwrote it), the off_bytes (zeros) are written anyway to clear the cave — prevents the "bytes mismatch before OFF" error that previously made it impossible to turn off a speed mode cheat after switching to another
- **Button type disable**: `effective_on` for `"type":"button"` cheats now respects an explicit `on=0` toggle — previously button cheats always forced enable, making it impossible to undo a button cheat from the UI; the button behavior (always fires ON when clicked) is unchanged for `on=1` requests

### Address Learning Cache

- New `src/cr_addr_cache.c` / `cr_addr_cache.h`: resolved addresses for SHN/MC4 entries are stored in `/data/cheatrunner/addr_cache.json` (up to 512 slots, keyed by file path + mtime + mod + entry index)
- On a cache hit, the stored original bytes are injected as `exp_bytes` so the address resolution path treats them as verified — avoids re-running the x86 probe on every apply for the same file version
- Cache is written atomically after each new resolution; oldest entry is evicted when full
- Controlled by new config key `cheat_addr_cache_enabled` (default: 1); exposed in the Settings panel
- New config key `cheat_inter_mod_delay_ms` (default: 0): after enabling a mod, CheatRunner sleeps this many ms and then verifies the game process is still alive — returns `-4` if the game exited, preventing further mods from being applied to a crashed game
- Cache only stores addresses resolved via x86 probe or unverified fallback (not already-verified entries)

### SHN Section Field Warning

- `shn_xml_to_json` now extracts `<Section>` child elements from each `<Cheatline>` and emits them as `"section": N` in the JSON memory entry
- In `apply_cheat_json`, any entry with `section > 0` emits a `[warn] [cheats]` log line indicating the section is not yet mapped to a module — the eboot base is used as a fallback

### Code Cave X86 Probe Skip

- The x86 instruction heuristic probe is now skipped for writes of 16 bytes or more (`byte_len >= 16`) — code cave payloads are always multi-byte and would incorrectly trigger the heuristic; the probe only applies to short hook patches (`byte_len < 16`)

### Dashboard — Settings Panel

- New **Settings** tab in the cheat modal with grouped controls for all key config options: address resolution, safety timers, logging level, and advanced toggles
- Toggle switches, dropdowns, and numeric inputs all save to `/api/config/set` immediately on change
- New config keys exposed in the panel: `cheat_addr_cache_enabled`, `cheat_inter_mod_delay_ms`, `cheat_address_auto_detect`, `cheat_validate_original_bytes`

### Dashboard — Per-Title Address Mode Override

- New address mode selector (`Auto` / `Absolute` / `Relative`) appears above the mod list for SHN and MC4 cheat files
- Preferences stored in `/data/cheatrunner/title_prefs.json`; applied per-apply before the normal address resolution flags, overriding `cheat_address_auto_detect` for that title
- New API endpoints: `GET /api/title-prefs?titleId=`, `POST /api/title-prefs/set?titleId=&addrMode=`, `POST /api/title-prefs/clear?titleId=`
- Setting mode to `Auto` clears the preference (same as calling `/clear`)

### Address Resolution — X86 Instruction Heuristic Probe

- For SHN and MC4 cheats without `expected` bytes, CheatRunner now probes both address candidates (absolute `offset` and relative `base + offset`) with an x86-64 instruction prefix heuristic when `cheat_address_auto_detect=1` is set (the default)
- Reads 4 bytes from each candidate and checks whether the first byte is a plausible x86-64 instruction prefix (REX, CALL, JMP, PUSH/POP, MOV, NOP, operand-size prefix, etc.) — executable code almost never starts with a null byte, while data or uninitialized memory almost always does
- Selects the candidate that looks like code; falls back to the configured `cheat_mc4_unverified_fallback` / `cheat_shn_unverified_fallback` policy only when both or neither candidate matches
- Resolves the most common SHN/MC4 failure mode: PS4 BC games (CUSA titles) store cheat offsets as **absolute** virtual addresses, while the `relative` fallback incorrectly adds the image base on top — the probe detects which candidate has actual executable code and picks it automatically
- New resolve status `CR_ADDR_RESOLVE_OK_X86_PROBE` reported in logs: `[info] [cheats.mem] addr_x86_probe off=… → absolute=… (x86 match) rel=… (non-x86)` or the inverse for relative
- When both or neither candidate has x86-like bytes, logs `addr_x86_probe … ambiguous` at debug level and falls through to the policy-based fallback

### Patch Engine — Mask Line PS4 BC Timeout Fix

- **Critical**: MASK-type patch lines in `patch_apply_entry_ex()` now use `write_process_memory_forced()` instead of `write_process_memory()` — the standard path calls `kernel_get_vmem_protection()`, which hangs indefinitely on PS4 BC vmem entries (e.g. Bloodborne / CUSA00900), blocking the HTTP handler thread until the browser's 10-second fetch timeout fires and the user sees "patch apply failed: request timed out"
- Explicit `pt_copyout` + `memcmp` readback verify added after the forced write for MASK lines, matching the verify already done for all other line types — `verify_failed` and `mask_verify_mismatch` error paths preserved
- All non-mask lines were already using `write_process_memory_forced()` since v0.12; this closes the last remaining call to `write_process_memory()` in the patch apply path

### Cheat Selection — Wrong-Version Fallback When No Better Candidate Exists

- Fixed: games whose only local cheat files have a version mismatch (e.g. Resident Evil 4 at v01.10 with only a `_01.00.json` file) now load the cheat menu instead of showing "No local cheat file found" — the user never saw the candidate selector to force-enable, making it impossible to use the cheat without knowing to re-download
- Root cause: `apply_selection_rules` cleared `best_path` when only `CAND_MATCH_WRONG_VERSION` candidates existed; this was intentional but created a dead end — no cheat menu, no candidate selector, no path to force-enable
- When version detection improved (more param.sfo paths added in v0.12), previously-undetected game versions started being resolved, causing files that auto-loaded before to suddenly fail for games like Resident Evil and Little Nightmares
- Fix: when no exact-match or generic-version candidate exists, fall back to the best wrong-version candidate and mark it `CHEAT_SEL_WRONG_VERSION` — the cheat menu loads, the version-mismatch warning banner and WRONG VER badge are visible, and the user can explicitly force or switch files; only games with zero local cheat files of any kind return "no cheat file"

### Crash — SQLite Concurrent Access From Icon Requests

- Fixed: CheatRunner crashed and required payload restart when the dashboard rendered game thumbnails for multiple games simultaneously (browser fires one `/appdb/icon?id=X` request per game tile on load)
- **Root cause**: SQLite is compiled with `-DSQLITE_THREADSAFE=0` — no internal locking whatsoever. Per SQLite docs, using the library from more than one thread simultaneously in this mode (even on separate connections) is undefined behavior. `appdb_resolve_icon_path()` and `appdb_collect_games_sqlite()` were both called concurrently, corrupting SQLite's shared global state (memory allocator, error strings, etc.) → SIGSEGV/SIGBUS → process death
- **Fix**: added `g_sqlite_lock` (static pthread_mutex_t) in `cr_appdb.c`; both `appdb_collect_games_sqlite` and `appdb_resolve_icon_path` now hold it for their entire SQLite session (open → query → close), serializing all library calls

### MC4 — Decrypt Output Validation

- `mc4_decrypt_to_xml()` now validates that the decrypted plaintext starts with `<` before returning — previously a corrupted or wrong-key MC4 file would decrypt to garbage bytes that `shn_xml_to_json()` would silently accept, find no `<Cheat` tags in, and return an empty mod list with no error message; now returns `NULL` so the caller emits "MC4 decrypt failed"
- Check applied after HTML entity unescaping (`&lt;` → `<`) so legitimately escaped XML openings are not rejected

### Cheats — Addr Cache Recovery: ON-State Bytes No Longer Accepted as Proof

- Stale cache recovery no longer accepts `on_bytes` at the cached address as proof the cached address is correct — previously, if another mod (or a prior session) had already written ON bytes to that location, recovery would conclude "address confirmed" and write ON bytes again on top of an already-patched location
- Only `exp_bytes` (the original off-state bytes) are now accepted as proof; if the location already has `on_bytes`, recovery falls through to cache-clear + unverified-fallback retry, which is the correct path for an idempotent re-apply

### Cheats — X86 Probe Resolved Address Blocked by Baseline Guard

- Fixed: SHN/MC4 mods where the x86 instruction heuristic probe found a clear winner (`addr_x86_probe off=… → relative=… (x86 match)`) were incorrectly blocked by the baseline guard with "no reliable baseline; set allow_legacy_shn_without_expected=1 to proceed" — even though `allow_legacy_shn_without_expected` defaults to `1`
- Root cause: `CR_ADDR_RESOLVE_OK_X86_PROBE` was not included in the `is_legacy_unverified` flag check; only the three UNVERIFIED_* statuses were listed — so a clear x86 probe result set `is_legacy_unverified = false`, making the guard fire regardless of the config default
- Fix: `CR_ADDR_RESOLVE_OK_X86_PROBE` added to `is_legacy_unverified` — semantically correct since x86 probe is also a no-baseline resolution (confidence is higher than pure policy fallback, but there are still no `expected` bytes to verify against)
- Log label updated: x86-probe applies now log `shn x86_probe mode: applying without baseline` instead of `shn legacy_unverified mode` for clarity
- Affected: any SHN/MC4 cheat file where the probe disambiguates cleanly (one candidate is x86, the other is not) — e.g. multiple Bloodborne mods that were silently failing while "Inf Stamina" (ambiguous probe → unverified path) worked fine

### Stability — "CheatRunner is busy" / "not responding" Under Game Load

- Fixed thread saturation that caused the dashboard to show "CheatRunner is busy" followed by "CheatRunner is not responding" after applying cheats to Bloodborne and similar PS4 BC games
- **Root cause 1**: state check called `pt_attach_timed(pid, 3000)` — when the game is deep in a kernel call (PS4 BC vmem operations, asset streaming), SIGSTOP is queued but not delivered for seconds; the dashboard's ~500ms auto-poll spawned a new 3-second-blocked thread on every tick; after ~8s the 16-thread limit was reached, returning 503 → "busy"
- **Root cause 2**: no process liveness check before ptrace — if the game crashed, every state poll waited the full timeout on a dead process
- **Root cause 3**: if a cheat apply was in progress (game already paused under ptrace by the apply thread), a concurrent state check attempted to attach the same process, raced with the apply, and hung for the full timeout
- **Fix**: three guards added before `pt_attach_timed` in the state handler: (1) `kill(pid, 0)` fast liveness check — bail immediately if process is dead; (2) skip ptrace if `g_cheat_applying` is set — return cached/unknown state instead of racing the apply thread; (3) timeout reduced from 3000ms → 1000ms — worst-case per-poll latency cut by 3×, capping accumulation to ~2 threads before recovery
- `HTTP_MAX_CONCURRENT` raised from 16 → 24 for additional headroom during burst scenarios

### Stability — Blocking pt_attach in State-Check Paths

- Four calls to blocking `pt_attach()` in `cr_api.c` replaced with `pt_attach_timed(pid, 2000)`: single-mod state check (address-debug), address-debug engine handler, full cheat state computation, and the engine probe path — the blocking variant calls `waitpid()` with no timeout, permanently hanging the HTTP handler thread if the game process is in an unkillable state (e.g. PS4 BC stuck in a kernel call)
- The main `/api/cheats/state` polling path already used `pt_attach_timed` since v0.11; these four were missed — on a stuck PS4 BC game, every dashboard state poll, address-debug request, and engine info request would silently accumulate hung threads until the service became unresponsive
- Timeout case returns `CHEAT_STATE_PROCESS_NOT_FOUND` with reason `"pt_attach timed out"` — consistent with attach failure handling elsewhere

### Stability — kill(pid, 0) ESRCH vs EPERM

- All five `kill(pid, 0) != 0` liveness checks now additionally require `errno == ESRCH` before concluding the process is dead — previously EPERM (process alive but unprivileged signal) was incorrectly treated as ESRCH (process gone), which could skip ptrace attempts on a running process
- Affected sites: state-handler fast-bail (`cr_api.c`), pre-read abort guard (`cr_cheats.c`), zombie detection in `rpc_refresh_title_and_notify` and `read_running_state` (`cr_game_monitor.c`), game-stop restore skip (`cr_patch_parser.c`)
- `<errno.h>` added to `cr_game_monitor.c` which previously relied on transitive inclusion

### Patch Engine — PS4 BC Kernel Panic Fix

- **Critical**: `patch_apply_entry()` now uses `write_process_memory_forced()` instead of `write_process_memory()` for non-mask address-based lines — the standard write path calls `kernel_get_vmem_protection()`, which dereferences an invalid kernel structure on PS4 BC game page types (special vmem entry format), triggering a `Fatal trap 12: page fault while in kernel mode` kernel panic
- `write_process_memory_forced()` skips `kernel_get_vmem_protection()` and calls `kernel_mprotect()` directly — this has always been safe for PS4 BC emulation pages (consistent with the existing `-5` fallback in the cheat apply path)
- Mask-type patch lines remain on `write_process_memory()` since `scan_for_pattern()` already proved the target address is mapped and readable, making the vmem query safe

### Patch Engine — XML Parser Fix (SIGILL Crash)

- Fixed `tag_attr()` XML parser in `cr_patch_parser.c` to handle `name= "value"` (space between `=` and `"`) — the PS5 XML patch format allows whitespace around the `=` in attributes; without this fix the `Offset` attribute parsed as empty, causing `match_offset=0` (write at wrong location), misaligning x86-64 instruction boundaries and crashing the game with SIGILL
- Fixed 13 XML patch files in `PS5-Payload-dev/patches/xml/` that had `Offset= "+N"` (space before quote): CUSA04301, CUSA06244, CUSA06267, CUSA08010, CUSA13529, CUSA13893, CUSA16075, CUSA16164, CUSA17587, CUSA18065, CUSA18068, CUSA19488, CUSA20096

### Patch Engine — Notifications

- `patch_apply_entry()` now calls `notify()` on success/failure — emits a PS5 system popup: `CheatRunner: Patch applied: <name>` or `CheatRunner: Patch failed: <name>`

### Patch Engine — Partial Overwrite Warning

- Fixed `mask_partial_overwrite` warning condition: previously checked `value_len < pattern_len` (firing even when `match_offset + value_len == pattern_len`); now correctly checks `match_offset + value_len < pattern_len` — only warns when original bytes genuinely remain after the write site
- `/api/patches` list response now includes `hasMaskPartialOverwrite: bool` per entry — UI can surface a warning without requiring the user to apply the patch first
- `/api/patches/apply` response now includes `hadMaskPartialOverwrite: bool` and a `warning` string when partial overwrite was detected

### Patch API — Performance

- `handle_patches_list()` now reads running game state from the monitor's in-memory cache (`running_state_get()`) instead of calling `get_running_game_ex()` + `read_running_state()` — eliminates two full sysctl process-list scans and multiple SCE syscalls per GET request; prevents HTTP thread saturation during game crash/relaunch loops
- `handle_patches_apply()` version guard and post-apply `patch_mark_applied()` also switched to cached state — reduces total `get_running_game_ex()` calls per apply from 3 to 0 in the HTTP handler (the call inside `patch_apply_entry()` itself still verifies liveness)

### Cheats — Stale Address Cache Recovery

- When `addr_cache` provides `orig_bytes` that don't match memory, CheatRunner now attempts three-step recovery instead of hard-failing: (1) probe the cached absolute address directly for on/expected bytes; (2) if that fails, clear the stale cache entry and retry without a baseline using the configured fallback policy; (3) only hard-fail if the retry also fails — prevents "addr_unresolved" errors after game updates or cache corruption

### Game Monitor — Crash Detection

- Game monitor now probes process liveness with `kill(pid, 0)` after resolving the game pid — immediately marks game as stopped for zombie processes that `sceKernelGetAppInfo` would otherwise report as running
- `rpc_refresh_title_and_notify()` checks `kill(cur.pid, 0)` after `read_running_state()`; if the process is dead despite the BigApp syscall reporting it running, forces state to stopped — catches game crashes the PS5 firmware hasn't cleaned up yet
- `apply_cheat_json()` checks `kill(pid, 0)` between pre-read entries; if the game died mid-apply, aborts cleanly and forces a monitor re-poll
- `pt_attach_timed` timeout in cheat apply reduced from 3000 ms → 2000 ms; calls `rpc_refresh_title_and_notify()` on timeout to update stale monitor state immediately

### Game Monitor — Version Detection

- Added system-wide param paths at the top of the candidate list in `read_param_value_by_title_id()`: `/system_data/priv/appmeta/{id}/param.json` and `.../external/{id}/param.json` — these paths are always accessible on PS5 regardless of game sandbox
- Removed `/system_ex/app/{id}/sce_sys/param.json` — `/system_ex` is a kernel-managed filesystem; `stat()`-ing paths there from homebrew triggered kernel panics on some firmware versions
- `get_running_game_ex()` now tries `sceSystemServiceGetAppTitleId(appId, buf)` as the primary title ID source (faster) with `sceKernelGetAppInfo()` as fallback
- Version detection in `read_running_state()` now probes `/proc/{pid}/root/patch0` and `/proc/{pid}/root/app0` before falling back to static installed paths — accesses the running game's filesystem namespace, correctly returning the installed update version rather than the base game version
- Version carry-forward: `rpc_refresh_title_and_notify()` now copies `content_version` / `app_version` from the previous poll tick when the same game and pid are still running and the current tick didn't resolve a version — avoids redundant filesystem probes every 500 ms once the version is known

### Dashboard — Local Patches Setup Item

- Setup panel now shows a **Local Patches** item alongside Local Cheats — displays count of games that have a matching patch XML file out of total known games

### Launch — Manual Close Detection

- `launch_worker_thread()` preflight now performs a live check via `sceSystemServiceGetAppIdOfRunningBigApp()` when the monitor cache reports the requested game is already running — if the live BigApp ID doesn't match the cached app ID (game closed since the last 500 ms poll), CheatRunner logs the stale state, forces a monitor re-poll, and proceeds with relaunch instead of incorrectly blocking it with "game already running"

### Dashboard — Unverified Mode Warning Banner

- Cheat modal now shows an amber warning banner when the loaded cheat file uses `legacy_unverified` mode (MC4 or SHN format with no `expected` bytes in any entry)
- Banner text advises enabling cheats one at a time and watching for crashes; displayed above the mods list
- Banner is hidden automatically when the cheat file has reliable expected bytes (JSON format, or MC4/SHN with explicit `expected` fields)
- `/api/cheats/state` now includes `hasLegacyUnverified: true|false` — `true` when any memory entry in any mod lacks expected bytes in a non-JSON cheat file

### Patch Engine — Multiple XML Files per TitleID

- TitleID index now maps `title_id → list of (path, source_kind)` pairs instead of `title_id → one path`; all XML files for a TitleID appear in `/api/patches` with independent entries
- Three source directories are scanned in deterministic order: `xml_prospero` → `xml` → `elf-arsenal/patches/xml`; within each directory files are sorted alphabetically
- Index reads up to 128 KB per XML file (previously 4 KB) so TitleID blocks beyond the first 4 KB are indexed correctly
- Duplicate `(title_id, path)` pairs are deduplicated; different paths with the same TitleID are preserved as separate entries
- Each patch entry now includes `sourcePath`, `sourceFile`, `sourceKind`, `sourceKindLabel`, and `metadataIndex` fields

### Patch Engine — elf-arsenal XML Scan

- Added `/data/elf-arsenal/patches/xml` as a third patch search path (`CR_PATCH_SOURCE_ELF_ARSENAL_XML = 2`)
- If the directory does not exist or is not accessible, it is skipped with an `info`-level log line; no error is returned
- `ELF_ARSENAL_PATCHES_XML_DIR` constant added to `cr_paths.h`

### Patch Engine — Rollback (all-or-nothing)

- `patch_apply_entry_ex()` replaces `patch_apply_entry()` and implements full backup/rollback:
  - Before each write: resolve address, read original bytes into a backup record
  - After each write: readback verify
  - On any failure (address resolve, backup read, write, verify): restore all previous writes in reverse order
- Rollback uses `write_process_memory_forced()` for all lines
- API response includes `rolledBack`, `rollbackErrors`, and `verifyFailCount` fields
- Failed patches are never marked applied

### Patch Engine — Verify Failure is Hard Failure

- Readback mismatch after write is now a hard failure: apply fails, rollback triggers
- `force=1` cannot bypass verify failure
- Response: `ok:false, applied:false, error:"verify_failed", rolledBack:true`

### Patch Engine — Unsupported Line Blocking

- Entries containing `mask_jump32` or any unknown line type set `hasUnsupported:true`, `canApply:false`, `blockReason:"unsupported_line_type"`
- `unsupportedCount` and `unsupportedTypes` (array of type name strings) added to API and entry struct
- `force=1` cannot bypass unsupported line type blocking
- Apply response when blocked: `ok:false, error:"unsupported_line_type"`

### Patch Engine — Zero-Line Entry Blocking

- Entries with `lineCount:0` (no supported lines) report `canApply:false, blockReason:"no_supported_lines"`
- Dashboard shows "No supported patch lines" for these entries with no Apply button

### Patch Engine — Stable entryId

- Each entry gets a stable `entryId` (8-hex-char FNV-1a hash of `source_path|source_kind|metadata_index|name|app_ver|line_count`)
- Applied-state is now keyed on `(pid, title_id, entry_id)` instead of `(title_id, entry_idx, pid)`
- Multiple XML files for the same TitleID cannot collide applied-state
- `/api/patches/apply` accepts `entryId=...` (preferred) with `index=N` as backward-compatible fallback
- `/api/patches/rescan` clears the full applied-state table and logs that it was cleared

### Patch Engine — Partial Overwrite Warning Fix

- Warning condition corrected: fires only when `type==MASK && match_offset>=0 && match_offset+value_len < pattern_len`; previously checked `value_len < pattern_len` which fired falsely when `match_offset+value_len == pattern_len`
- New helper `cr_patch_line_has_trailing_pattern_bytes()` used consistently in parser, apply, and API
- API field renamed from `hasMaskPartialOverwrite` to `hasPartialOverwriteWarning`

### Patch API — Improved Fields

- `/api/patches` entries now include: `entryId`, `appVerMatches`, `supportedLineCount`, `unsupportedCount`, `unsupportedTypes`, `hasUnsupported`, `hasMask`, `hasPartialOverwriteWarning`, `canApply`, `blockReason`, `warnings[]`, `sourcePath`, `sourceFile`, `sourceKind`, `sourceKindLabel`, `metadataIndex`
- `blockReason` values: `version_mismatch`, `unsupported_line_type`, `no_supported_lines`, `missing_game`
- `warnings[]` values: `mask_scan`, `partial_overwrite`, `force_required_for_version_mismatch`

### Dashboard — Patches Tab

- Source kind and file label shown per entry
- `hasUnsupported` entries show "Contains unsupported line types; patch cannot be applied safely." (replaces incorrect "Contains pattern-scan lines not applied in this version")
- Mask scan entries show "Mask scan" badge (mask is supported)
- Partial-overwrite warning uses corrected condition text
- Force button shown only for `blockReason:"version_mismatch"`; unsupported/blocked entries show reason text with no action button
- Apply uses `entryId` when available; falls back to `index`

### Docs — Auto-Apply Removed

- README no longer claims auto-apply patches on launch
- README documents all three scan directories and strict apply behavior
- Unsupported line types documented explicitly

---

## v0.11

### Log Quality

#### `[cheat_select]` — Log Only on Change
- `[cheat_select] [game]` log in `/api/cheats/state` handler now only fires when title, cheat path, detected version, or version match status changes — previously logged on every poll interval causing significant noise in the log buffer

#### `[cheats.mem]` — Per-Write Detail Moved to Debug
- `mprotect_rwx`, `copyin`, `int_verify ok` lines in `cr_memory.c` downgraded from `info` to `debug` — these are internal write-mechanism details that repeat 3× per write
- `begin`, `write_rc`, `ext_verify_skip`, `icache_sync` lines in `cr_cheats.c` downgraded from `info` to `debug` — 4 more lines per write; the `[cheats] write[N] … ok` summary line remains at `info` level
- Net result: a mod with 4 writes previously emitted ~28 `[cheats.mem]` info lines; now emits only the 4 summary lines plus any warnings/errors

#### `[game.ver]` — Version Undetected Warning Throttled
- `version undetected` warn in `cr_game_monitor.c` now fires only once per unique `(title_id, pid)` pair — previously emitted on every poll cycle when all SFO paths failed

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

### PS4 BC — Write Fallback
- `write_process_memory_ex` returns `-5` when `kernel_get_vmem_protection` cannot query PS4 BC emulation-layer pages; the write loop now retries with `write_process_memory_forced` (skips vmem query, applies `kernel_mprotect` directly) — same pattern already used by the rollback path
- On fallback success, logs `[info] cheats.mem vmem_prot_fallback ok addr=… title=… mod=…`; on fallback failure, logs `[warn] cheats.mem vmem_prot_fallback failed … forced_rc=…` for diagnosis
- Fixes PS4 BC game cheats that previously failed silently with "write failed at … (rc=-5)"

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
- `/data/etaHEN/cheats` added as search path for etaHEN cheat file compatibility
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
