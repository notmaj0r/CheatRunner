#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ps5/kernel.h>

#include "ps5sdk_compat.h"
#include "priv_bootstrap.h"
#include "pt.h"
#include "third_party/cJSON.h"
#include "cr_log.h"
#include "cr_paths.h"
#include "cr_config.h"
#include "cr_activity.h"
#include "cr_notifications.h"
#include "cr_titles.h"
#include "cr_game_monitor.h"
#include "cr_memory.h"
#include "cr_cheat_formats.h"
#include "cr_cheats.h"

#define CHEAT_SEARCH_MAX_DEPTH 6

static pthread_mutex_t g_cheat_apply_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct applied_patch {
  intptr_t addr;
  uint8_t old_bytes[128];
  size_t len;
} applied_patch_t;
static int
cheat_candidate_score(const cheat_file_search_t *ctx, const char *path, const char *name, int kind) {
  int score = 0;
  if (kind == 1) {
    score += 300;
  } else if (kind == 2) {
    score += 200;
  } else if (kind == 3) {
    score += 100;
  }
  if (case_contains(path, "/cheats/") && case_contains(path, ctx->want)) {
    score += 30;
  }
  if (case_contains(path, "/cheats/json/")) {
    score += 20;
  } else if (case_contains(path, "/cheats/shn/")) {
    score += 10;
  }
  if (ctx->preferred_ver[0] && case_contains(name, ctx->preferred_ver)) {
    score += 40;
  }
  return score;
}

static int
cheat_path_component_ok(const char *name) {
  return name && name[0] && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && !strchr(name, '/') && !strchr(name, '\\');
}

static void
find_cheat_file_for_title_dir(const char *dir, int depth, cheat_file_search_t *ctx) {
  if (!dir || !ctx || depth > CHEAT_SEARCH_MAX_DEPTH) {
    return;
  }
  DIR *d = opendir(dir);
  if (!d) {
    return;
  }
  struct dirent *ent = NULL;
  while ((ent = readdir(d)) != NULL) {
    const char *name = ent->d_name;
    if (!cheat_path_component_ok(name)) {
      continue;
    }
    char path[512];
    int pn = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (pn <= 0 || (size_t)pn >= sizeof(path)) {
      continue;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      find_cheat_file_for_title_dir(path, depth + 1, ctx);
      continue;
    }
    if (!S_ISREG(st.st_mode)) {
      continue;
    }
    int kind = recognised_cheat_extension(name);
    if (!kind) {
      continue;
    }
    char id[10];
    if (!extract_title_id_prefix(name, id, sizeof(id)) && !find_title_id_in_string(path, id)) {
      continue;
    }
    if (strcmp(id, ctx->want) != 0) {
      continue;
    }
    int score = cheat_candidate_score(ctx, path, name, kind);
    const char *fmt_name = kind == 1 ? "json" : (kind == 2 ? "shn" : "mc4");
    /* store in candidates list (deduplicated) */
    int dup = 0;
    for (int ci = 0; ci < ctx->candidate_count; ci++) {
      if (strcmp(ctx->candidates[ci].path, path) == 0) { dup = 1; break; }
    }
    if (!dup && ctx->candidate_count < MAX_CHEAT_CANDIDATES) {
      snprintf(ctx->candidates[ctx->candidate_count].path, sizeof(ctx->candidates[ctx->candidate_count].path), "%s", path);
      ctx->candidates[ctx->candidate_count].kind = kind;
      ctx->candidates[ctx->candidate_count].score = score;
      ctx->candidate_count++;
    }
    if (score > ctx->best_score || (score == ctx->best_score && kind < ctx->best_kind)) {
      if (ctx->log_candidates) {
        cr_log("info", "cheats", "candidate title=%s rank=%d format=%s path=%s", ctx->want, score, fmt_name, path);
      }
      ctx->best_score = score;
      ctx->best_kind = kind;
      snprintf(ctx->best_path, sizeof(ctx->best_path), "%s", path);
    }
  }
  closedir(d);
}

/*
 * Parse /data/cheatrunner/cheats/<fmt_name>.txt for locally saved filenames.
 * Each line may be a bare filename, a relative path, or a full path.
 * URL lines (http/https) are skipped.
 */
static void
scan_local_txt_index(const char *fmt_name, int kind, cheat_file_search_t *ctx) {
  char idx_path[256];
  snprintf(idx_path, sizeof(idx_path), "%s/%s.txt", CHEATRUNNER_CHEATS_DIR, fmt_name);
  char *text = NULL;
  if (read_file_text(idx_path, &text) != 0 || !text) {
    return;
  }
  char *line = text;
  while (line && *line) {
    char *nl = strchr(line, '\n');
    if (nl) { *nl = '\0'; }
    while (*line == ' ' || *line == '\t' || *line == '\r') { line++; }
    if (!*line || *line == '#' ||
        strncmp(line, "http://", 7) == 0 || strncmp(line, "https://", 8) == 0) {
      line = nl ? nl + 1 : NULL;
      continue;
    }
    /* strip inline comment / URL suffix */
    char *eq = strchr(line, '=');
    if (eq) { *eq = '\0'; }
    /* build full path: if no slash, look under fmt subdir and cheats root */
    char full[512];
    struct stat st;
    if (strchr(line, '/') == NULL) {
      snprintf(full, sizeof(full), "%s/%s/%s", CHEATRUNNER_CHEATS_DIR, fmt_name, line);
      if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(full, sizeof(full), "%s/%s", CHEATRUNNER_CHEATS_DIR, line);
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
          line = nl ? nl + 1 : NULL;
          continue;
        }
      }
    } else {
      snprintf(full, sizeof(full), "%s", line);
      if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        line = nl ? nl + 1 : NULL;
        continue;
      }
    }
    const char *fname = strrchr(full, '/');
    fname = fname ? fname + 1 : full;
    if (!is_safe_filename(fname)) {
      line = nl ? nl + 1 : NULL;
      continue;
    }
    char id[10] = {0};
    if (!extract_title_id_prefix(fname, id, sizeof(id)) && !find_title_id_in_string(full, id)) {
      line = nl ? nl + 1 : NULL;
      continue;
    }
    if (strcmp(id, ctx->want) != 0) {
      line = nl ? nl + 1 : NULL;
      continue;
    }
    int score = cheat_candidate_score(ctx, full, fname, kind);
    /* deduplicate */
    int dup = 0;
    for (int ci = 0; ci < ctx->candidate_count; ci++) {
      if (strcmp(ctx->candidates[ci].path, full) == 0) { dup = 1; break; }
    }
    if (!dup && ctx->candidate_count < MAX_CHEAT_CANDIDATES) {
      snprintf(ctx->candidates[ctx->candidate_count].path, sizeof(ctx->candidates[ctx->candidate_count].path), "%s", full);
      ctx->candidates[ctx->candidate_count].kind = kind;
      ctx->candidates[ctx->candidate_count].score = score;
      ctx->candidate_count++;
    }
    if (score > ctx->best_score || (score == ctx->best_score && kind < ctx->best_kind)) {
      if (ctx->log_candidates) {
        cr_log("info", "cheats", "candidate (txt-idx) title=%s rank=%d format=%s path=%s", ctx->want, score, fmt_name, full);
      }
      ctx->best_score = score;
      ctx->best_kind = kind;
      snprintf(ctx->best_path, sizeof(ctx->best_path), "%s", full);
    }
    line = nl ? nl + 1 : NULL;
  }
  free(text);
}

/* Per-title throttle: only log candidates when the title changes. */
static char g_last_cand_title[16] = "";
static pthread_mutex_t g_last_cand_lock = PTHREAD_MUTEX_INITIALIZER;

int
find_cheat_file_for_title(const char *title_id, char *out, size_t out_size, int *kind_out) {
  char want[10];
  if (!title_id_normalize(title_id, want)) {
    return 0;
  }
  cheat_file_search_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.want_buf, sizeof(ctx.want_buf), "%s", want);
  ctx.want = ctx.want_buf;
  ctx.best_kind = 99;
  ctx.best_score = -1;

  /* throttle: only log when title changes and config allows it */
  pthread_mutex_lock(&g_last_cand_lock);
  int title_changed = (strcmp(g_last_cand_title, want) != 0);
  if (title_changed) {
    snprintf(g_last_cand_title, sizeof(g_last_cand_title), "%s", want);
  }
  pthread_mutex_unlock(&g_last_cand_lock);
  {
    int cfg_log = 0;
    pthread_mutex_lock(&g_cfg_lock);
    cfg_log = g_cfg.cheat_log_candidates;
    pthread_mutex_unlock(&g_cfg_lock);
    ctx.log_candidates = cfg_log && title_changed;
  }

  read_param_value_by_title_id(want, "contentVersion", ctx.preferred_ver, sizeof(ctx.preferred_ver));
  if (!ctx.preferred_ver[0]) {
    read_param_value_by_title_id(want, "appVersion", ctx.preferred_ver, sizeof(ctx.preferred_ver));
  }

  /* Recursive directory scan covers json/, shn/, mc4/ sub-dirs automatically */
  find_cheat_file_for_title_dir(CHEATRUNNER_CHEATS_DIR, 0, &ctx);
  /* Also scan txt index files for locally saved cheat entries */
  scan_local_txt_index("json", 1, &ctx);
  scan_local_txt_index("shn", 2, &ctx);
  scan_local_txt_index("mc4", 3, &ctx);

  if (!ctx.best_path[0]) {
    return 0;
  }
  const char *sel_fmt = ctx.best_kind == 1 ? "json" : (ctx.best_kind == 2 ? "shn" : "mc4");
  if (ctx.log_candidates) {
    cr_log("info", "cheats", "selected cheat title=%s rank=%d format=%s path=%s", want, ctx.best_score, sel_fmt,
           ctx.best_path);
  }
  int n = snprintf(out, out_size, "%s", ctx.best_path);
  if (n <= 0 || (size_t)n >= out_size) {
    return 0;
  }
  if (kind_out) {
    *kind_out = ctx.best_kind;
  }
  return 1;
}

/* Fills *ctx_out with all candidates + best selection without logging. */
int
find_cheat_candidates(const char *title_id, cheat_file_search_t *ctx_out) {
  char want[10];
  if (!title_id_normalize(title_id, want)) return 0;
  cheat_file_search_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.want_buf, sizeof(ctx.want_buf), "%s", want);
  ctx.want      = ctx.want_buf;
  ctx.best_kind = 99;
  ctx.best_score= -1;
  ctx.log_candidates = 0;
  read_param_value_by_title_id(want, "contentVersion", ctx.preferred_ver, sizeof(ctx.preferred_ver));
  if (!ctx.preferred_ver[0])
    read_param_value_by_title_id(want, "appVersion", ctx.preferred_ver, sizeof(ctx.preferred_ver));
  find_cheat_file_for_title_dir(CHEATRUNNER_CHEATS_DIR, 0, &ctx);
  scan_local_txt_index("json", 1, &ctx);
  scan_local_txt_index("shn", 2, &ctx);
  scan_local_txt_index("mc4", 3, &ctx);
  if (ctx_out) *ctx_out = ctx;
  return ctx.best_path[0] ? 1 : 0;
}


/* Live re-check: verify the expected app is still running and hasn't changed pid/appId. */
static int
cr_check_app_safe(const char *title_id, pid_t expected_pid, int expected_app_id,
                  char *err, size_t err_size) {
  int live_id = sceSystemServiceGetAppIdOfRunningBigApp();
  if (live_id <= 0) {
    snprintf(err, err_size, "game is no longer running (BigApp gone)");
    cr_log("warn", "cheats.guard",
           "unsafe app state title=%s expected_appId=0x%x live_appId=none state=not_running",
           title_id, expected_app_id);
    return -1;
  }
  if (live_id != expected_app_id) {
    snprintf(err, err_size, "game changed (was appId=0x%x, now 0x%x)", expected_app_id, live_id);
    cr_log("warn", "cheats.guard",
           "unsafe app state title=%s expected_appId=0x%x live_appId=0x%x state=swapped",
           title_id, expected_app_id, live_id);
    return -1;
  }
  pid_t live_pid = find_pid_for_app_id((uint32_t)live_id);
  if (live_pid != expected_pid) {
    snprintf(err, err_size, "game pid changed (expected %d, got %d)", (int)expected_pid, (int)live_pid);
    cr_log("warn", "cheats.guard",
           "unsafe app state title=%s appId=0x%x expected_pid=%d live_pid=%d state=pid_changed",
           title_id, expected_app_id, (int)expected_pid, (int)live_pid);
    return -1;
  }
  return 0;
}


mod_disabled_rec_t g_mods_disabled[MOD_DISABLED_MAX];
int g_mods_disabled_n = 0;
pthread_mutex_t g_mods_disabled_lock = PTHREAD_MUTEX_INITIALIZER;
static void mod_disabled_set(const char *tid, pid_t pid, int mod) {
  pthread_mutex_lock(&g_mods_disabled_lock);
  for (int i = 0; i < g_mods_disabled_n; i++) {
    if (g_mods_disabled[i].mod_index == mod && strcmp(g_mods_disabled[i].title_id, tid) == 0) {
      g_mods_disabled[i].pid = pid;
      pthread_mutex_unlock(&g_mods_disabled_lock); return;
    }
  }
  if (g_mods_disabled_n < MOD_DISABLED_MAX) {
    snprintf(g_mods_disabled[g_mods_disabled_n].title_id, sizeof(g_mods_disabled[0].title_id), "%s", tid);
    g_mods_disabled[g_mods_disabled_n].pid = pid;
    g_mods_disabled[g_mods_disabled_n].mod_index = mod;
    g_mods_disabled_n++;
  }
  pthread_mutex_unlock(&g_mods_disabled_lock);
}
static void mod_disabled_clear(const char *tid, int mod) {
  pthread_mutex_lock(&g_mods_disabled_lock);
  for (int i = 0; i < g_mods_disabled_n; i++) {
    if (g_mods_disabled[i].mod_index == mod && strcmp(g_mods_disabled[i].title_id, tid) == 0) {
      g_mods_disabled[i] = g_mods_disabled[--g_mods_disabled_n]; break;
    }
  }
  pthread_mutex_unlock(&g_mods_disabled_lock);
}
int mod_disabled_check(const char *tid, pid_t pid, int mod) {
  pthread_mutex_lock(&g_mods_disabled_lock);
  for (int i = 0; i < g_mods_disabled_n; i++) {
    if (g_mods_disabled[i].mod_index == mod && g_mods_disabled[i].pid == pid &&
        strcmp(g_mods_disabled[i].title_id, tid) == 0) {
      pthread_mutex_unlock(&g_mods_disabled_lock); return 1;
    }
  }
  pthread_mutex_unlock(&g_mods_disabled_lock); return 0;
}

crash_guard_state_t g_crash_guard = {0};
crash_suspect_rec_t g_crash_suspects[CRASH_SUSPECT_MAX];
int g_crash_suspects_n = 0;
pthread_mutex_t g_crash_guard_lock = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t g_last_game_exit_lock = PTHREAD_MUTEX_INITIALIZER;
last_game_exit_t g_last_game_exit = {0};
pthread_mutex_t g_last_apply_lock = PTHREAD_MUTEX_INITIALIZER;
last_apply_rec_t g_last_apply_rec = {0};
volatile int g_cheat_applying = 0;
volatile uint64_t g_last_apply_at_ms = 0;
volatile uint64_t g_post_apply_guard_until_ms = 0;

/* Returns a pointer to the first mod in 'mods' whose name contains "master code" or "mastercode"
 * (case-insensitive). Returns NULL if none found. */
static cJSON *
find_master_code_mod(cJSON *mods) {
  cJSON *m = NULL;
  cJSON_ArrayForEach(m, mods) {
    cJSON *name_j = cJSON_GetObjectItem(m, "name");
    if (!cJSON_IsString(name_j) || !name_j->valuestring) continue;
    if (strcasestr(name_j->valuestring, "master code") ||
        strcasestr(name_j->valuestring, "mastercode")) {
      return m;
    }
  }
  return NULL;
}

/* Returns 1 if the mod name indicates an MC-dependent cheat (contains "MC" but not "master code"). */
static int
mod_is_mc_dependent(const char *name) {
  if (!name) return 0;
  if (strcasestr(name, "master code") || strcasestr(name, "mastercode")) return 0;
  return strstr(name, "MC") != NULL;
}

/* Fallback MC address fixup: combines high bytes of master code offset with low byte of dep offset.
 * Returns the adjusted offset. */
static uint64_t
fixup_mc_dependent_addr(uint64_t mc_base_off, uint64_t dep_off) {
  return (mc_base_off & ~(uint64_t)0xff) | (dep_off & 0xff);
}

/* Returns the first entry's offset from a mod's memory array, or 0 if unavailable. */
static uint64_t
mc_mod_first_offset(cJSON *mc_mod) {
  cJSON *mem = cJSON_GetObjectItem(mc_mod, "memory");
  if (!cJSON_IsArray(mem) || cJSON_GetArraySize(mem) == 0) return 0;
  cJSON *first = cJSON_GetArrayItem(mem, 0);
  cJSON *off_j = cJSON_GetObjectItem(first, "offset");
  if (!cJSON_IsString(off_j)) return 0;
  uint64_t off = 0;
  parse_offset_hex_checked(off_j->valuestring, &off);
  return off;
}

void
fmt_hex16(const uint8_t *b, size_t len, char *buf, size_t buf_sz) {
  size_t n = len < 16 ? len : 16;
  size_t pos = 0;
  for (size_t i = 0; i < n && pos + 4 < buf_sz; i++) {
    pos += (size_t)snprintf(buf + pos, buf_sz - pos, "%02x ", b[i]);
  }
  if (len > 16 && pos + 4 < buf_sz) {
    snprintf(buf + pos, buf_sz - pos, "...");
  }
}

int
apply_cheat_json(const char *title_id, int mod_index, int turn_on, char *err, size_t err_size) {
  char path[256];
  int kind = 0;
  pid_t pid = -1;
  intptr_t base = 0;
  int app_id_live = 0;
  char running_title[16] = {0};
  char running_name[256] = {0};
  char run_norm[10] = {0};
  char cheat_norm[10] = {0};
  int attached = 0;
  int rc = -1;
  applied_patch_t applied[64];  /* pre-read: addr, old_bytes, len */
  int pre_n = 0;                /* entries pre-read */
  uint8_t  wi_data[64][128];    /* bytes to write per entry */
  int      wi_is_cave[64];      /* 1 if len >= 16 (code cave) */
  int      written_order[64];   /* pre_n indices in actual write order (for rollback) */
  int      write_ok_n = 0;      /* entries successfully written */

  if (!find_cheat_file_for_title(title_id, path, sizeof(path), &kind)) {
    snprintf(err, err_size, "no cheat file for %s", title_id);
    return -1;
  }
  cr_log("info", "cheats", "apply request title=%s mod=%d on=%d file=%s format=%s", title_id, mod_index, turn_on ? 1 : 0,
         path, kind == 1 ? "json" : (kind == 2 ? "shn" : "mc4"));
  pthread_mutex_lock(&g_cfg_lock);
  int engine_enabled    = g_cfg.cheat_engine;
  int validate_original = g_cfg.cheat_validate_original_bytes;
  int restore_rx        = g_cfg.cheat_restore_rx;
  int auto_detect       = g_cfg.cheat_address_auto_detect;
  int allow_unsafe_mc4  = g_cfg.allow_unsafe_mc4_apply;
  int one_at_a_time     = g_cfg.cheat_apply_one_at_a_time;
  int cooldown_ms       = g_cfg.cheat_apply_cooldown_ms;
  int min_stable_ms     = g_cfg.cheat_min_stable_ms;
  int mc_fixup          = g_cfg.cheat_master_code_fixup;
  int codecave_fb       = g_cfg.cheat_codecave_fallback;
  pthread_mutex_unlock(&g_cfg_lock);
  if (!engine_enabled) {
    snprintf(err, err_size, "cheat engine disabled");
    return -1;
  }
  if (get_running_game_ex(&pid, running_title, sizeof(running_title), &base, &app_id_live) != 0) {
    snprintf(err, err_size, "no game is currently running");
    return -1;
  }
  cr_log("info", "cheats", "running title=%s pid=%d base=0x%lx", running_title, (int)pid, (long)base);
  running_game_state_t run_state;
  if (read_running_state(&run_state) == 0) {
    snprintf(running_name, sizeof(running_name), "%s", run_state.title_name);
  }
  if (!running_name[0]) {
    read_param_value_by_title_id(running_title, "titleName", running_name, sizeof(running_name));
  }
  if (!running_name[0]) {
    snprintf(running_name, sizeof(running_name), "%s", running_title);
  }
  if (!title_id_normalize(running_title, run_norm) || !title_id_normalize(title_id, cheat_norm) ||
      strcmp(run_norm, cheat_norm) != 0) {
    snprintf(err, err_size, "running game (%s) does not match cheat target (%s)", running_title, title_id);
    return -1;
  }

  /* App stability check: game must have been running for at least cheat_min_stable_ms */
  if (min_stable_ms > 0 && run_state.started_at > 0) {
    uint64_t uptime_ms = 0;
    uint64_t now_sec = (uint64_t)time(NULL);
    if (now_sec > run_state.started_at)
      uptime_ms = (now_sec - run_state.started_at) * 1000;
    if (uptime_ms < (uint64_t)min_stable_ms) {
      snprintf(err, err_size, "app_not_stable");
      cr_log("warn", "cheats.guard",
             "app not stable title=%s appId=0x%x pid=%d uptimeMs=%llu min_stable_ms=%d",
             title_id, app_id_live, (int)pid,
             (unsigned long long)uptime_ms, min_stable_ms);
      return -1;
    }
  }

  char *txt = NULL;
  if (read_file_text(path, &txt) != 0 || !txt) {
    snprintf(err, err_size, "could not read cheat file");
    return -1;
  }

  char *json_txt = NULL;
  if (kind == 1) {
    json_txt = txt;
  } else if (kind == 2) {
    json_txt = shn_xml_to_json(txt, strlen(txt));
    free(txt);
  } else if (kind == 3) {
    char *xml = mc4_decrypt_to_xml(txt, strlen(txt), NULL);
    free(txt);
    if (!xml) {
      snprintf(err, err_size, "MC4 decrypt failed");
      return -1;
    }
    json_txt = shn_xml_to_json(xml, strlen(xml));
    free(xml);
  }
  if (!json_txt) {
    snprintf(err, err_size, "cheat format parse failed");
    return -1;
  }

  cJSON *root = cJSON_Parse(json_txt);
  free(json_txt);
  if (!root) {
    snprintf(err, err_size, "cheat JSON parse failed");
    return -1;
  }

  cJSON *mods = cJSON_GetObjectItem(root, "mods");
  if (!cJSON_IsArray(mods) || mod_index < 0 || mod_index >= cJSON_GetArraySize(mods)) {
    cJSON_Delete(root);
    snprintf(err, err_size, "cheat index out of range");
    return -1;
  }
  cJSON *mod = cJSON_GetArrayItem(mods, mod_index);
  cJSON *mname_j = cJSON_GetObjectItem(mod, "module_name");
  const char *module_name = (cJSON_IsString(mname_j) && mname_j->valuestring) ? mname_j->valuestring : "";
  cJSON *memory = cJSON_GetObjectItem(mod, "memory");
  if (!cJSON_IsArray(memory)) {
    memory = cJSON_GetObjectItem(mod, "patches");
  }
  cJSON *type_j = cJSON_GetObjectItem(mod, "type");
  const char *type = (cJSON_IsString(type_j) && type_j->valuestring) ? type_j->valuestring : "checkbox";
  int effective_on = !strcasecmp(type, "button") ? 1 : (turn_on ? 1 : 0);
  if (!cJSON_IsArray(memory) || cJSON_GetArraySize(memory) == 0) {
    cJSON_Delete(root);
    snprintf(err, err_size, "mod has no memory entries");
    return -1;
  }

  if (!cr_priv_can_patch_game_memory()) {
    cJSON_Delete(root);
    snprintf(err, err_size,
             "privilege bootstrap incomplete; cannot patch game memory (check /api/dev/privileges)");
    cr_log("error", "cheats", "apply blocked: canPatchGameMemory=false title=%s mod=%d", title_id, mod_index);
    return -1;
  }

  /* One-at-a-time guard (Task 3) */
  if (one_at_a_time) {
    if (pthread_mutex_trylock(&g_cheat_apply_lock) != 0) {
      cJSON_Delete(root);
      snprintf(err, err_size,
               "Another cheat is being applied. Wait for the safety window to finish.");
      cr_log("warn", "cheats.guard", "apply_in_progress rejected title=%s mod=%d", title_id, mod_index);
      return -2;
    }
  } else {
    pthread_mutex_lock(&g_cheat_apply_lock);
  }

  /* Cooldown check */
  if (cooldown_ms > 0 && g_last_apply_at_ms > 0) {
    uint64_t _elapsed_cd = now_ms() - g_last_apply_at_ms;
    if (_elapsed_cd < (uint64_t)cooldown_ms) {
      pthread_mutex_unlock(&g_cheat_apply_lock);
      cJSON_Delete(root);
      snprintf(err, err_size, "Cooldown active: wait %llums before next toggle.",
               (unsigned long long)((uint64_t)cooldown_ms - _elapsed_cd));
      cr_log("warn", "cheats.guard",
             "cooldown active title=%s mod=%d remaining_ms=%llu",
             title_id, mod_index,
             (unsigned long long)((uint64_t)cooldown_ms - _elapsed_cd));
      return -2;
    }
  }

  /* App lifecycle guard — live re-check before ptrace (Task 1) */
  {
    char _ge[128] = {0};
    if (cr_check_app_safe(title_id, pid, app_id_live, _ge, sizeof(_ge)) != 0) {
      pthread_mutex_unlock(&g_cheat_apply_lock);
      cJSON_Delete(root);
      snprintf(err, err_size, "Game is not in a safe running state: %s", _ge);
      return -1;
    }
  }

  /* Mark apply in progress — suppresses scanner refresh during ptrace (Task 2) */
  g_cheat_applying = 1;
  cr_log("info", "cheats.guard",
         "pausing scanners during apply title=%s mod=%d pid=%d appId=0x%x",
         title_id, mod_index, (int)pid, app_id_live);

  if (pt_attach(pid) < 0) {
    g_cheat_applying = 0;
    pthread_mutex_unlock(&g_cheat_apply_lock);
    cJSON_Delete(root);
    snprintf(err, err_size, "pt_attach failed (%d)", errno);
    return -1;
  }
  attached = 1;

  /* Resolve per-mod base (may differ from eboot base if module_name is set) */
  intptr_t mod_base = base;
  if (module_name[0]) {
    if (resolve_module_base(pid, module_name, base, &mod_base) != 0) {
      pt_detach(pid, 0);
      g_cheat_applying = 0;
      pthread_mutex_unlock(&g_cheat_apply_lock);
      cJSON_Delete(root);
      snprintf(err, err_size, "module_not_loaded: '%s' is not loaded in pid=%d", module_name, (int)pid);
      return -1;
    }
    cr_log("info", "cheats", "module_base '%s'=0x%lx title=%s mod=%d",
           module_name, (long)mod_base, title_id, mod_index);
  }

  /* PS2 emulation detection — all addresses are absolute when PS2 emu is running */
  int is_ps2 = process_is_ps2_emu(pid);
  if (is_ps2) {
    cr_log("info", "cheats", "ps2_emu detected pid=%d all addrs treated as absolute", (int)pid);
  }

  /* Master Code fixup setup (only when enabled in config) */
  int do_mc_fixup = 0;
  uint64_t mc_base_off = 0;
  if (mc_fixup) {
    cJSON *name_j_mc = cJSON_GetObjectItem(mod, "name");
    const char *mstr = (cJSON_IsString(name_j_mc) && name_j_mc->valuestring) ? name_j_mc->valuestring : "";
    if (mod_is_mc_dependent(mstr)) {
      cJSON *mc_mod = find_master_code_mod(mods);
      if (mc_mod) {
        mc_base_off = mc_mod_first_offset(mc_mod);
        if (mc_base_off != 0) {
          do_mc_fixup = 1;
          cr_log("info", "cheats", "mc_fixup active mc_base_off=0x%llx dep='%s'",
                 (unsigned long long)mc_base_off, mstr);
        }
      }
    }
  }

  /* ---- Pre-read pass: parse, validate, backup old bytes, classify (Task 4) ---- */
  rc = 0;
  {
    cJSON *m = NULL;
    cJSON_ArrayForEach(m, memory) {
      if (pre_n >= 64) {
        rc = -1;
        snprintf(err, err_size, "entry[%d] too many patches (max 64)", pre_n);
        break;
      }
      cJSON *off_j      = cJSON_GetObjectItem(m, "offset");
      cJSON *on_j       = cJSON_GetObjectItem(m, "on");
      cJSON *off_j2     = cJSON_GetObjectItem(m, "off");
      cJSON *expected_j = cJSON_GetObjectItem(m, "expected");
      cJSON *abs_j      = cJSON_GetObjectItem(m, "absolute");
      if (!cJSON_IsString(off_j) || !cJSON_IsString(on_j) || !cJSON_IsString(off_j2)) {
        rc = -1;
        snprintf(err, err_size, "entry[%d] missing offset/on/off", pre_n);
        break;
      }
      uint8_t on_bytes[128], off_bytes[128], exp_bytes[128], cur_bytes[128];
      size_t on_len = 0, off_len = 0, exp_len = 0;
      if (parse_hex_bytes_checked(on_j->valuestring, on_bytes, sizeof(on_bytes), &on_len) != 0 ||
          parse_hex_bytes_checked(off_j2->valuestring, off_bytes, sizeof(off_bytes), &off_len) != 0) {
        rc = -1;
        snprintf(err, err_size, "entry[%d] hex parse failed or empty", pre_n);
        break;
      }
      if (on_len != off_len) {
        rc = -1;
        snprintf(err, err_size, "entry[%d] on/off size mismatch (%zu vs %zu)", pre_n, on_len, off_len);
        break;
      }
      if (cJSON_IsString(expected_j) && expected_j->valuestring && expected_j->valuestring[0]) {
        if (parse_hex_bytes_checked(expected_j->valuestring, exp_bytes, sizeof(exp_bytes), &exp_len) != 0 ||
            exp_len != on_len) {
          rc = -1;
          snprintf(err, err_size, "entry[%d] expected parse failed/size mismatch", pre_n);
          break;
        }
      }
      uint64_t off_u = 0;
      if (parse_offset_hex_checked(off_j->valuestring, &off_u) != 0) {
        rc = -1;
        snprintf(err, err_size, "entry[%d] invalid offset '%s'", pre_n, off_j->valuestring);
        break;
      }
      intptr_t addr = 0;
      {
        int af = 0, inj = 0, adet = auto_detect;
        get_cheat_addr_flags(kind, cJSON_IsTrue(abs_j) ? 1 : 0, auto_detect, &af, &inj, &adet);
        /* PS2 emu: all addresses are absolute */
        if (is_ps2) { af = 1; inj = 0; adet = 0; }
        /* MC fixup: adjust dependent offset relative to master code location */
        if (do_mc_fixup && mc_base_off != 0) {
          off_u = fixup_mc_dependent_addr(mc_base_off, off_u);
        }
        const uint8_t *expect_cmp = exp_len > 0 ? exp_bytes : off_bytes;
        addr = cheat_resolve_write_addr(pid, mod_base, off_u, af, inj, adet, on_bytes, on_len, expect_cmp);
      }
      const uint8_t *new_data = effective_on ? on_bytes : off_bytes;
      size_t wlen = on_len;
      if (read_process_memory(pid, addr, cur_bytes, wlen) != 0) {
        rc = -1;
        snprintf(err, err_size, "entry[%d] pre-read failed at 0x%lx", pre_n, (long)addr);
        break;
      }
      if (validate_original) {
        if (effective_on) {
          int off_reliable = (kind == 1) || (exp_len > 0);
          if (!off_reliable) {
            if (!allow_unsafe_mc4) {
              rc = -1;
              snprintf(err, err_size,
                       "entry[%d] MC4/SHN no reliable baseline at 0x%lx; set allow_unsafe_mc4_apply=1 to proceed",
                       pre_n, (long)addr);
              break;
            }
            cr_log("info", "cheats", "MC4/SHN trainer mode: applying without baseline at 0x%lx", (long)addr);
          } else {
            const uint8_t *must = (exp_len > 0) ? exp_bytes : off_bytes;
            if (memcmp(cur_bytes, must, wlen) != 0) {
              rc = -1;
              snprintf(err, err_size, "entry[%d] bytes mismatch before ON at 0x%lx", pre_n, (long)addr);
              break;
            }
          }
        } else {
          if (memcmp(cur_bytes, on_bytes, wlen) != 0) {
            rc = -1;
            snprintf(err, err_size, "entry[%d] bytes mismatch before OFF at 0x%lx", pre_n, (long)addr);
            break;
          }
        }
      }
      applied[pre_n].addr = addr;
      applied[pre_n].len  = wlen;
      memcpy(applied[pre_n].old_bytes, cur_bytes, wlen);
      memcpy(wi_data[pre_n], new_data, wlen);
      wi_is_cave[pre_n] = (wlen >= 16) ? 1 : 0;
      pre_n++;
    }
  }

  /* ---- Write pass: cave-first (enable) / hook-first (disable) (Task 4) ---- */
  if (rc == 0 && pre_n > 0) {
    int cave_count = 0, hook_count = 0;
    for (int _i = 0; _i < pre_n; _i++) {
      if (wi_is_cave[_i]) cave_count++; else hook_count++;
    }
    int hook_codecave = (cave_count > 0 && hook_count > 0);
    if (hook_codecave) {
      cr_log("info", "cheats.apply",
             "mod=%d pattern=hook_codecave caveWrites=%d hookWrites=%d",
             mod_index, cave_count, hook_count);
      cr_log("info", "cheats.apply", effective_on ?
             "enable order: caves first, hooks last" :
             "disable order: hooks first, caves last");
    }

    for (int pass = 0; pass < 2 && rc == 0; pass++) {
      if (!hook_codecave && pass == 1) break;
      /* want_cave: for enable pass0=caves, pass1=hooks; for disable pass0=hooks, pass1=caves */
      int want_cave = effective_on ? (pass == 0) : (pass == 1);
      for (int i = 0; i < pre_n && rc == 0; i++) {
        if (hook_codecave && (wi_is_cave[i] ? 1 : 0) != want_cave) continue;
        intptr_t addr = applied[i].addr;
        size_t   wlen = applied[i].len;
        const uint8_t *data = wi_data[i];
        intptr_t _pg = (intptr_t)ROUND_PG_DOWN((uintptr_t)addr);
        size_t   _sp = (size_t)(ROUND_PG_UP((uintptr_t)addr + wlen) - (uintptr_t)_pg);
        cr_log("info", "cheats.mem",
               "begin title=%s mod=%d write=%d addr=0x%lx len=%zu page=0x%lx span=0x%zx type=%s",
               title_id, mod_index, write_ok_n, (long)addr, wlen, (long)_pg, _sp,
               wi_is_cave[i] ? "cave" : "hook");
        int wrc = write_process_memory(pid, addr, data, wlen);
        int used_cave = 0;
        /* Code-cave fallback: if write was silently dropped (-3) and config allows it */
        if (wrc == -3 && codecave_fb) {
          cr_log("info", "cheats.mem", "write_dropped trying codecave addr=0x%lx len=%zu", (long)addr, wlen);
          wrc = write_via_codecave(pid, addr, data, wlen);
          if (wrc == 0) {
            used_cave = 1;
          }
        }
        cr_log("info", "cheats.mem", "write_rc=%d cave=%d addr=0x%lx", wrc, used_cave, (long)addr);
        if (wrc != 0) {
          rc = -1;
          snprintf(err, err_size, "entry[%d] write failed at 0x%lx (rc=%d)", i, (long)addr, wrc);
          break;
        }
        if (!used_cave) {
          /* External verify — page still RWX after write_process_memory success */
          uint8_t cur_v[128];
          int rdrc = read_process_memory(pid, addr, cur_v, wlen);
          if (rdrc != 0) {
            rc = -1;
            snprintf(err, err_size, "entry[%d] readback failed at 0x%lx (rc=%d)", i, (long)addr, rdrc);
            break;
          }
          if (memcmp(cur_v, data, wlen) != 0) {
            char exp_h[52] = {0}, got_h[52] = {0};
            fmt_hex16(data, wlen, exp_h, sizeof(exp_h));
            fmt_hex16(cur_v, wlen, got_h, sizeof(got_h));
            cr_log("error", "cheats.mem",
                   "ext_verify_fail title=%s mod=%d write=%d addr=0x%lx exp=[%s] got=[%s]",
                   title_id, mod_index, write_ok_n, (long)addr, exp_h, got_h);
            rc = -1;
            snprintf(err, err_size,
                     "entry[%d] verify failed at 0x%lx — bytes not observed after write; see cheats.mem logs",
                     i, (long)addr);
            break;
          }
          /* PS5 is x86-64: i/d caches coherent; mprotect RX provides the serialization barrier. */
          cr_log("info", "cheats.mem",
                 "icache_sync addr=0x%lx len=%zu x86_coherent mprotect_barrier",
                 (long)addr, wlen);
          if (restore_rx) {
            int rrc = kernel_mprotect(pid, _pg, _sp, PROT_READ | PROT_EXEC);
            cr_log("info", "cheats.mem", "restore_rx page=0x%lx span=0x%zx rc=%d", (long)_pg, _sp, rrc);
          }
        }
        cr_log("info", "cheats", "write[%d] addr=0x%lx len=%zu type=%s ok",
               write_ok_n, (long)addr, wlen, wi_is_cave[i] ? "cave" : "hook");
        written_order[write_ok_n++] = i;
      }
    }
  }

  /* ---- Rollback on failure ---- */
  if (rc != 0 && write_ok_n > 0) {
    cr_log("info", "cheats.rollback", "begin title=%s mod=%d writes_to_undo=%d",
           title_id, mod_index, write_ok_n);
    for (int i = write_ok_n - 1; i >= 0; i--) {
      int wi = written_order[i];
      int rrc = write_process_memory(pid, applied[wi].addr, applied[wi].old_bytes, applied[wi].len);
      if (rrc == 0) {
        uint8_t chk[128];
        int vok = (applied[wi].len <= sizeof(chk) &&
                   read_process_memory(pid, applied[wi].addr, chk, applied[wi].len) == 0 &&
                   memcmp(chk, applied[wi].old_bytes, applied[wi].len) == 0);
        cr_log("info", "cheats.rollback", "write[%d] addr=0x%lx len=%zu rc=0 verify_old=%s",
               i, (long)applied[wi].addr, applied[wi].len, vok ? "ok" : "fail");
      } else {
        cr_log("warn", "cheats.rollback", "write[%d] addr=0x%lx len=%zu rc=%d (write failed)",
               i, (long)applied[wi].addr, applied[wi].len, rrc);
      }
    }
    cr_log("warn", "cheats", "rollback performed for %s mod=%d (%d patches)",
           title_id, mod_index, write_ok_n);
  } else if (rc == 0) {
    cJSON *name_j = cJSON_GetObjectItem(mod, "name");
    const char *mod_name = cJSON_IsString(name_j) && name_j->valuestring ? name_j->valuestring : "mod";
    pthread_mutex_lock(&g_activity_lock);
    snprintf(g_activity_last_cheat_used, sizeof(g_activity_last_cheat_used), "%s:%s", title_id, mod_name);
    int aidx = activity_find_title_index_locked(title_id);
    if (aidx >= 0) {
      snprintf(g_activity_titles[aidx].last_cheat, sizeof(g_activity_titles[aidx].last_cheat), "%s", mod_name);
    }
    pthread_mutex_unlock(&g_activity_lock);
    activity_save();
    notification_add(effective_on ? "cheat_applied" : "cheat_disabled", "Cheat %s: %s #%d",
                     effective_on ? "applied" : "disabled", title_id, mod_index);
    if (effective_on) {
      mod_disabled_clear(title_id, mod_index);
    } else {
      mod_disabled_set(title_id, pid, mod_index);
    }
    cr_log("info", "cheats", "applied %s mod=%d state=%d appId=0x%x",
           title_id, mod_index, effective_on, app_id_live);
    if (effective_on) {
      int watch_ms = 0, mark_crash = 0;
      pthread_mutex_lock(&g_cfg_lock);
      watch_ms = g_cfg.cheat_post_apply_watch_ms;
      mark_crash = g_cfg.cheat_mark_crash_suspect;
      pthread_mutex_unlock(&g_cfg_lock);
      if (mark_crash && watch_ms > 0) {
        int _hcc = 0, _hch = 0;
        for (int _i = 0; _i < pre_n; _i++) { if (wi_is_cave[_i]) _hcc++; else _hch++; }
        pthread_mutex_lock(&g_crash_guard_lock);
        snprintf(g_crash_guard.title_id, sizeof(g_crash_guard.title_id), "%s", title_id);
        g_crash_guard.mod_index = mod_index;
        snprintf(g_crash_guard.mod_name, sizeof(g_crash_guard.mod_name), "%s", mod_name);
        g_crash_guard.pid = pid;
        g_crash_guard.enabled_at_ms = now_ms();
        pthread_mutex_unlock(&g_crash_guard_lock);
        g_post_apply_guard_until_ms = now_ms() + (uint64_t)watch_ms;
        cr_log("info", "cheats.guard",
               "monitoring title=%s pid=%d appId=0x%x mod=%d name=\"%s\" for %dms hc=%d",
               title_id, (int)pid, app_id_live, mod_index, mod_name, watch_ms,
               (_hcc > 0 && _hch > 0) ? 1 : 0);
      }
    } else {
      pthread_mutex_lock(&g_crash_guard_lock);
      if (strcmp(g_crash_guard.title_id, title_id) == 0 && g_crash_guard.mod_index == mod_index) {
        memset(&g_crash_guard, 0, sizeof(g_crash_guard));
        g_post_apply_guard_until_ms = 0;
      }
      pthread_mutex_unlock(&g_crash_guard_lock);
    }
  }

  /* Record last apply for debug/diag (Task 7) */
  {
    cJSON *name_j2 = cJSON_GetObjectItem(mod, "name");
    const char *mn = cJSON_IsString(name_j2) && name_j2->valuestring ? name_j2->valuestring : "mod";
    int hcc = 0, hch = 0;
    for (int _i = 0; _i < pre_n; _i++) { if (wi_is_cave[_i]) hcc++; else hch++; }
    pthread_mutex_lock(&g_last_apply_lock);
    memset(&g_last_apply_rec, 0, sizeof(g_last_apply_rec));
    snprintf(g_last_apply_rec.title_id,  sizeof(g_last_apply_rec.title_id),  "%s", title_id);
    snprintf(g_last_apply_rec.mod_name,  sizeof(g_last_apply_rec.mod_name),  "%s", mn);
    g_last_apply_rec.mod_index    = mod_index;
    g_last_apply_rec.effective_on = effective_on;
    g_last_apply_rec.ok           = (rc == 0) ? 1 : 0;
    g_last_apply_rec.hook_codecave= (hcc > 0 && hch > 0) ? 1 : 0;
    g_last_apply_rec.cave_count   = hcc;
    g_last_apply_rec.hook_count   = hch;
    g_last_apply_rec.ts_ms        = now_ms();
    int wc = write_ok_n < LAST_APPLY_WRITES_MAX ? write_ok_n : LAST_APPLY_WRITES_MAX;
    for (int _i = 0; _i < wc; _i++) {
      int wi = written_order[_i];
      g_last_apply_rec.writes[_i].addr    = applied[wi].addr;
      g_last_apply_rec.writes[_i].len     = applied[wi].len;
      g_last_apply_rec.writes[_i].is_cave = wi_is_cave[wi];
    }
    g_last_apply_rec.write_count = wc;
    pthread_mutex_unlock(&g_last_apply_lock);
  }

  if (attached) {
    pt_detach(pid, 0);
  }

  g_cheat_applying = 0;
  g_last_apply_at_ms = now_ms();
  cr_log("info", "cheats.guard", "resuming scanners after apply title=%s mod=%d rc=%d",
         title_id, mod_index, rc);

  pthread_mutex_unlock(&g_cheat_apply_lock);
  cJSON_Delete(root);
  return rc;
}




