#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "cr_addr_cache.h"
#include "cr_cheats.h"
#include "cr_launch.h"
#include "cr_patch_parser.h"
#include "cr_config.h"
#include "cr_activity.h"
#include "cr_game_monitor.h"
#include "cr_log.h"
#include "cr_notifications.h"
#include "cr_paths.h"
#include "cr_titles.h"
#include "ps5sdk_compat.h"

static pthread_mutex_t g_title_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_current_title[32] = "No game running";

static pthread_mutex_t g_running_lock = PTHREAD_MUTEX_INITIALIZER;
static running_game_state_t g_running = {0};
volatile int g_game_monitor_running = 1;

static struct { char title_id[16]; pid_t pid; } g_ver_warn_last = {0};
static pthread_mutex_t g_ver_warn_lock = PTHREAD_MUTEX_INITIALIZER;

pid_t
find_pid_by_name(const char *name) {
  int mib[4] = {1, 14, 8, 0};
  pid_t mypid = getpid();
  pid_t pid = -1;
  size_t buf_size = 0;
  uint8_t *buf = NULL;

  if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) != 0) {
    return -1;
  }
  buf = malloc(buf_size);
  if (!buf) {
    return -1;
  }
  if (sysctl(mib, 4, buf, &buf_size, NULL, 0) != 0) {
    free(buf);
    return -1;
  }

  for (uint8_t *ptr = buf; ptr < (buf + buf_size);) {
    int ki_structsize = *(int *)ptr;
    if (ki_structsize <= 0 || (size_t)ki_structsize > (size_t)((buf + buf_size) - ptr)) break;
    /* ki_pid is at +72 and ki_tdname (a NUL-terminated name array) at +447. Skip
     * any record too short to contain those fields rather than read into the next
     * record — or past the buffer end on the final record. */
    if (ki_structsize <= 467) { ptr += ki_structsize; continue; }
    pid_t ki_pid = *(pid_t *)&ptr[72];
    char *ki_tdname = (char *)&ptr[447];
    ptr += ki_structsize;
    if (!strcmp(name, ki_tdname) && ki_pid != mypid) {
      pid = ki_pid;
    }
  }

  free(buf);
  return pid;
}

pid_t
find_pid_for_app_id(uint32_t app_id) {
  int mib[4] = {1, 14, 8, 0};
  size_t buf_size = 0;
  if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) != 0) {
    return -1;
  }
  uint8_t *buf = malloc(buf_size);
  if (!buf) {
    return -1;
  }
  if (sysctl(mib, 4, buf, &buf_size, NULL, 0) != 0) {
    free(buf);
    return -1;
  }
  pid_t result = -1;
  app_info_t info;
  for (uint8_t *ptr = buf; ptr < (buf + buf_size);) {
    int ki_structsize = *(int *)ptr;
    if (ki_structsize <= 0 || (size_t)ki_structsize > (size_t)((buf + buf_size) - ptr)) break;
    if (ki_structsize <= 76) { ptr += ki_structsize; continue; }  /* ki_pid at +72 */
    pid_t pid = *(pid_t *)&ptr[72];
    ptr += ki_structsize;
    memset(&info, 0, sizeof(info));
    if (sceKernelGetAppInfo(pid, &info) == 0 && info.app_id == app_id) {
      result = pid;
      break;
    }
  }
  free(buf);
  return result;
}

int
get_running_game_ex(pid_t *out_pid, char *out_title, size_t title_size, intptr_t *out_base, int *out_app_id) {
  int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
  if (app_id <= 0) {
    return -1;
  }
  pid_t pid = find_pid_for_app_id((uint32_t)app_id);
  if (pid <= 0) {
    return -1;
  }
  /* Quick liveness check: fail immediately for dead processes (ESRCH). */
  if (kill(pid, 0) != 0 && errno == ESRCH) {
    return -1;
  }
  /* Get title ID: try sceSystemServiceGetAppTitleId first — faster and doesn't
   * require reading from the process namespace.
   * Fall back to sceKernelGetAppInfo if unavailable. */
  char title_id_buf[16] = {0};
  if (sceSystemServiceGetAppTitleId(app_id, title_id_buf) != 0 || title_id_buf[0] == '\0') {
    app_info_t info;
    memset(&info, 0, sizeof(info));
    if (sceKernelGetAppInfo(pid, &info) != 0 || info.title_id[0] == '\0') {
      return -1;
    }
    snprintf(title_id_buf, sizeof(title_id_buf), "%s", info.title_id);
  }
  if (!is_game_title_id(title_id_buf)) {
    return -1;
  }
  if (out_title && title_size > 0) {
    snprintf(out_title, title_size, "%s", title_id_buf);
  }
  if (out_base) {
    *out_base = kernel_dynlib_mapbase_addr(pid, 0);
  }
  if (out_app_id) {
    *out_app_id = app_id;
  }
  if (out_pid) {
    *out_pid = pid;
  }
  return 0;
}

int
get_running_game(pid_t *out_pid, char *out_title, size_t title_size, intptr_t *out_base) {
  return get_running_game_ex(out_pid, out_title, title_size, out_base, NULL);
}

void
running_state_set(const running_game_state_t *st) {
  pthread_mutex_lock(&g_running_lock);
  if (st) {
    g_running = *st;
  } else {
    memset(&g_running, 0, sizeof(g_running));
  }
  pthread_mutex_unlock(&g_running_lock);
}

void
running_state_get(running_game_state_t *out) {
  if (!out) {
    return;
  }
  pthread_mutex_lock(&g_running_lock);
  *out = g_running;
  pthread_mutex_unlock(&g_running_lock);
}

int
read_running_state(running_game_state_t *out) {
  running_game_state_t st;
  memset(&st, 0, sizeof(st));
  pid_t pid = -1;
  intptr_t base = 0;
  int app_id = 0;
  char title[16] = {0};
  if (get_running_game_ex(&pid, title, sizeof(title), &base, &app_id) != 0) {
    if (out) {
      *out = st;
    }
    return -1;
  }
  st.running = 1;
  st.app_id = app_id;
  st.pid = pid;
  st.image_base = base;
  snprintf(st.title_id, sizeof(st.title_id), "%s", title);
  snprintf(st.platform, sizeof(st.platform), "%s", platform_for_title_id(title));
  read_param_value_by_title_id(title, "titleName", st.title_name, sizeof(st.title_name));
  if (!st.title_name[0]) {
    snprintf(st.title_name, sizeof(st.title_name), "%s", title);
  }
  /* Version detection — probe in priority order:
   *   1. /proc/{pid}/root/patch0  — installed update, contains the current game version
   *   2. /proc/{pid}/root/app0    — base game, correct only when no update is installed
   *   3. /app0                    — direct path (works if CheatRunner runs in the game namespace)
   *   4. Static installed paths   — appmeta scan + known filesystem layout patterns
   *
   * The old "/proc/{pid}/root/user/patch/{title}/..." path was wrong: PS5 updates are
   * mounted as patch0 inside the game's namespace, not under user/patch/. */
  char proc_root[32];
  snprintf(proc_root, sizeof(proc_root), "/proc/%d/root", (int)pid);

  /* Probe patch0 then app0 via proc namespace for both version fields */
  {
    const char *const subdirs[] = { "patch0", "app0", NULL };
    for (int _si = 0; subdirs[_si] && !st.content_version[0]; _si++) {
      char dir[128];
      snprintf(dir, sizeof(dir), "%s/%s", proc_root, subdirs[_si]);
      read_param_value_from_dir(dir, "contentVersion", st.content_version, sizeof(st.content_version));
    }
    for (int _si = 0; subdirs[_si] && !st.app_version[0]; _si++) {
      char dir[128];
      snprintf(dir, sizeof(dir), "%s/%s", proc_root, subdirs[_si]);
      read_param_value_from_dir(dir, "appVersion", st.app_version, sizeof(st.app_version));
    }
  }
  /* Direct /app0 path (only useful if CheatRunner runs inside the game's sandbox) */
  if (!st.content_version[0])
    read_param_value_from_sfo("/app0/sce_sys/param.sfo", "contentVersion", st.content_version, sizeof(st.content_version));
  if (!st.app_version[0])
    read_param_value_from_sfo("/app0/sce_sys/param.sfo", "appVersion", st.app_version, sizeof(st.app_version));
  /* Static installed paths (fast — no directory scan) */
  if (!st.content_version[0])
    read_param_value_by_title_id(title, "contentVersion", st.content_version, sizeof(st.content_version));
  if (!st.app_version[0])
    read_param_value_by_title_id(title, "appVersion", st.app_version, sizeof(st.app_version));
  /* Last resort: scan /user/appmeta for the real content ID → sandbox path.
   * This is the slowest path (directory listing) — only runs when all else fails. */
  if (!st.content_version[0] && !st.app_version[0])
    read_param_value_from_appmeta(title, "contentVersion", st.content_version, sizeof(st.content_version));
  if (!st.content_version[0] && !st.app_version[0])
    read_param_value_from_appmeta(title, "appVersion", st.app_version, sizeof(st.app_version));

  if (!st.content_version[0] && !st.app_version[0]) {
    pthread_mutex_lock(&g_ver_warn_lock);
    if (strcmp(g_ver_warn_last.title_id, title) != 0 || g_ver_warn_last.pid != pid) {
      cr_log("warn", "game.ver", "version undetected title=%s pid=%d — tried %s/patch0, %s/app0, static paths",
             title, (int)pid, proc_root, proc_root);
      snprintf(g_ver_warn_last.title_id, sizeof(g_ver_warn_last.title_id), "%s", title);
      g_ver_warn_last.pid = pid;
    }
    pthread_mutex_unlock(&g_ver_warn_lock);
  }
  snprintf(st.eboot_path, sizeof(st.eboot_path), "/app0/eboot.bin");
  if (out) {
    *out = st;
  }
  return 0;
}

void
set_current_title(const char *title) {
  pthread_mutex_lock(&g_title_lock);
  snprintf(g_current_title, sizeof(g_current_title), "%s", title ? title : "No game running");
  pthread_mutex_unlock(&g_title_lock);
}

void
get_current_title(char *out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  pthread_mutex_lock(&g_title_lock);
  snprintf(out, out_size, "%s", g_current_title);
  pthread_mutex_unlock(&g_title_lock);
}


void
rpc_refresh_title_and_notify(void) {
  running_game_state_t cur;
  running_game_state_t prev;
  running_state_get(&prev);
  if (read_running_state(&cur) != 0) {
    memset(&cur, 0, sizeof(cur));
  }
  /* If the BigApp syscall still reports running but the pid is dead, treat as stopped.
   * This catches crashes that the PS5 firmware hasn't cleaned up yet. */
  if (cur.running && cur.pid > 0 && kill(cur.pid, 0) != 0 && errno == ESRCH) {
    cr_log("info", "game", "pid %d is dead (kill probe failed) — forcing game_stopped", (int)cur.pid);
    memset(&cur, 0, sizeof(cur));
  }
  /* Carry forward already-detected version when the same game/pid is still running.
   * Avoids re-probing the same param.json / param.sfo files every 500 ms poll tick. */
  if (cur.running && prev.running && cur.pid == prev.pid &&
      strcmp(cur.title_id, prev.title_id) == 0) {
    if (!cur.content_version[0] && prev.content_version[0])
      snprintf(cur.content_version, sizeof(cur.content_version), "%s", prev.content_version);
    if (!cur.app_version[0] && prev.app_version[0])
      snprintf(cur.app_version, sizeof(cur.app_version), "%s", prev.app_version);
  }
  if (cur.running) {
    if (prev.running && strcmp(prev.title_id, cur.title_id) == 0 && prev.started_at > 0) {
      cur.started_at = prev.started_at;
    } else {
      cur.started_at = (uint64_t)time(NULL);
    }
  }
  running_state_set(&cur);
  set_current_title(cur.running ? cur.title_id : "No game running");

  int changed = (prev.running != cur.running) || strcmp(prev.title_id, cur.title_id) != 0;
  if (changed) {
    int swapped = prev.running && cur.running && strcmp(prev.title_id, cur.title_id) != 0;
    if ((prev.running && !cur.running) || swapped) {
      notification_add("game_stopped", "Game stopped: %s", prev.title_id[0] ? prev.title_id : "unknown");
      cr_log("info", "game", "game stopped %s", prev.title_id);
      int just_suspected = 0;
      {
        int watch_ms = 8000;
        pthread_mutex_lock(&g_cfg_lock);
        if (g_cfg.cheat_post_apply_watch_ms > 0) {
          watch_ms = g_cfg.cheat_post_apply_watch_ms;
        }
        pthread_mutex_unlock(&g_cfg_lock);
        pthread_mutex_lock(&g_crash_guard_lock);
        for (int _gi = g_crash_guard_n - 1; _gi >= 0; _gi--) {
          crash_guard_state_t *cg = &g_crash_guard_arr[_gi];
          if (!cg->title_id[0] || cg->enabled_at_ms == 0) continue;
          if (strcmp(cg->title_id, prev.title_id) != 0 || cg->pid != prev.pid) continue;
          uint64_t elapsed = now_ms() - cg->enabled_at_ms;
          if (elapsed < (uint64_t)watch_ms && g_crash_suspects_n < CRASH_SUSPECT_MAX) {
            crash_suspect_rec_t *rec = &g_crash_suspects[g_crash_suspects_n++];
            snprintf(rec->title_id, sizeof(rec->title_id), "%s", cg->title_id);
            rec->mod_index  = cg->mod_index;
            snprintf(rec->mod_name, sizeof(rec->mod_name), "%s", cg->mod_name);
            rec->pid        = cg->pid;
            rec->elapsed_ms = elapsed;
            rec->ts         = time(NULL);
            rec->app_id     = prev.app_id;
            just_suspected  = 1;
            cr_log("warn", "cheats.guard",
                   "game stopped %llums after enabling mod=%d name=\"%s\"; marking crash_suspect",
                   (unsigned long long)elapsed, cg->mod_index, cg->mod_name);
          }
          g_crash_guard_arr[_gi] = g_crash_guard_arr[--g_crash_guard_n];
        }
        pthread_mutex_unlock(&g_crash_guard_lock);
        if (just_suspected) {
          crash_suspects_save();
          /* Clear addr_cache for the suspected title so the same unverified
           * address is not reused on the next launch, repeating the crash. */
          char _susp_path[256]; int _susp_kind = 0;
          if (find_cheat_file_for_title(prev.title_id, _susp_path,
                                        sizeof(_susp_path), &_susp_kind)) {
            addr_cache_clear_for_path(_susp_path);
            cr_log("info", "addr_cache",
                   "cleared for crash_suspect title=%s path=%s",
                   prev.title_id, _susp_path);
          }
        }

        pthread_mutex_lock(&g_mods_disabled_lock);
        for (int i = g_mods_disabled_n - 1; i >= 0; i--) {
          if (g_mods_disabled[i].pid == prev.pid) {
            g_mods_disabled[i] = g_mods_disabled[--g_mods_disabled_n];
          }
        }
        pthread_mutex_unlock(&g_mods_disabled_lock);
        mod_enabled_clear_for_pid(prev.pid);
        /* Skip the ptrace-bearing patch restore while a cheat apply is in
         * progress: the apply thread already owns this pid under ptrace, so a
         * second pt_attach here would stall the 500 ms monitor poll for the full
         * 2 s attach timeout and fail anyway (pid already traced). If the game
         * genuinely died mid-apply the patched bytes are gone with the process,
         * so there is nothing to restore; backups are still cleared below and the
         * next poll re-evaluates once the apply releases. */
        if (!g_cheat_applying) {
          patch_restore_all_for_pid(prev.pid, prev.title_id);
        }
        patch_clear_backups_for_pid(prev.pid);
        patch_clear_for_pid(prev.pid);

        {
          uint64_t now_t = now_ms();
          uint64_t last_apply_t = g_last_apply_at_ms;
          int lm_idx = -1;
          char lm_name[64] = {0};
          pthread_mutex_lock(&g_last_apply_lock);
          if (g_last_apply_rec.ts_ms > 0 &&
              strcmp(g_last_apply_rec.title_id, prev.title_id) == 0) {
            lm_idx = g_last_apply_rec.mod_index;
            snprintf(lm_name, sizeof(lm_name), "%s", g_last_apply_rec.mod_name);
          }
          pthread_mutex_unlock(&g_last_apply_lock);
          pthread_mutex_lock(&g_last_game_exit_lock);
          memset(&g_last_game_exit, 0, sizeof(g_last_game_exit));
          snprintf(g_last_game_exit.title_id, sizeof(g_last_game_exit.title_id), "%s", prev.title_id);
          g_last_game_exit.app_id = prev.app_id;
          g_last_game_exit.pid = prev.pid;
          snprintf(g_last_game_exit.reason, sizeof(g_last_game_exit.reason), "%s", "game_stopped");
          g_last_game_exit.ts_ms = now_t;
          g_last_game_exit.elapsed_after_last_cheat_ms =
              (last_apply_t > 0 && now_t >= last_apply_t) ? (now_t - last_apply_t) : 0;
          g_last_game_exit.last_mod_index = lm_idx;
          snprintf(g_last_game_exit.last_mod_name, sizeof(g_last_game_exit.last_mod_name), "%s", lm_name);
          g_last_game_exit.suspected = just_suspected;
          pthread_mutex_unlock(&g_last_game_exit_lock);
        }
      }
    }
    if (cur.running) {
      notification_add("game_started", "Game started: %s", cur.title_id);
      cr_log("info", "game", "game started %s", cur.title_id);
      launch_status_recover_for_game(cur.title_id);
    }
  }

  /* Auto-clear crash suspects whose watch window passed without a game crash.
   * Runs every poll tick while the game is running. If a mod stayed enabled for
   * longer than cheat_post_apply_watch_ms with no crash, it is no longer a suspect. */
  if (cur.running) {
    int wcms = 8000;
    pthread_mutex_lock(&g_cfg_lock);
    if (g_cfg.cheat_post_apply_watch_ms > 0) wcms = g_cfg.cheat_post_apply_watch_ms;
    pthread_mutex_unlock(&g_cfg_lock);
    uint64_t now_t = now_ms();
    int suspects_auto_cleared = 0;
    pthread_mutex_lock(&g_crash_guard_lock);
    for (int _gi = g_crash_guard_n - 1; _gi >= 0; _gi--) {
      crash_guard_state_t *cg = &g_crash_guard_arr[_gi];
      if (cg->enabled_at_ms == 0) continue;
      if (strcmp(cg->title_id, cur.title_id) != 0 || cg->pid != cur.pid) continue;
      if (now_t - cg->enabled_at_ms < (uint64_t)wcms) continue;
      /* Window expired with no crash: remove matching suspects */
      for (int _si = g_crash_suspects_n - 1; _si >= 0; _si--) {
        if (strcmp(g_crash_suspects[_si].title_id, cg->title_id) == 0 &&
            g_crash_suspects[_si].mod_index == cg->mod_index) {
          g_crash_suspects[_si] = g_crash_suspects[--g_crash_suspects_n];
          suspects_auto_cleared++;
          cr_log("info", "cheats.guard",
                 "crash_suspect cleared: mod=%d name=\"%s\" survived %dms watch window",
                 cg->mod_index, cg->mod_name, wcms);
        }
      }
      g_crash_guard_arr[_gi] = g_crash_guard_arr[--g_crash_guard_n];
    }
    pthread_mutex_unlock(&g_crash_guard_lock);
    if (suspects_auto_cleared > 0) {
      crash_suspects_save();
    }
  }

  pthread_mutex_lock(&g_activity_lock);
  if (cur.running && (!g_activity_session_open || strcmp(g_activity_session_title_id, cur.title_id) != 0)) {
    if (g_activity_session_open && g_activity_session_title_id[0]) {
      time_t now = time(NULL);
      int idx = activity_find_title_index_locked(g_activity_session_title_id);
      if (idx >= 0 && now > g_activity_session_start) {
        g_activity_titles[idx].total_seconds += (unsigned int)(now - g_activity_session_start);
      }
    }
    g_activity_session_open = 1;
    g_activity_session_start = time(NULL);
    snprintf(g_activity_session_title_id, sizeof(g_activity_session_title_id), "%s", cur.title_id);
  } else if (!cur.running && g_activity_session_open) {
    time_t now = time(NULL);
    int idx = activity_find_title_index_locked(g_activity_session_title_id);
    if (idx >= 0 && now > g_activity_session_start) {
      g_activity_titles[idx].total_seconds += (unsigned int)(now - g_activity_session_start);
    }
    g_activity_session_open = 0;
    g_activity_session_title_id[0] = '\0';
    g_activity_session_start = 0;
  }
  pthread_mutex_unlock(&g_activity_lock);
}

void *
game_monitor_thread(void *arg) {
  (void)arg;
  time_t last_save = 0;
  while (g_game_monitor_running) {
    rpc_refresh_title_and_notify();
    time_t now = time(NULL);
    if (now - last_save >= 5) {
      activity_save();
      last_save = now;
    }
    int poll_ms = 500;
    pthread_mutex_lock(&g_cfg_lock);
    if (g_cfg.cheat_post_apply_poll_ms >= 100) {
      poll_ms = g_cfg.cheat_post_apply_poll_ms;
    }
    pthread_mutex_unlock(&g_cfg_lock);
    config_check_reload();
    usleep((useconds_t)(poll_ms * 1000));
  }
  return NULL;
}
