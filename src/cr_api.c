#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <ps5/kernel.h>

#include "ps5sdk_compat.h"
#include "jb.h"
#include "priv_bootstrap.h"
#include "pt.h"
#include "third_party/cJSON.h"
#include "cr_log.h"
#include "cr_paths.h"
#include "cr_config.h"
#include "cr_notifications.h"
#include "cr_activity.h"
#include "cr_titles.h"
#include "cr_appdb.h"
#include "cr_game_monitor.h"
#include "cr_launch.h"
#include "cr_memory.h"
#include "cr_json.h"
#include "cr_cheat_formats.h"
#include "cr_cheats.h"
#include "cr_http.h"
#include "cr_remote_sources.h"
#include "cr_source_jobs.h"
#include "cr_repo_mirror.h"
#include "cr_api.h"
#include "cr_api_internal.h"
#include "cr_api_dashboard.h"
#include "cr_api_games.h"
#include "cr_api_cheats.h"
#include "cr_api_sources.h"
#include "cr_api_dev.h"
#include "cr_api_logs.h"
#include "cr_shutdown.h"
#include "http_client.h"
#if CHEATRUNNER_HAVE_BROWSER_OPEN
#include "cr_browser.h"
#endif

#ifndef CHEATRUNNER_VERSION
#define CHEATRUNNER_VERSION "0.1"
#endif

#define MAX_GAMES 1024

const char g_dashboard_html[] =
#include "dashboard_html.inc"
;
#include "cheatrunner_png.h"

static int read_title_lookup_cache_name(const char *title_id, char *out, size_t out_size);
static int write_title_lookup_cache_name(const char *title_id, const char *name);
static int
url_decode(char *dst, size_t dst_size, const char *src, size_t src_len) {
  size_t o = 0;
  for (size_t i = 0; i < src_len; i++) {
    if (o + 1 >= dst_size) {
      return -1;
    }
    if (src[i] == '+') {
      dst[o++] = ' ';
      continue;
    }
    if (src[i] == '%' && i + 2 < src_len) {
      int hi = isdigit((unsigned char)src[i + 1]) ? src[i + 1] - '0' : (tolower((unsigned char)src[i + 1]) - 'a' + 10);
      int lo = isdigit((unsigned char)src[i + 2]) ? src[i + 2] - '0' : (tolower((unsigned char)src[i + 2]) - 'a' + 10);
      if (hi < 0 || hi > 15 || lo < 0 || lo > 15) {
        return -1;
      }
      dst[o++] = (char)((hi << 4) | lo);
      i += 2;
      continue;
    }
    dst[o++] = src[i];
  }
  dst[o] = '\0';
  return 0;
}

int
query_value(const char *query, const char *key, char *out, size_t out_size) {
  if (!query || !key || !out || out_size == 0) {
    return -1;
  }
  size_t key_len = strlen(key);
  const char *p = query;
  while (*p) {
    const char *amp = strchr(p, '&');
    size_t part_len = amp ? (size_t)(amp - p) : strlen(p);
    const char *eq = memchr(p, '=', part_len);
    if (eq) {
      size_t k_len = (size_t)(eq - p);
      if (k_len == key_len && strncmp(p, key, key_len) == 0) {
        const char *val = eq + 1;
        size_t val_len = part_len - (k_len + 1);
        return url_decode(out, out_size, val, val_len);
      }
    }
    if (!amp) {
      break;
    }
    p = amp + 1;
  }
  return -1;
}

static const char *
status_text_for(int status) {
  switch (status) {
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 409:
    return "Conflict";
  case 413:
    return "Payload Too Large";
  case 429:
    return "Too Many Requests";
  case 500:
    return "Internal Server Error";
  case 502:
    return "Bad Gateway";
  case 503:
    return "Service Unavailable";
  default:
    return "OK";
  }
}

static int
socket_send_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t *)buf;
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(fd, p + sent, len - sent, 0);
    if (n > 0) {
      sent += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return -1;
  }
  return 0;
}

void
http_send_response(int fd, int status, const char *content_type, const uint8_t *body, size_t body_len) {
  char header[512];
  int n = snprintf(header, sizeof(header),
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %u\r\n"
                   "Connection: close\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "Cache-Control: no-cache\r\n"
                   "\r\n",
                   status, status_text_for(status), content_type ? content_type : "application/octet-stream",
                   (unsigned int)body_len);
  if (n > 0) {
    if (socket_send_all(fd, header, (size_t)n) != 0) {
      return;
    }
    if (body_len > 0) {
      (void)socket_send_all(fd, body, body_len);
    }
  }
}

void
http_send_json(int fd, int status, const char *body) {
  http_send_response(fd, status, "application/json", (const uint8_t *)body, strlen(body));
}

static int
read_title_lookup_cache_name(const char *title_id, char *out, size_t out_size) {
  if (!title_id || !out || out_size == 0) {
    return 0;
  }
  out[0] = '\0';
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.txt", CHEATRUNNER_CACHE_TITLE_NAMES_DIR, title_id);
  char *txt = NULL;
  if (read_file_text(path, &txt) != 0 || !txt) {
    free(txt);
    return 0;
  }
  str_trim(txt);
  if (!txt[0] || cr_title_name_is_unresolved(title_id, txt)) {
    free(txt);
    return 0;
  }
  snprintf(out, out_size, "%s", txt);
  free(txt);
  return 1;
}

static int
write_title_lookup_cache_name(const char *title_id, const char *name) {
  if (!title_id || !name || !name[0] || cr_title_name_is_unresolved(title_id, name)) {
    return -1;
  }
  ensure_dir_recursive(CHEATRUNNER_CACHE_TITLE_NAMES_DIR);
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.txt", CHEATRUNNER_CACHE_TITLE_NAMES_DIR, title_id);
  return write_file_atomic(path, (const uint8_t *)name, strlen(name));
}

void
handle_api_status(int fd) {
  char title[32];
  char body[512];
  int http_port = CHEATRUNNER_HTTP_PORT;
  pthread_mutex_lock(&g_cfg_lock);
  http_port = g_cfg.http_port;
  pthread_mutex_unlock(&g_cfg_lock);
  get_current_title(title, sizeof(title));
  snprintf(body, sizeof(body),
           "{\"ok\":true,\"titleId\":\"%s\",\"httpPort\":%d,"
           "\"browserUrl\":\"http://%s:%d/\"}",
           title, http_port, g_listen_ip, http_port);
  http_send_json(fd, 200, body);
}

void
handle_api_launch_status(int fd) {
  char body[1024];
  char ls_phase[32];
  char ls_title[16];
  char ls_message[256];
  char ls_method[32];
  char ls_hex[16];
  int  ls_busy, ls_rc, ls_verified, ls_fgvalid;

  pthread_mutex_lock(&g_launch_status_lock);
  snprintf(ls_phase,   sizeof(ls_phase),   "%s", g_launch_status.phase);
  snprintf(ls_title,   sizeof(ls_title),   "%s", g_launch_status.title_id);
  snprintf(ls_message, sizeof(ls_message), "%s", g_launch_status.message);
  snprintf(ls_method,  sizeof(ls_method),  "%s", g_launch_status.method);
  snprintf(ls_hex,     sizeof(ls_hex),     "%s", g_launch_status.hex);
  ls_busy    = g_launch_status.busy;
  ls_rc      = g_launch_status.rc;
  ls_verified= g_launch_status.verified;
  ls_fgvalid = g_launch_status.foreground_user_valid;
  pthread_mutex_unlock(&g_launch_status_lock);

  /* Task 11: if launch ended in failure but the game is actually running,
     report ready so the UI is not stuck in a failed state */
  if (strcmp(ls_phase, "failed") == 0 && ls_title[0] != '\0') {
    running_game_state_t gm;
    running_state_get(&gm);
    if (gm.running && strcmp(gm.title_id, ls_title) == 0) {
      snprintf(ls_phase,   sizeof(ls_phase),   "ready");
      snprintf(ls_message, sizeof(ls_message), "Game is running; previous launch return code was ignored.");
      ls_verified = 1;
      ls_busy     = 0;
    }
  }

  snprintf(body, sizeof(body),
           "{\"ok\":true,\"busy\":%s,\"phase\":\"%s\",\"titleId\":\"%s\",\"message\":\"%s\","
           "\"method\":\"%s\",\"rc\":%d,\"hex\":\"%s\",\"verified\":%s,\"foregroundUserValid\":%s}",
           ls_busy ? "true" : "false", ls_phase, ls_title, ls_message,
           ls_method, ls_rc, ls_hex, ls_verified ? "true" : "false",
           ls_fgvalid ? "true" : "false");
  http_send_json(fd, 200, body);
}

void
handle_api_running(int fd) {
  running_game_state_t st;
  if (read_running_state(&st) != 0 || !st.running) {
    http_send_json(fd, 200, "{\"ok\":true,\"running\":false}");
    return;
  }
  char name[512];
  char cv[128];
  char av[128];
  char body[1400];
  char *name_esc = json_escape(st.title_name);
  snprintf(name, sizeof(name), "%s", name_esc ? name_esc : st.title_name);
  free(name_esc);
  char *cv_esc = json_escape(st.content_version);
  char *av_esc = json_escape(st.app_version);
  snprintf(cv, sizeof(cv), "%s", cv_esc ? cv_esc : st.content_version);
  snprintf(av, sizeof(av), "%s", av_esc ? av_esc : st.app_version);
  free(cv_esc);
  free(av_esc);
  snprintf(body, sizeof(body),
           "{\"ok\":true,\"running\":true,\"titleId\":\"%s\",\"titleName\":\"%s\",\"pid\":%d,"
           "\"appId\":\"0x%X\",\"imageBase\":\"0x%lx\",\"platform\":\"%s\","
           "\"contentVersion\":\"%s\",\"appVersion\":\"%s\",\"startedAt\":%llu}",
           st.title_id, name, (int)st.pid, (unsigned int)st.app_id, (long)st.image_base, st.platform, cv, av,
           (unsigned long long)st.started_at);
  http_send_json(fd, 200, body);
}

void
handle_api_debug_process(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 || !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad titleId\"}");
    return;
  }
  running_game_state_t st;
  if (read_running_state(&st) != 0 || !st.running || strcmp(st.title_id, title_id) != 0) {
    http_send_json(fd, 200, "{\"ok\":true,\"titleId\":\"\",\"running\":false}");
    return;
  }
  char body[1024];
  snprintf(body, sizeof(body),
           "{\"ok\":true,\"titleId\":\"%s\",\"running\":true,\"pid\":%d,\"appId\":\"0x%X\","
           "\"imageBase\":\"0x%lx\",\"ebootPath\":\"%s\",\"contentVersion\":\"%s\",\"appVersion\":\"%s\"}",
           st.title_id, (int)st.pid, (unsigned int)st.app_id, (long)st.image_base, st.eboot_path, st.content_version,
           st.app_version);
  http_send_json(fd, 200, body);
}

void
handle_api_config(int fd) {
  char body[4096];
  pthread_mutex_lock(&g_cfg_lock);
  snprintf(body, sizeof(body),
           "{\"ok\":true,\"http_port\":%d,\"auto_load_cheat_menu\":%d,"
           "\"auto_download_missing_cheat\":%d,\"launch_kill_current\":%d,\"launch_kill_delay_ms\":%d,"
           "\"launch_wait_timeout_ms\":%d,\"cheat_engine\":%d,\"cheat_validate_original_bytes\":%d,"
           "\"cheat_restore_rx\":%d,\"cheat_index_cache_ttl_sec\":%d,\"allow_force_enable\":%d,"
           "\"cheat_state_after_launch_delay_ms\":%d,\"dev_reload_enabled\":%d,\"dev_shutdown_delay_ms\":%d,"
           "\"cheat_sources_enabled\":%d,\"cheat_remote_download_enabled\":%d,"
           "\"cheat_source_cache_ttl_seconds\":%d,\"cheat_remote_max_file_bytes\":%d,"
           "\"title_lookup_enabled\":%d,\"title_lookup_cache_enabled\":%d,\"title_lookup_timeout_ms\":%d,"
           "\"games_cache_ttl_ms\":%d,\"appdb_debug_names\":%d,\"log_level\":\"%s\","
           "\"theme\":\"%s\"}",
           g_cfg.http_port, g_cfg.auto_load_cheat_menu,
           g_cfg.auto_download_missing_cheat, g_cfg.launch_kill_current, g_cfg.launch_kill_delay_ms,
           g_cfg.launch_wait_timeout_ms, g_cfg.cheat_engine, g_cfg.cheat_validate_original_bytes, g_cfg.cheat_restore_rx,
           g_cfg.cheat_index_cache_ttl_sec, g_cfg.allow_force_enable, g_cfg.cheat_state_after_launch_delay_ms,
           g_cfg.dev_reload_enabled, g_cfg.dev_shutdown_delay_ms,
           g_cfg.cheat_sources_enabled, g_cfg.cheat_remote_download_enabled,
           g_cfg.cheat_source_cache_ttl_seconds, g_cfg.cheat_remote_max_file_bytes,
           g_cfg.title_lookup_enabled, g_cfg.title_lookup_cache_enabled, g_cfg.title_lookup_timeout_ms,
           g_cfg.games_cache_ttl_ms, g_cfg.appdb_debug_names, g_cfg.log_level,
           g_cfg.theme);
  pthread_mutex_unlock(&g_cfg_lock);
  http_send_json(fd, 200, body);
}

void
handle_api_config_set(int fd, const char *query) {
  char key[64] = {0};
  char value[128] = {0};
  if (query_value(query, "key", key, sizeof(key)) != 0 || query_value(query, "value", value, sizeof(value)) != 0) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"missing key/value\"}");
    return;
  }
  pthread_mutex_lock(&g_cfg_lock);
  if (!strcmp(key, "auto_load_cheat_menu")) {
    g_cfg.auto_load_cheat_menu = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "rpc_legacy_enabled") || !strcmp(key, "rpc_json_enabled") ||
             !strcmp(key, "rpc_legacy_port") || !strcmp(key, "rpc_json_port") ||
             !strcmp(key, "rpc_heartbeat_sec") || !strcmp(key, "rpc_emit_cheat_events") ||
             !strcmp(key, "rpc_port")) {
    /* removed — silently ignore */
    (void)value;
  } else if (!strcmp(key, "auto_download_missing_cheat")) {
    g_cfg.auto_download_missing_cheat = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "launch_kill_current")) {
    g_cfg.launch_kill_current = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "launch_kill_delay_ms")) {
    g_cfg.launch_kill_delay_ms = atoi(value);
  } else if (!strcmp(key, "launch_wait_timeout_ms")) {
    g_cfg.launch_wait_timeout_ms = atoi(value);
  } else if (!strcmp(key, "cheat_engine")) {
    g_cfg.cheat_engine = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "cheat_validate_original_bytes")) {
    g_cfg.cheat_validate_original_bytes = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "cheat_restore_rx")) {
    g_cfg.cheat_restore_rx = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "cheat_index_cache_ttl_sec")) {
    g_cfg.cheat_index_cache_ttl_sec = atoi(value);
  } else if (!strcmp(key, "allow_force_enable")) {
    g_cfg.allow_force_enable = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "cheat_state_after_launch_delay_ms")) {
    g_cfg.cheat_state_after_launch_delay_ms = atoi(value);
    if (g_cfg.cheat_state_after_launch_delay_ms < 0 || g_cfg.cheat_state_after_launch_delay_ms > 60000) {
      g_cfg.cheat_state_after_launch_delay_ms = 8000;
    }
  } else if (!strcmp(key, "dev_reload_enabled")) {
    g_cfg.dev_reload_enabled = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "dev_shutdown_delay_ms")) {
    g_cfg.dev_shutdown_delay_ms = atoi(value);
    if (g_cfg.dev_shutdown_delay_ms < 0 || g_cfg.dev_shutdown_delay_ms > 10000) {
      g_cfg.dev_shutdown_delay_ms = 700;
    }
  } else if (!strcmp(key, "dev_reload_token")) {
    /* removed — silently ignore */
    (void)value;
  } else if (!strcmp(key, "cheat_post_apply_watch_ms")) {
    g_cfg.cheat_post_apply_watch_ms = atoi(value);
    if (g_cfg.cheat_post_apply_watch_ms < 0) g_cfg.cheat_post_apply_watch_ms = 8000;
  } else if (!strcmp(key, "cheat_mark_crash_suspect")) {
    g_cfg.cheat_mark_crash_suspect = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "cheat_sources_enabled")) {
    g_cfg.cheat_sources_enabled = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "cheat_remote_download_enabled")) {
    g_cfg.cheat_remote_download_enabled = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "cheat_source_cache_ttl_seconds")) {
    int v = atoi(value);
    g_cfg.cheat_source_cache_ttl_seconds = (v >= 60 && v <= 604800) ? v : 21600;
  } else if (!strcmp(key, "cheat_remote_max_file_bytes")) {
    int v = atoi(value);
    g_cfg.cheat_remote_max_file_bytes = (v >= 1024 && v <= (8 * 1024 * 1024)) ? v : 1048576;
  } else if (!strcmp(key, "title_lookup_enabled")) {
    g_cfg.title_lookup_enabled = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "title_lookup_cache_enabled")) {
    g_cfg.title_lookup_cache_enabled = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "title_lookup_timeout_ms")) {
    int v = atoi(value);
    g_cfg.title_lookup_timeout_ms = (v >= 1000 && v <= 30000) ? v : 8000;
  } else if (!strcmp(key, "games_cache_ttl_ms")) {
    int v = atoi(value);
    g_cfg.games_cache_ttl_ms = (v >= 1000 && v <= 300000) ? v : 30000;
  } else if (!strcmp(key, "appdb_debug_names")) {
    g_cfg.appdb_debug_names = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "log_level")) {
    if (!strcmp(value, "debug") || !strcmp(value, "info") || !strcmp(value, "warn") || !strcmp(value, "error")) {
      snprintf(g_cfg.log_level, sizeof(g_cfg.log_level), "%s", value);
      cr_log_set_level(g_cfg.log_level);
    } else {
      pthread_mutex_unlock(&g_cfg_lock);
      http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid value; allowed: debug info warn error\"}");
      return;
    }
  } else if (!strcmp(key, "theme")) {
    snprintf(g_cfg.theme, sizeof(g_cfg.theme), "%s", value);
  } else if (!strncmp(key, "hotkey_", 7)) {
    /* removed — silently ignore */
    (void)value;
  } else {
    pthread_mutex_unlock(&g_cfg_lock);
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"unknown key\"}");
    return;
  }
  int rc = config_save_locked();
  pthread_mutex_unlock(&g_cfg_lock);
  cr_log("info", "config", "config changed: %s=%s", key, value);
  http_send_json(fd, rc == 0 ? 200 : 500, rc == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"save failed\"}");
}

static volatile long long g_startup_ms     = 0;
static pthread_mutex_t   g_games_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static char             *g_games_cache_json = NULL;
static size_t            g_games_cache_len  = 0;
static int               g_games_scan_busy  = 0;
static long long         g_games_cache_ts_ms = 0;
static long long         g_games_last_refresh_req_ms = 0;

void
handle_api_health(int fd) {
  struct timespec _now;
  clock_gettime(CLOCK_MONOTONIC, &_now);
  long long now_ms = (long long)_now.tv_sec * 1000LL + (long long)(_now.tv_nsec / 1000000L);
  long long uptime  = g_startup_ms ? (now_ms - g_startup_ms) : 0;

  pthread_mutex_lock(&g_games_cache_lock);
  int games_busy = g_games_scan_busy;
  pthread_mutex_unlock(&g_games_cache_lock);

  int source_busy = cr_source_jobs_is_busy();

  pthread_mutex_lock(&g_repo_mirror.lock);
  int mirror_busy = (g_repo_mirror.state == REPO_MIRROR_RUNNING);
  pthread_mutex_unlock(&g_repo_mirror.lock);

  int  http_active  = cr_http_active_clients();
  int  http_max     = cr_http_max_concurrent();
  long long http_tmr = cr_http_too_many_requests_count();

  char buf[384];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"version\":\"%s\",\"uptimeMs\":%lld,"
           "\"busy\":{\"gamesScan\":%s,\"sourceJob\":%s,\"repoMirror\":%s},"
           "\"http\":{\"activeClients\":%d,\"maxConcurrent\":%d,\"tooManyRequestsCount\":%lld}}",
           CHEATRUNNER_VERSION, uptime,
           games_busy  ? "true" : "false",
           source_busy ? "true" : "false",
           mirror_busy ? "true" : "false",
           http_active, http_max, http_tmr);
  http_send_json(fd, 200, buf);
}

void
handle_api_logs(int fd, const char *query) {
  int limit = 200;
  int since = -1;
  char buf[32];
  buf[0] = '\0';
  if (query_value(query, "all", buf, sizeof(buf)) == 0 && atoi(buf) != 0) {
    limit = MAX_LOG_ENTRIES;
  } else if (query_value(query, "limit", buf, sizeof(buf)) == 0) {
    int v = atoi(buf);
    if (v > 0 && v <= MAX_LOG_ENTRIES) { limit = v; }
  }
  if (query_value(query, "since", buf, sizeof(buf)) == 0) {
    since = atoi(buf);
  }

  cJSON *root = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(root, "logs");
  cJSON_AddBoolToObject(root, "ok", 1);

  int last_seq = -1;
  int truncated = 0;
  pthread_mutex_lock(&g_log_lock);
  int eligible = 0;
  for (int i = 0; i < g_log_count; i++) {
    int idx = (g_log_start + i) % MAX_LOG_ENTRIES;
    if (since < 0 || g_logs[idx].seq > since) { eligible++; }
  }
  int skip = (eligible > limit) ? (eligible - limit) : 0;
  if (skip > 0) { truncated = 1; }
  int skipped = 0;
  for (int i = 0; i < g_log_count; i++) {
    int idx = (g_log_start + i) % MAX_LOG_ENTRIES;
    if (since >= 0 && g_logs[idx].seq <= since) { continue; }
    if (skipped < skip) { skipped++; continue; }
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "seq", g_logs[idx].seq);
    cJSON_AddNumberToObject(e, "ts", (double)g_logs[idx].ts);
    cJSON_AddStringToObject(e, "level", g_logs[idx].level);
    cJSON_AddStringToObject(e, "tag", g_logs[idx].tag);
    cJSON_AddStringToObject(e, "message", g_logs[idx].message);
    cJSON_AddItemToArray(arr, e);
    last_seq = g_logs[idx].seq;
  }
  pthread_mutex_unlock(&g_log_lock);

  cJSON_AddNumberToObject(root, "lastSeq", last_seq);
  cJSON_AddBoolToObject(root, "truncated", truncated);
  char *txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!txt) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}");
    return;
  }
  http_send_response(fd, 200, "application/json", (const uint8_t *)txt, strlen(txt));
  free(txt);
}

void
handle_api_logs_clear(int fd) {
  pthread_mutex_lock(&g_log_lock);
  g_log_start = 0;
  g_log_count = 0;
  pthread_mutex_unlock(&g_log_lock);
  http_send_json(fd, 200, "{\"ok\":true}");
}

void
handle_api_notifications(int fd) {
  cJSON *root = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(root, "notifications");
  cJSON_AddBoolToObject(root, "ok", 1);
  pthread_mutex_lock(&g_notifications_lock);
  for (int i = 0; i < g_notifications_count; i++) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "id", g_notifications[i].id);
    cJSON_AddNumberToObject(e, "ts", (double)g_notifications[i].ts);
    cJSON_AddBoolToObject(e, "read", g_notifications[i].read ? 1 : 0);
    cJSON_AddStringToObject(e, "kind", g_notifications[i].kind);
    cJSON_AddStringToObject(e, "message", g_notifications[i].message);
    cJSON_AddItemToArray(arr, e);
  }
  pthread_mutex_unlock(&g_notifications_lock);
  char *txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!txt) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}");
    return;
  }
  http_send_response(fd, 200, "application/json", (const uint8_t *)txt, strlen(txt));
  free(txt);
}

void
handle_api_notifications_read(int fd, const char *query) {
  char id_s[32] = {0};
  int mark_all = query_value(query, "all", id_s, sizeof(id_s)) == 0 && atoi(id_s) == 1;
  int wanted_id = -1;
  if (!mark_all && query_value(query, "id", id_s, sizeof(id_s)) == 0) {
    wanted_id = atoi(id_s);
  }
  pthread_mutex_lock(&g_notifications_lock);
  for (int i = 0; i < g_notifications_count; i++) {
    if (mark_all || (wanted_id > 0 && g_notifications[i].id == wanted_id)) {
      g_notifications[i].read = 1;
    }
  }
  pthread_mutex_unlock(&g_notifications_lock);
  notifications_save();
  http_send_json(fd, 200, "{\"ok\":true}");
}

void
handle_api_notifications_clear(int fd) {
  pthread_mutex_lock(&g_notifications_lock);
  g_notifications_count = 0;
  pthread_mutex_unlock(&g_notifications_lock);
  notifications_save();
  http_send_json(fd, 200, "{\"ok\":true}");
}

void
handle_api_activity(int fd) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", 1);
  pthread_mutex_lock(&g_activity_lock);
  cJSON_AddNumberToObject(root, "launchCount", g_activity_launch_count);
  cJSON_AddNumberToObject(root, "lastLaunch", (double)g_activity_last_launch);
  cJSON_AddStringToObject(root, "lastPlayedTitleId", g_activity_last_played_title_id);
  cJSON_AddStringToObject(root, "lastCheatUsed", g_activity_last_cheat_used);
  cJSON *titles = cJSON_AddArrayToObject(root, "titles");
  for (int i = 0; i < g_activity_titles_count; i++) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "titleId", g_activity_titles[i].title_id);
    cJSON_AddNumberToObject(e, "launchCount", g_activity_titles[i].launch_count);
    cJSON_AddNumberToObject(e, "lastLaunch", (double)g_activity_titles[i].last_launch);
    cJSON_AddNumberToObject(e, "totalSeconds", g_activity_titles[i].total_seconds);
    cJSON_AddStringToObject(e, "lastCheat", g_activity_titles[i].last_cheat);
    cJSON_AddItemToArray(titles, e);
  }
  pthread_mutex_unlock(&g_activity_lock);
  char *txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!txt) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}");
    return;
  }
  http_send_response(fd, 200, "application/json", (const uint8_t *)txt, strlen(txt));
  free(txt);
}

void
handle_api_activity_title(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 ||
      !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad titleId\"}");
    return;
  }
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", 1);
  cJSON_AddStringToObject(root, "titleId", title_id);
  pthread_mutex_lock(&g_activity_lock);
  int idx = activity_find_title_index_locked(title_id);
  if (idx >= 0) {
    cJSON_AddNumberToObject(root, "launchCount", g_activity_titles[idx].launch_count);
    cJSON_AddNumberToObject(root, "lastLaunch", (double)g_activity_titles[idx].last_launch);
    cJSON_AddNumberToObject(root, "totalSeconds", g_activity_titles[idx].total_seconds);
    cJSON_AddStringToObject(root, "lastCheat", g_activity_titles[idx].last_cheat);
  } else {
    cJSON_AddNumberToObject(root, "launchCount", 0);
    cJSON_AddNumberToObject(root, "lastLaunch", 0);
    cJSON_AddNumberToObject(root, "totalSeconds", 0);
    cJSON_AddStringToObject(root, "lastCheat", "");
  }
  pthread_mutex_unlock(&g_activity_lock);
  char *txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!txt) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}");
    return;
  }
  http_send_response(fd, 200, "application/json", (const uint8_t *)txt, strlen(txt));
  free(txt);
}

void
handle_api_activity_reset(int fd) {
  pthread_mutex_lock(&g_activity_lock);
  g_activity_launch_count = 0;
  g_activity_last_launch = 0;
  g_activity_titles_count = 0;
  g_activity_last_played_title_id[0] = '\0';
  g_activity_last_cheat_used[0] = '\0';
  pthread_mutex_unlock(&g_activity_lock);
  activity_save();
  http_send_json(fd, 200, "{\"ok\":true}");
}

void
handle_api_state(int fd) {
  running_game_state_t rs;
  running_state_get(&rs);
  int local_count = 0;
  int has_cheat = 0;
  int cheat_kind = 0;
  const char *cheat_fmt = "";
  char cheat_path[256];
  if (rs.running && find_cheat_file_for_title(rs.title_id, cheat_path, sizeof(cheat_path), &cheat_kind)) {
    has_cheat = 1;
    cheat_fmt = cheat_kind == 1 ? "json" : (cheat_kind == 2 ? "shn" : "mc4");
  }
  DIR *d = opendir(CHEATRUNNER_CHEATS_DIR);
  if (d) {
    struct dirent *ent = NULL;
    while ((ent = readdir(d)) != NULL) {
      if (ent->d_name[0] == '.') {
        continue;
      }
      if (recognised_cheat_extension(ent->d_name)) {
        local_count++;
      }
    }
    closedir(d);
  }
  int http_port = CHEATRUNNER_HTTP_PORT;
  int cheat_engine = 1;
  int auto_load = 1;
  int auto_download = 0;
  int allow_force_enable = 0;
  int cheat_state_delay_ms = 8000;
  int dev_reload_enabled = 0;
  int dev_shutdown_delay_ms = 700;
  pthread_mutex_lock(&g_cfg_lock);
  http_port = g_cfg.http_port;
  cheat_engine = g_cfg.cheat_engine;
  auto_load = g_cfg.auto_load_cheat_menu;
  auto_download = g_cfg.auto_download_missing_cheat;
  allow_force_enable = g_cfg.allow_force_enable;
  cheat_state_delay_ms = g_cfg.cheat_state_after_launch_delay_ms;
  dev_reload_enabled = g_cfg.dev_reload_enabled;
  dev_shutdown_delay_ms = g_cfg.dev_shutdown_delay_ms;
  pthread_mutex_unlock(&g_cfg_lock);
  char last_local[128];
  pthread_mutex_lock(&g_activity_lock);
  snprintf(last_local, sizeof(last_local), "%s", g_activity_last_cheat_used);
  pthread_mutex_unlock(&g_activity_lock);
  char *name_esc = json_escape(rs.title_name);
  char *last_esc = json_escape(last_local);
  char body[4096];
  if (rs.running) {
    snprintf(
        body, sizeof(body),
        "{\"ok\":true,\"version\":\"%s\",\"browserUrl\":\"http://%s:%d/\",\"httpPort\":%d,"
        "\"cheatSource\":\"local\",\"running\":{\"running\":true,\"titleId\":\"%s\",\"titleName\":\"%s\","
        "\"platform\":\"%s\",\"appId\":\"0x%X\",\"pid\":%d,\"imageBase\":\"0x%lx\",\"contentVersion\":\"%s\","
        "\"appVersion\":\"%s\",\"startedAt\":%llu,\"hasCheat\":%s,\"cheatFormat\":%s},"
        "\"cheats\":{\"engine\":%s,\"activeCount\":0,\"lastApplied\":\"%s\",\"localCount\":%d},"
        "\"config\":{\"autoLoadCheatMenu\":%s,"
        "\"autoDownloadMissingCheat\":%s,\"allowForceEnable\":%s,\"cheatStateAfterLaunchDelayMs\":%d},"
        "\"dev\":{\"reloadEnabled\":%s,\"shutdownDelayMs\":%d}}",
        CHEATRUNNER_VERSION, g_listen_ip, http_port, http_port, rs.title_id, name_esc ? name_esc : rs.title_name,
        rs.platform, rs.app_id, (int)rs.pid, (long)rs.image_base, rs.content_version, rs.app_version,
        (unsigned long long)rs.started_at, has_cheat ? "true" : "false",
        has_cheat ? "\"json\"" : "null", cheat_engine ? "true" : "false", last_esc ? last_esc : "", local_count,
        auto_load ? "true" : "false", auto_download ? "true" : "false", allow_force_enable ? "true" : "false",
        cheat_state_delay_ms, dev_reload_enabled ? "true" : "false", dev_shutdown_delay_ms);
    if (has_cheat) {
      char fixed[4096];
      snprintf(
          fixed, sizeof(fixed),
          "{\"ok\":true,\"version\":\"%s\",\"browserUrl\":\"http://%s:%d/\",\"httpPort\":%d,"
          "\"cheatSource\":\"local\",\"running\":{\"running\":true,\"titleId\":\"%s\",\"titleName\":\"%s\","
          "\"platform\":\"%s\",\"appId\":\"0x%X\",\"pid\":%d,\"imageBase\":\"0x%lx\",\"contentVersion\":\"%s\","
          "\"appVersion\":\"%s\",\"startedAt\":%llu,\"hasCheat\":true,\"cheatFormat\":\"%s\"},"
          "\"cheats\":{\"engine\":%s,\"activeCount\":0,\"lastApplied\":\"%s\",\"localCount\":%d},"
          "\"config\":{\"autoLoadCheatMenu\":%s,"
          "\"autoDownloadMissingCheat\":%s,\"allowForceEnable\":%s,\"cheatStateAfterLaunchDelayMs\":%d},"
          "\"dev\":{\"reloadEnabled\":%s,\"shutdownDelayMs\":%d}}",
          CHEATRUNNER_VERSION, g_listen_ip, http_port, http_port, rs.title_id,
          name_esc ? name_esc : rs.title_name, rs.platform, rs.app_id, (int)rs.pid, (long)rs.image_base,
          rs.content_version, rs.app_version, (unsigned long long)rs.started_at, cheat_fmt,
          cheat_engine ? "true" : "false", last_esc ? last_esc : "", local_count,
          auto_load ? "true" : "false",
          auto_download ? "true" : "false", allow_force_enable ? "true" : "false", cheat_state_delay_ms,
          dev_reload_enabled ? "true" : "false", dev_shutdown_delay_ms);
      snprintf(body, sizeof(body), "%s", fixed);
    }
  } else {
    snprintf(
        body, sizeof(body),
        "{\"ok\":true,\"version\":\"%s\",\"browserUrl\":\"http://%s:%d/\",\"httpPort\":%d,"
        "\"cheatSource\":\"local\",\"running\":{\"running\":false},"
        "\"cheats\":{\"engine\":%s,\"activeCount\":0,\"lastApplied\":\"%s\",\"localCount\":%d},"
        "\"config\":{\"autoLoadCheatMenu\":%s,"
        "\"autoDownloadMissingCheat\":%s,\"allowForceEnable\":%s,\"cheatStateAfterLaunchDelayMs\":%d},"
        "\"dev\":{\"reloadEnabled\":%s,\"shutdownDelayMs\":%d}}",
        CHEATRUNNER_VERSION, g_listen_ip, http_port, http_port, cheat_engine ? "true" : "false",
        last_esc ? last_esc : "", local_count,
        auto_load ? "true" : "false", auto_download ? "true" : "false",
        allow_force_enable ? "true" : "false", cheat_state_delay_ms, dev_reload_enabled ? "true" : "false",
        dev_shutdown_delay_ms);
  }
  /* Splice in appdb diagnostics before final closing brace */
  {
    char amode[32] = {0}, areason[64] = {0};
    size_t alast = 0;
    appdb_diag_get(amode, sizeof(amode), areason, sizeof(areason), &alast);
    size_t blen = strlen(body);
    if (blen > 0 && body[blen - 1] == '}') {
      char *aesc = json_escape(areason);
      int compiled = CHEATRUNNER_HAVE_SQLITE_APPDB;
      int is_sqlite = (strcmp(amode, "sqlite") == 0);
      char appdb_frag[384];
      snprintf(appdb_frag, sizeof(appdb_frag),
               ",\"appdb\":{\"compiled\":%s,\"mode\":\"%s\",\"fallback\":%s,\"lastCount\":%u,\"reason\":\"%s\"}}",
               compiled ? "true" : "false",
               amode[0] ? amode : "unknown",
               is_sqlite ? "false" : "true",
               (unsigned)alast,
               aesc ? aesc : "");
      free(aesc);
      if (blen - 1 + strlen(appdb_frag) < sizeof(body)) {
        snprintf(body + blen - 1, sizeof(body) - blen + 1, "%s", appdb_frag);
      }
    }
  }
  free(last_esc);
  free(name_esc);
  http_send_json(fd, 200, body);
}

/* Returns length of the games JSON array written into buf (does NOT include NUL). */
static size_t
build_games_json_array(char *buf, size_t cap, int debug_names) {
  game_entry_t *entries = calloc(MAX_GAMES, sizeof(*entries));
  if (!entries) {
    if (cap >= 3) { buf[0] = '['; buf[1] = ']'; buf[2] = '\0'; }
    return 2;
  }
  size_t count = 0;
  appdb_collect_games(entries, &count);
  char running_title[32];
  get_current_title(running_title, sizeof(running_title));
  size_t off = 0;
  off += (size_t)snprintf(buf + off, cap - off, "[");
  size_t emitted = 0;
  for (size_t i = 0; i < count; i++) {
    if (!is_game_title_id(entries[i].title_id)) continue;
    char ver[32] = "";
    char content_id[96] = "";
    char icon_path[512];
    char pic0_path[512];
    char cheat_path[256];
    int cheat_kind = 0;
    int has_icon = resolve_icon_path(entries[i].title_id, icon_path, sizeof(icon_path)) == 0;
    int has_pic0 = resolve_pic0_path(entries[i].title_id, pic0_path, sizeof(pic0_path)) == 0;
    int has_cheat = find_cheat_file_for_title(entries[i].title_id, cheat_path, sizeof(cheat_path), &cheat_kind);
    int is_app = cr_title_is_known_media_app(entries[i].title_id, entries[i].title_name);
    const char *platform = is_app ? "APP" : platform_for_title_id(entries[i].title_id);
    int running = strcmp(running_title, entries[i].title_id) == 0;
    const char *cheat_fmt = has_cheat ? (cheat_kind == 1 ? "json" : (cheat_kind == 2 ? "shn" : "mc4")) : "";
    if (read_param_value_by_title_id(entries[i].title_id, "contentVersion", ver, sizeof(ver)) != 0)
      read_param_value_by_title_id(entries[i].title_id, "appVersion", ver, sizeof(ver));
    if (entries[i].content_id[0])
      snprintf(content_id, sizeof(content_id), "%s", entries[i].content_id);
    else
      read_param_value_by_title_id(entries[i].title_id, "contentId", content_id, sizeof(content_id));
    char *name_esc = json_escape(entries[i].title_name);
    char *ver_esc  = json_escape(ver[0] ? ver : "unknown");
    char *cid_esc  = json_escape(content_id[0] ? content_id : "");
    if (!name_esc) { free(ver_esc); free(cid_esc); continue; }
    off += (size_t)snprintf(
        buf + off, cap - off,
        "%s{\"titleId\":\"%s\",\"titleName\":\"%s\",\"platform\":\"%s\",\"kind\":\"%s\",\"isApp\":%s,"
        "\"version\":\"%s\","
        "\"contentId\":\"%s\",\"icon\":%s,\"iconUrl\":\"/appdb/icon?id=%s\",\"pic0\":%s,"
        "\"pic0Url\":\"/appdb/pic0?id=%s\",\"hasCheat\":%s,\"cheatFormat\":\"%s\",\"running\":%s",
        emitted == 0 ? "" : ",", entries[i].title_id, name_esc, platform,
        is_app ? "app" : "game", is_app ? "true" : "false",
        ver_esc ? ver_esc : "unknown", cid_esc ? cid_esc : "",
        has_icon ? "true" : "false", entries[i].title_id, has_pic0 ? "true" : "false", entries[i].title_id,
        has_cheat ? "true" : "false", cheat_fmt, running ? "true" : "false");
    if (debug_names) {
      char *src_esc = json_escape(entries[i].name_source[0] ? entries[i].name_source : "fallback_title_id");
      off += (size_t)snprintf(buf + off, cap - off, ",\"nameSource\":\"%s\",\"nameResolved\":%s",
                              src_esc ? src_esc : "fallback_title_id",
                              entries[i].name_resolved ? "true" : "false");
      free(src_esc);
    }
    off += (size_t)snprintf(buf + off, cap - off, "}");
    emitted++;
    free(name_esc); free(ver_esc); free(cid_esc);
    if (off + 256 >= cap) break;
  }
  off += (size_t)snprintf(buf + off, cap - off, "]");
  free(entries);
  return off;
}

static void *
games_scan_thread(void *arg) {
  (void)arg;
  size_t cap = 262144;
  char *arr = malloc(cap);
  if (!arr) {
    pthread_mutex_lock(&g_games_cache_lock);
    g_games_scan_busy = 0;
    pthread_mutex_unlock(&g_games_cache_lock);
    return NULL;
  }
  size_t arr_len = build_games_json_array(arr, cap, 0);
  pthread_mutex_lock(&g_games_cache_lock);
  free(g_games_cache_json);
  g_games_cache_json = arr;
  g_games_cache_len  = arr_len;
  g_games_cache_ts_ms = (long long)now_ms();
  g_games_scan_busy  = 0;
  pthread_mutex_unlock(&g_games_cache_lock);
  return NULL;
}

void
handle_api_games(int fd, const char *query) {
  int debug_names = 0;
  char dbg_q[8] = {0};
  if (query_value(query, "debug", dbg_q, sizeof(dbg_q)) == 0 && atoi(dbg_q) != 0)
    debug_names = 1;

  /* debug mode: bypass cache, scan synchronously */
  if (debug_names) {
    size_t cap = 262144;
    char *arr = malloc(cap);
    if (!arr) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}"); return; }
    size_t arr_len = build_games_json_array(arr, cap, 1);
    size_t resp_cap = arr_len + 32;
    char *resp = malloc(resp_cap);
    if (!resp) { free(arr); http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}"); return; }
    size_t resp_len = (size_t)snprintf(resp, resp_cap, "{\"ok\":true,\"games\":%.*s}", (int)arr_len, arr);
    http_send_response(fd, 200, "application/json", (const uint8_t *)resp, resp_len);
    free(resp); free(arr);
    return;
  }

  char refresh_q[8] = {0};
  int force_refresh = (query_value(query, "refresh", refresh_q, sizeof(refresh_q)) == 0 && atoi(refresh_q) != 0);
  const int refresh_min_interval_ms = 5000;
  int ttl_ms = 30000;
  pthread_mutex_lock(&g_cfg_lock);
  ttl_ms = g_cfg.games_cache_ttl_ms;
  pthread_mutex_unlock(&g_cfg_lock);
  if (ttl_ms < 1000 || ttl_ms > 300000) {
    ttl_ms = 30000;
  }
  long long now = (long long)now_ms();

  int need_start = 0;
  int throttled = 0;
  long long cache_age_ms = -1;
  pthread_mutex_lock(&g_games_cache_lock);
  int has_cache = (g_games_cache_json != NULL);
  if (has_cache && g_games_cache_ts_ms > 0 && now >= g_games_cache_ts_ms) {
    cache_age_ms = now - g_games_cache_ts_ms;
  }

  int cache_fresh = (has_cache && cache_age_ms >= 0 && cache_age_ms < ttl_ms);
  if (!g_games_scan_busy) {
    if (!has_cache) {
      g_games_scan_busy = 1;
      g_games_last_refresh_req_ms = now;
      need_start = 1;
    } else if (force_refresh) {
      if (g_games_last_refresh_req_ms > 0 && (now - g_games_last_refresh_req_ms) < refresh_min_interval_ms) {
        throttled = 1;
      } else {
        g_games_scan_busy = 1;
        g_games_last_refresh_req_ms = now;
        need_start = 1;
      }
    } else if (!cache_fresh) {
      g_games_scan_busy = 1;
      g_games_last_refresh_req_ms = now;
      need_start = 1;
    }
  }
  int busy = g_games_scan_busy;
  pthread_mutex_unlock(&g_games_cache_lock);

  if (need_start) {
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, games_scan_thread, NULL) != 0) {
      pthread_mutex_lock(&g_games_cache_lock);
      g_games_scan_busy = 0;
      busy = 0;
      if (!g_games_cache_json) {
        cache_age_ms = -1;
      }
      pthread_mutex_unlock(&g_games_cache_lock);
    }
    pthread_attr_destroy(&attr);
  }

  pthread_mutex_lock(&g_games_cache_lock);
  char *arr_copy = NULL;
  size_t arr_len = 0;
  if (g_games_cache_json) {
    arr_len  = g_games_cache_len;
    arr_copy = malloc(arr_len + 1);
    if (arr_copy) { memcpy(arr_copy, g_games_cache_json, arr_len); arr_copy[arr_len] = '\0'; }
    if (g_games_cache_ts_ms > 0 && now >= g_games_cache_ts_ms) {
      cache_age_ms = now - g_games_cache_ts_ms;
    }
  }
  pthread_mutex_unlock(&g_games_cache_lock);

  if (!arr_copy) {
    const char *msg = busy
      ? "{\"ok\":true,\"games\":[],\"refreshing\":true,\"cacheAgeMs\":-1,\"throttled\":false}"
      : "{\"ok\":true,\"games\":[],\"refreshing\":false,\"cacheAgeMs\":-1,\"throttled\":false}";
    http_send_json(fd, 200, msg);
    return;
  }

  if (cache_age_ms < 0) {
    cache_age_ms = 0;
  }
  size_t resp_cap = arr_len + 128;
  char *resp = malloc(resp_cap);
  if (!resp) { free(arr_copy); http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}"); return; }
  size_t resp_len = (size_t)snprintf(resp, resp_cap,
                                     "{\"ok\":true,\"games\":%.*s,\"refreshing\":%s,\"cacheAgeMs\":%lld,\"throttled\":%s}",
                                     (int)arr_len, arr_copy, busy ? "true" : "false",
                                     cache_age_ms, throttled ? "true" : "false");
  http_send_response(fd, 200, "application/json", (const uint8_t *)resp, resp_len);
  free(resp);
  free(arr_copy);
}

void
handle_appdb_lookup(int fd, const char *query) {
  char title_id[32] = {0};
  char upper_id[16] = {0};
  char name[256] = {0};
  char source[64] = "local";
  char body[1024];
  int cache_enabled = 1;

  if (query_value(query, "id", title_id, sizeof(title_id)) != 0) {
    query_value(query, "titleId", title_id, sizeof(title_id));
  }
  if (!title_id[0] || !title_id_normalize(title_id, upper_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad id\"}");
    return;
  }
  if (!is_valid_title_id(upper_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"unsupported title id\"}");
    return;
  }

  pthread_mutex_lock(&g_cfg_lock);
  cache_enabled = g_cfg.title_lookup_cache_enabled;
  pthread_mutex_unlock(&g_cfg_lock);

  if (cache_enabled && read_title_lookup_cache_name(upper_id, name, sizeof(name))) {
    snprintf(source, sizeof(source), "%s", "cache_lookup");
    cr_log("debug", "title.lookup", "cache hit title=%s name=\"%s\"", upper_id, name);
  }

  if (cr_title_name_is_unresolved(upper_id, name)) {
    game_entry_t entries[MAX_GAMES];
    size_t count = 0;
    memset(entries, 0, sizeof(entries));
    appdb_collect_games(entries, &count);
    for (size_t i = 0; i < count; i++) {
      if (!strcmp(entries[i].title_id, upper_id) &&
          !cr_title_name_is_unresolved(upper_id, entries[i].title_name)) {
        snprintf(name, sizeof(name), "%s", entries[i].title_name);
        snprintf(source, sizeof(source), "%s", entries[i].name_source[0] ? entries[i].name_source : "local");
        break;
      }
    }
  }

  if (cr_title_name_is_unresolved(upper_id, name) &&
      read_param_value_by_title_id(upper_id, "titleName", name, sizeof(name)) == 0 &&
      !cr_title_name_is_unresolved(upper_id, name)) {
    snprintf(source, sizeof(source), "%s", "param_json");
  }

  if (cr_title_name_is_unresolved(upper_id, name)) {
    snprintf(name, sizeof(name), "%s", upper_id);
    snprintf(source, sizeof(source), "%s", "fallback_title_id");
  } else if (cache_enabled && strcmp(source, "cache_lookup") != 0) {
    write_title_lookup_cache_name(upper_id, name);
  }

  char *name_esc = json_escape(name);
  snprintf(body, sizeof(body), "{\"ok\":true,\"id\":\"%s\",\"titleId\":\"%s\",\"name\":\"%s\",\"source\":\"%s\"}",
           upper_id, upper_id, name_esc ? name_esc : "", source);
  free(name_esc);
  http_send_json(fd, 200, body);
}

void
handle_api_titles_lookup(int fd, const char *query) {
  handle_appdb_lookup(fd, query);
}

void
handle_appdb_icon(int fd, const char *query) {
  char title_id[32] = {0};
  char norm_id[10] = {0};
  char path[512];
  uint8_t *buf = NULL;
  size_t len = 0;
  if (query_value(query, "id", title_id, sizeof(title_id)) != 0 || !title_id_normalize(title_id, norm_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad id\"}");
    return;
  }
  if (cache_media_ensure(norm_id, 0, path, sizeof(path)) != 0 || read_file_bytes(path, &buf, &len) != 0) {
    http_send_json(fd, 404, "{\"ok\":false,\"error\":\"no icon\"}");
    return;
  }
  http_send_response(fd, 200, "image/png", buf, len);
  free(buf);
}

void
handle_appdb_pic0(int fd, const char *query) {
  char title_id[32] = {0};
  char norm_id[10] = {0};
  char path[512];
  uint8_t *buf = NULL;
  size_t len = 0;
  if (query_value(query, "id", title_id, sizeof(title_id)) != 0 || !title_id_normalize(title_id, norm_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad id\"}");
    return;
  }
  if (cache_media_ensure(norm_id, 1, path, sizeof(path)) != 0 || read_file_bytes(path, &buf, &len) != 0) {
    http_send_json(fd, 404, "{\"ok\":false,\"error\":\"no pic0\"}");
    return;
  }
  http_send_response(fd, 200, "image/png", buf, len);
  free(buf);
}

void
handle_launch(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  char args[512] = {0};
  char body[384];
  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 ||
      !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid titleId\"}");
    return;
  }

  pthread_mutex_lock(&g_launch_status_lock);
  if (g_launch_status.busy) {
    snprintf(body, sizeof(body),
             "{\"ok\":false,\"titleId\":\"%s\",\"error\":\"launch busy\",\"phase\":\"%s\",\"message\":\"%s\"}",
             g_launch_status.title_id, g_launch_status.phase, g_launch_status.message);
    pthread_mutex_unlock(&g_launch_status_lock);
    http_send_json(fd, 409, body);
    return;
  }
  pthread_mutex_unlock(&g_launch_status_lock);
  set_launch_status_ex(1, "killing_current", title_id, "Queued", 0, "", 0, 0);

  launch_worker_request_t *req = malloc(sizeof(*req));
  if (!req) {
    set_launch_status(0, "failed", title_id, "alloc failed", 0);
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}");
    return;
  }
  snprintf(req->title_id, sizeof(req->title_id), "%s", title_id);
  if (query_value(query, "args", args, sizeof(args)) == 0) {
    snprintf(req->args, sizeof(req->args), "%s", args);
  } else {
    req->args[0] = '\0';
  }
  pthread_t t;
  if (pthread_create(&t, NULL, launch_worker_thread, req) != 0) {
    free(req);
    set_launch_status(0, "failed", title_id, "thread failed", 0);
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"thread\"}");
    return;
  }
  pthread_detach(t);

  pthread_mutex_lock(&g_activity_lock);
  g_activity_launch_count++;
  g_activity_last_launch = time(NULL);
  snprintf(g_activity_last_played_title_id, sizeof(g_activity_last_played_title_id), "%s", title_id);
  int idx = activity_find_title_index_locked(title_id);
  if (idx < 0 && g_activity_titles_count < (int)(sizeof(g_activity_titles) / sizeof(g_activity_titles[0]))) {
    idx = g_activity_titles_count++;
    memset(&g_activity_titles[idx], 0, sizeof(g_activity_titles[idx]));
    snprintf(g_activity_titles[idx].title_id, sizeof(g_activity_titles[idx].title_id), "%s", title_id);
  }
  if (idx >= 0) {
    g_activity_titles[idx].launch_count++;
    g_activity_titles[idx].last_launch = g_activity_last_launch;
  }
  pthread_mutex_unlock(&g_activity_lock);
  activity_save();

  snprintf(body, sizeof(body), "{\"ok\":true,\"titleId\":\"%s\",\"busy\":true,\"phase\":\"killing_current\",\"message\":\"Queued\"}",
           title_id);
  http_send_json(fd, 200, body);
}

void
handle_api_cheats_index(int fd) {
  ensure_data_dirs();

  cJSON *root = cJSON_CreateObject();
  cJSON *files = cJSON_AddArrayToObject(root, "files");
  if (!root || !files) {
    cJSON_Delete(root);
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}");
    return;
  }
  cJSON_AddBoolToObject(root, "ok", 1);

  DIR *d = opendir(CHEATRUNNER_CHEATS_DIR);
  if (d) {
    struct dirent *ent = NULL;
    char seen[512][16];
    int seen_n = 0;
    while ((ent = readdir(d)) != NULL) {
      const char *name = ent->d_name;
      if (name[0] == '.') {
        continue;
      }
      int kind = recognised_cheat_extension(name);
      if (!kind || !is_safe_filename(name)) {
        continue;
      }
      char id[16] = {0};
      if (!extract_title_id_prefix(name, id, sizeof(id))) {
        continue;
      }

      int dup = 0;
      for (int i = 0; i < seen_n; i++) {
        if (!strcmp(seen[i], id)) {
          dup = 1;
          break;
        }
      }
      if (dup) {
        continue;
      }
      if (seen_n < (int)(sizeof(seen) / sizeof(seen[0]))) {
        snprintf(seen[seen_n], sizeof(seen[seen_n]), "%s", id);
        seen_n++;
      }

      char best_path[256];
      int best_kind = 0;
      char display[256];
      display[0] = '\0';
      if (find_cheat_file_for_title(id, best_path, sizeof(best_path), &best_kind)) {
        read_cheat_display_name(best_path, best_kind, display, sizeof(display));
      } else {
        best_kind = kind;
      }

      const char *fmt = best_kind == 1 ? "json" : (best_kind == 2 ? "shn" : "mc4");
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "titleId", id);
      cJSON_AddStringToObject(e, "name", display[0] ? display : id);
      cJSON_AddStringToObject(e, "format", fmt);
      cJSON_AddItemToArray(files, e);
    }
    closedir(d);
  }

  pid_t pid = -1;
  intptr_t base = 0;
  char running[16] = {0};
  if (get_running_game(&pid, running, sizeof(running), &base) == 0) {
    cJSON *running_obj = cJSON_AddObjectToObject(root, "running");
    if (running_obj) {
      char base_hex[32];
      snprintf(base_hex, sizeof(base_hex), "0x%lx", (long)base);
      cJSON_AddStringToObject(running_obj, "titleId", running);
      cJSON_AddNumberToObject(running_obj, "pid", pid);
      cJSON_AddStringToObject(running_obj, "imageBase", base_hex);
    }
  } else {
    cJSON_AddNullToObject(root, "running");
  }

  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!body) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}");
    return;
  }
  http_send_response(fd, 200, "application/json", (const uint8_t *)body, strlen(body));
  free(body);
}

void
handle_api_cheats_get(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  char path[256];
  int kind = 0;
  char *txt = NULL;

  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 || !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad titleId\"}");
    return;
  }
  if (!find_cheat_file_for_title(title_id, path, sizeof(path), &kind)) {
    http_send_json(fd, 404, "{\"ok\":false,\"error\":\"no cheat file\"}");
    return;
  }
  if (read_file_text(path, &txt) != 0 || !txt) {
    http_send_json(fd, 404, "{\"ok\":false,\"error\":\"could not read cheat file\"}");
    return;
  }

  if (kind == 1) {
    http_send_response(fd, 200, "application/json", (const uint8_t *)txt, strlen(txt));
    free(txt);
    return;
  }
  if (kind == 2) {
    char *json = shn_xml_to_json(txt, strlen(txt));
    free(txt);
    if (!json) {
      http_send_json(fd, 500, "{\"ok\":false,\"error\":\"SHN parse failed\"}");
      return;
    }
    http_send_response(fd, 200, "application/json", (const uint8_t *)json, strlen(json));
    free(json);
    return;
  }

  char *xml = mc4_decrypt_to_xml(txt, strlen(txt), NULL);
  free(txt);
  if (!xml) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"MC4 decrypt failed\"}");
    return;
  }
  char *json = shn_xml_to_json(xml, strlen(xml));
  free(xml);
  if (!json) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"MC4 XML parse failed\"}");
    return;
  }
  http_send_response(fd, 200, "application/json", (const uint8_t *)json, strlen(json));
  free(json);
}

static cJSON *
load_cheat_json_root_for_title_ex(const char *title_id, char *err, size_t err_size, char *path_out, size_t path_out_size,
                                  int *kind_out) {
  char path[256];
  int kind = 0;
  if (!find_cheat_file_for_title(title_id, path, sizeof(path), &kind)) {
    snprintf(err, err_size, "no cheat file");
    return NULL;
  }
  char *txt = NULL;
  if (read_file_text(path, &txt) != 0 || !txt) {
    snprintf(err, err_size, "could not read cheat file");
    return NULL;
  }
  char *json_txt = NULL;
  if (kind == 1) {
    json_txt = txt;
  } else if (kind == 2) {
    json_txt = shn_xml_to_json(txt, strlen(txt));
    free(txt);
  } else {
    char *xml = mc4_decrypt_to_xml(txt, strlen(txt), NULL);
    free(txt);
    if (!xml) {
      snprintf(err, err_size, "MC4 decrypt failed");
      return NULL;
    }
    json_txt = shn_xml_to_json(xml, strlen(xml));
    free(xml);
  }
  if (!json_txt) {
    snprintf(err, err_size, "parse failed");
    return NULL;
  }
  cJSON *root = cJSON_Parse(json_txt);
  free(json_txt);
  if (!root) {
    snprintf(err, err_size, "json parse failed");
    return NULL;
  }
  if (path_out && path_out_size > 0) {
    snprintf(path_out, path_out_size, "%s", path);
  }
  if (kind_out) {
    *kind_out = kind;
  }
  return root;
}

static cJSON *
load_cheat_json_root_for_title(const char *title_id, char *err, size_t err_size) {
  return load_cheat_json_root_for_title_ex(title_id, err, err_size, NULL, 0, NULL);
}

typedef enum cheat_state_kind {
  CHEAT_STATE_OFF = 0,
  CHEAT_STATE_ON,
  CHEAT_STATE_MIXED,
  CHEAT_STATE_MISMATCH,
  CHEAT_STATE_GAME_NOT_RUNNING,
  CHEAT_STATE_PROCESS_NOT_FOUND,
  CHEAT_STATE_BASE_NOT_READY,
  CHEAT_STATE_READ_FAILED,
  CHEAT_STATE_INVALID_CHEAT,
  CHEAT_STATE_GAME_LOADING,
  CHEAT_STATE_BASELINE_UNKNOWN,
  CHEAT_STATE_ADDRESS_UNRESOLVED,
  CHEAT_STATE_FORMAT_NEEDS_REVIEW,
  CHEAT_STATE_OFF_UNVERIFIED,
  CHEAT_STATE_CRASH_SUSPECT,
  CHEAT_STATE_UNKNOWN
} cheat_state_kind_t;

static const char *
cheat_state_key(cheat_state_kind_t s) {
  switch (s) {
  case CHEAT_STATE_OFF:
    return "off";
  case CHEAT_STATE_ON:
    return "on";
  case CHEAT_STATE_MIXED:
    return "mixed";
  case CHEAT_STATE_MISMATCH:
    return "mismatch";
  case CHEAT_STATE_GAME_NOT_RUNNING:
    return "game_not_ready";
  case CHEAT_STATE_PROCESS_NOT_FOUND:
    return "process_not_found";
  case CHEAT_STATE_BASE_NOT_READY:
    return "base_not_ready";
  case CHEAT_STATE_READ_FAILED:
    return "read_failed";
  case CHEAT_STATE_INVALID_CHEAT:
    return "invalid_cheat";
  case CHEAT_STATE_GAME_LOADING:
    return "game_loading";
  case CHEAT_STATE_BASELINE_UNKNOWN:
    return "baseline_unknown";
  case CHEAT_STATE_ADDRESS_UNRESOLVED:
    return "address_unresolved";
  case CHEAT_STATE_FORMAT_NEEDS_REVIEW:
    return "format_needs_review";
  case CHEAT_STATE_OFF_UNVERIFIED:
    return "off_unverified";
  case CHEAT_STATE_CRASH_SUSPECT:
    return "crash_suspect";
  default:
    return "unknown";
  }
}

static const char *
cheat_state_label(cheat_state_kind_t s) {
  switch (s) {
  case CHEAT_STATE_OFF:
    return "OFF";
  case CHEAT_STATE_ON:
    return "ON";
  case CHEAT_STATE_MIXED:
    return "PARTIAL";
  case CHEAT_STATE_MISMATCH:
    return "VERSION MISMATCH";
  case CHEAT_STATE_GAME_NOT_RUNNING:
    return "GAME NOT READY";
  case CHEAT_STATE_PROCESS_NOT_FOUND:
    return "PROCESS NOT FOUND";
  case CHEAT_STATE_BASE_NOT_READY:
    return "BASE NOT READY";
  case CHEAT_STATE_READ_FAILED:
    return "READ FAILED";
  case CHEAT_STATE_INVALID_CHEAT:
    return "INVALID CHEAT";
  case CHEAT_STATE_GAME_LOADING:
    return "WAITING FOR GAME";
  case CHEAT_STATE_BASELINE_UNKNOWN:
    return "BASELINE UNKNOWN";
  case CHEAT_STATE_ADDRESS_UNRESOLVED:
    return "ADDRESS UNRESOLVED";
  case CHEAT_STATE_FORMAT_NEEDS_REVIEW:
    return "FORMAT NEEDS REVIEW";
  case CHEAT_STATE_OFF_UNVERIFIED:
    return "OFF";
  case CHEAT_STATE_CRASH_SUSPECT:
    return "CRASH SUSPECT";
  default:
    return "UNKNOWN";
  }
}

void
handle_api_cheats_state(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  char err[128] = {0};
  char cheat_path[256] = {0};
  int cheat_kind = 0;
  int debug_mode = 0;
  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 || !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad titleId\"}");
    return;
  }
  {
    char dbg[8] = {0};
    if (query_value(query, "debug", dbg, sizeof(dbg)) == 0 && atoi(dbg) != 0) {
      debug_mode = 1;
    }
  }
  cJSON *root = load_cheat_json_root_for_title_ex(title_id, err, sizeof(err), cheat_path, sizeof(cheat_path), &cheat_kind);
  if (!root) {
    char body[256];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", err);
    http_send_json(fd, 404, body);
    return;
  }
  cr_log("debug", "cheats", "state request title=%s debug=%d", title_id, debug_mode);
  cJSON *mods = cJSON_GetObjectItem(root, "mods");
  if (!cJSON_IsArray(mods)) {
    cJSON_Delete(root);
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid mods\"}");
    return;
  }

  running_game_state_t st;
  int running_ok = (read_running_state(&st) == 0 && st.running);
  int title_match = running_ok && strcmp(st.title_id, title_id) == 0;
  pid_t pid = running_ok ? st.pid : -1;
  intptr_t base = running_ok ? st.image_base : 0;
  int attach_ok = 0;
  int global_state_delay_ms = 0;
  int auto_detect = 1;
  pthread_mutex_lock(&g_cfg_lock);
  global_state_delay_ms = g_cfg.cheat_state_after_launch_delay_ms;
  auto_detect = g_cfg.cheat_address_auto_detect;
  pthread_mutex_unlock(&g_cfg_lock);
  int in_loading_window = 0;
  if (title_match && global_state_delay_ms > 0 && g_last_launch_verified_at_ms > 0) {
    uint64_t age = now_ms() - g_last_launch_verified_at_ms;
    if (age < (uint64_t)global_state_delay_ms) {
      in_loading_window = 1;
    }
  }
  int can_probe = title_match && pid > 0 && base != 0 && !in_loading_window;
  if (running_ok) {
    cr_log("debug", "cheats", "running title=%s pid=%d base=0x%lx appId=0x%X", st.title_id, (int)pid, (long)base,
           (unsigned int)st.app_id);
  }
  if (can_probe) {
    if (pt_attach(pid) == 0) {
      attach_ok = 1;
    } else {
      can_probe = 0;
    }
  }
  const char *fmt = cheat_kind == 1 ? "json" : (cheat_kind == 2 ? "shn" : "mc4");
  cr_log("debug", "cheats", "loaded file=%s format=%s mods=%d", cheat_path, fmt, cJSON_GetArraySize(mods));

  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "ok", 1);
  cJSON_AddStringToObject(out, "titleId", title_id);
  cJSON_AddStringToObject(out, "cheatPath", cheat_path);
  cJSON_AddStringToObject(out, "cheatFormat", fmt);
  cJSON *game = cJSON_AddObjectToObject(out, "game");
  if (game) {
    cJSON_AddBoolToObject(game, "running", running_ok ? 1 : 0);
    cJSON_AddStringToObject(game, "runningTitleId", running_ok ? st.title_id : "");
    cJSON_AddNumberToObject(game, "pid", running_ok ? st.pid : -1);
    char base_hex[32];
    snprintf(base_hex, sizeof(base_hex), "0x%lx", (long)(running_ok ? st.image_base : 0));
    cJSON_AddStringToObject(game, "base", base_hex);
    cJSON_AddStringToObject(game, "contentVersion", running_ok ? st.content_version : "");
    cJSON_AddStringToObject(game, "appVersion", running_ok ? st.app_version : "");
  }
  cJSON *arr = cJSON_AddArrayToObject(out, "mods");
  cJSON *mismatches = cJSON_AddArrayToObject(out, "mismatches");
  int summary_on = 0, summary_off = 0, summary_mismatch = 0, summary_not_ready = 0, summary_baseline_unknown = 0;
  int idx = 0;
  cJSON *mod = NULL;
  cJSON_ArrayForEach(mod, mods) {
    cJSON *e = cJSON_CreateObject();
    int mod_index = idx++;
    cJSON_AddNumberToObject(e, "index", mod_index);
    cJSON *n = cJSON_GetObjectItem(mod, "name");
    const char *mod_name = cJSON_IsString(n) ? n->valuestring : "mod";
    cJSON_AddStringToObject(e, "name", mod_name);
    cJSON *mem = cJSON_GetObjectItem(mod, "memory");
    if (!cJSON_IsArray(mem)) {
      mem = cJSON_GetObjectItem(mod, "patches");
    }
    cheat_state_kind_t state = CHEAT_STATE_UNKNOWN;
    const char *reason = "";
    if (!cJSON_IsArray(mem) || cJSON_GetArraySize(mem) == 0) {
      state = CHEAT_STATE_INVALID_CHEAT;
      reason = "mod has no memory entries";
    } else if (!running_ok) {
      state = CHEAT_STATE_GAME_NOT_RUNNING;
      reason = "no game is currently running";
    } else if (!title_match) {
      state = CHEAT_STATE_PROCESS_NOT_FOUND;
      reason = "running game does not match cheat title";
    } else if (in_loading_window) {
      state = CHEAT_STATE_GAME_LOADING;
      reason = "game just started";
    } else if (pid <= 0) {
      state = CHEAT_STATE_PROCESS_NOT_FOUND;
      reason = "process not found";
    } else if (base == 0) {
      state = CHEAT_STATE_BASE_NOT_READY;
      reason = "base address not ready";
    } else if (!attach_ok) {
      state = CHEAT_STATE_PROCESS_NOT_FOUND;
      reason = "pt_attach failed";
    } else {
      int total = 0;
      int on_matches = 0;
      int off_matches = 0;
      int mismatch = 0;
      int baseline_unknown = 0;
      int mismatch_logged = 0;
      cJSON *m = NULL;
      cJSON_ArrayForEach(m, mem) {
        cJSON *off_j = cJSON_GetObjectItem(m, "offset");
        cJSON *on_j = cJSON_GetObjectItem(m, "on");
        cJSON *off_j2 = cJSON_GetObjectItem(m, "off");
        cJSON *expected_j = cJSON_GetObjectItem(m, "expected");
        cJSON *abs_j = cJSON_GetObjectItem(m, "absolute");
        if (!cJSON_IsString(off_j) || !cJSON_IsString(on_j) || !cJSON_IsString(off_j2)) {
          state = CHEAT_STATE_INVALID_CHEAT;
          reason = "memory entry missing offset/on/off";
          break;
        }
        uint8_t on_b[128], off_b[128], exp_b[128], cur[128];
        size_t on_len = 0, off_len = 0, exp_len = 0;
        if (parse_hex_bytes_checked(on_j->valuestring, on_b, sizeof(on_b), &on_len) != 0 ||
            parse_hex_bytes_checked(off_j2->valuestring, off_b, sizeof(off_b), &off_len) != 0 || on_len != off_len) {
          state = CHEAT_STATE_INVALID_CHEAT;
          reason = "invalid on/off bytes";
          break;
        }
        uint64_t off_u = 0;
        if (parse_offset_hex_checked(off_j->valuestring, &off_u) != 0) {
          state = CHEAT_STATE_INVALID_CHEAT;
          reason = "invalid offset";
          break;
        }
        if (cJSON_IsString(expected_j) && expected_j->valuestring && expected_j->valuestring[0]) {
          if (parse_hex_bytes_checked(expected_j->valuestring, exp_b, sizeof(exp_b), &exp_len) != 0 || exp_len != on_len) {
            state = CHEAT_STATE_INVALID_CHEAT;
            reason = "invalid expected bytes";
            break;
          }
        }
        int af_s = 0, inj_s = 0, adet_s = auto_detect;
        get_cheat_addr_flags(cheat_kind, cJSON_IsTrue(abs_j) ? 1 : 0, auto_detect, &af_s, &inj_s, &adet_s);
        const uint8_t *expect_cmp_s = exp_len > 0 ? exp_b : off_b;
        intptr_t addr = cheat_resolve_write_addr(pid, base, off_u,
                                                  af_s, inj_s, adet_s,
                                                  on_b, on_len, expect_cmp_s);
        /* Try absolute candidate for ADDRESS_UNRESOLVED detection */
        intptr_t abs_cand2 = (intptr_t)off_u;
        intptr_t rel_cand2 = base + (intptr_t)off_u;
        if (read_process_memory(pid, addr, cur, on_len) != 0) {
          /* Try the other candidate before declaring unresolved */
          intptr_t alt = (addr == abs_cand2) ? rel_cand2 : abs_cand2;
          if (on_len <= sizeof(cur) && read_process_memory(pid, alt, cur, on_len) == 0) {
            addr = alt;
          } else {
            state = CHEAT_STATE_ADDRESS_UNRESOLVED;
            reason = "read failed on both address candidates";
            break;
          }
        }
        total++;
        int is_on = memcmp(cur, on_b, on_len) == 0;
        /* For MC4/SHN without explicit expected bytes, off_b is NOT a reliable original */
        int off_reliable = (cheat_kind == 1) || (exp_len > 0);
        int is_off = off_reliable && (memcmp(cur, (exp_len > 0 ? exp_b : off_b), on_len) == 0);
        if (is_on) {
          on_matches++;
        }
        if (is_off) {
          off_matches++;
        }
        if (!is_on && !is_off && off_reliable) {
          mismatch++;
        }
        if (!is_on && !off_reliable) {
          /* MC4/SHN without reliable baseline */
          baseline_unknown++;
        }
        if (!is_on && !is_off && off_reliable) {
          if (!mismatch_logged) {
            char cur_hex[512], on_hex[128], exp_hex[128], addr_hex[32], off_hex_addr[32];
            bytes_to_hex(cur, on_len > 32 ? 32 : on_len, cur_hex, sizeof(cur_hex));
            bytes_to_hex(on_b, on_len > 16 ? 16 : on_len, on_hex, sizeof(on_hex));
            bytes_to_hex(exp_len > 0 ? exp_b : off_b, on_len > 16 ? 16 : on_len, exp_hex, sizeof(exp_hex));
            snprintf(addr_hex, sizeof(addr_hex), "0x%lx", (long)addr);
            snprintf(off_hex_addr, sizeof(off_hex_addr), "0x%llX", (unsigned long long)off_u);
            cr_log("warn", "cheats",
                   "mismatch title=%s contentVersion=%s file=%s format=%s mod=\"%s\" modIndex=%d writeIndex=%d pid=%d "
                   "base=0x%lx offset=%s addr=%s expected=%s...(len=%zu) current=%s...(len=%zu) on=%s...(len=%zu)",
                   title_id, st.content_version[0] ? st.content_version : "unknown", cheat_path, fmt, mod_name, mod_index,
                   total - 1, (int)pid, (long)base, off_hex_addr, addr_hex, exp_hex, on_len, cur_hex, on_len, on_hex, on_len);
            mismatch_logged = 1;
          }
          if (debug_mode && mismatches && cJSON_GetArraySize(mismatches) < 128) {
            char cur_hex[512], on_hex[512], off_hex[512], exp_hex[512], addr_hex[32], off_hex_addr[32];
            char abs_addr_hex[32], rel_addr_hex[32];
            char abs_cur_hex[512], rel_cur_hex[512];
            uint8_t abs_buf[128], rel_buf[128];
            intptr_t abs_cand = (intptr_t)off_u;
            intptr_t rel_cand = base + (intptr_t)off_u;
            int abs_read = (on_len <= sizeof(abs_buf) && read_process_memory(pid, abs_cand, abs_buf, on_len) == 0);
            int rel_read = (on_len <= sizeof(rel_buf) && read_process_memory(pid, rel_cand, rel_buf, on_len) == 0);
            bytes_to_hex(cur, on_len, cur_hex, sizeof(cur_hex));
            bytes_to_hex(on_b, on_len, on_hex, sizeof(on_hex));
            bytes_to_hex(off_b, on_len, off_hex, sizeof(off_hex));
            bytes_to_hex(exp_len > 0 ? exp_b : off_b, on_len, exp_hex, sizeof(exp_hex));
            snprintf(addr_hex, sizeof(addr_hex), "0x%lx", (long)addr);
            snprintf(off_hex_addr, sizeof(off_hex_addr), "0x%llX", (unsigned long long)off_u);
            snprintf(abs_addr_hex, sizeof(abs_addr_hex), "0x%lx", (long)abs_cand);
            snprintf(rel_addr_hex, sizeof(rel_addr_hex), "0x%lx", (long)rel_cand);
            if (abs_read) { bytes_to_hex(abs_buf, on_len, abs_cur_hex, sizeof(abs_cur_hex)); } else { snprintf(abs_cur_hex, sizeof(abs_cur_hex), "(read failed)"); }
            if (rel_read) { bytes_to_hex(rel_buf, on_len, rel_cur_hex, sizeof(rel_cur_hex)); } else { snprintf(rel_cur_hex, sizeof(rel_cur_hex), "(read failed)"); }
            cJSON *mm = cJSON_CreateObject();
            cJSON_AddStringToObject(mm, "mod", mod_name);
            cJSON_AddNumberToObject(mm, "modIndex", mod_index);
            cJSON_AddNumberToObject(mm, "writeIndex", total - 1);
            cJSON_AddStringToObject(mm, "offset", off_hex_addr);
            cJSON_AddStringToObject(mm, "address", addr_hex);
            cJSON_AddStringToObject(mm, "expected", exp_hex);
            cJSON_AddStringToObject(mm, "off", off_hex);
            cJSON_AddStringToObject(mm, "current", cur_hex);
            cJSON_AddStringToObject(mm, "on", on_hex);
            /* both address candidates for diagnosis */
            cJSON *cands = cJSON_AddArrayToObject(mm, "candidates");
            if (cands) {
              cJSON *ca = cJSON_CreateObject();
              cJSON_AddStringToObject(ca, "mode", "absolute");
              cJSON_AddStringToObject(ca, "address", abs_addr_hex);
              cJSON_AddStringToObject(ca, "current", abs_cur_hex);
              cJSON_AddBoolToObject(ca, "readOk", abs_read);
              cJSON_AddItemToArray(cands, ca);
              cJSON *cr2 = cJSON_CreateObject();
              cJSON_AddStringToObject(cr2, "mode", "relative");
              cJSON_AddStringToObject(cr2, "address", rel_addr_hex);
              cJSON_AddStringToObject(cr2, "current", rel_cur_hex);
              cJSON_AddBoolToObject(cr2, "readOk", rel_read);
              cJSON_AddItemToArray(cands, cr2);
            }
            cJSON_AddItemToArray(mismatches, mm);
          }
        }
      }
      if (state == CHEAT_STATE_UNKNOWN) {
        if (on_matches == total && total > 0) {
          state = CHEAT_STATE_ON;
        } else if (mismatch > 0 && baseline_unknown == 0 && on_matches == 0 && off_matches == 0) {
          /* All patches have reliable baseline and none match — true version mismatch */
          state = CHEAT_STATE_MISMATCH;
          reason = "current bytes match neither ON nor expected/off";
        } else if (baseline_unknown > 0 && mismatch == 0 && on_matches == 0) {
          /* All non-ON patches are MC4/SHN without reliable baseline */
          state = CHEAT_STATE_BASELINE_UNKNOWN;
          reason = "MC4/SHN: no reliable original bytes; state cannot be confirmed";
        } else if (off_matches == total && total > 0 && baseline_unknown == 0) {
          state = CHEAT_STATE_OFF;
        } else {
          state = CHEAT_STATE_MIXED;
          reason = "some writes differ";
        }
      }
    }
    /* Promote BASELINE_UNKNOWN → OFF_UNVERIFIED when mod was explicitly disabled this session */
    if (state == CHEAT_STATE_BASELINE_UNKNOWN && can_probe &&
        mod_disabled_check(title_id, pid, mod_index)) {
      state = CHEAT_STATE_OFF_UNVERIFIED;
      reason = "Disabled using runtime backup; MC4 baseline unknown.";
    }
    /* Mark crash_suspect if game stopped shortly after enabling */
    {
      pthread_mutex_lock(&g_crash_guard_lock);
      for (int _ci = 0; _ci < g_crash_suspects_n; _ci++) {
        if (strcmp(g_crash_suspects[_ci].title_id, title_id) == 0 &&
            g_crash_suspects[_ci].mod_index == mod_index) {
          state = CHEAT_STATE_CRASH_SUSPECT;
          reason = "Game stopped shortly after enabling this cheat.";
          break;
        }
      }
      pthread_mutex_unlock(&g_crash_guard_lock);
    }
    cJSON_AddStringToObject(e, "state", cheat_state_key(state));
    cJSON_AddStringToObject(e, "label", cheat_state_label(state));
    if (reason && reason[0]) {
      cJSON_AddStringToObject(e, "reason", reason);
    }
    /* visualState + capability flags for frontend */
    {
      const char *visual_state;
      int can_enable_flag, can_disable_flag;
      if (state == CHEAT_STATE_ON) {
        visual_state = "on"; can_enable_flag = 0; can_disable_flag = 1;
      } else if (state == CHEAT_STATE_OFF || state == CHEAT_STATE_OFF_UNVERIFIED ||
                 state == CHEAT_STATE_BASELINE_UNKNOWN) {
        visual_state = "off"; can_enable_flag = 1; can_disable_flag = 0;
      } else if (state == CHEAT_STATE_CRASH_SUSPECT) {
        visual_state = "error"; can_enable_flag = 0; can_disable_flag = 0;
      } else {
        visual_state = "disabled"; can_enable_flag = 0; can_disable_flag = 0;
      }
      cJSON_AddStringToObject(e, "visualState", visual_state);
      cJSON_AddBoolToObject(e, "canEnable", can_enable_flag);
      cJSON_AddBoolToObject(e, "canDisable", can_disable_flag);
    }
    if (state == CHEAT_STATE_ON) {
      summary_on++;
    } else if (state == CHEAT_STATE_OFF || state == CHEAT_STATE_OFF_UNVERIFIED) {
      summary_off++;
    } else if (state == CHEAT_STATE_MISMATCH) {
      summary_mismatch++;
    } else if (state == CHEAT_STATE_BASELINE_UNKNOWN) {
      summary_baseline_unknown++;
    } else {
      summary_not_ready++;
    }
    cJSON_AddItemToArray(arr, e);
  }
  if (attach_ok) {
    pt_detach(pid, 0);
  }
  cJSON_Delete(root);
  cJSON *summary = cJSON_AddObjectToObject(out, "summary");
  if (summary) {
    cJSON_AddNumberToObject(summary, "on", summary_on);
    cJSON_AddNumberToObject(summary, "off", summary_off);
    cJSON_AddNumberToObject(summary, "mismatch", summary_mismatch);
    cJSON_AddNumberToObject(summary, "baselineUnknown", summary_baseline_unknown);
    cJSON_AddNumberToObject(summary, "notReady", summary_not_ready);
  }
  cr_log("debug", "cheats", "state summary title=%s on=%d off=%d mismatch=%d baselineUnknown=%d notReady=%d file=%s format=%s pid=%d base=0x%lx",
         title_id, summary_on, summary_off, summary_mismatch, summary_baseline_unknown, summary_not_ready, cheat_path, fmt, (int)pid, (long)base);

  char *txt = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  if (!txt) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}");
    return;
  }
  http_send_response(fd, 200, "application/json", (const uint8_t *)txt, strlen(txt));
  free(txt);
}

void
handle_api_cheats_clear_crash_flags(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 || !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad titleId\"}");
    return;
  }
  pthread_mutex_lock(&g_crash_guard_lock);
  int cleared = 0;
  for (int i = g_crash_suspects_n - 1; i >= 0; i--) {
    if (strcmp(g_crash_suspects[i].title_id, title_id) == 0) {
      g_crash_suspects[i] = g_crash_suspects[--g_crash_suspects_n];
      cleared++;
    }
  }
  pthread_mutex_unlock(&g_crash_guard_lock);
  char body[64];
  snprintf(body, sizeof(body), "{\"ok\":true,\"cleared\":%d}", cleared);
  cr_log("info", "cheats", "cleared crash flags title=%s n=%d", title_id, cleared);
  http_send_json(fd, 200, body);
}

void
handle_api_cheats_debug(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  char q[96];
  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 || !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad titleId\"}");
    return;
  }
  snprintf(q, sizeof(q), "titleId=%s&debug=1", title_id);
  handle_api_cheats_state(fd, q);
}

static cheat_state_kind_t
check_mod_enable_state(const char *title_id, int mod_index, char *reason, size_t reason_size) {
  char err[128] = {0};
  int cheat_kind_cme = 0;
  cJSON *root = load_cheat_json_root_for_title_ex(title_id, err, sizeof(err), NULL, 0, &cheat_kind_cme);
  if (!root) {
    snprintf(reason, reason_size, "%s", err[0] ? err : "invalid cheat");
    return CHEAT_STATE_INVALID_CHEAT;
  }
  int auto_detect_cme = 1;
  pthread_mutex_lock(&g_cfg_lock);
  auto_detect_cme = g_cfg.cheat_address_auto_detect;
  pthread_mutex_unlock(&g_cfg_lock);
  cJSON *mods = cJSON_GetObjectItem(root, "mods");
  if (!cJSON_IsArray(mods) || mod_index < 0 || mod_index >= cJSON_GetArraySize(mods)) {
    cJSON_Delete(root);
    snprintf(reason, reason_size, "mod index out of range");
    return CHEAT_STATE_INVALID_CHEAT;
  }
  running_game_state_t st;
  if (read_running_state(&st) != 0 || !st.running) {
    cJSON_Delete(root);
    snprintf(reason, reason_size, "no game is currently running");
    return CHEAT_STATE_GAME_NOT_RUNNING;
  }
  if (strcmp(st.title_id, title_id) != 0) {
    cJSON_Delete(root);
    snprintf(reason, reason_size, "running game does not match cheat title");
    return CHEAT_STATE_PROCESS_NOT_FOUND;
  }
  if (st.pid <= 0) {
    cJSON_Delete(root);
    snprintf(reason, reason_size, "process not found");
    return CHEAT_STATE_PROCESS_NOT_FOUND;
  }
  if (st.image_base == 0) {
    cJSON_Delete(root);
    snprintf(reason, reason_size, "base address not ready");
    return CHEAT_STATE_BASE_NOT_READY;
  }
  int delay_ms = 8000;
  pthread_mutex_lock(&g_cfg_lock);
  delay_ms = g_cfg.cheat_state_after_launch_delay_ms;
  pthread_mutex_unlock(&g_cfg_lock);
  if (delay_ms > 0 && g_last_launch_verified_at_ms > 0) {
    uint64_t age = now_ms() - g_last_launch_verified_at_ms;
    if (age < (uint64_t)delay_ms) {
      cJSON_Delete(root);
      snprintf(reason, reason_size, "game still loading");
      return CHEAT_STATE_GAME_LOADING;
    }
  }
  if (pt_attach(st.pid) < 0) {
    cJSON_Delete(root);
    snprintf(reason, reason_size, "pt_attach failed");
    return CHEAT_STATE_PROCESS_NOT_FOUND;
  }
  cheat_state_kind_t state = CHEAT_STATE_UNKNOWN;
  cJSON *mod = cJSON_GetArrayItem(mods, mod_index);
  cJSON *mem = cJSON_GetObjectItem(mod, "memory");
  if (!cJSON_IsArray(mem)) {
    mem = cJSON_GetObjectItem(mod, "patches");
  }
  if (!cJSON_IsArray(mem) || cJSON_GetArraySize(mem) == 0) {
    state = CHEAT_STATE_INVALID_CHEAT;
    snprintf(reason, reason_size, "mod has no memory entries");
  } else {
    cJSON *m = NULL;
    int on_matches = 0;
    int off_matches = 0;
    int total = 0;
    int mismatches = 0;
    int baseline_unknown_cme = 0;
    cJSON_ArrayForEach(m, mem) {
      cJSON *off_j = cJSON_GetObjectItem(m, "offset");
      cJSON *on_j = cJSON_GetObjectItem(m, "on");
      cJSON *off_j2 = cJSON_GetObjectItem(m, "off");
      cJSON *expected_j = cJSON_GetObjectItem(m, "expected");
      cJSON *abs_j = cJSON_GetObjectItem(m, "absolute");
      if (!cJSON_IsString(off_j) || !cJSON_IsString(on_j) || !cJSON_IsString(off_j2)) {
        state = CHEAT_STATE_INVALID_CHEAT;
        snprintf(reason, reason_size, "memory entry missing offset/on/off");
        break;
      }
      uint8_t on_b[128], off_b[128], exp_b[128], cur[128];
      size_t on_len = 0, off_len = 0, exp_len = 0;
      if (parse_hex_bytes_checked(on_j->valuestring, on_b, sizeof(on_b), &on_len) != 0 ||
          parse_hex_bytes_checked(off_j2->valuestring, off_b, sizeof(off_b), &off_len) != 0 || on_len != off_len) {
        state = CHEAT_STATE_INVALID_CHEAT;
        snprintf(reason, reason_size, "invalid on/off bytes");
        break;
      }
      uint64_t off_u = 0;
      if (parse_offset_hex_checked(off_j->valuestring, &off_u) != 0) {
        state = CHEAT_STATE_INVALID_CHEAT;
        snprintf(reason, reason_size, "invalid offset");
        break;
      }
      if (cJSON_IsString(expected_j) && expected_j->valuestring && expected_j->valuestring[0]) {
        if (parse_hex_bytes_checked(expected_j->valuestring, exp_b, sizeof(exp_b), &exp_len) != 0 || exp_len != on_len) {
          state = CHEAT_STATE_INVALID_CHEAT;
          snprintf(reason, reason_size, "invalid expected bytes");
          break;
        }
      }
      int af_cme = 0, inj_cme = 0, adet_cme = auto_detect_cme;
      get_cheat_addr_flags(cheat_kind_cme, cJSON_IsTrue(abs_j) ? 1 : 0, auto_detect_cme, &af_cme, &inj_cme, &adet_cme);
      const uint8_t *expect_cme = exp_len > 0 ? exp_b : off_b;
      intptr_t addr = cheat_resolve_write_addr(st.pid, st.image_base, off_u,
                                                af_cme, inj_cme, adet_cme,
                                                on_b, on_len, expect_cme);
      if (read_process_memory(st.pid, addr, cur, on_len) != 0) {
        state = CHEAT_STATE_READ_FAILED;
        snprintf(reason, reason_size, "read failed");
        break;
      }
      total++;
      int is_on = memcmp(cur, on_b, on_len) == 0;
      int off_reliable_cme = (cheat_kind_cme == 1) || (exp_len > 0);
      int is_off = off_reliable_cme && (memcmp(cur, (exp_len > 0 ? exp_b : off_b), on_len) == 0);
      if (is_on) { on_matches++; }
      if (is_off) { off_matches++; }
      if (!is_on && !is_off && off_reliable_cme) { mismatches++; }
      if (!is_on && !off_reliable_cme) { baseline_unknown_cme++; }
    }
    if (state == CHEAT_STATE_UNKNOWN) {
      if (on_matches == total && total > 0) {
        state = CHEAT_STATE_ON;
      } else if (mismatches > 0 && baseline_unknown_cme == 0 && on_matches == 0 && off_matches == 0) {
        state = CHEAT_STATE_MISMATCH;
        snprintf(reason, reason_size, "current bytes match neither ON nor expected/off");
      } else if (baseline_unknown_cme > 0 && mismatches == 0 && on_matches == 0) {
        state = CHEAT_STATE_BASELINE_UNKNOWN;
        snprintf(reason, reason_size, "MC4/SHN: no reliable original bytes");
      } else if (off_matches == total && total > 0 && baseline_unknown_cme == 0) {
        state = CHEAT_STATE_OFF;
      } else {
        state = CHEAT_STATE_MIXED;
      }
    }
  }
  pt_detach(st.pid, 0);
  cJSON_Delete(root);
  /* Block crash_suspect mods from being re-enabled */
  if (state != CHEAT_STATE_ON) {
    pthread_mutex_lock(&g_crash_guard_lock);
    for (int _ci = 0; _ci < g_crash_suspects_n; _ci++) {
      if (strcmp(g_crash_suspects[_ci].title_id, title_id) == 0 &&
          g_crash_suspects[_ci].mod_index == mod_index) {
        state = CHEAT_STATE_CRASH_SUSPECT;
        if (reason && reason_size > 0)
          snprintf(reason, reason_size, "Game stopped shortly after enabling this cheat.");
        break;
      }
    }
    pthread_mutex_unlock(&g_crash_guard_lock);
  }
  return state;
}

static void
classify_cheat_error_code(const char *msg, char *out, size_t out_size) {
  const char *code = "apply_failed";
  if (msg && *msg) {
    if (strstr(msg, "bytes mismatch")) {
      code = "bytes_mismatch";
    } else if (strstr(msg, "verify_failed")) {
      code = "verify_failed";
    } else if (strstr(msg, "pt_attach failed")) {
      code = "attach_failed";
    } else if (strstr(msg, "no game is currently running")) {
      code = "process_not_found";
    } else if (strstr(msg, "cheat JSON parse failed") || strstr(msg, "cheat format parse failed") ||
               strstr(msg, "no cheat file")) {
      code = "invalid_cheat";
    } else if (strstr(msg, "memory write failed")) {
      code = "write_failed";
    } else if (strstr(msg, "cheat engine disabled")) {
      code = "engine_disabled";
    } else if (strstr(msg, "app_not_stable")) {
      code = "app_not_stable";
    }
  }
  snprintf(out, out_size, "%s", code);
}

/* GET /api/cheats/apply-dryrun?titleId=X&mod=N
   Returns what a real apply would do: resolved addresses, current bytes,
   page protection, ON bytes — no actual writes. */
void
handle_api_cheats_apply_dryrun(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  char mod_s[16] = {0};
  char err[128] = {0};

  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 ||
      !title_id_normalize(raw_title_id, title_id) ||
      query_value(query, "mod", mod_s, sizeof(mod_s)) != 0) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"titleId and mod required\"}");
    return;
  }
  int mod_index = atoi(mod_s);
  if (mod_index < 0) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"mod must be >= 0\"}");
    return;
  }

  char cheat_path[256] = {0};
  int cheat_kind = 0;
  cJSON *root = load_cheat_json_root_for_title_ex(title_id, err, sizeof(err), cheat_path, sizeof(cheat_path), &cheat_kind);
  if (!root) {
    char body[256];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", err);
    http_send_json(fd, 404, body);
    return;
  }

  cJSON *mods = cJSON_GetObjectItem(root, "mods");
  if (!cJSON_IsArray(mods) || mod_index >= cJSON_GetArraySize(mods)) {
    cJSON_Delete(root);
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"mod index out of range\"}");
    return;
  }

  running_game_state_t st;
  int running_ok = (read_running_state(&st) == 0 && st.running && strcmp(st.title_id, title_id) == 0);
  pid_t pid = running_ok ? st.pid : -1;
  intptr_t base = running_ok ? st.image_base : 0;

  int auto_detect = 1;
  pthread_mutex_lock(&g_cfg_lock);
  auto_detect = g_cfg.cheat_address_auto_detect;
  pthread_mutex_unlock(&g_cfg_lock);

  int attached = 0;
  if (running_ok && pid > 0) {
    attached = (pt_attach(pid) == 0);
  }

  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "ok", 1);
  cJSON_AddStringToObject(out, "titleId", title_id);
  cJSON_AddStringToObject(out, "cheatFile", cheat_path);
  cJSON_AddStringToObject(out, "format", cheat_kind == 1 ? "json" : (cheat_kind == 2 ? "shn" : "mc4"));
  {
    char ph[32]; snprintf(ph, sizeof(ph), "0x%lx", (long)base);
    cJSON_AddNumberToObject(out, "pid", (double)(running_ok ? pid : -1));
    cJSON_AddStringToObject(out, "base", ph);
    cJSON_AddBoolToObject(out, "attached", attached);
  }
  cJSON_AddNumberToObject(out, "mod", mod_index);

  cJSON *mod = cJSON_GetArrayItem(mods, mod_index);
  cJSON *mod_name_j = cJSON_GetObjectItem(mod, "name");
  cJSON_AddStringToObject(out, "modName",
    cJSON_IsString(mod_name_j) && mod_name_j->valuestring ? mod_name_j->valuestring : "");

  cJSON *memory = cJSON_GetObjectItem(mod, "memory");
  if (!cJSON_IsArray(memory)) memory = cJSON_GetObjectItem(mod, "patches");

  cJSON *writes = cJSON_AddArrayToObject(out, "writes");

  if (cJSON_IsArray(memory)) {
    int wi = 0;
    cJSON *m = NULL;
    cJSON_ArrayForEach(m, memory) {
      cJSON *off_j   = cJSON_GetObjectItem(m, "offset");
      cJSON *on_j    = cJSON_GetObjectItem(m, "on");
      cJSON *off_j2  = cJSON_GetObjectItem(m, "off");
      cJSON *exp_j   = cJSON_GetObjectItem(m, "expected");
      cJSON *abs_j   = cJSON_GetObjectItem(m, "absolute");

      cJSON *we = cJSON_CreateObject();
      cJSON_AddNumberToObject(we, "index", wi);

      if (!cJSON_IsString(off_j) || !cJSON_IsString(on_j) || !cJSON_IsString(off_j2)) {
        cJSON_AddStringToObject(we, "error", "missing offset/on/off");
        cJSON_AddItemToArray(writes, we);
        wi++;
        continue;
      }

      uint8_t on_b[128], off_b[128], exp_b[128], cur_b[128];
      size_t on_len = 0, off_len = 0, exp_len = 0;
      uint64_t off_u = 0;

      if (parse_hex_bytes_checked(on_j->valuestring, on_b, sizeof(on_b), &on_len) != 0 ||
          parse_hex_bytes_checked(off_j2->valuestring, off_b, sizeof(off_b), &off_len) != 0 ||
          parse_offset_hex_checked(off_j->valuestring, &off_u) != 0 ||
          on_len == 0 || on_len != off_len) {
        cJSON_AddStringToObject(we, "error", "hex parse failed or on/off size mismatch");
        cJSON_AddItemToArray(writes, we);
        wi++;
        continue;
      }
      if (cJSON_IsString(exp_j) && exp_j->valuestring && exp_j->valuestring[0]) {
        parse_hex_bytes_checked(exp_j->valuestring, exp_b, sizeof(exp_b), &exp_len);
      }

      cJSON_AddStringToObject(we, "rawOffset", off_j->valuestring);
      cJSON_AddStringToObject(we, "onBytes", on_j->valuestring);
      cJSON_AddStringToObject(we, "offBytes", off_j2->valuestring);
      cJSON_AddNumberToObject(we, "len", (double)on_len);

      int af = 0, inj = 0, adet = auto_detect;
      get_cheat_addr_flags(cheat_kind, cJSON_IsTrue(abs_j) ? 1 : 0, auto_detect, &af, &inj, &adet);
      const uint8_t *expect_cmp = exp_len > 0 ? exp_b : off_b;
      intptr_t addr = (pid > 0 && attached) ?
        cheat_resolve_write_addr(pid, base, off_u, af, inj, adet, on_b, on_len, expect_cmp) :
        (af ? (intptr_t)off_u : base + (intptr_t)off_u);

      char addrh[32]; snprintf(addrh, sizeof(addrh), "0x%lx", (long)addr);
      cJSON_AddStringToObject(we, "addr", addrh);

      intptr_t page = (intptr_t)ROUND_PG_DOWN((uintptr_t)addr);
      size_t span = (size_t)(ROUND_PG_UP((uintptr_t)addr + on_len) - (uintptr_t)page);
      char pageh[32]; snprintf(pageh, sizeof(pageh), "0x%lx", (long)page);
      cJSON_AddStringToObject(we, "page", pageh);
      snprintf(pageh, sizeof(pageh), "0x%zx", span);
      cJSON_AddStringToObject(we, "span", pageh);

      if (attached && pid > 0) {
        int prot = kernel_get_vmem_protection(pid, page, span);
        cJSON_AddNumberToObject(we, "vmProt", (double)prot);

        int rdok = (read_process_memory(pid, addr, cur_b, on_len) == 0);
        cJSON_AddBoolToObject(we, "readOk", rdok);
        if (rdok) {
          char cur_hex[49] = {0};
          fmt_hex16(cur_b, on_len, cur_hex, sizeof(cur_hex));
          cJSON_AddStringToObject(we, "currentBytes", cur_hex);
          int is_on  = (memcmp(cur_b, on_b,  on_len) == 0);
          int is_off = (exp_len > 0) ? (memcmp(cur_b, exp_b, on_len) == 0) :
                                       (memcmp(cur_b, off_b, on_len) == 0);
          cJSON_AddBoolToObject(we, "currentIsOn",  is_on);
          cJSON_AddBoolToObject(we, "currentIsOff", is_off);
        }
      } else {
        cJSON_AddBoolToObject(we, "readOk", 0);
        cJSON_AddStringToObject(we, "currentBytes", "");
      }

      cJSON_AddItemToArray(writes, we);
      wi++;
    }
  }

  if (attached) pt_detach(pid, 0);
  cJSON_Delete(root);

  char *body = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  if (body) {
    http_send_json(fd, 200, body);
    free(body);
  } else {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"json encode failed\"}");
  }
}

void
handle_api_cheats_toggle(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  char idx_s[16] = {0};
  char on_s[16] = {0};
  char body[512];
  char err[256] = {0};

  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 || !title_id_normalize(raw_title_id, title_id) ||
      query_value(query, "index", idx_s, sizeof(idx_s)) != 0 || query_value(query, "on", on_s, sizeof(on_s)) != 0) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"missing args\"}");
    return;
  }

  int idx = atoi(idx_s);
  int on = strcmp(on_s, "0") != 0;
  int force = 0;
  char force_s[8] = {0};
  if (query_value(query, "force", force_s, sizeof(force_s)) == 0 && atoi(force_s) != 0) {
    force = 1;
  }
  if (idx < 0) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad index\"}");
    return;
  }
  int allow_force = 0;
  int allow_unsafe_mc4 = 0;
  pthread_mutex_lock(&g_cfg_lock);
  allow_force = g_cfg.allow_force_enable;
  allow_unsafe_mc4 = g_cfg.allow_unsafe_mc4_apply;
  pthread_mutex_unlock(&g_cfg_lock);
  if (on && !(force && allow_force)) {
    char reason[160] = {0};
    cheat_state_kind_t st = check_mod_enable_state(title_id, idx, reason, sizeof(reason));
    int block = 0;
    if (st == CHEAT_STATE_BASELINE_UNKNOWN && !allow_unsafe_mc4) {
      block = 1;
    } else if (st == CHEAT_STATE_CRASH_SUSPECT) {
      block = 1;
    } else if (st == CHEAT_STATE_ADDRESS_UNRESOLVED || st == CHEAT_STATE_FORMAT_NEEDS_REVIEW ||
               st == CHEAT_STATE_MISMATCH || st == CHEAT_STATE_GAME_NOT_RUNNING ||
               st == CHEAT_STATE_PROCESS_NOT_FOUND || st == CHEAT_STATE_BASE_NOT_READY ||
               st == CHEAT_STATE_READ_FAILED || st == CHEAT_STATE_INVALID_CHEAT ||
               st == CHEAT_STATE_GAME_LOADING) {
      block = 1;
    }
    if (block) {
      char *esc = json_escape(reason[0] ? reason : "refusing to patch");
      snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\",\"message\":\"%s\"}",
               st == CHEAT_STATE_MISMATCH ? "version_mismatch" : cheat_state_key(st), esc ? esc : "refusing to patch");
      free(esc);
      cr_log("warn", "cheats", "refusing enable mod=%d title=%s reason=%s", idx, title_id, reason);
      http_send_json(fd, 400, body);
      return;
    }
  }

  int rc = apply_cheat_json(title_id, idx, on, err, sizeof(err));
  if (rc == -2) {
    http_send_json(fd, 429,
      "{\"ok\":false,\"error\":\"apply_in_progress\","
      "\"message\":\"Another cheat is being applied or monitored. Wait for the safety window to finish.\"}");
    return;
  }
  if (rc != 0) {
    char *esc = json_escape(err[0] ? err : "apply failed");
    char code[64];
    running_game_state_t st;
    running_state_get(&st);
    classify_cheat_error_code(err, code, sizeof(code));
    if (!strcmp(code, "app_not_stable")) {
      char *msg_esc = json_escape("Game is not stable enough for patching yet. Wait a few seconds and try again.");
      snprintf(body, sizeof(body),
               "{\"ok\":false,\"error\":\"app_not_stable\",\"message\":\"%s\"}",
               msg_esc ? msg_esc : "Game not stable");
      free(msg_esc);
      free(esc);
      http_send_json(fd, 400, body);
      return;
    }
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", esc ? esc : "apply failed");
    free(esc);
    notification_add("cheat_error", "Cheat apply failed: %s idx=%d (%s)", title_id, idx, err);
    cr_log("error", "cheats", "apply failed %s idx=%d: %s", title_id, idx, err);
    http_send_json(fd, 400, body);
    return;
  }
  snprintf(body, sizeof(body), "{\"ok\":true,\"enabled\":%s}", on ? "true" : "false");
  http_send_json(fd, 200, body);
}

void
handle_api_cheats_delete(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  int removed = 0;

  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 || !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad titleId\"}");
    return;
  }

  DIR *d = opendir(CHEATRUNNER_CHEATS_DIR);
  if (d) {
    struct dirent *ent = NULL;
    while ((ent = readdir(d)) != NULL) {
      const char *name = ent->d_name;
      if (name[0] == '.' || !is_safe_filename(name) || !recognised_cheat_extension(name)) {
        continue;
      }
      char id[10];
      if (!extract_title_id_prefix(name, id, sizeof(id))) {
        continue;
      }
      if (strcmp(id, title_id) != 0) {
        continue;
      }
      char path[256];
      snprintf(path, sizeof(path), "%s/%s", CHEATRUNNER_CHEATS_DIR, name);
      if (unlink(path) == 0 || errno == ENOENT) {
        removed++;
      }
    }
    closedir(d);
  }

  char body[128];
  snprintf(body, sizeof(body), "{\"ok\":true,\"removed\":%d}", removed);
  http_send_json(fd, 200, body);
}

void
handle_api_cheats_engine(int fd) {
  char body[64];
  pthread_mutex_lock(&g_cfg_lock);
  snprintf(body, sizeof(body), "{\"ok\":true,\"enabled\":%s}", g_cfg.cheat_engine ? "true" : "false");
  pthread_mutex_unlock(&g_cfg_lock);
  http_send_json(fd, 200, body);
}

void
handle_api_cheats_engine_toggle(int fd, const char *query) {
  char on_s[16] = {0};
  int on = 0;
  if (query_value(query, "on", on_s, sizeof(on_s)) != 0) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"missing on\"}");
    return;
  }
  on = atoi(on_s) ? 1 : 0;
  pthread_mutex_lock(&g_cfg_lock);
  g_cfg.cheat_engine = on;
  int rc = config_save_locked();
  pthread_mutex_unlock(&g_cfg_lock);
  if (rc != 0) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"save failed\"}");
    return;
  }
  notification_add("cheat_engine", "Cheat engine %s", on ? "enabled" : "disabled");
  cr_log("info", "cheats", "cheat engine %s", on ? "enabled" : "disabled");
  http_send_json(fd, 200, on ? "{\"ok\":true,\"enabled\":true}" : "{\"ok\":true,\"enabled\":false}");
}

void
handle_api_cheats_raw(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  char path[256];
  int kind = 0;
  char *txt = NULL;
  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 || !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad titleId\"}");
    return;
  }
  if (!find_cheat_file_for_title(title_id, path, sizeof(path), &kind)) {
    http_send_json(fd, 404, "{\"ok\":false,\"error\":\"no cheat file\"}");
    return;
  }
  if (read_file_text(path, &txt) != 0 || !txt) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"read failed\"}");
    return;
  }
  http_send_response(fd, 200, "text/plain; charset=utf-8", (const uint8_t *)txt, strlen(txt));
  free(txt);
}

void
handle_api_cheats_validate(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  char err[128] = {0};
  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 || !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad titleId\"}");
    return;
  }
  cJSON *root = load_cheat_json_root_for_title(title_id, err, sizeof(err));
  if (!root) {
    char body[256];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", err);
    http_send_json(fd, 400, body);
    return;
  }
  cJSON *mods = cJSON_GetObjectItem(root, "mods");
  if (!cJSON_IsArray(mods)) {
    cJSON_Delete(root);
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"mods missing\"}");
    return;
  }
  int invalid = 0;
  cJSON *m = NULL;
  cJSON_ArrayForEach(m, mods) {
    cJSON *mem = cJSON_GetObjectItem(m, "memory");
    if (!cJSON_IsArray(mem)) {
      mem = cJSON_GetObjectItem(m, "patches");
    }
    if (!cJSON_IsArray(mem) || cJSON_GetArraySize(mem) == 0) {
      invalid++;
      continue;
    }
    cJSON *w = NULL;
    cJSON_ArrayForEach(w, mem) {
      cJSON *off_j = cJSON_GetObjectItem(w, "offset");
      cJSON *on_j = cJSON_GetObjectItem(w, "on");
      cJSON *off_j2 = cJSON_GetObjectItem(w, "off");
      if (!cJSON_IsString(off_j) || !cJSON_IsString(on_j) || !cJSON_IsString(off_j2)) {
        invalid++;
        continue;
      }
      uint8_t b1[128], b2[128];
      size_t l1 = 0, l2 = 0;
      uint64_t off_u = 0;
      if (parse_offset_hex_checked(off_j->valuestring, &off_u) != 0 ||
          parse_hex_bytes_checked(on_j->valuestring, b1, sizeof(b1), &l1) != 0 ||
          parse_hex_bytes_checked(off_j2->valuestring, b2, sizeof(b2), &l2) != 0 || l1 != l2) {
        invalid++;
      }
    }
  }
  cJSON_Delete(root);
  char body[256];
  snprintf(body, sizeof(body), "{\"ok\":%s,\"titleId\":\"%s\",\"invalidCount\":%d}", invalid ? "false" : "true", title_id, invalid);
  http_send_json(fd, invalid ? 400 : 200, body);
}

void
handle_api_cheats_upload(int fd, const char *query, const char *body, size_t body_len) {
  char filename[128] = {0};
  if (query_value(query, "filename", filename, sizeof(filename)) != 0 || !filename[0]) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"missing filename\"}");
    return;
  }
  if (!is_safe_filename(filename)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"unsafe filename\"}");
    return;
  }
  const char *ext = strrchr(filename, '.');
  if (!ext) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"no extension\"}");
    return;
  }
  const char *dir = NULL;
  size_t max_size = 0;
  if (strcasecmp(ext, ".json") == 0) {
    dir = CHEATRUNNER_CHEATS_JSON_DIR;
    max_size = 2 * 1024 * 1024;
  } else if (strcasecmp(ext, ".shn") == 0) {
    dir = CHEATRUNNER_CHEATS_SHN_DIR;
    max_size = 1024 * 1024;
  } else if (strcasecmp(ext, ".mc4") == 0) {
    dir = CHEATRUNNER_CHEATS_MC4_DIR;
    max_size = 1024 * 1024;
  } else {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"unsupported extension\"}");
    return;
  }
  if (body_len == 0) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"empty body\"}");
    return;
  }
  if (body_len > max_size) {
    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"ok\":false,\"error\":\"payload_too_large\",\"message\":\"Upload exceeds maximum allowed size for this format.\",\"maxBytes\":%u}",
             (unsigned int)max_size);
    http_send_json(fd, 413, resp);
    return;
  }
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir, filename);
  if (write_file_atomic(path, (const uint8_t *)body, body_len) != 0) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"write failed\"}");
    return;
  }
  char resp[256];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"filename\":\"%s\"}", filename);
  http_send_json(fd, 200, resp);
}

/* Task 13: /api/cheats/address-debug?titleId=X
   For each patch in the cheat file: computes absolute & relative candidate
   addresses, reads bytes from both, and shows which one matches on/off/expected.
   Only works if the game is currently running. */
void
handle_api_cheats_address_debug(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 || !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad titleId\"}");
    return;
  }

  char cheat_path[256] = {0};
  char err[128] = {0};
  int cheat_kind = 0;
  cJSON *root = load_cheat_json_root_for_title_ex(title_id, err, sizeof(err), cheat_path, sizeof(cheat_path), &cheat_kind);
  if (!root) {
    char body[256];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", err);
    http_send_json(fd, 404, body);
    return;
  }

  running_game_state_t st;
  int running_ok = (read_running_state(&st) == 0 && st.running);
  int title_match = running_ok && strcmp(st.title_id, title_id) == 0;
  pid_t pid = running_ok ? st.pid : -1;
  intptr_t base = running_ok ? st.image_base : 0;

  int auto_detect = 1;
  pthread_mutex_lock(&g_cfg_lock);
  auto_detect = g_cfg.cheat_address_auto_detect;
  pthread_mutex_unlock(&g_cfg_lock);

  int attach_ok = 0;
  if (title_match && pid > 0 && base != 0) {
    attach_ok = (pt_attach(pid) == 0);
  }

  cJSON *mods = cJSON_GetObjectItem(root, "mods");
  if (!cJSON_IsArray(mods)) { mods = cJSON_GetObjectItem(root, "patches"); }

  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "ok", 1);
  cJSON_AddStringToObject(out, "titleId", title_id);
  cJSON_AddStringToObject(out, "cheatPath", cheat_path);
  const char *fmt_s = cheat_kind == 1 ? "json" : (cheat_kind == 2 ? "shn" : "mc4");
  cJSON_AddStringToObject(out, "cheatFormat", fmt_s);
  {
    cJSON *g = cJSON_AddObjectToObject(out, "game");
    if (g) {
      cJSON_AddBoolToObject(g, "running", running_ok);
      cJSON_AddBoolToObject(g, "titleMatch", title_match);
      cJSON_AddBoolToObject(g, "attachOk", attach_ok);
      char bh[32]; snprintf(bh, sizeof(bh), "0x%lx", (long)base);
      cJSON_AddStringToObject(g, "base", bh);
      cJSON_AddNumberToObject(g, "pid", (double)(running_ok ? pid : -1));
    }
  }
  cJSON_AddBoolToObject(out, "autoDetect", auto_detect);

  cJSON *mod_arr = cJSON_AddArrayToObject(out, "mods");

  if (cJSON_IsArray(mods)) {
    cJSON *mod = NULL;
    cJSON_ArrayForEach(mod, mods) {
      cJSON *mod_obj = cJSON_CreateObject();
      cJSON *n = cJSON_GetObjectItem(mod, "name");
      cJSON_AddStringToObject(mod_obj, "name", cJSON_IsString(n) ? n->valuestring : "");

      cJSON *mem = cJSON_GetObjectItem(mod, "memory");
      if (!cJSON_IsArray(mem)) mem = cJSON_GetObjectItem(mod, "patches");

      cJSON *patch_arr = cJSON_AddArrayToObject(mod_obj, "patches");

      if (cJSON_IsArray(mem)) {
        int wi = 0;
        cJSON *m = NULL;
        cJSON_ArrayForEach(m, mem) {
          cJSON *off_j = cJSON_GetObjectItem(m, "offset");
          cJSON *on_j  = cJSON_GetObjectItem(m, "on");
          cJSON *off_j2= cJSON_GetObjectItem(m, "off");
          cJSON *exp_j = cJSON_GetObjectItem(m, "expected");
          cJSON *abs_j = cJSON_GetObjectItem(m, "absolute");

          cJSON *pe = cJSON_CreateObject();
          cJSON_AddNumberToObject(pe, "index", wi++);

          if (!cJSON_IsString(off_j) || !cJSON_IsString(on_j) || !cJSON_IsString(off_j2)) {
            cJSON_AddStringToObject(pe, "error", "missing offset/on/off");
            cJSON_AddItemToArray(patch_arr, pe);
            continue;
          }

          uint8_t on_b[128], off_b[128], exp_b[128];
          size_t on_len = 0, off_len = 0, exp_len = 0;
          uint64_t off_u = 0;

          if (parse_hex_bytes_checked(on_j->valuestring, on_b, sizeof(on_b), &on_len) != 0 ||
              parse_hex_bytes_checked(off_j2->valuestring, off_b, sizeof(off_b), &off_len) != 0 ||
              on_len != off_len || on_len == 0) {
            cJSON_AddStringToObject(pe, "error", "invalid on/off bytes");
            cJSON_AddItemToArray(patch_arr, pe);
            continue;
          }
          if (parse_offset_hex_checked(off_j->valuestring, &off_u) != 0) {
            cJSON_AddStringToObject(pe, "error", "invalid offset");
            cJSON_AddItemToArray(patch_arr, pe);
            continue;
          }
          if (cJSON_IsString(exp_j) && exp_j->valuestring && exp_j->valuestring[0]) {
            parse_hex_bytes_checked(exp_j->valuestring, exp_b, sizeof(exp_b), &exp_len);
          }

          char off_hex[32]; snprintf(off_hex, sizeof(off_hex), "0x%llX", (unsigned long long)off_u);
          cJSON_AddStringToObject(pe, "offset", off_hex);
          cJSON_AddBoolToObject(pe, "absoluteFlag", cJSON_IsTrue(abs_j));

          intptr_t abs_cand = (intptr_t)off_u;
          intptr_t rel_cand = base + (intptr_t)off_u;

          const uint8_t *expect_cmp = exp_len > 0 ? exp_b : off_b;

          intptr_t resolved = cheat_resolve_write_addr(pid, base, off_u,
                                                        cJSON_IsTrue(abs_j), cheat_kind != 1, auto_detect,
                                                        on_b, on_len, expect_cmp);
          char res_hex[32]; snprintf(res_hex, sizeof(res_hex), "0x%lx", (long)resolved);
          cJSON_AddStringToObject(pe, "resolvedAddress", res_hex);
          cJSON_AddStringToObject(pe, "resolvedMode",
                                  resolved == abs_cand ? "absolute" : "relative");

          cJSON *cands_j = cJSON_AddArrayToObject(pe, "candidates");

          /* absolute candidate */
          {
            cJSON *ca = cJSON_CreateObject();
            char ah[32]; snprintf(ah, sizeof(ah), "0x%lx", (long)abs_cand);
            cJSON_AddStringToObject(ca, "mode", "absolute");
            cJSON_AddStringToObject(ca, "address", ah);
            if (attach_ok && on_len <= 128) {
              uint8_t buf[128];
              int rd = read_process_memory(pid, abs_cand, buf, on_len);
              cJSON_AddBoolToObject(ca, "readOk", rd == 0);
              if (rd == 0) {
                char hx[512]; bytes_to_hex(buf, on_len, hx, sizeof(hx));
                cJSON_AddStringToObject(ca, "current", hx);
                cJSON_AddBoolToObject(ca, "matchOn",  memcmp(buf, on_b,  on_len) == 0);
                cJSON_AddBoolToObject(ca, "matchOff", memcmp(buf, expect_cmp, on_len) == 0);
              }
            } else {
              cJSON_AddBoolToObject(ca, "readOk", 0);
            }
            cJSON_AddItemToArray(cands_j, ca);
          }

          /* relative candidate */
          {
            cJSON *cr2 = cJSON_CreateObject();
            char rh[32]; snprintf(rh, sizeof(rh), "0x%lx", (long)rel_cand);
            cJSON_AddStringToObject(cr2, "mode", "relative");
            cJSON_AddStringToObject(cr2, "address", rh);
            if (attach_ok && on_len <= 128) {
              uint8_t buf[128];
              int rd = read_process_memory(pid, rel_cand, buf, on_len);
              cJSON_AddBoolToObject(cr2, "readOk", rd == 0);
              if (rd == 0) {
                char hx[512]; bytes_to_hex(buf, on_len, hx, sizeof(hx));
                cJSON_AddStringToObject(cr2, "current", hx);
                cJSON_AddBoolToObject(cr2, "matchOn",  memcmp(buf, on_b,  on_len) == 0);
                cJSON_AddBoolToObject(cr2, "matchOff", memcmp(buf, expect_cmp, on_len) == 0);
              }
            } else {
              cJSON_AddBoolToObject(cr2, "readOk", 0);
            }
            cJSON_AddItemToArray(cands_j, cr2);
          }

          char on_hex_s[512], off_hex_s[512], exp_hex_s[512];
          bytes_to_hex(on_b, on_len, on_hex_s, sizeof(on_hex_s));
          bytes_to_hex(off_b, on_len, off_hex_s, sizeof(off_hex_s));
          bytes_to_hex(expect_cmp, on_len, exp_hex_s, sizeof(exp_hex_s));
          cJSON_AddStringToObject(pe, "on",  on_hex_s);
          cJSON_AddStringToObject(pe, "off", off_hex_s);
          cJSON_AddStringToObject(pe, "expected", exp_hex_s);
          cJSON_AddNumberToObject(pe, "byteLen", (double)on_len);

          cJSON_AddItemToArray(patch_arr, pe);
        }
      }
      cJSON_AddItemToArray(mod_arr, mod_obj);
    }
  }

  if (attach_ok) { pt_detach(pid, 0); }
  cJSON_Delete(root);

  char *s = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  if (!s) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}"); return; }
  http_send_json(fd, 200, s);
  free(s);
}


void
handle_api_cheats_sources(int fd) {
  source_config_model_t model;
  source_model_load(&model);
  int sources_enabled = 1;
  cfg_get_cheat_remote_opts(&sources_enabled, NULL, NULL, NULL);
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "ok", 1);
  cJSON *arr = cJSON_AddArrayToObject(out, "sources");
  for (int i = 0; i < model.cheat_count; i++) {
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "id", model.cheat_sources[i].id);
    cJSON_AddStringToObject(s, "name", model.cheat_sources[i].name);
    cJSON_AddStringToObject(s, "type", model.cheat_sources[i].type);
    cJSON_AddBoolToObject(s, "enabled", sources_enabled && model.cheat_sources[i].enabled);
    cJSON_AddItemToArray(arr, s);
  }
  char *txt = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  if (!txt) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}");
    return;
  }
  http_send_json(fd, 200, txt);
  free(txt);
}

/* ---- /api/sources/jobs/start ---- */

void
handle_api_sources_jobs_start(int fd, const char *body_json) {
  cJSON *req = NULL;
  if (parse_json_from_body(body_json, &req) != 0) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid_json\"}");
    return;
  }
  int job_id = 0;
  int rc = cr_source_job_start(req, &job_id);
  cJSON_Delete(req);
  if (rc == -2) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"unknown_type\"}");
    return;
  }
  if (rc == -3) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid_titleId\"}");
    return;
  }
  if (rc == -4) {
    http_send_json(fd, 503, "{\"ok\":false,\"error\":\"job_queue_full\",\"message\":\"Too many concurrent remote operations. Try again.\"}");
    return;
  }
  if (rc != 0) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"job_start_failed\"}");
    return;
  }
  char resp[64];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"jobId\":%d}", job_id);
  http_send_json(fd, 202, resp);
}

/* ---- /api/sources/jobs/status ---- */

void
handle_api_sources_jobs_status(int fd, const char *query) {
  char id_str[32] = {0};
  if (query_value(query, "id", id_str, sizeof(id_str)) != 0 || !id_str[0]) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"missing_id\"}");
    return;
  }
  int job_id = atoi(id_str);
  if (job_id <= 0) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid_id\"}");
    return;
  }
  cJSON *out = cr_source_job_status_json(job_id);
  if (!out) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}");
    return;
  }
  int status = cJSON_GetObjectItem(out, "notFound") ? 404 : 200;
  char *txt = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  if (!txt) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}");
    return;
  }
  http_send_json(fd, status, txt);
  free(txt);
}

/* ---- Legacy stubs (replaced by /api/sources/jobs/start) ---- */

void
handle_api_cheats_remote_find(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  char version[64] = {0};
  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 ||
      !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid_titleId\"}");
    return;
  }
  query_value(query, "version", version, sizeof(version));
  (void)title_id; (void)version;
  http_send_json(fd, 409, "{\"ok\":false,\"error\":\"use_jobs\",\"message\":\"Use POST /api/sources/jobs/start instead.\"}");
}


void
handle_api_cheats_remote_download(int fd, const char *body_json) {
  (void)body_json;
  http_send_json(fd, 409, "{\"ok\":false,\"error\":\"use_jobs\",\"message\":\"Use POST /api/sources/jobs/start instead.\"}");
}

void
handle_api_cheats_find(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 ||
      !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid titleId\"}");
    return;
  }

  cheat_file_search_t ctx;
  int found_local = find_cheat_candidates(title_id, &ctx);

  char cands_buf[2048] = "[";
  int cpos = 1;
  for (int ci = 0; ci < ctx.candidate_count && ci < MAX_CHEAT_CANDIDATES; ci++) {
    const char *cf = ctx.candidates[ci].kind == 1 ? "json" : (ctx.candidates[ci].kind == 2 ? "shn" : "mc4");
    const char *cb = strrchr(ctx.candidates[ci].path, '/');
    cb = cb ? cb + 1 : ctx.candidates[ci].path;
    char entry[512];
    int en = snprintf(entry, sizeof(entry), "%s{\"format\":\"%s\",\"filename\":\"%s\",\"path\":\"%s\",\"score\":%d}",
                      ci > 0 ? "," : "", cf, cb, ctx.candidates[ci].path, ctx.candidates[ci].score);
    if (en > 0 && cpos + en + 2 < (int)sizeof(cands_buf)) {
      memcpy(cands_buf + cpos, entry, en);
      cpos += en;
    }
  }
  cands_buf[cpos++] = ']';
  cands_buf[cpos]   = '\0';

  if (found_local) {
    const char *sel_fmt = ctx.best_kind == 1 ? "json" : (ctx.best_kind == 2 ? "shn" : "mc4");
    const char *sel_base = strrchr(ctx.best_path, '/');
    sel_base = sel_base ? sel_base + 1 : ctx.best_path;
    char body[4096];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"local\":true,\"source\":\"local\",\"titleId\":\"%s\","
             "\"format\":\"%s\",\"filename\":\"%s\",\"path\":\"%s\","
             "\"selectedReason\":\"highest score local file\","
             "\"candidateCount\":%d,\"candidates\":%s}",
             title_id, sel_fmt, sel_base, ctx.best_path,
             ctx.candidate_count, cands_buf);
    http_send_json(fd, 200, body);
    return;
  }

  char body[512];
  snprintf(body, sizeof(body),
           "{\"ok\":false,\"error\":\"no_local_cheat_found\","
           "\"message\":\"No local cheat file found in /data/cheatrunner/cheats.\","
           "\"candidateCount\":%d,\"candidates\":%s}",
           ctx.candidate_count, cands_buf);
  http_send_json(fd, 404, body);
}

void
handle_api_cheats_download(int fd, const char *query) {
  (void)query;
  http_send_json(fd, 410,
    "{\"ok\":false,\"error\":\"remote_download_removed\","
    "\"message\":\"Remote cheat download was removed. Use local cheat files in /data/cheatrunner/cheats.\"}");
}

void
handle_api_cheats_list(int fd) {
  DIR *d = opendir(CHEATRUNNER_CHEATS_DIR);
  if (!d) {
    http_send_json(fd, 200, "{\"ok\":true,\"files\":[]}");
    return;
  }
  char body[4096];
  size_t off = 0;
  off += (size_t)snprintf(body + off, sizeof(body) - off, "{\"ok\":true,\"files\":[");
  int first = 1;
  struct dirent *ent = NULL;
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.') {
      continue;
    }
    if (!is_safe_filename(ent->d_name)) {
      continue;
    }
    off += (size_t)snprintf(body + off, sizeof(body) - off, "%s\"%s\"", first ? "" : ",", ent->d_name);
    first = 0;
    if (off + 32 >= sizeof(body)) {
      break;
    }
  }
  closedir(d);
  snprintf(body + off, sizeof(body) - off, "]}");
  http_send_json(fd, 200, body);
}

/* POST /api/cheats/repo/download?source=<name|all>[&overwrite=1] */
void
handle_api_cheats_repo_download(int fd, const char *query) {
  char source[32] = {0};
  char ovr_s[4]   = {0};
  query_value(query, "source",    source, sizeof(source));
  query_value(query, "overwrite", ovr_s,  sizeof(ovr_s));
  int overwrite = (ovr_s[0] == '1') ? 1 : 0;

  if (!source[0] || (strcmp(source, "hencollection") != 0 &&
                     strcmp(source, "ps5cheats")     != 0 &&
                     strcmp(source, "goldhen")       != 0 &&
                     strcmp(source, "all")           != 0)) {
    http_send_json(fd, 400,
      "{\"ok\":false,\"error\":\"invalid_source\","
      "\"message\":\"source must be hencollection, ps5cheats, goldhen, or all\"}");
    return;
  }

  if (repo_mirror_start(source, overwrite) != 0) {
    char buf[256];
    repo_mirror_status_json(buf, sizeof(buf));
    http_send_json(fd, 409, buf);
    return;
  }

  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"ok\":true,\"state\":\"running\",\"source\":\"%s\",\"overwrite\":%d}", source, overwrite);
  http_send_json(fd, 200, buf);
}

/* GET /api/cheats/repo/download/status */
void
handle_api_cheats_repo_download_status(int fd) {
  char buf[2048];
  repo_mirror_status_json(buf, sizeof(buf));
  http_send_json(fd, 200, buf);
}

/* Legacy /api/cheats/download-all — kept for backward compat, now delegates to repo_mirror */
void
handle_api_cheats_download_all(int fd) {
  handle_api_cheats_repo_download_status(fd);
}

void
handle_api_cheats_download_all_status(int fd) {
  handle_api_cheats_repo_download_status(fd);
}

void
handle_api_cheats_index_status(int fd) {
  http_send_json(fd, 200, "{\"ok\":true,\"available\":false,\"indexes\":[]}");
}

void
handle_api_cheats_index_refresh(int fd) {
  http_send_json(fd, 410, "{\"ok\":false,\"error\":\"remote_index_removed\"}");
}

/* ── /api/dev/privileges ────────────────────────────────────────── */
void
handle_api_dev_privileges(int fd) {
  const cr_priv_status_t *ps = cr_priv_get();
  cJSON *out = cJSON_CreateObject();
  if (!out) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}"); return; }

  cJSON_AddBoolToObject(out, "ok", ps->can_patch_game ? 1 : 0);
  cJSON_AddNumberToObject(out, "uid", ps->uid);
  cJSON_AddNumberToObject(out, "gid", ps->gid);
  cJSON_AddNumberToObject(out, "euid", ps->euid);
  cJSON_AddNumberToObject(out, "egid", ps->egid);
  cJSON_AddBoolToObject(out, "uidOk", ps->uid_ok);
  cJSON_AddBoolToObject(out, "sandboxEscaped", ps->sandbox_ok);
  cJSON_AddBoolToObject(out, "rootDirOk", ps->rootvnode_ok);
  cJSON_AddBoolToObject(out, "sceAuthOk", ps->authid_ok);
  cJSON_AddBoolToObject(out, "sceCapsOk", ps->caps_ok);
  cJSON_AddBoolToObject(out, "sceAttrsOk", ps->attrs_ok);
  cJSON_AddBoolToObject(out, "kernelRwOk", ps->kernel_rw_ok);
  cJSON_AddBoolToObject(out, "kernelMprotectOk", ps->kernel_mprotect_ok);
  cJSON_AddBoolToObject(out, "gameMemoryPatchOk", ps->can_patch_game);
  cJSON_AddBoolToObject(out, "canPatchGameMemory", ps->can_patch_game);

  char authid_hex[24];
  snprintf(authid_hex, sizeof(authid_hex), "0x%llx", (unsigned long long)ps->authid);
  cJSON_AddStringToObject(out, "authId", authid_hex);

  cJSON *warn_arr = cJSON_AddArrayToObject(out, "warnings");
  if (warn_arr) {
    for (int i = 0; i < ps->n_warnings; i++) {
      cJSON *ws = cJSON_CreateString(ps->warnings[i]);
      if (ws) cJSON_AddItemToArray(warn_arr, ws);
    }
  }

  char *s = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  if (!s) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}"); return; }
  http_send_json(fd, 200, s);
  free(s);
}

/* ── /api/dev/diag ──────────────────────────────────────────────── */
void
handle_api_dev_diag(int fd) {
  cJSON *out = cJSON_CreateObject();
  if (!out) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}"); return; }
  cJSON_AddBoolToObject(out, "ok", 1);

  char *s = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  if (!s) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}"); return; }
  http_send_json(fd, 200, s);
  free(s);
}

void
handle_api_dev_open_browser(int fd, const char *method, const char *query) {
  if (strcmp(method, "POST") != 0) {
    http_send_json(fd, 405, "{\"ok\":false,\"error\":\"method_not_allowed\",\"message\":\"Use POST.\"}");
    return;
  }

  int dev_enabled = 0;
  pthread_mutex_lock(&g_cfg_lock);
  dev_enabled = g_cfg.dev_reload_enabled;
  pthread_mutex_unlock(&g_cfg_lock);
  if (!dev_enabled) {
    http_send_json(fd, 403, "{\"ok\":false,\"error\":\"dev_reload_disabled\"}");
    return;
  }

  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  query_value(query, "titleId", raw_title_id, sizeof(raw_title_id));
  if (raw_title_id[0] && !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid_titleId\"}");
    return;
  }
  if (!title_id[0]) {
    running_game_state_t st;
    memset(&st, 0, sizeof(st));
    if (read_running_state(&st) == 0 && st.running && st.title_id[0]) {
      snprintf(title_id, sizeof(title_id), "%s", st.title_id);
    }
  }

  char url[192];
  if (title_id[0]) {
    snprintf(url, sizeof(url), "http://127.0.0.1:9999/#trainer=%s", title_id);
  } else {
    snprintf(url, sizeof(url), "http://127.0.0.1:9999");
  }

#if CHEATRUNNER_HAVE_BROWSER_OPEN
  int rc = cr_browser_open_url(url);
  char body[512];
  snprintf(body, sizeof(body), "{\"ok\":true,\"url\":\"%s\",\"titleId\":\"%s\",\"rc\":%d}",
           url, title_id, rc);
  http_send_json(fd, 200, body);
#else
  (void)url;
  (void)title_id;
  http_send_json(fd, 501, "{\"ok\":false,\"error\":\"browser_open_not_compiled\",\"message\":\"Browser test endpoint is unavailable in this build.\"}");
#endif
}

/* ── /api/dev/memtest ───────────────────────────────────────────── */
void
handle_api_dev_memtest(int fd) {
  cJSON *out = cJSON_CreateObject();
  if (!out) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}"); return; }

  cJSON_AddBoolToObject(out, "ok", 1);

  /* Level 1: self-process buffer test */
  static const uint8_t PATTERN[8] = {0xDE,0xAD,0xBE,0xEF,0xCA,0xFE,0xBA,0xBE};
  uint8_t *buf = (uint8_t *)mmap(NULL, 0x4000, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int self_ok = 0;
  char self_err[64] = "mmap failed";
  if (buf != MAP_FAILED) {
    memset(buf, 0, 8);
    memcpy(buf, PATTERN, sizeof(PATTERN));
    if (memcmp(buf, PATTERN, sizeof(PATTERN)) == 0) {
      memset(buf, 0, 8);
      self_ok = 1;
      self_err[0] = '\0';
    } else {
      snprintf(self_err, sizeof(self_err), "readback mismatch");
    }
    munmap(buf, 0x4000);
  }

  cJSON *self = cJSON_AddObjectToObject(out, "selfTest");
  if (self) {
    cJSON_AddBoolToObject(self, "ok", self_ok);
    if (!self_ok) cJSON_AddStringToObject(self, "error", self_err);
  }

  /* Level 2: running game process dry test (read + mprotect probe, no writes) */
  running_game_state_t rgs;
  running_state_get(&rgs);
  cJSON *game = cJSON_AddObjectToObject(out, "gameProcessTest");
  if (game) {
    if (!rgs.running || rgs.pid <= 0) {
      cJSON_AddBoolToObject(game, "skipped", 1);
      cJSON_AddStringToObject(game, "reason", "no game running");
    } else {
      cJSON_AddBoolToObject(game, "skipped", 0);
      cJSON_AddNumberToObject(game, "pid", (double)rgs.pid);
      char base_hex[24];
      snprintf(base_hex, sizeof(base_hex), "0x%lx", (long)rgs.image_base);
      cJSON_AddStringToObject(game, "imageBase", base_hex);

      /* Probe read at image base — requires ptrace attach first */
      uint8_t probe[8] = {0};
      int pt_rc = -1;
      int mprotect_rc = -1;
      int attach_rc = -1;
      if (rgs.image_base) {
        intptr_t pg = (intptr_t)ROUND_PG_DOWN((uintptr_t)rgs.image_base);
        size_t sp = (size_t)0x4000;
        int orig_prot = kernel_get_vmem_protection(rgs.pid, pg, sp);
        mprotect_rc = kernel_mprotect(rgs.pid, pg, sp, PROT_READ | PROT_WRITE | PROT_EXEC);
        if (mprotect_rc == 0) {
          attach_rc = pt_attach(rgs.pid);
          if (attach_rc == 0) {
            pt_rc = pt_copyout(rgs.pid, rgs.image_base, probe, sizeof(probe));
            pt_detach(rgs.pid, 0);
          }
          int rp = orig_prot >= 0 ? orig_prot : (PROT_READ | PROT_EXEC);
          kernel_mprotect(rgs.pid, pg, sp, rp);
        }
      }

      char probe_hex[28] = {0};
      fmt_hex16(probe, sizeof(probe), probe_hex, sizeof(probe_hex));
      cJSON_AddBoolToObject(game, "mprotectOk", mprotect_rc == 0);
      cJSON_AddBoolToObject(game, "ptAttachOk", attach_rc == 0);
      cJSON_AddBoolToObject(game, "ptReadOk", pt_rc >= 0);
      cJSON_AddStringToObject(game, "probeBytes", probe_hex);
      cJSON_AddNumberToObject(game, "mprotectRc", mprotect_rc);
      cJSON_AddNumberToObject(game, "ptAttachRc", attach_rc);
      cJSON_AddNumberToObject(game, "ptReadRc", pt_rc);
    }
  }

  char *s = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  if (!s) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}"); return; }
  http_send_json(fd, 200, s);
  free(s);
}

void
handle_api_dev_shutdown(int fd, const char *method, const char *query, const char *token_header, const char *client_ip) {
  (void)query; (void)token_header;
  int enabled = 0;
  int delay_ms = 700;
  char body[512];

  if (strcmp(method, "POST") != 0) {
    http_send_json(fd, 405, "{\"ok\":false,\"error\":\"method_not_allowed\",\"message\":\"Use POST for shutdown.\"}");
    return;
  }

  pthread_mutex_lock(&g_cfg_lock);
  enabled = g_cfg.dev_reload_enabled;
  delay_ms = g_cfg.dev_shutdown_delay_ms;
  pthread_mutex_unlock(&g_cfg_lock);

  if (!enabled) {
    cr_log("warn", "dev", "shutdown rejected: dev reload disabled");
    http_send_json(fd, 403,
                   "{\"ok\":false,\"error\":\"dev_reload_disabled\",\"message\":\"Dev reload mode is disabled.\"}");
    return;
  }

  cr_log("info", "dev", "shutdown request received from %s", client_ip ? client_ip : "unknown");
  cr_log("info", "dev", "shutdown accepted; exiting in %dms", delay_ms);
  snprintf(body, sizeof(body),
           "{\"ok\":true,\"message\":\"CheatRunner is shutting down. Send the new ELF from your PC.\",\"delayMs\":%d}",
           delay_ms);
  http_send_json(fd, 200, body);
  cheatrunner_request_shutdown(delay_ms);
}

/* ── /api/user/context ──────────────────────────────────────────── */
void
handle_api_user_context(int fd) {
  char uid_cfg[32] = "auto";
  pthread_mutex_lock(&g_cfg_lock);
  snprintf(uid_cfg, sizeof(uid_cfg), "%s", g_cfg.launch_user_id);
  pthread_mutex_unlock(&g_cfg_lock);

  /* Determine configured mode */
  const char *mode = "auto";
  char mode_user[24] = "";
  if (strcasecmp(uid_cfg, "none") == 0) {
    mode = "none";
  } else if (strcasecmp(uid_cfg, "auto") != 0 && uid_cfg[0] != '\0') {
    mode = "config";
    snprintf(mode_user, sizeof(mode_user), "%s", uid_cfg);
  }

  /* Foreground user */
  uint32_t fguser = 0xFFFFFFFFu;
  int fg_valid = (sceUserServiceGetForegroundUser(&fguser) == 0 &&
                  fguser != 0xFFFFFFFFu && (int32_t)fguser != -1);
  char fg_str[24];
  if (fg_valid) snprintf(fg_str, sizeof(fg_str), "0x%08X", fguser);
  else          snprintf(fg_str, sizeof(fg_str), "none");

  /* Login user list */
  uint32_t uid_list[16] = {0};
  size_t uid_actual = 0;
  int list_ok = 0;
  char users_json[320] = "[]";
#if CHEATRUNNER_HAVE_SCE_USER_LIST
  list_ok = (sceUserServiceGetLoginUserIdList(uid_list, 16, &uid_actual) == 0);
  if (list_ok && uid_actual > 0) {
    char tmp[320];
    int pos = 0;
    tmp[pos++] = '[';
    for (size_t i = 0; i < uid_actual && i < 16 && pos < (int)sizeof(tmp) - 20; i++) {
      if (i > 0) tmp[pos++] = ',';
      pos += snprintf(tmp + pos, sizeof(tmp) - pos, "\"0x%08X\"", uid_list[i]);
    }
    if (pos < (int)sizeof(tmp) - 2) { tmp[pos++] = ']'; tmp[pos] = '\0'; }
    snprintf(users_json, sizeof(users_json), "%s", tmp);
  }
#endif

  /* Resolve which user would be used for launch */
  const char *src = "none";
  char resolved_str[24] = "none";
  int resolved_ok = 0;
  if (strcmp(mode, "none") == 0) {
    src = "none";
  } else if (strcmp(mode, "config") == 0) {
    src = "config";
    snprintf(resolved_str, sizeof(resolved_str), "%s", mode_user);
    resolved_ok = 1;
  } else { /* auto */
    if (fg_valid) {
      src = "foreground";
      snprintf(resolved_str, sizeof(resolved_str), "0x%08X", fguser);
      resolved_ok = 1;
    }
#if CHEATRUNNER_HAVE_SCE_USER_LIST
    else if (list_ok && uid_actual == 1 &&
             uid_list[0] != 0xFFFFFFFFu && (int32_t)uid_list[0] != -1) {
      src = "login_list";
      snprintf(resolved_str, sizeof(resolved_str), "0x%08X", uid_list[0]);
      resolved_ok = 1;
    }
#endif
  }

  const char *msg = resolved_ok ? "user resolved"
    : (strcmp(mode, "none") == 0 ? "user context disabled" : "no valid user found");

  char body[896];
  snprintf(body, sizeof(body),
    "{\"ok\":true,"
    "\"configured\":{\"mode\":\"%s\",\"userId\":\"%s\"},"
    "\"foreground\":{\"ok\":%s,\"userId\":\"%s\"},"
    "\"loginUserList\":{\"available\":%s,\"count\":%zu,\"users\":%s},"
    "\"resolved\":{\"ok\":%s,\"source\":\"%s\",\"userId\":\"%s\"},"
    "\"message\":\"%s\"}",
    mode, mode_user,
    fg_valid ? "true" : "false", fg_str,
    list_ok ? "true" : "false", uid_actual, users_json,
    resolved_ok ? "true" : "false", src, resolved_str,
    msg);
  http_send_json(fd, 200, body);
}

/* ── /api/diag/title ────────────────────────────────────────────── */
void
handle_api_diag_title(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 || !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad titleId\"}");
    return;
  }
  cJSON *out = cJSON_CreateObject();
  if (!out) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}");
    return;
  }
  cJSON_AddBoolToObject(out, "ok", 1);
  cJSON_AddStringToObject(out, "titleId", title_id);

  running_game_state_t rgs;
  running_state_get(&rgs);
  int title_match = rgs.running && strcmp(rgs.title_id, title_id) == 0;
  {
    cJSON *r = cJSON_AddObjectToObject(out, "running");
    if (r) {
      cJSON_AddBoolToObject(r, "running", rgs.running);
      cJSON_AddBoolToObject(r, "titleMatch", title_match);
      cJSON_AddStringToObject(r, "runningTitleId", rgs.title_id);
      cJSON_AddNumberToObject(r, "pid", (double)(rgs.running ? (int)rgs.pid : -1));
      cJSON_AddNumberToObject(r, "appId", (double)rgs.app_id);
      char base_hex[32]; snprintf(base_hex, sizeof(base_hex), "0x%lx", (long)rgs.image_base);
      cJSON_AddStringToObject(r, "imageBase", base_hex);
    }
  }

  {
    char ls_phase[64] = {0}, ls_title[12] = {0}, ls_message[160] = {0};
    char ls_method[64] = {0}, ls_hex[16] = {0};
    int ls_busy = 0, ls_rc = 0, ls_verified = 0;
    pthread_mutex_lock(&g_launch_status_lock);
    snprintf(ls_phase,   sizeof(ls_phase),   "%s", g_launch_status.phase);
    snprintf(ls_title,   sizeof(ls_title),   "%s", g_launch_status.title_id);
    snprintf(ls_message, sizeof(ls_message), "%s", g_launch_status.message);
    snprintf(ls_method,  sizeof(ls_method),  "%s", g_launch_status.method);
    snprintf(ls_hex,     sizeof(ls_hex),     "%s", g_launch_status.hex);
    ls_busy     = g_launch_status.busy;
    ls_rc       = g_launch_status.rc;
    ls_verified = g_launch_status.verified;
    pthread_mutex_unlock(&g_launch_status_lock);
    cJSON *ls = cJSON_AddObjectToObject(out, "launchStatus");
    if (ls) {
      cJSON_AddBoolToObject(ls, "busy", ls_busy);
      cJSON_AddStringToObject(ls, "phase", ls_phase);
      cJSON_AddStringToObject(ls, "titleId", ls_title);
      cJSON_AddStringToObject(ls, "message", ls_message);
      cJSON_AddStringToObject(ls, "method", ls_method);
      cJSON_AddStringToObject(ls, "rcHex", ls_hex);
      cJSON_AddNumberToObject(ls, "rc", ls_rc);
      cJSON_AddBoolToObject(ls, "verified", ls_verified);
    }
  }

  {
    char cheat_path[256] = {0};
    int cheat_kind = 0;
    char cheat_err[128] = {0};
    int found = 0;
    int mod_count = 0;
    cJSON *root = load_cheat_json_root_for_title_ex(title_id, cheat_err, sizeof(cheat_err), cheat_path, sizeof(cheat_path), &cheat_kind);
    if (root) {
      found = 1;
      cJSON *mods = cJSON_GetObjectItem(root, "mods");
      if (!cJSON_IsArray(mods)) mods = cJSON_GetObjectItem(root, "patches");
      if (cJSON_IsArray(mods)) mod_count = cJSON_GetArraySize(mods);
      cJSON_Delete(root);
    }
    const char *fmt_str = cheat_kind == 1 ? "json" : (cheat_kind == 2 ? "shn" : (cheat_kind == 3 ? "mc4" : "none"));
    cJSON *cf = cJSON_AddObjectToObject(out, "cheat");
    if (cf) {
      cJSON_AddBoolToObject(cf, "found", found);
      cJSON_AddStringToObject(cf, "format", fmt_str);
      cJSON_AddStringToObject(cf, "path", found ? cheat_path : "");
      cJSON_AddNumberToObject(cf, "modCount", mod_count);
      if (!found && cheat_err[0]) cJSON_AddStringToObject(cf, "error", cheat_err);
    }
  }

  {
    cheat_file_search_t cfs;
    find_cheat_candidates(title_id, &cfs);
    cJSON *scan = cJSON_AddObjectToObject(out, "scan");
    if (scan) {
      cJSON_AddNumberToObject(scan, "candidateCount", cfs.candidate_count);
      cJSON_AddStringToObject(scan, "preferredVer", cfs.preferred_ver);
      cJSON *cands = cJSON_AddArrayToObject(scan, "candidates");
      if (cands) {
        for (int i = 0; i < cfs.candidate_count; i++) {
          cJSON *ce = cJSON_CreateObject();
          const char *p = cfs.candidates[i].path;
          const char *base = strrchr(p, '/');
          cJSON_AddStringToObject(ce, "file", base ? base + 1 : p);
          cJSON_AddStringToObject(ce, "path", p);
          cJSON_AddNumberToObject(ce, "score", cfs.candidates[i].score);
          const char *fmt = cfs.candidates[i].kind == 1 ? "json" : (cfs.candidates[i].kind == 2 ? "shn" : "mc4");
          cJSON_AddStringToObject(ce, "format", fmt);
          cJSON_AddItemToArray(cands, ce);
        }
      }
    }
  }

  {
    char uid_cfg[32] = {0}, mc4mode[16] = {0}, shnmode[16] = {0};
    int unsafe_mc4 = 0;
    pthread_mutex_lock(&g_cfg_lock);
    snprintf(uid_cfg,  sizeof(uid_cfg),  "%s", g_cfg.launch_user_id);
    snprintf(mc4mode,  sizeof(mc4mode),  "%s", g_cfg.cheat_mc4_address_mode);
    snprintf(shnmode,  sizeof(shnmode),  "%s", g_cfg.cheat_shn_address_mode);
    unsafe_mc4 = g_cfg.allow_unsafe_mc4_apply;
    pthread_mutex_unlock(&g_cfg_lock);
    cJSON *cfg = cJSON_AddObjectToObject(out, "config");
    if (cfg) {
      cJSON_AddStringToObject(cfg, "launchUserId", uid_cfg[0] ? uid_cfg : "auto");
      cJSON_AddStringToObject(cfg, "cheatMc4AddressMode", mc4mode[0] ? mc4mode : "auto");
      cJSON_AddStringToObject(cfg, "cheatShnAddressMode", shnmode[0] ? shnmode : "auto");
      cJSON_AddBoolToObject(cfg, "allowUnsafeMc4Apply", unsafe_mc4);
    }
  }

  {
    const cr_priv_status_t *ps = cr_priv_get();
    cJSON *priv = cJSON_AddObjectToObject(out, "privileges");
    if (priv) {
      cJSON_AddNumberToObject(priv, "uid", ps->uid);
      cJSON_AddNumberToObject(priv, "gid", ps->gid);
      cJSON_AddBoolToObject(priv, "uidOk", ps->uid_ok);
      cJSON_AddBoolToObject(priv, "sandboxEscaped", ps->sandbox_ok);
      cJSON_AddBoolToObject(priv, "sceAuthOk", ps->authid_ok);
      cJSON_AddBoolToObject(priv, "sceCapsOk", ps->caps_ok);
      cJSON_AddBoolToObject(priv, "kernelRwOk", ps->kernel_rw_ok);
      cJSON_AddBoolToObject(priv, "kernelMprotectOk", ps->kernel_mprotect_ok);
      cJSON_AddBoolToObject(priv, "canPatchGameMemory", ps->can_patch_game);
      cJSON *wa = cJSON_AddArrayToObject(priv, "warnings");
      if (wa) {
        for (int i = 0; i < ps->n_warnings; i++) {
          cJSON *ws = cJSON_CreateString(ps->warnings[i]);
          if (ws) cJSON_AddItemToArray(wa, ws);
        }
      }
    }
  }

  {
    last_game_exit_t lge;
    pthread_mutex_lock(&g_last_game_exit_lock);
    lge = g_last_game_exit;
    pthread_mutex_unlock(&g_last_game_exit_lock);
    if (lge.title_id[0]) {
      cJSON *lge_j = cJSON_AddObjectToObject(out, "lastGameExit");
      if (lge_j) {
        cJSON_AddStringToObject(lge_j, "titleId", lge.title_id);
        char appid_hex[16]; snprintf(appid_hex, sizeof(appid_hex), "0x%x", lge.app_id);
        cJSON_AddStringToObject(lge_j, "appId", appid_hex);
        cJSON_AddNumberToObject(lge_j, "pid", (double)lge.pid);
        cJSON_AddStringToObject(lge_j, "reason", lge.reason);
        cJSON_AddNumberToObject(lge_j, "tsMs", (double)lge.ts_ms);
        cJSON_AddNumberToObject(lge_j, "elapsedAfterLastCheatMs", (double)lge.elapsed_after_last_cheat_ms);
        cJSON_AddNumberToObject(lge_j, "lastModIndex", (double)lge.last_mod_index);
        cJSON_AddStringToObject(lge_j, "lastModName", lge.last_mod_name);
        cJSON_AddBoolToObject(lge_j, "suspected", lge.suspected);
      }
    }
  }

  {
    last_apply_rec_t lar;
    pthread_mutex_lock(&g_last_apply_lock);
    lar = g_last_apply_rec;
    pthread_mutex_unlock(&g_last_apply_lock);
    if (lar.ts_ms > 0) {
      cJSON *lar_j = cJSON_AddObjectToObject(out, "lastApply");
      if (lar_j) {
        cJSON_AddStringToObject(lar_j, "titleId", lar.title_id);
        cJSON_AddNumberToObject(lar_j, "modIndex", (double)lar.mod_index);
        cJSON_AddStringToObject(lar_j, "modName", lar.mod_name);
        cJSON_AddBoolToObject(lar_j, "effectiveOn", lar.effective_on);
        cJSON_AddBoolToObject(lar_j, "ok", lar.ok);
        cJSON_AddBoolToObject(lar_j, "hookCodecave", lar.hook_codecave);
        cJSON_AddNumberToObject(lar_j, "caveCount", (double)lar.cave_count);
        cJSON_AddNumberToObject(lar_j, "hookCount", (double)lar.hook_count);
        cJSON_AddNumberToObject(lar_j, "tsMs", (double)lar.ts_ms);
        cJSON *writes_j = cJSON_AddArrayToObject(lar_j, "writes");
        if (writes_j) {
          for (int _i = 0; _i < lar.write_count; _i++) {
            cJSON *w = cJSON_CreateObject();
            if (w) {
              char addr_hex[20]; snprintf(addr_hex, sizeof(addr_hex), "0x%lx", (long)lar.writes[_i].addr);
              cJSON_AddStringToObject(w, "addr", addr_hex);
              cJSON_AddNumberToObject(w, "len", (double)lar.writes[_i].len);
              cJSON_AddBoolToObject(w, "isCave", lar.writes[_i].is_cave);
              cJSON_AddItemToArray(writes_j, w);
            }
          }
        }
      }
    }
  }
  cJSON_AddBoolToObject(out, "scannerPausedDuringApply", g_cheat_applying ? 1 : 0);

  char *s = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  if (!s) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}"); return; }
  http_send_json(fd, 200, s);
  free(s);
}

/* ── /api/cheats/scan ───────────────────────────────────────────── */
void
handle_api_cheats_scan(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 || !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad titleId\"}");
    return;
  }
  cheat_file_search_t cfs;
  memset(&cfs, 0, sizeof(cfs));
  find_cheat_candidates(title_id, &cfs);

  cJSON *out = cJSON_CreateObject();
  if (!out) {
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}");
    return;
  }
  cJSON_AddBoolToObject(out, "ok", 1);
  cJSON_AddStringToObject(out, "titleId", title_id);
  cJSON_AddStringToObject(out, "dir", CHEATRUNNER_CHEATS_DIR);
  cJSON_AddNumberToObject(out, "candidateCount", cfs.candidate_count);
  cJSON_AddStringToObject(out, "preferredVersion", cfs.preferred_ver);
  cJSON_AddStringToObject(out, "selected", cfs.best_path[0] ? cfs.best_path : "");
  const char *sfmt = cfs.best_kind == 1 ? "json" : (cfs.best_kind == 2 ? "shn" : (cfs.best_kind == 3 ? "mc4" : "none"));
  cJSON_AddStringToObject(out, "selectedFormat", cfs.best_path[0] ? sfmt : "none");
  cJSON *cands = cJSON_AddArrayToObject(out, "candidates");
  if (cands) {
    for (int i = 0; i < cfs.candidate_count; i++) {
      cJSON *ce = cJSON_CreateObject();
      const char *p = cfs.candidates[i].path;
      const char *base = strrchr(p, '/');
      cJSON_AddStringToObject(ce, "file", base ? base + 1 : p);
      cJSON_AddStringToObject(ce, "path", p);
      cJSON_AddNumberToObject(ce, "score", cfs.candidates[i].score);
      const char *fmt = cfs.candidates[i].kind == 1 ? "json" : (cfs.candidates[i].kind == 2 ? "shn" : "mc4");
      cJSON_AddStringToObject(ce, "format", fmt);
      cJSON_AddBoolToObject(ce, "selected", strcmp(p, cfs.best_path) == 0);
      cJSON_AddItemToArray(cands, ce);
    }
  }
  char *s = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  if (!s) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}"); return; }
  http_send_json(fd, 200, s);
  free(s);
}

/* ── /api/cheats/mc4-debug ──────────────────────────────────────── */
void
handle_api_cheats_mc4_debug(int fd, const char *query) {
  char raw_title_id[32] = {0};
  char title_id[10] = {0};
  if (query_value(query, "titleId", raw_title_id, sizeof(raw_title_id)) != 0 || !title_id_normalize(raw_title_id, title_id)) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"bad titleId\"}");
    return;
  }
  char cheat_path[256] = {0};
  char err[128] = {0};
  int cheat_kind = 0;
  cJSON *root = load_cheat_json_root_for_title_ex(title_id, err, sizeof(err), cheat_path, sizeof(cheat_path), &cheat_kind);
  if (!root) {
    char body[256]; snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", err);
    http_send_json(fd, 404, body);
    return;
  }
  if (cheat_kind == 1) {
    cJSON_Delete(root);
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"cheat is JSON format; mc4-debug is for SHN/MC4 only\"}");
    return;
  }
  cJSON *out = cJSON_CreateObject();
  if (!out) {
    cJSON_Delete(root);
    http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}");
    return;
  }
  cJSON_AddBoolToObject(out, "ok", 1);
  cJSON_AddStringToObject(out, "titleId", title_id);
  cJSON_AddStringToObject(out, "cheatPath", cheat_path);
  cJSON_AddStringToObject(out, "format", cheat_kind == 2 ? "shn" : "mc4");

  if (cheat_kind == 3) {
    char *txt = NULL;
    if (read_file_text(cheat_path, &txt) == 0 && txt) {
      size_t xml_sz = 0;
      char *xml = mc4_decrypt_to_xml(txt, strlen(txt), &xml_sz);
      free(txt);
      if (xml) {
        char excerpt[2048];
        snprintf(excerpt, sizeof(excerpt), "%.*s", (int)(xml_sz < 1999u ? xml_sz : 1999u), xml);
        cJSON_AddStringToObject(out, "xmlExcerpt", excerpt);
        free(xml);
      } else {
        cJSON_AddStringToObject(out, "xmlExcerpt", "(decrypt failed)");
      }
    }
  }

  cJSON *mods = cJSON_GetObjectItem(root, "mods");
  if (!cJSON_IsArray(mods)) mods = cJSON_GetObjectItem(root, "patches");
  cJSON *mod_arr = cJSON_AddArrayToObject(out, "mods");
  if (cJSON_IsArray(mods) && mod_arr) {
    cJSON *mod = NULL;
    cJSON_ArrayForEach(mod, mods) {
      cJSON *mo = cJSON_CreateObject();
      if (!mo) break;
      cJSON *n = cJSON_GetObjectItem(mod, "name");
      cJSON_AddStringToObject(mo, "name", cJSON_IsString(n) ? n->valuestring : "");
      cJSON *mem = cJSON_GetObjectItem(mod, "memory");
      if (!cJSON_IsArray(mem)) mem = cJSON_GetObjectItem(mod, "patches");
      cJSON *patch_arr = cJSON_AddArrayToObject(mo, "patches");
      if (cJSON_IsArray(mem) && patch_arr) {
        cJSON *m = NULL;
        int pi = 0;
        cJSON_ArrayForEach(m, mem) {
          cJSON *pe = cJSON_CreateObject();
          if (!pe) break;
          cJSON_AddNumberToObject(pe, "index", pi++);
          cJSON *on_j  = cJSON_GetObjectItem(m, "on");
          cJSON *off_j = cJSON_GetObjectItem(m, "off");
          cJSON *exp_j = cJSON_GetObjectItem(m, "expected");
          cJSON *abs_j = cJSON_GetObjectItem(m, "absolute");
          cJSON_AddStringToObject(pe, "valueOn",  cJSON_IsString(on_j)  ? on_j->valuestring  : "");
          cJSON_AddStringToObject(pe, "valueOff", cJSON_IsString(off_j) ? off_j->valuestring : "");
          cJSON_AddStringToObject(pe, "expected", cJSON_IsString(exp_j) ? exp_j->valuestring : "");
          cJSON_AddBoolToObject(pe, "absoluteFlag", cJSON_IsTrue(abs_j));
          size_t elen = 0;
          if (cJSON_IsString(exp_j) && exp_j->valuestring && exp_j->valuestring[0]) {
            uint8_t tmp[128];
            parse_hex_bytes_checked(exp_j->valuestring, tmp, sizeof(tmp), &elen);
          }
          int off_reliable = (elen > 0);
          cJSON_AddBoolToObject(pe, "offReliable", off_reliable);
          cJSON_AddStringToObject(pe, "offNote",
            off_reliable ? "expected field present — off comparison is reliable" :
                           "no expected field — ValueOff may not match original bytes");
          cJSON_AddItemToArray(patch_arr, pe);
        }
      }
      cJSON_AddItemToArray(mod_arr, mo);
    }
  }
  cJSON_Delete(root);
  char *s = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  if (!s) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"oom\"}"); return; }
  http_send_json(fd, 200, s);
  free(s);
}

void
http_send_png_asset(int fd) {
  char header[256];
  size_t len = sizeof(g_cheatrunner_png);
  int n = snprintf(header, sizeof(header),
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: image/png\r\n"
    "Content-Length: %u\r\n"
    "Connection: close\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Cache-Control: public, max-age=86400\r\n"
    "\r\n",
    (unsigned int)len);
  if (n > 0) {
    if (socket_send_all(fd, header, (size_t)n) != 0) {
      return;
    }
    (void)socket_send_all(fd, g_cheatrunner_png, len);
  }
}

void
http_route(int fd, const char *method, const char *path, const char *query, const char *token_header,
           const char *client_ip, const char *body, size_t body_len) {
  if (!g_startup_ms) {
    struct timespec _ts; clock_gettime(CLOCK_MONOTONIC, &_ts);
    g_startup_ms = (long long)_ts.tv_sec * 1000LL + (long long)(_ts.tv_nsec / 1000000L);
  }

  if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
    http_send_json(fd, 405, "{\"ok\":false,\"error\":\"GET/POST only\"}");
    return;
  }
  if (cr_api_dashboard_handle(fd, method, path, query, body, body_len)) return;
  if (cr_api_games_handle(fd, method, path, query, body, body_len)) return;
  if (cr_api_cheats_handle(fd, method, path, query, body, body_len)) return;
  if (cr_api_sources_handle(fd, method, path, query, body, body_len)) return;
  if (cr_api_logs_handle(fd, method, path, query, body, body_len)) return;
  /* shutdown needs token_header + client_ip - handle directly */
  if (!strcmp(path, "/api/dev/shutdown")) {
    handle_api_dev_shutdown(fd, method, query, token_header, client_ip);
    return;
  }
  if (cr_api_dev_handle(fd, method, path, query, body, body_len)) return;
  http_send_json(fd, 404, "{\"ok\":false,\"error\":\"not found\"}");
}






