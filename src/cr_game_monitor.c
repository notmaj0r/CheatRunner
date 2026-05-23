#include <signal.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "cr_cheats.h"
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
  app_info_t info;
  memset(&info, 0, sizeof(info));
  if (sceKernelGetAppInfo(pid, &info) != 0 || info.title_id[0] == '\0') {
    return -1;
  }
  if (!is_game_title_id(info.title_id)) {
    return -1;
  }
  if (out_title && title_size > 0) {
    snprintf(out_title, title_size, "%s", info.title_id);
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
  read_param_value_by_title_id(title, "contentVersion", st.content_version, sizeof(st.content_version));
  read_param_value_by_title_id(title, "appVersion", st.app_version, sizeof(st.app_version));
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
        if (g_crash_guard.title_id[0] &&
            strcmp(g_crash_guard.title_id, prev.title_id) == 0 &&
            g_crash_guard.pid == prev.pid &&
            g_crash_guard.enabled_at_ms > 0) {
          uint64_t elapsed = now_ms() - g_crash_guard.enabled_at_ms;
          if (elapsed < (uint64_t)watch_ms && g_crash_suspects_n < CRASH_SUSPECT_MAX) {
            crash_suspect_rec_t *rec = &g_crash_suspects[g_crash_suspects_n++];
            snprintf(rec->title_id, sizeof(rec->title_id), "%s", g_crash_guard.title_id);
            rec->mod_index = g_crash_guard.mod_index;
            snprintf(rec->mod_name, sizeof(rec->mod_name), "%s", g_crash_guard.mod_name);
            rec->pid = g_crash_guard.pid;
            rec->elapsed_ms = elapsed;
            rec->ts = time(NULL);
            rec->app_id = prev.app_id;
            just_suspected = 1;
            cr_log("warn", "cheats.guard",
                   "game stopped %llums after enabling mod=%d name=\"%s\"; marking crash_suspect",
                   (unsigned long long)elapsed, g_crash_guard.mod_index, g_crash_guard.mod_name);
          }
          memset(&g_crash_guard, 0, sizeof(g_crash_guard));
        }
        pthread_mutex_unlock(&g_crash_guard_lock);

        pthread_mutex_lock(&g_mods_disabled_lock);
        for (int i = g_mods_disabled_n - 1; i >= 0; i--) {
          if (g_mods_disabled[i].pid == prev.pid) {
            g_mods_disabled[i] = g_mods_disabled[--g_mods_disabled_n];
          }
        }
        pthread_mutex_unlock(&g_mods_disabled_lock);

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
    usleep((useconds_t)(poll_ms * 1000));
  }
  return NULL;
}
