#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "http_client.h"
#include "cr_log.h"
#include "cr_paths.h"
#include "cr_config.h"
#include "cr_titles.h"
#include "cr_cheat_formats.h"
#include "cr_version.h"
#include "cr_remote_sources.h"
static void
source_make_slug(const char *name, char *out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (!name || !name[0]) {
    snprintf(out, out_size, "source");
    return;
  }
  size_t w = 0;
  int prev_dash = 0;
  for (const char *p = name; *p && w + 1 < out_size; p++) {
    unsigned char c = (unsigned char)*p;
    if (isalnum(c)) {
      out[w++] = (char)tolower(c);
      prev_dash = 0;
    } else if (!prev_dash) {
      out[w++] = '-';
      prev_dash = 1;
    }
  }
  while (w > 0 && out[w - 1] == '-') {
    w--;
  }
  out[w] = '\0';
  if (!out[0]) {
    snprintf(out, out_size, "source");
  }
}

static void
source_set(remote_source_t *s, const char *name, const char *type, const char *owner,
           const char *repo, const char *branch, const char *path, int enabled) {
  if (!s) {
    return;
  }
  memset(s, 0, sizeof(*s));
  snprintf(s->name, sizeof(s->name), "%s", name ? name : "");
  snprintf(s->type, sizeof(s->type), "%s", type ? type : "github");
  snprintf(s->owner, sizeof(s->owner), "%s", owner ? owner : "");
  snprintf(s->repo, sizeof(s->repo), "%s", repo ? repo : "");
  snprintf(s->branch, sizeof(s->branch), "%s", branch ? branch : "main");
  snprintf(s->path, sizeof(s->path), "%s", path ? path : "");
  s->enabled = enabled ? 1 : 0;
  source_make_slug(s->name, s->id, sizeof(s->id));
}

static void
source_model_defaults(source_config_model_t *m) {
  if (!m) {
    return;
  }
  memset(m, 0, sizeof(*m));
  source_set(&m->cheat_sources[0], "HEN-Cheats-Collection", "github", "TeeKay87",
             "HEN-Cheats-Collection", "master", "cheats", 1);
  source_set(&m->cheat_sources[1], "PS5-Cheats", "github", "etaHEN",
             "PS5_Cheats", "main", "", 1);
  source_set(&m->cheat_sources[2], "GoldHEN-Cheats", "github", "GoldHEN",
             "GoldHEN_Cheat_Repository", "main", "", 1);
  source_set(&m->cheat_sources[3], "HEN-PPSA-Cheats", "github", "RDX-Sci01",
           "HEN-PPSA-Cheats", "main", "cheats", 1);
  m->cheat_count = 4;
}

static int
source_read_json_entry(cJSON *obj, remote_source_t *out) {
  if (!cJSON_IsObject(obj) || !out) {
    return 0;
  }
  cJSON *name_j = cJSON_GetObjectItem(obj, "name");
  cJSON *type_j = cJSON_GetObjectItem(obj, "type");
  cJSON *owner_j = cJSON_GetObjectItem(obj, "owner");
  cJSON *repo_j = cJSON_GetObjectItem(obj, "repo");
  cJSON *branch_j = cJSON_GetObjectItem(obj, "branch");
  cJSON *path_j = cJSON_GetObjectItem(obj, "path");
  cJSON *enabled_j = cJSON_GetObjectItem(obj, "enabled");
  cJSON *id_j = cJSON_GetObjectItem(obj, "id");
  if (!cJSON_IsString(name_j) || !name_j->valuestring || !name_j->valuestring[0] ||
      !cJSON_IsString(type_j) || !type_j->valuestring || !type_j->valuestring[0] ||
      !cJSON_IsString(owner_j) || !owner_j->valuestring || !owner_j->valuestring[0] ||
      !cJSON_IsString(repo_j) || !repo_j->valuestring || !repo_j->valuestring[0] ||
      !cJSON_IsString(branch_j) || !branch_j->valuestring || !branch_j->valuestring[0] ||
      !cJSON_IsString(path_j) || !path_j->valuestring || !path_j->valuestring[0]) {
    return 0;
  }
  source_set(out, name_j->valuestring, type_j->valuestring, owner_j->valuestring,
             repo_j->valuestring, branch_j->valuestring, path_j->valuestring,
             cJSON_IsBool(enabled_j) ? cJSON_IsTrue(enabled_j) : 1);
  if (cJSON_IsString(id_j) && id_j->valuestring && id_j->valuestring[0]) {
    source_make_slug(id_j->valuestring, out->id, sizeof(out->id));
  }
  if (!out->id[0]) {
    source_make_slug(out->name, out->id, sizeof(out->id));
  }
  return 1;
}

void
source_model_load(source_config_model_t *m) {
  source_model_defaults(m);
  char *txt = NULL;
  if (read_file_text(CHEATRUNNER_SOURCES_PATH, &txt) != 0 || !txt) {
    free(txt);
    return;
  }
  cJSON *root = cJSON_Parse(txt);
  free(txt);
  if (!root) {
    return;
  }
  cJSON *cheat_arr = cJSON_GetObjectItem(root, "cheatSources");

  if (cJSON_IsArray(cheat_arr)) {
    int n = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, cheat_arr) {
      if (n >= MAX_REMOTE_SOURCES) {
        break;
      }
      remote_source_t tmp;
      if (source_read_json_entry(it, &tmp)) {
        m->cheat_sources[n++] = tmp;
      }
    }
    if (n > 0) {
      m->cheat_count = n;
    }
  }
  cJSON_Delete(root);
}


static void
source_cache_file_path(const char *source_id, char *out, size_t out_size) {
  char slug[64];
  source_make_slug(source_id ? source_id : "source", slug, sizeof(slug));
  snprintf(out, out_size, "%s/%s.json", CHEATRUNNER_CACHE_SOURCES_DIR, slug);
}

static int
source_cache_load_entries(const char *source_id, int ttl_seconds, cJSON **entries_out) {
  if (!entries_out) {
    return 0;
  }
  *entries_out = NULL;
  char path[512];
  source_cache_file_path(source_id, path, sizeof(path));
  char *txt = NULL;
  if (read_file_text(path, &txt) != 0 || !txt) {
    free(txt);
    return 0;
  }
  cJSON *root = cJSON_Parse(txt);
  free(txt);
  if (!root) {
    return 0;
  }
  cJSON *fetched = cJSON_GetObjectItem(root, "fetchedAt");
  cJSON *entries = cJSON_GetObjectItem(root, "entries");
  int ok = 0;
  if (cJSON_IsNumber(fetched) && cJSON_IsArray(entries)) {
    time_t now = time(NULL);
    time_t age = now - (time_t)fetched->valuedouble;
    if (age >= 0 && age <= ttl_seconds) {
      *entries_out = cJSON_Duplicate(entries, 1);
      ok = (*entries_out != NULL);
    }
  }
  cJSON_Delete(root);
  return ok;
}

static int
source_cache_load_stale(const char *source_id, cJSON **entries_out) {
  if (!entries_out) {
    return 0;
  }
  *entries_out = NULL;
  char path[512];
  source_cache_file_path(source_id, path, sizeof(path));
  char *txt = NULL;
  if (read_file_text(path, &txt) != 0 || !txt) {
    free(txt);
    return 0;
  }
  cJSON *root = cJSON_Parse(txt);
  free(txt);
  if (!root) {
    return 0;
  }
  cJSON *entries = cJSON_GetObjectItem(root, "entries");
  int ok = 0;
  if (cJSON_IsArray(entries) && cJSON_GetArraySize(entries) > 0) {
    *entries_out = cJSON_Duplicate(entries, 1);
    ok = (*entries_out != NULL);
  }
  cJSON_Delete(root);
  return ok;
}

static void
source_cache_save_entries(const char *source_id, int ttl_seconds, cJSON *entries) {
  if (!source_id || !entries || !cJSON_IsArray(entries)) {
    return;
  }
  ensure_dir_recursive(CHEATRUNNER_CACHE_SOURCES_DIR);
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    return;
  }
  cJSON_AddStringToObject(root, "sourceId", source_id);
  cJSON_AddNumberToObject(root, "fetchedAt", (double)time(NULL));
  cJSON_AddNumberToObject(root, "ttl", ttl_seconds);
  cJSON_AddItemToObject(root, "entries", cJSON_Duplicate(entries, 1));
  char *out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!out) {
    return;
  }
  char path[512];
  source_cache_file_path(source_id, path, sizeof(path));
  write_file_atomic(path, (const uint8_t *)out, strlen(out));
  free(out);
}

int
body_looks_rate_limited(const uint8_t *data, size_t len) {
  if (!data || len == 0) {
    return 0;
  }
  size_t n = len > 255 ? 255 : len;
  char probe[256];
  memcpy(probe, data, n);
  probe[n] = '\0';
  for (size_t i = 0; probe[i]; i++) {
    probe[i] = (char)tolower((unsigned char)probe[i]);
  }
  return strstr(probe, "rate limit") != NULL || strstr(probe, "api rate limit") != NULL;
}

int
http_fetch_bytes_checked(const char *url, size_t max_bytes, int *status_out,
                         uint8_t **data_out, size_t *len_out) {
  int status = -1;
  uint8_t *data = NULL;
  size_t len = 0;
  int rc = http_get_url_ex("CheatRunner/0.1", url, max_bytes, &status, &data, &len);
  if (status_out) {
    *status_out = status;
  }
  if (rc != 0) {
    free(data);
    if (rc == -2) {
      return -2;
    }
    return -1;
  }
  if (data_out) {
    *data_out = data;
  } else {
    free(data);
  }
  if (len_out) {
    *len_out = len;
  }
  return 0;
}

static int
json_array_add_entry(cJSON *arr, const char *path, const char *name, const char *format, int size, const char *download_url) {
  if (!cJSON_IsArray(arr) || !path || !name) {
    return 0;
  }
  cJSON *o = cJSON_CreateObject();
  if (!o) {
    return 0;
  }
  cJSON_AddStringToObject(o, "path", path);
  cJSON_AddStringToObject(o, "name", name);
  cJSON_AddStringToObject(o, "format", format ? format : "");
  cJSON_AddNumberToObject(o, "size", size >= 0 ? size : 0);
  if (download_url && download_url[0]) {
    cJSON_AddStringToObject(o, "downloadUrl", download_url);
  }
  cJSON_AddItemToArray(arr, o);
  return 1;
}

static int
json_array_contains_path(cJSON *arr, const char *path) {
  if (!cJSON_IsArray(arr) || !path) {
    return 0;
  }
  cJSON *it = NULL;
  cJSON_ArrayForEach(it, arr) {
    cJSON *p = cJSON_GetObjectItem(it, "path");
    if (cJSON_IsString(p) && p->valuestring && strcmp(p->valuestring, path) == 0) {
      return 1;
    }
  }
  return 0;
}

static void
json_entry_set_string(cJSON *obj, const char *key, const char *val) {
  if (!cJSON_IsObject(obj) || !key || !key[0]) {
    return;
  }
  cJSON_DeleteItemFromObject(obj, key);
  cJSON_AddStringToObject(obj, key, val ? val : "");
}

static void
annotate_source_entries(cJSON *entries, const remote_source_t *src) {
  if (!cJSON_IsArray(entries) || !src) {
    return;
  }
  cJSON *it = NULL;
  cJSON_ArrayForEach(it, entries) {
    if (!cJSON_IsObject(it)) {
      continue;
    }
    json_entry_set_string(it, "sourceId", src->id);
    json_entry_set_string(it, "sourceName", src->name);
    json_entry_set_string(it, "owner", src->owner);
    json_entry_set_string(it, "repo", src->repo);
    json_entry_set_string(it, "branch", src->branch);
    json_entry_set_string(it, "sourcePath", src->path);
  }
}

static void
build_github_contents_url(const remote_source_t *src, const char *path, char *out, size_t out_size) {
  snprintf(out, out_size, "https://api.github.com/repos/%s/%s/contents/%s?ref=%s",
           src->owner, src->repo, path, src->branch);
}

static void
build_github_tree_url(const remote_source_t *src, char *out, size_t out_size) {
  snprintf(out, out_size, "https://api.github.com/repos/%s/%s/git/trees/%s?recursive=1",
           src->owner, src->repo, src->branch);
}

void
build_github_raw_url(const remote_source_t *src, const char *path, char *out, size_t out_size) {
  snprintf(out, out_size, "https://raw.githubusercontent.com/%s/%s/%s/%s",
           src->owner, src->repo, src->branch, path);
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
github_list_entries_tree(const remote_source_t *src, int want_cheats,
                         cJSON **entries_out, int max_response_bytes, int *status_out,
                         int *truncated_out, char *err, size_t err_size) {
  if (!src || !entries_out) {
    if (err && err_size) snprintf(err, err_size, "invalid_source");
    return -1;
  }
  *entries_out = NULL;
  if (status_out) *status_out = -1;
  if (truncated_out) *truncated_out = 0;

  char url[1024];
  build_github_tree_url(src, url, sizeof(url));
  int status = -1;
  uint8_t *body = NULL;
  size_t body_len = 0;
  cr_log("debug", "cheats.remote", "git tree request url=%s", url);
  int frc = http_fetch_bytes_checked(url, (size_t)max_response_bytes, &status, &body, &body_len);
  if (status_out) *status_out = status;
  if (frc != 0) {
    if (err && err_size) snprintf(err, err_size, frc == -2 ? "response_too_large" : "network_error");
    free(body);
    return -1;
  }
  if (status == 403 && body_looks_rate_limited(body, body_len)) {
    free(body);
    if (err && err_size) snprintf(err, err_size, "rate_limited");
    return -1;
  }
  if (status == 404) {
    free(body);
    if (err && err_size) snprintf(err, err_size, "source_not_found");
    return -1;
  }
  if (status != 200) {
    free(body);
    if (err && err_size) snprintf(err, err_size, "http_%d", status);
    return -1;
  }

  char *json_txt = malloc(body_len + 1);
  if (!json_txt) {
    free(body);
    if (err && err_size) snprintf(err, err_size, "oom");
    return -1;
  }
  memcpy(json_txt, body, body_len);
  json_txt[body_len] = '\0';
  free(body);

  cJSON *root = cJSON_Parse(json_txt);
  free(json_txt);
  if (!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    if (err && err_size) snprintf(err, err_size, "invalid_json");
    return -1;
  }

  int truncated = cJSON_IsTrue(cJSON_GetObjectItem(root, "truncated")) ? 1 : 0;
  if (truncated_out) {
    *truncated_out = truncated;
  }
  if (truncated) {
    cr_log("warn", "cheats.remote", "git tree response truncated source=%s", src->name);
  }

  cJSON *tree = cJSON_GetObjectItem(root, "tree");
  if (!cJSON_IsArray(tree)) {
    cJSON_Delete(root);
    if (err && err_size) snprintf(err, err_size, "invalid_json");
    return -1;
  }

  cJSON *entries = cJSON_CreateArray();
  if (!entries) {
    cJSON_Delete(root);
    if (err && err_size) snprintf(err, err_size, "oom");
    return -1;
  }

  cJSON *it = NULL;
  cJSON_ArrayForEach(it, tree) {
    cJSON *type_j = cJSON_GetObjectItem(it, "type");
    cJSON *path_j = cJSON_GetObjectItem(it, "path");
    cJSON *size_j = cJSON_GetObjectItem(it, "size");
    if (!cJSON_IsString(type_j) || !cJSON_IsString(path_j) ||
        !type_j->valuestring || !path_j->valuestring) {
      continue;
    }
    if (strcmp(type_j->valuestring, "blob") != 0) {
      continue;
    }
    const char *repo_path = path_j->valuestring;
    int path_ok = path_under_root(repo_path, src->path);
    if (!is_safe_repo_rel_path(repo_path) || !path_ok) {
      continue;
    }
    const char *base = path_basename_ptr(repo_path);
    if (!is_safe_filename(base)) {
      continue;
    }

    if (want_cheats) {
      if (!recognised_cheat_extension(base)) {
        continue;
      }
    } else {
      continue;
    }

    if (json_array_contains_path(entries, repo_path)) {
      continue;
    }
    char raw_url[1024];
    build_github_raw_url(src, repo_path, raw_url, sizeof(raw_url));
    const char *fmt = "";
    if (want_cheats) {
      int k = recognised_cheat_extension(base);
      fmt = (k == 1) ? "json" : ((k == 2) ? "shn" : (k == 3 ? "mc4" : ""));
    }
    int sz = cJSON_IsNumber(size_j) ? size_j->valueint : 0;
    if (!json_array_add_entry(entries, repo_path, base, fmt, sz, raw_url)) {
      cJSON_Delete(entries);
      cJSON_Delete(root);
      if (err && err_size) snprintf(err, err_size, "oom");
      return -1;
    }
    if (cJSON_GetArraySize(entries) >= MAX_SOURCE_INDEX_ENTRIES) {
      break;
    }
  }
  cr_log("info", "cheats.remote", "git tree ok entries=%d", cJSON_GetArraySize(entries));
  cJSON_Delete(root);
  *entries_out = entries;
  if (err && err_size) err[0] = '\0';
  return 0;
}

static int
github_list_entries_recursive(const remote_source_t *src, const char *root_path,
                              int want_cheats, cJSON **entries_out,
                              int max_response_bytes, char *err, size_t err_size) {
  if (!src || !root_path || !entries_out || !is_safe_repo_rel_path(root_path)) {
    if (err && err_size) snprintf(err, err_size, "bad_source_path");
    return -1;
  }
  *entries_out = NULL;
  cJSON *entries = cJSON_CreateArray();
  if (!entries) {
    if (err && err_size) snprintf(err, err_size, "oom");
    return -1;
  }

  typedef struct walk_item {
    char path[256];
    int depth;
  } walk_item_t;
  walk_item_t queue[256];
  int q_head = 0;
  int q_tail = 0;
  snprintf(queue[q_tail].path, sizeof(queue[q_tail].path), "%s", root_path);
  queue[q_tail].depth = 0;
  q_tail++;

  while (q_head < q_tail) {
    walk_item_t cur = queue[q_head++];
    char url[1024];
    build_github_contents_url(src, cur.path, url, sizeof(url));
    int status = -1;
    uint8_t *body = NULL;
    size_t body_len = 0;
    int frc = http_fetch_bytes_checked(url, (size_t)max_response_bytes, &status, &body, &body_len);
    if (frc != 0) {
      cJSON_Delete(entries);
      if (err && err_size) snprintf(err, err_size, frc == -2 ? "response_too_large" : "network_error");
      return -1;
    }
    if (status == 403 && body_looks_rate_limited(body, body_len)) {
      free(body);
      cJSON_Delete(entries);
      if (err && err_size) snprintf(err, err_size, "rate_limited");
      return -1;
    }
    if (status != 200) {
      free(body);
      cJSON_Delete(entries);
      if (err && err_size) snprintf(err, err_size, "http_%d", status);
      return -1;
    }

    char *json_txt = malloc(body_len + 1);
    if (!json_txt) {
      free(body);
      cJSON_Delete(entries);
      if (err && err_size) snprintf(err, err_size, "oom");
      return -1;
    }
    memcpy(json_txt, body, body_len);
    json_txt[body_len] = '\0';
    free(body);
    cJSON *doc = cJSON_Parse(json_txt);
    free(json_txt);
    if (!doc) {
      cJSON_Delete(entries);
      if (err && err_size) snprintf(err, err_size, "invalid_json");
      return -1;
    }
    cJSON *arr = doc;
    if (!cJSON_IsArray(arr)) {
      arr = cJSON_CreateArray();
      if (cJSON_IsObject(doc)) {
        cJSON_AddItemToArray(arr, cJSON_Duplicate(doc, 1));
      }
    }
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
      cJSON *type_j = cJSON_GetObjectItem(it, "type");
      cJSON *path_j = cJSON_GetObjectItem(it, "path");
      cJSON *name_j = cJSON_GetObjectItem(it, "name");
      cJSON *size_j = cJSON_GetObjectItem(it, "size");
      cJSON *download_j = cJSON_GetObjectItem(it, "download_url");
      if (!cJSON_IsString(type_j) || !cJSON_IsString(path_j) || !path_j->valuestring) {
        continue;
      }
      const char *type = type_j->valuestring;
      const char *path = path_j->valuestring;
      const char *name = cJSON_IsString(name_j) ? name_j->valuestring : path_basename_ptr(path);
      int size = cJSON_IsNumber(size_j) ? size_j->valueint : 0;
      const char *download_url = cJSON_IsString(download_j) ? download_j->valuestring : "";
      if (!is_safe_repo_rel_path(path)) {
        continue;
      }
      if (!strcmp(type, "dir")) {
        if (cur.depth < 5 && q_tail < (int)(sizeof(queue) / sizeof(queue[0]))) {
          snprintf(queue[q_tail].path, sizeof(queue[q_tail].path), "%s", path);
          queue[q_tail].depth = cur.depth + 1;
          q_tail++;
        }
        continue;
      }
      if (strcmp(type, "file") != 0) {
        continue;
      }
      if (want_cheats && !recognised_cheat_extension(name)) {
        continue;
      }
      if (json_array_contains_path(entries, path)) {
        continue;
      }
      const char *fmt = "";
      if (want_cheats) {
        int k = recognised_cheat_extension(name);
        fmt = (k == 1) ? "json" : ((k == 2) ? "shn" : (k == 3 ? "mc4" : ""));
      }
      if (!json_array_add_entry(entries, path, name, fmt, size, download_url)) {
        cJSON_Delete(doc);
        cJSON_Delete(entries);
        if (err && err_size) snprintf(err, err_size, "oom");
        return -1;
      }
      if (cJSON_GetArraySize(entries) >= MAX_SOURCE_INDEX_ENTRIES) {
        break;
      }
    }
    cJSON_Delete(doc);
    if (cJSON_GetArraySize(entries) >= MAX_SOURCE_INDEX_ENTRIES) {
      break;
    }
  }

  *entries_out = entries;
  if (err && err_size) err[0] = '\0';
  return 0;
}

static int
github_fetch_cheat_txt_indexes(const remote_source_t *src, cJSON **entries_out, char *err, size_t err_size) {
  if (!src || !entries_out) {
    if (err && err_size) snprintf(err, err_size, "invalid_source");
    return -1;
  }
  *entries_out = NULL;
  cJSON *entries = cJSON_CreateArray();
  if (!entries) {
    if (err && err_size) snprintf(err, err_size, "oom");
    return -1;
  }
  const char *fmts[] = {"json", "shn", "mc4"};
  int any_ok = 0;
  int any_conn_fail = 0;
  int any_404 = 0;
  for (int i = 0; i < 3; i++) {
    char rel[512];
    char url[1024];
    snprintf(rel, sizeof(rel), "%s/%s.txt", src->path, fmts[i]);
    build_github_raw_url(src, rel, url, sizeof(url));
    int status = -1;
    uint8_t *body = NULL;
    size_t body_len = 0;
    int frc = http_fetch_bytes_checked(url, 2 * 1024 * 1024, &status, &body, &body_len);
    if (frc != 0) {
      any_conn_fail++;
      cr_log("debug", "cheats.remote", "txt index fetch source=%s format=%s status=conn_fail rc=%d", src->name, fmts[i], frc);
      free(body);
      continue;
    }
    if (status == 403 && body_looks_rate_limited(body, body_len)) {
      free(body);
      cJSON_Delete(entries);
      if (err && err_size) snprintf(err, err_size, "rate_limited");
      return -1;
    }
    if (status == 404) {
      any_404++;
      cr_log("debug", "cheats.remote", "txt index fetch source=%s format=%s status=404", src->name, fmts[i]);
      free(body);
      continue;
    }
    if (status != 200) {
      cr_log("debug", "cheats.remote", "txt index fetch source=%s format=%s status=%d", src->name, fmts[i], status);
      free(body);
      continue;
    }
    any_ok = 1;
    char *txt = malloc(body_len + 1);
    if (!txt) {
      free(body);
      cJSON_Delete(entries);
      if (err && err_size) snprintf(err, err_size, "oom");
      return -1;
    }
    memcpy(txt, body, body_len);
    txt[body_len] = '\0';
    free(body);
    /* Scan the buffer for CUSA/PPSA tokens that carry a recognised cheat
       extension matching this format pass.  This is robust against .txt
       files that embed game names, '=' separators, or other decoration. */
    const char *p = txt;
    while (p && *p) {
      const char *cu = strstr(p, "CUSA");
      const char *pp = strstr(p, "PPSA");
      const char *tok = NULL;
      if (cu && pp) tok = (cu < pp) ? cu : pp;
      else if (cu)  tok = cu;
      else if (pp)  tok = pp;
      else          break;

      char token[256];
      size_t tlen = 0;
      const char *q = tok;
      while (*q && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r' &&
             *q != ',' && *q != ';' && *q != '=' && *q != '"' && *q != '\'' &&
             tlen + 1 < sizeof(token)) {
        token[tlen++] = *q++;
      }
      token[tlen] = '\0';
      p = (*q) ? q : NULL;

      if (tlen == 0) { if (p) p++; continue; }

      int kind = recognised_cheat_extension(token);
      const char *fmt_kind = (kind == 1) ? "json" : (kind == 2) ? "shn" : (kind == 3 ? "mc4" : "");
      if (!fmt_kind[0] || strcmp(fmt_kind, fmts[i]) != 0) continue;

      char rel_path[512];
      if (strchr(token, '/')) {
        snprintf(rel_path, sizeof(rel_path), "%s", token);
      } else {
        snprintf(rel_path, sizeof(rel_path), "%s/%s/%s", src->path, fmts[i], token);
      }
      if (!is_safe_repo_rel_path(rel_path)) continue;
      if (json_array_contains_path(entries, rel_path)) continue;

      const char *base = path_basename_ptr(rel_path);
      char raw_url[1024];
      build_github_raw_url(src, rel_path, raw_url, sizeof(raw_url));
      json_array_add_entry(entries, rel_path, base, fmts[i], 0, raw_url);
      if (cJSON_GetArraySize(entries) >= MAX_SOURCE_INDEX_ENTRIES) break;
    }
    free(txt);
    if (cJSON_GetArraySize(entries) >= MAX_SOURCE_INDEX_ENTRIES) {
      break;
    }
  }
  if (!any_ok || cJSON_GetArraySize(entries) == 0) {
    cJSON_Delete(entries);
    if (err && err_size) {
      if (any_ok) {
        snprintf(err, err_size, "index_empty");
      } else if (any_conn_fail > 0 && any_404 == 0) {
        /* Pure transport failure with no 404s — connectivity problem */
        snprintf(err, err_size, "network_error");
      } else {
        /* All 404, or mixed 404+transport (txt index files absent from repo) */
        snprintf(err, err_size, "index_missing");
      }
    }
    cr_log("info", "cheats.remote", "txt indexes source=%s ok=%d 404=%d conn_fail=%d err=%s",
           src->name, any_ok, any_404, any_conn_fail, (err && err[0]) ? err : "index_missing");
    return -1;
  }
  *entries_out = entries;
  if (err && err_size) err[0] = '\0';
  return 0;
}

void
cfg_get_cheat_remote_opts(int *sources_enabled, int *download_enabled, int *ttl_sec, int *max_bytes) {
  pthread_mutex_lock(&g_cfg_lock);
  if (sources_enabled) *sources_enabled = g_cfg.cheat_sources_enabled;
  if (download_enabled) *download_enabled = g_cfg.cheat_remote_download_enabled;
  if (ttl_sec) *ttl_sec = g_cfg.cheat_source_cache_ttl_seconds;
  if (max_bytes) *max_bytes = g_cfg.cheat_remote_max_file_bytes;
  pthread_mutex_unlock(&g_cfg_lock);
}

int
source_load_cheat_entries(const remote_source_t *src, int ttl_sec, cJSON **entries_out, char *err, size_t err_size) {
  if (!src || !entries_out) {
    if (err && err_size) snprintf(err, err_size, "invalid_source");
    return -1;
  }
  *entries_out = NULL;

  if (ttl_sec <= 0) {
    ttl_sec = 21600;
  }

  cJSON *cached = NULL;
  if (source_cache_load_entries(src->id, ttl_sec, &cached) && cached) {
    annotate_source_entries(cached, src);
    *entries_out = cached;
    if (err && err_size) err[0] = '\0';
    return 0;
  }

  int max_response_bytes = 2 * 1024 * 1024;
  cfg_get_cheat_remote_opts(NULL, NULL, NULL, &max_response_bytes);
  if (max_response_bytes < 256 * 1024) {
    max_response_bytes = 256 * 1024;
  }
  if (max_response_bytes > 16 * 1024 * 1024) {
    max_response_bytes = 16 * 1024 * 1024;
  }

  cJSON *entries = NULL;
  int status = -1;
  int truncated = 0;
  char tree_err[96] = {0};
  if (github_list_entries_tree(src, 1, &entries, max_response_bytes, &status, &truncated,
                               tree_err, sizeof(tree_err)) == 0 && entries) {
    annotate_source_entries(entries, src);
    source_cache_save_entries(src->id, ttl_sec, entries);
    *entries_out = entries;
    if (err && err_size) err[0] = '\0';
    return 0;
  }

  cr_log("warn", "cheats.remote", "tree failed source=%s status=%d reason=%s; trying contents fallback",
         src->name, status, tree_err[0] ? tree_err : "unknown");

  entries = NULL;
  char fb_err[96] = {0};
  if (github_list_entries_recursive(src, src->path, 1, &entries, max_response_bytes,
                                    fb_err, sizeof(fb_err)) == 0 && entries) {
    annotate_source_entries(entries, src);
    source_cache_save_entries(src->id, ttl_sec, entries);
    *entries_out = entries;
    if (err && err_size) err[0] = '\0';
    return 0;
  }

  cJSON *txt_entries = NULL;
  char txt_err[96] = {0};
  if (github_fetch_cheat_txt_indexes(src, &txt_entries, txt_err, sizeof(txt_err)) == 0 && txt_entries) {
    annotate_source_entries(txt_entries, src);
    source_cache_save_entries(src->id, ttl_sec, txt_entries);
    *entries_out = txt_entries;
    if (err && err_size) err[0] = '\0';
    return 0;
  }

  cJSON *stale = NULL;
  if (source_cache_load_stale(src->id, &stale) && stale) {
    cr_log("warn", "cheats.remote", "using stale source cache source=%s", src->name);
    annotate_source_entries(stale, src);
    *entries_out = stale;
    if (err && err_size) snprintf(err, err_size, "stale_cache");
    return 0;
  }

  if (err && err_size) {
    if (txt_err[0]) snprintf(err, err_size, "%s", txt_err);
    else if (fb_err[0]) snprintf(err, err_size, "%s", fb_err);
    else if (tree_err[0]) snprintf(err, err_size, "%s", tree_err);
    else snprintf(err, err_size, "network_error");
  }
  return -1;
}

int
parse_version_triplet(const char *s, int *a, int *b, int *c) {
  if (!s || !*s || !a || !b || !c) {
    return 0;
  }
  int v1 = -1, v2 = -1, v3 = -1;
  if (sscanf(s, "%d.%d.%d", &v1, &v2, &v3) == 3 && v1 >= 0 && v2 >= 0 && v3 >= 0) {
    *a = v1; *b = v2; *c = v3;
    return 1;
  }
  return 0;
}

void
extract_version_from_filename(const char *filename, char *out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (!filename) {
    return;
  }
  const char *base = path_basename_ptr(filename);
  const char *dot = strrchr(base, '.');
  const char *sep = strchr(base, '_');
  if (!sep || !dot || dot <= sep + 1) {
    return;
  }
  sep++;
  size_t n = (size_t)(dot - sep);
  if (n == 0 || n + 1 >= out_size) {
    return;
  }
  memcpy(out, sep, n);
  out[n] = '\0';
  for (size_t i = 0; i < n; i++) {
    if (!(isdigit((unsigned char)out[i]) || out[i] == '.')) {
      out[0] = '\0';
      return;
    }
  }
}

int
cheat_remote_match_score(const char *want_version, const char *cand_version) {
  if (want_version && want_version[0] && cand_version && cand_version[0]) {
    if (cr_version_equal_known(want_version, cand_version)) {
      return 300;
    }
  }
  return 150;
}

int
candidate_cmp_desc(const void *a, const void *b) {
  const remote_candidate_t *ca = (const remote_candidate_t *)a;
  const remote_candidate_t *cb = (const remote_candidate_t *)b;
  if (cb->score != ca->score) {
    return cb->score - ca->score;
  }
  return strcasecmp(ca->filename, cb->filename);
}

int
candidate_find_path(remote_candidate_t *arr, int n, const char *path) {
  for (int i = 0; i < n; i++) {
    if (strcmp(arr[i].path, path) == 0) {
      return i;
    }
  }
  return -1;
}

const remote_source_t *
find_source_by_name_or_id(remote_source_t *sources, int count, const char *name_or_id) {
  if (!sources || count <= 0 || !name_or_id || !name_or_id[0]) {
    return NULL;
  }
  for (int i = 0; i < count; i++) {
    if (!strcasecmp(sources[i].name, name_or_id) || !strcasecmp(sources[i].id, name_or_id)) {
      return &sources[i];
    }
  }
  return NULL;
}

int
extract_title_id_from_candidate_path(const char *path, char out[10]) {
  if (!path || !out) {
    return 0;
  }
  const char *base = path_basename_ptr(path);
  if (extract_title_id_prefix(base, out, 10)) {
    return 1;
  }
  return find_title_id_in_string(path, out);
}





