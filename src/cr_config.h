#ifndef CR_CONFIG_H
#define CR_CONFIG_H

#include <pthread.h>
#include <stddef.h>
#include "cr_paths.h"

#define CHEATRUNNER_HTTP_PORT 9999

typedef struct cheatrunner_config {
  int http_port;
  int auto_load_cheat_menu;
  int auto_download_missing_cheat;
  int launch_kill_current;
  int launch_kill_delay_ms;
  int launch_wait_timeout_ms;
  int cheat_engine;
  int cheat_validate_original_bytes;
  int cheat_restore_rx;          /* legacy/deprecated: use cheat_restore_original_prot instead */
  int cheat_restore_original_prot;
  int cheat_index_cache_ttl_sec;
  int allow_force_enable;
  int cheat_state_after_launch_delay_ms;
  int launch_post_timeout_grace_ms;
  int cheat_address_auto_detect;
  int dev_reload_enabled;
  int dev_shutdown_delay_ms;
  char theme[16];
  char launch_user_id[32];
  char cheat_mc4_address_mode[16];
  char cheat_shn_address_mode[16];
  int allow_unsafe_mc4_apply;
  int allow_unsafe_shn_apply;
  int allow_legacy_mc4_without_expected; /* permit MC4 write when no reliable expected bytes (manual apply) */
  int allow_legacy_shn_without_expected;
  char cheat_mc4_unverified_fallback[16]; /* block|legacy|absolute|relative */
  char cheat_shn_unverified_fallback[16];
  int cheat_log_candidates;
  int launch_quick_verify_ms;
  int cheat_post_apply_watch_ms;
  int cheat_mark_crash_suspect;
  int cheat_apply_one_at_a_time;
  int cheat_apply_cooldown_ms;
  int cheat_post_apply_poll_ms;
  int cheat_min_stable_ms;
  int cheat_sources_enabled;
  int cheat_remote_download_enabled;
  int cheat_source_cache_ttl_seconds;
  int cheat_remote_max_file_bytes;
  int title_lookup_enabled;
  int title_lookup_cache_enabled;
  int title_lookup_timeout_ms;
  int games_cache_ttl_ms;
  int appdb_debug_names;
  char log_level[16];
  int cheat_master_code_fixup;
  int cheat_codecave_fallback;
} cheatrunner_config_t;

extern pthread_mutex_t      g_cfg_lock;
extern cheatrunner_config_t g_cfg;

void config_set_defaults(cheatrunner_config_t *cfg);
int  config_load(void);
int  config_save_locked(void);

void cfg_get_cheat_remote_opts(int *sources_enabled, int *download_enabled, int *ttl_sec, int *max_bytes);

#endif /* CR_CONFIG_H */
