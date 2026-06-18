#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cr_api_internal.h"
#include "cr_config.h"
#include "cr_log.h"

void
handle_api_config(int fd) {
  char body[6144];
  pthread_mutex_lock(&g_cfg_lock);
  snprintf(body, sizeof(body),
           "{\"ok\":true,\"http_port\":%d,\"auto_load_cheat_menu\":%d,"
           "\"auto_download_missing_cheat\":%d,\"launch_kill_current\":%d,\"launch_kill_delay_ms\":%d,"
           "\"launch_wait_timeout_ms\":%d,\"cheat_engine\":%d,\"cheat_validate_original_bytes\":%d,"
           "\"cheat_restore_rx\":%d,\"cheat_restore_original_prot\":%d,"
           "\"allow_force_enable\":%d,"
           "\"cheat_state_after_launch_delay_ms\":%d,\"dev_reload_enabled\":%d,\"dev_shutdown_delay_ms\":%d,"
           "\"cheat_sources_enabled\":%d,\"cheat_remote_download_enabled\":%d,"
           "\"cheat_source_cache_ttl_seconds\":%d,\"cheat_remote_max_file_bytes\":%d,"
           "\"title_lookup_enabled\":%d,\"title_lookup_cache_enabled\":%d,\"title_lookup_timeout_ms\":%d,"
           "\"games_cache_ttl_ms\":%d,\"appdb_debug_names\":%d,\"log_level\":\"%s\","
           "\"theme\":\"%s\","
           "\"allow_legacy_mc4_without_expected\":%d,\"allow_legacy_shn_without_expected\":%d,"
           "\"cheat_mc4_unverified_fallback\":\"%s\",\"cheat_shn_unverified_fallback\":\"%s\","
           "\"cheat_min_stable_ms\":%d,\"cheat_apply_cooldown_ms\":%d,"
           "\"cheat_master_code_fixup\":%d,"
           "\"cheat_addr_cache_enabled\":%d,\"cheat_inter_mod_delay_ms\":%d,"
           "\"fan_min_c\":%d,\"fan_max_c\":%d,"
           "\"allow_unsafe_mc4_apply\":%d,\"allow_unsafe_shn_apply\":%d,"
           "\"cheat_log_candidates\":%d,\"cheat_mark_crash_suspect\":%d,"
           "\"cheat_apply_one_at_a_time\":%d,"
           "\"cheat_address_auto_detect\":%d}",
           g_cfg.http_port, g_cfg.auto_load_cheat_menu,
           g_cfg.auto_download_missing_cheat, g_cfg.launch_kill_current, g_cfg.launch_kill_delay_ms,
           g_cfg.launch_wait_timeout_ms, g_cfg.cheat_engine, g_cfg.cheat_validate_original_bytes,
           g_cfg.cheat_restore_rx, g_cfg.cheat_restore_original_prot,
           g_cfg.allow_force_enable, g_cfg.cheat_state_after_launch_delay_ms,
           g_cfg.dev_reload_enabled, g_cfg.dev_shutdown_delay_ms,
           g_cfg.cheat_sources_enabled, g_cfg.cheat_remote_download_enabled,
           g_cfg.cheat_source_cache_ttl_seconds, g_cfg.cheat_remote_max_file_bytes,
           g_cfg.title_lookup_enabled, g_cfg.title_lookup_cache_enabled, g_cfg.title_lookup_timeout_ms,
           g_cfg.games_cache_ttl_ms, g_cfg.appdb_debug_names, g_cfg.log_level,
           g_cfg.theme,
           g_cfg.allow_legacy_mc4_without_expected, g_cfg.allow_legacy_shn_without_expected,
           g_cfg.cheat_mc4_unverified_fallback, g_cfg.cheat_shn_unverified_fallback,
           g_cfg.cheat_min_stable_ms, g_cfg.cheat_apply_cooldown_ms,
           g_cfg.cheat_master_code_fixup,
           g_cfg.cheat_addr_cache_enabled, g_cfg.cheat_inter_mod_delay_ms,
           g_cfg.fan_min_c, g_cfg.fan_max_c,
           g_cfg.allow_unsafe_mc4_apply, g_cfg.allow_unsafe_shn_apply,
           g_cfg.cheat_log_candidates, g_cfg.cheat_mark_crash_suspect,
           g_cfg.cheat_apply_one_at_a_time,
           g_cfg.cheat_address_auto_detect);
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
  } else if (!strcmp(key, "cheat_restore_original_prot")) {
    g_cfg.cheat_restore_original_prot = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "allow_legacy_mc4_without_expected")) {
    g_cfg.allow_legacy_mc4_without_expected = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "allow_legacy_shn_without_expected")) {
    g_cfg.allow_legacy_shn_without_expected = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "cheat_mc4_unverified_fallback")) {
    if (!strcmp(value, "block") || !strcmp(value, "legacy") ||
        !strcmp(value, "absolute") || !strcmp(value, "relative")) {
      snprintf(g_cfg.cheat_mc4_unverified_fallback, sizeof(g_cfg.cheat_mc4_unverified_fallback), "%s", value);
    } else {
      pthread_mutex_unlock(&g_cfg_lock);
      http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid value; allowed: block legacy absolute relative\"}");
      return;
    }
  } else if (!strcmp(key, "cheat_shn_unverified_fallback")) {
    if (!strcmp(value, "block") || !strcmp(value, "legacy") ||
        !strcmp(value, "absolute") || !strcmp(value, "relative")) {
      snprintf(g_cfg.cheat_shn_unverified_fallback, sizeof(g_cfg.cheat_shn_unverified_fallback), "%s", value);
    } else {
      pthread_mutex_unlock(&g_cfg_lock);
      http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid value; allowed: block legacy absolute relative\"}");
      return;
    }
  } else if (!strcmp(key, "cheat_min_stable_ms")) {
    int v = atoi(value);
    g_cfg.cheat_min_stable_ms = (v >= 0 && v <= 60000) ? v : 8000;
  } else if (!strcmp(key, "cheat_apply_cooldown_ms")) {
    int v = atoi(value);
    g_cfg.cheat_apply_cooldown_ms = (v >= 0 && v <= 10000) ? v : 500;
  } else if (!strcmp(key, "cheat_master_code_fixup")) {
    g_cfg.cheat_master_code_fixup = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "cheat_addr_cache_enabled")) {
    g_cfg.cheat_addr_cache_enabled = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "cheat_inter_mod_delay_ms")) {
    int v = atoi(value);
    g_cfg.cheat_inter_mod_delay_ms = (v >= 0 && v <= 10000) ? v : 0;
  } else if (!strcmp(key, "cheat_address_auto_detect")) {
    g_cfg.cheat_address_auto_detect = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "cheat_validate_original_bytes")) {
    g_cfg.cheat_validate_original_bytes = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "allow_unsafe_mc4_apply")) {
    g_cfg.allow_unsafe_mc4_apply = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "allow_unsafe_shn_apply")) {
    g_cfg.allow_unsafe_shn_apply = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "cheat_log_candidates")) {
    g_cfg.cheat_log_candidates = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "cheat_apply_one_at_a_time")) {
    g_cfg.cheat_apply_one_at_a_time = atoi(value) ? 1 : 0;
  } else if (!strcmp(key, "fan_min_c")) {
    int v = atoi(value);
    g_cfg.fan_min_c = (v >= 10 && v <= 60) ? v : 30;
  } else if (!strcmp(key, "fan_max_c")) {
    int v = atoi(value);
    g_cfg.fan_max_c = (v >= 50 && v <= 100) ? v : 90;
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

/* Reset all settings to their defaults. The live HTTP port is preserved so the
 * connection the user is on doesn't move out from under them. */
void
handle_api_config_reset(int fd) {
  pthread_mutex_lock(&g_cfg_lock);
  int keep_port = g_cfg.http_port;
  config_set_defaults(&g_cfg);
  g_cfg.http_port = keep_port;
  cr_log_set_level(g_cfg.log_level);
  int rc = config_save_locked();
  pthread_mutex_unlock(&g_cfg_lock);
  cr_log("info", "config", "config reset to defaults (port preserved=%d)", keep_port);
  http_send_json(fd, rc == 0 ? 200 : 500, rc == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"save failed\"}");
}

/* Curated setting bundles: safe = validated defaults, compat = allow unsafe applies, debug = verbose logging. */
void
handle_api_config_preset(int fd, const char *query) {
  char name[32] = {0};
  if (query_value(query, "name", name, sizeof(name)) != 0) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"missing name\"}");
    return;
  }
  pthread_mutex_lock(&g_cfg_lock);
  if (!strcmp(name, "safe")) {
    g_cfg.cheat_validate_original_bytes = 1;
    g_cfg.allow_legacy_mc4_without_expected = 0;
    g_cfg.allow_legacy_shn_without_expected = 0;
    g_cfg.allow_unsafe_mc4_apply = 0;
    g_cfg.allow_unsafe_shn_apply = 0;
    g_cfg.cheat_address_auto_detect = 1;
    g_cfg.cheat_master_code_fixup = 1;
    g_cfg.cheat_log_candidates = 0;
    snprintf(g_cfg.cheat_mc4_unverified_fallback, sizeof(g_cfg.cheat_mc4_unverified_fallback), "%s", "relative");
    snprintf(g_cfg.cheat_shn_unverified_fallback, sizeof(g_cfg.cheat_shn_unverified_fallback), "%s", "relative");
    snprintf(g_cfg.log_level, sizeof(g_cfg.log_level), "%s", "info");
  } else if (!strcmp(name, "compat")) {
    g_cfg.cheat_validate_original_bytes = 1;
    g_cfg.allow_legacy_mc4_without_expected = 1;
    g_cfg.allow_legacy_shn_without_expected = 1;
    g_cfg.allow_unsafe_mc4_apply = 1;
    g_cfg.allow_unsafe_shn_apply = 1;
    g_cfg.cheat_address_auto_detect = 1;
    g_cfg.cheat_master_code_fixup = 1;
    snprintf(g_cfg.cheat_mc4_unverified_fallback, sizeof(g_cfg.cheat_mc4_unverified_fallback), "%s", "relative");
    snprintf(g_cfg.cheat_shn_unverified_fallback, sizeof(g_cfg.cheat_shn_unverified_fallback), "%s", "relative");
    snprintf(g_cfg.log_level, sizeof(g_cfg.log_level), "%s", "info");
  } else if (!strcmp(name, "debug")) {
    g_cfg.cheat_log_candidates = 1;
    snprintf(g_cfg.log_level, sizeof(g_cfg.log_level), "%s", "debug");
  } else {
    pthread_mutex_unlock(&g_cfg_lock);
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"unknown preset\"}");
    return;
  }
  cr_log_set_level(g_cfg.log_level);
  int rc = config_save_locked();
  pthread_mutex_unlock(&g_cfg_lock);
  cr_log("info", "config", "preset applied: %s", name);
  http_send_json(fd, rc == 0 ? 200 : 500, rc == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"save failed\"}");
}
