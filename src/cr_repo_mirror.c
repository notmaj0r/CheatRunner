#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "third_party/cJSON.h"
#include "cr_log.h"
#include "cr_paths.h"
#include "http_client.h"
#include "cr_repo_mirror.h"

#define AGENT         "CheatRunner/0.1"
#define TREE_LIST_MAX (16 * 1024 * 1024)
#define DIR_LIST_MAX  (4 * 1024 * 1024)
#define FILE_MAX      (2 * 1024 * 1024)

typedef struct {
  const char *id;
  const char *owner;
  const char *repo;
  const char *branch;
  const char *cheat_root;
} repo_def_t;

typedef struct {
  char path[512];
  char basename[128];
  char raw_url[1024];
  const char *dest_dir;
  int kind;
} mirror_entry_t;

typedef struct {
  mirror_entry_t *items;
  int count;
  int cap;
} mirror_entry_list_t;

static const repo_def_t REPOS[] = {
  { "hencollection", "TeeKay87", "HEN-Cheats-Collection", "master", "cheats" },
  { "ps5cheats",     "etaHEN",   "PS5_Cheats",            "main",   ""       },
  { "goldhen",       "GoldHEN",  "GoldHEN_Cheat_Repository", "main", ""      },
};

#define REPOS_COUNT ((int)(sizeof(REPOS) / sizeof(REPOS[0])))

repo_mirror_progress_t g_repo_mirror = {
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .state = REPO_MIRROR_IDLE,
};

typedef struct {
  int repo_idx;
  int overwrite;
  int all_sources;
} mirror_args_t;

static const repo_def_t *
find_repo_by_name(const char *source, int *out_idx) {
  if (!source) {
    return NULL;
  }
  for (int i = 0; i < REPOS_COUNT; i++) {
    if (strcmp(REPOS[i].id, source) == 0) {
      if (out_idx) {
        *out_idx = i;
      }
      return &REPOS[i];
    }
  }
  return NULL;
}

static int
entry_kind_from_name(const char *name) {
  if (!name || !name[0]) {
    return 0;
  }
  if (ends_with_case(name, ".json")) return 1;
  if (ends_with_case(name, ".shn")) return 2;
  if (ends_with_case(name, ".mc4")) return 3;
  return 0;
}

static const char *
entry_dest_dir_from_kind(int kind) {
  if (kind == 1) return CHEATRUNNER_CHEATS_JSON_DIR;
  if (kind == 2) return CHEATRUNNER_CHEATS_SHN_DIR;
  if (kind == 3) return CHEATRUNNER_CHEATS_MC4_DIR;
  return NULL;
}

static void
entry_list_free(mirror_entry_list_t *list) {
  if (!list) {
    return;
  }
  free(list->items);
  list->items = NULL;
  list->count = 0;
  list->cap = 0;
}

static int
entry_list_add(mirror_entry_list_t *list, const mirror_entry_t *in) {
  if (!list || !in) {
    return -1;
  }
  if (list->count == list->cap) {
    int new_cap = list->cap ? (list->cap * 2) : 256;
    mirror_entry_t *np = realloc(list->items, sizeof(*np) * (size_t)new_cap);
    if (!np) {
      return -1;
    }
    list->items = np;
    list->cap = new_cap;
  }
  list->items[list->count++] = *in;
  return 0;
}

static void
progress_set_current(const char *s) {
  pthread_mutex_lock(&g_repo_mirror.lock);
  snprintf(g_repo_mirror.current, sizeof(g_repo_mirror.current), "%s", s ? s : "");
  pthread_mutex_unlock(&g_repo_mirror.lock);
}

static void
progress_set_mode(const char *mode) {
  pthread_mutex_lock(&g_repo_mirror.lock);
  snprintf(g_repo_mirror.mode, sizeof(g_repo_mirror.mode), "%s", mode ? mode : "");
  pthread_mutex_unlock(&g_repo_mirror.lock);
}

static void
progress_set_warning_if_empty(const char *warning) {
  pthread_mutex_lock(&g_repo_mirror.lock);
  if (g_repo_mirror.warning[0] == '\0') {
    snprintf(g_repo_mirror.warning, sizeof(g_repo_mirror.warning), "%s", warning ? warning : "");
  }
  pthread_mutex_unlock(&g_repo_mirror.lock);
}

static void
progress_set_truncated(int truncated) {
  pthread_mutex_lock(&g_repo_mirror.lock);
  g_repo_mirror.truncated = truncated ? 1 : 0;
  pthread_mutex_unlock(&g_repo_mirror.lock);
}

static void
progress_mark_incomplete(void) {
  pthread_mutex_lock(&g_repo_mirror.lock);
  g_repo_mirror.complete = 0;
  pthread_mutex_unlock(&g_repo_mirror.lock);
}

static void
progress_add_missing(const char *name) {
  pthread_mutex_lock(&g_repo_mirror.lock);
  if (g_repo_mirror.missing_count < REPO_MIRROR_MISSING_MAX) {
    snprintf(g_repo_mirror.missing[g_repo_mirror.missing_count],
             REPO_MIRROR_MISSING_NAME_MAX, "%s", name ? name : "?");
    g_repo_mirror.missing_count++;
  }
  pthread_mutex_unlock(&g_repo_mirror.lock);
}

static void
progress_set_source_progress(int idx, int total) {
  pthread_mutex_lock(&g_repo_mirror.lock);
  g_repo_mirror.source_idx = idx;
  g_repo_mirror.source_total = total;
  pthread_mutex_unlock(&g_repo_mirror.lock);
}

static void
repo_mirror_reset_locked(void) {
  g_repo_mirror.state = REPO_MIRROR_IDLE;
  g_repo_mirror.total = 0;
  g_repo_mirror.downloaded = 0;
  g_repo_mirror.skipped = 0;
  g_repo_mirror.failed = 0;
  g_repo_mirror.verified = 0;
  g_repo_mirror.missing_count = 0;
  memset(g_repo_mirror.missing, 0, sizeof(g_repo_mirror.missing));
  g_repo_mirror.current[0] = '\0';
  g_repo_mirror.error[0] = '\0';
  g_repo_mirror.source[0] = '\0';
  g_repo_mirror.mode[0] = '\0';
  g_repo_mirror.warning[0] = '\0';
  g_repo_mirror.truncated = 0;
  g_repo_mirror.complete = 1;
  g_repo_mirror.source_idx = 0;
  g_repo_mirror.source_total = 0;
  g_repo_mirror.started_at = 0;
  g_repo_mirror.finished_at = 0;
}

static void
build_tree_url(const repo_def_t *repo, char *out, size_t out_size) {
  snprintf(out, out_size,
           "https://api.github.com/repos/%s/%s/git/trees/%s?recursive=1",
           repo->owner, repo->repo, repo->branch);
}

static void
build_raw_url(const repo_def_t *repo, const char *repo_path, char *out, size_t out_size) {
  snprintf(out, out_size,
           "https://raw.githubusercontent.com/%s/%s/%s/%s",
           repo->owner, repo->repo, repo->branch, repo_path);
}

static void
build_contents_url(const repo_def_t *repo, const char *subdir, char *out, size_t out_size) {
  char path[256];
  if (repo->cheat_root && repo->cheat_root[0]) {
    snprintf(path, sizeof(path), "%s/%s", repo->cheat_root, subdir);
  } else {
    snprintf(path, sizeof(path), "%s", subdir);
  }
  snprintf(out, out_size,
           "https://api.github.com/repos/%s/%s/contents/%s?ref=%s",
           repo->owner, repo->repo, path, repo->branch);
}

static int
path_under_root(const char *path, const char *root) {
  if (!root || !root[0]) {
    return 1;
  }
  size_t rl = strlen(root);
  if (strncmp(path, root, rl) != 0) {
    return 0;
  }
  return path[rl] == '/' || path[rl] == '\0';
}

static int
entry_cmp_by_dest(const void *a, const void *b) {
  const mirror_entry_t *ea = (const mirror_entry_t *)a;
  const mirror_entry_t *eb = (const mirror_entry_t *)b;
  if (ea->kind != eb->kind) {
    return ea->kind - eb->kind;
  }
  return strcmp(ea->basename, eb->basename);
}

static int
repo_list_from_tree(const repo_def_t *repo, mirror_entry_list_t *out, int *out_truncated) {
  char url[512];
  build_tree_url(repo, url, sizeof(url));
  cr_log("info", "repo_mirror", "tree listing source=%s url=%s", repo->id, url);

  int status = 0;
  uint8_t *data = NULL;
  size_t data_len = 0;
  if (http_get_url_ex(AGENT, url, TREE_LIST_MAX, &status, &data, &data_len) != 0 ||
      status != 200 || !data || data_len == 0) {
    cr_log("warn", "repo_mirror", "tree failed source=%s status=%d; trying contents fallback", repo->id, status);
    free(data);
    return -1;
  }

  char *text = malloc(data_len + 1);
  if (!text) {
    free(data);
    return -1;
  }
  memcpy(text, data, data_len);
  text[data_len] = '\0';
  free(data);

  cJSON *root = cJSON_Parse(text);
  free(text);
  if (!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    cr_log("warn", "repo_mirror", "tree JSON invalid source=%s; trying contents fallback", repo->id);
    return -1;
  }

  int truncated = cJSON_IsTrue(cJSON_GetObjectItem(root, "truncated")) ? 1 : 0;
  if (out_truncated) {
    *out_truncated = truncated;
  }
  if (truncated) {
    cr_log("warn", "repo_mirror", "git tree response truncated source=%s", repo->id);
  }

  cJSON *tree = cJSON_GetObjectItem(root, "tree");
  if (!cJSON_IsArray(tree)) {
    cJSON_Delete(root);
    cr_log("warn", "repo_mirror", "tree missing array source=%s; trying contents fallback", repo->id);
    return -1;
  }

  int total_tree_entries = cJSON_GetArraySize(tree);
  cr_log("info", "repo_mirror", "tree entries source=%s total=%d", repo->id, total_tree_entries);

  cJSON *it = NULL;
  cJSON_ArrayForEach(it, tree) {
    cJSON *type_j = cJSON_GetObjectItem(it, "type");
    cJSON *path_j = cJSON_GetObjectItem(it, "path");
    if (!cJSON_IsString(type_j) || !cJSON_IsString(path_j) ||
        !type_j->valuestring || !path_j->valuestring) {
      continue;
    }
    if (strcmp(type_j->valuestring, "blob") != 0) {
      continue;
    }
    const char *repo_path = path_j->valuestring;
    if (!is_safe_repo_rel_path(repo_path)) {
      continue;
    }
    if (!path_under_root(repo_path, repo->cheat_root)) {
      continue;
    }
    const char *base = path_basename_ptr(repo_path);
    if (!is_safe_filename(base)) {
      continue;
    }
    int kind = entry_kind_from_name(base);
    if (!kind) {
      continue;
    }
    const char *dest = entry_dest_dir_from_kind(kind);
    if (!dest) {
      continue;
    }

    mirror_entry_t e;
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    e.dest_dir = dest;
    snprintf(e.path, sizeof(e.path), "%s", repo_path);
    snprintf(e.basename, sizeof(e.basename), "%s", base);
    build_raw_url(repo, repo_path, e.raw_url, sizeof(e.raw_url));
    if (entry_list_add(out, &e) != 0) {
      cJSON_Delete(root);
      return -1;
    }
  }

  cJSON_Delete(root);
  return 0;
}

static void
repo_add_contents_entry(const repo_def_t *repo, const char *format_dir, cJSON *entry, mirror_entry_list_t *out) {
  cJSON *name_j = cJSON_GetObjectItem(entry, "name");
  cJSON *type_j = cJSON_GetObjectItem(entry, "type");
  cJSON *url_j  = cJSON_GetObjectItem(entry, "download_url");
  if (!cJSON_IsString(name_j) || !cJSON_IsString(type_j) ||
      !name_j->valuestring || !type_j->valuestring) {
    return;
  }
  if (strcmp(type_j->valuestring, "file") != 0) {
    return;
  }
  const char *base = name_j->valuestring;
  if (!is_safe_filename(base)) {
    return;
  }
  int kind = entry_kind_from_name(base);
  if (!kind) {
    return;
  }
  const char *dest = entry_dest_dir_from_kind(kind);
  if (!dest) {
    return;
  }

  mirror_entry_t e;
  memset(&e, 0, sizeof(e));
  e.kind = kind;
  e.dest_dir = dest;
  snprintf(e.basename, sizeof(e.basename), "%s", base);
  if (repo->cheat_root && repo->cheat_root[0]) {
    snprintf(e.path, sizeof(e.path), "%s/%s/%s", repo->cheat_root, format_dir, base);
  } else {
    snprintf(e.path, sizeof(e.path), "%s/%s", format_dir, base);
  }
  if (cJSON_IsString(url_j) && url_j->valuestring && url_j->valuestring[0]) {
    snprintf(e.raw_url, sizeof(e.raw_url), "%s", url_j->valuestring);
  } else {
    build_raw_url(repo, e.path, e.raw_url, sizeof(e.raw_url));
  }
  (void)entry_list_add(out, &e);
}

static int
repo_list_from_contents_fallback(const repo_def_t *repo, mirror_entry_list_t *out, int *out_partial) {
  const char *dirs[3] = { "json", "shn", "mc4" };
  char url[512];
  int partial = 0;
  progress_set_mode("contents_fallback");
  cr_log("info", "repo_mirror", "contents fallback source=%s", repo->id);

  for (int di = 0; di < 3; di++) {
    build_contents_url(repo, dirs[di], url, sizeof(url));
    int status = 0;
    uint8_t *data = NULL;
    size_t data_len = 0;
    if (http_get_url_ex(AGENT, url, DIR_LIST_MAX, &status, &data, &data_len) != 0 ||
        status != 200 || !data || data_len == 0) {
      cr_log("warn", "repo_mirror", "contents listing failed source=%s dir=%s status=%d", repo->id, dirs[di], status);
      partial = 1;
      free(data);
      continue;
    }
    char *text = malloc(data_len + 1);
    if (!text) {
      free(data);
      return -1;
    }
    memcpy(text, data, data_len);
    text[data_len] = '\0';
    free(data);
    cJSON *arr = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsArray(arr)) {
      partial = 1;
      cJSON_Delete(arr);
      continue;
    }
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, arr) {
      repo_add_contents_entry(repo, dirs[di], entry, out);
    }
    cJSON_Delete(arr);
  }
  if (out_partial) {
    *out_partial = partial;
  }

  return out->count > 0 ? 0 : -1;
}

static void
progress_add_total(int n) {
  pthread_mutex_lock(&g_repo_mirror.lock);
  g_repo_mirror.total += n;
  pthread_mutex_unlock(&g_repo_mirror.lock);
}

static void
progress_inc_downloaded(void) {
  pthread_mutex_lock(&g_repo_mirror.lock);
  g_repo_mirror.downloaded++;
  pthread_mutex_unlock(&g_repo_mirror.lock);
}

static void
progress_inc_skipped(void) {
  pthread_mutex_lock(&g_repo_mirror.lock);
  g_repo_mirror.skipped++;
  pthread_mutex_unlock(&g_repo_mirror.lock);
}

static void
progress_inc_failed(void) {
  pthread_mutex_lock(&g_repo_mirror.lock);
  g_repo_mirror.failed++;
  pthread_mutex_unlock(&g_repo_mirror.lock);
}

static void
process_entries_download(const repo_def_t *repo, mirror_entry_list_t *list, int overwrite) {
  if (!list || list->count <= 0) {
    return;
  }

  qsort(list->items, (size_t)list->count, sizeof(list->items[0]), entry_cmp_by_dest);
  progress_add_total(list->count);

  for (int i = 0; i < list->count; i++) {
    mirror_entry_t *e = &list->items[i];
    if (i > 0) {
      mirror_entry_t *prev = &list->items[i - 1];
      if (prev->kind == e->kind && strcmp(prev->basename, e->basename) == 0) {
        progress_inc_skipped();
        cr_log("warn", "repo_mirror", "duplicate basename in same run source=%s file=%s path=%s",
               repo->id, e->basename, e->path);
        continue;
      }
    }

    char dest_path[768];
    snprintf(dest_path, sizeof(dest_path), "%s/%s", e->dest_dir, e->basename);

    if (!overwrite) {
      struct stat st;
      if (stat(dest_path, &st) == 0 && S_ISREG(st.st_mode)) {
        progress_inc_skipped();
        continue;
      }
    }

    progress_set_current(e->basename);

    int status = 0;
    uint8_t *fdata = NULL;
    size_t flen = 0;
    int rc = http_get_url_ex(AGENT, e->raw_url, FILE_MAX, &status, &fdata, &flen);
    if (rc != 0 || status != 200 || !fdata || flen == 0) {
      progress_inc_failed();
      progress_add_missing(e->basename);
      cr_log("warn", "repo_mirror", "download failed source=%s file=%s status=%d", repo->id, e->basename, status);
      free(fdata);
      continue;
    }
    if (write_file_atomic(dest_path, fdata, flen) != 0) {
      progress_inc_failed();
      progress_add_missing(e->basename);
      cr_log("warn", "repo_mirror", "write failed source=%s file=%s", repo->id, e->basename);
      free(fdata);
      continue;
    }
    free(fdata);
    progress_inc_downloaded();
    cr_log("info", "repo_mirror", "saved %s (%zu bytes)", e->basename, flen);
  }
}

static void
mirror_one_source(const repo_def_t *repo, int overwrite) {
  mirror_entry_list_t list;
  memset(&list, 0, sizeof(list));

  progress_set_current(repo->id);
  progress_set_mode("git_tree");

  int truncated = 0;
  int tree_ok = (repo_list_from_tree(repo, &list, &truncated) == 0);
  if (tree_ok) {
    if (truncated) {
      progress_set_truncated(1);
      progress_set_warning_if_empty("git_tree_truncated");
      progress_mark_incomplete();
    }
  } else {
    entry_list_free(&list);
    memset(&list, 0, sizeof(list));
    int fallback_partial = 0;
    if (repo_list_from_contents_fallback(repo, &list, &fallback_partial) != 0) {
      progress_inc_failed();
      cr_log("warn", "repo_mirror", "both tree and contents fallback failed source=%s", repo->id);
      progress_mark_incomplete();
      progress_set_warning_if_empty("listing_failed");
      entry_list_free(&list);
      return;
    }
    progress_mark_incomplete();
    if (fallback_partial) {
      progress_set_warning_if_empty("contents_fallback_partial");
    } else {
      progress_set_warning_if_empty("contents_fallback_nonrecursive");
    }
  }

  process_entries_download(repo, &list, overwrite);
  entry_list_free(&list);
}

static void *
mirror_thread(void *arg) {
  mirror_args_t *ma = (mirror_args_t *)arg;
  int repo_idx  = ma->repo_idx;
  int overwrite = ma->overwrite;
  int all_sources = ma->all_sources;
  free(ma);

  int start_idx = all_sources ? 0 : repo_idx;
  int end_idx   = all_sources ? (REPOS_COUNT - 1) : repo_idx;
  int source_total = end_idx - start_idx + 1;

  for (int i = start_idx; i <= end_idx; i++) {
    const repo_def_t *repo = &REPOS[i];
    progress_set_source_progress((i - start_idx) + 1, source_total);
    cr_log("info", "repo_mirror", "start source=%s overwrite=%d", repo->id, overwrite);
    mirror_one_source(repo, overwrite);
  }

  progress_set_current("");
  pthread_mutex_lock(&g_repo_mirror.lock);
  g_repo_mirror.verified = g_repo_mirror.downloaded + g_repo_mirror.skipped;
  if (g_repo_mirror.downloaded == 0 && g_repo_mirror.skipped == 0 && g_repo_mirror.failed > 0) {
    snprintf(g_repo_mirror.error, sizeof(g_repo_mirror.error),
             "All downloads failed - check network or GitHub rate limit");
    g_repo_mirror.state = REPO_MIRROR_ERROR;
  } else {
    g_repo_mirror.state = REPO_MIRROR_DONE;
  }
  g_repo_mirror.finished_at = time(NULL);
  cr_log("info", "repo_mirror", "done source=%s dl=%d skip=%d fail=%d verified=%d",
         g_repo_mirror.source, g_repo_mirror.downloaded, g_repo_mirror.skipped,
         g_repo_mirror.failed, g_repo_mirror.verified);
  pthread_mutex_unlock(&g_repo_mirror.lock);
  return NULL;
}

int
repo_mirror_start(const char *source, int overwrite) {
  if (!source || !source[0]) {
    return -1;
  }

  int repo_idx = -1;
  int all_sources = 0;
  if (strcmp(source, "all") == 0) {
    all_sources = 1;
  } else if (!find_repo_by_name(source, &repo_idx)) {
    cr_log("warn", "repo_mirror", "unknown source '%s'", source);
    return -1;
  }

  pthread_mutex_lock(&g_repo_mirror.lock);
  if (g_repo_mirror.state == REPO_MIRROR_RUNNING) {
    pthread_mutex_unlock(&g_repo_mirror.lock);
    cr_log("warn", "repo_mirror", "already running, ignoring start request");
    return -1;
  }
  repo_mirror_reset_locked();
  g_repo_mirror.state = REPO_MIRROR_RUNNING;
  snprintf(g_repo_mirror.source, sizeof(g_repo_mirror.source), "%s", source);
  g_repo_mirror.started_at = time(NULL);
  g_repo_mirror.source_total = all_sources ? REPOS_COUNT : 1;
  g_repo_mirror.source_idx = 0;
  pthread_mutex_unlock(&g_repo_mirror.lock);

  mirror_args_t *args = malloc(sizeof(*args));
  if (!args) {
    pthread_mutex_lock(&g_repo_mirror.lock);
    g_repo_mirror.state = REPO_MIRROR_IDLE;
    pthread_mutex_unlock(&g_repo_mirror.lock);
    return -1;
  }
  args->repo_idx = repo_idx;
  args->overwrite = overwrite;
  args->all_sources = all_sources;

  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&tid, &attr, mirror_thread, args) != 0) {
    cr_log("warn", "repo_mirror", "pthread_create failed errno=%d", errno);
    pthread_attr_destroy(&attr);
    free(args);
    pthread_mutex_lock(&g_repo_mirror.lock);
    g_repo_mirror.state = REPO_MIRROR_IDLE;
    pthread_mutex_unlock(&g_repo_mirror.lock);
    return -1;
  }
  pthread_attr_destroy(&attr);
  cr_log("info", "repo_mirror", "thread started source=%s", source);
  return 0;
}

void
repo_mirror_status_json(char *buf, size_t buf_size) {
  if (!buf || buf_size == 0) {
    return;
  }

  pthread_mutex_lock(&g_repo_mirror.lock);
  repo_mirror_state_t state = g_repo_mirror.state;
  char source[32]; snprintf(source, sizeof(source), "%s", g_repo_mirror.source);
  int total = g_repo_mirror.total;
  int downloaded = g_repo_mirror.downloaded;
  int skipped = g_repo_mirror.skipped;
  int failed = g_repo_mirror.failed;
  int verified = g_repo_mirror.verified;
  int missing_n = g_repo_mirror.missing_count;
  char current[128]; snprintf(current, sizeof(current), "%s", g_repo_mirror.current);
  char error[256]; snprintf(error, sizeof(error), "%s", g_repo_mirror.error);
  char mode[32]; snprintf(mode, sizeof(mode), "%s", g_repo_mirror.mode);
  char warning[64]; snprintf(warning, sizeof(warning), "%s", g_repo_mirror.warning);
  int truncated = g_repo_mirror.truncated;
  int complete = g_repo_mirror.complete;
  int source_idx = g_repo_mirror.source_idx;
  int source_total = g_repo_mirror.source_total;
  time_t started = g_repo_mirror.started_at;
  time_t finished = g_repo_mirror.finished_at;
  char missing_names[REPO_MIRROR_MISSING_MAX][REPO_MIRROR_MISSING_NAME_MAX];
  for (int i = 0; i < missing_n; i++) {
    snprintf(missing_names[i], REPO_MIRROR_MISSING_NAME_MAX, "%s", g_repo_mirror.missing[i]);
  }
  pthread_mutex_unlock(&g_repo_mirror.lock);

  const char *state_str =
    state == REPO_MIRROR_RUNNING ? "running" :
    state == REPO_MIRROR_DONE    ? "done" :
    state == REPO_MIRROR_ERROR   ? "error" : "idle";
  int pct = (total > 0) ? (int)(((downloaded + skipped) * 100) / total) : 0;
  if (pct > 100) {
    pct = 100;
  }

  cJSON *out = cJSON_CreateObject();
  if (!out) {
    snprintf(buf, buf_size, "{\"ok\":false,\"error\":\"oom\"}");
    return;
  }
  cJSON_AddBoolToObject(out, "ok", 1);
  cJSON_AddStringToObject(out, "state", state_str);
  cJSON_AddStringToObject(out, "source", source);
  cJSON_AddNumberToObject(out, "total", total);
  cJSON_AddNumberToObject(out, "downloaded", downloaded);
  cJSON_AddNumberToObject(out, "skipped", skipped);
  cJSON_AddNumberToObject(out, "failed", failed);
  cJSON_AddNumberToObject(out, "verified", verified);
  cJSON_AddNumberToObject(out, "pct", pct);
  cJSON_AddStringToObject(out, "current", current);
  cJSON_AddStringToObject(out, "error", error);
  cJSON_AddStringToObject(out, "mode", mode);
  cJSON_AddStringToObject(out, "warning", warning);
  cJSON_AddBoolToObject(out, "truncated", truncated ? 1 : 0);
  cJSON_AddBoolToObject(out, "complete", complete ? 1 : 0);
  cJSON_AddNumberToObject(out, "sourceIndex", source_idx);
  cJSON_AddNumberToObject(out, "sourceTotal", source_total);
  cJSON_AddNumberToObject(out, "missingCount", missing_n);
  cJSON_AddNumberToObject(out, "startedAt", (double)started);
  cJSON_AddNumberToObject(out, "finishedAt", (double)finished);
  cJSON *arr = cJSON_AddArrayToObject(out, "missing");
  for (int i = 0; arr && i < missing_n; i++) {
    cJSON_AddItemToArray(arr, cJSON_CreateString(missing_names[i]));
  }

  char *txt = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  if (!txt) {
    snprintf(buf, buf_size, "{\"ok\":false,\"error\":\"oom\"}");
    return;
  }
  snprintf(buf, buf_size, "%s", txt);
  free(txt);
}
