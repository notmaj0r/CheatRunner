#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "third_party/cJSON.h"
#include "cr_log.h"
#include "cr_paths.h"
#include "cr_config.h"
#include "cr_json.h"
#include "cr_cheat_formats.h"
#include "cr_cheats.h"
#include "cr_memory.h"
#include "cr_http.h"
#include "cr_appdb.h"
#include "cr_titles.h"
#include "cr_remote_sources.h"
#include "cr_source_jobs.h"

pthread_mutex_t g_jobs_lock = PTHREAD_MUTEX_INITIALIZER;
cr_source_job_t g_jobs[CR_JOB_MAX];
int g_next_job_id = 1;

/* ---- Forward declarations for internal work functions ---- */
static cJSON *remote_cheat_find_work(const char *title_id, const char *version,
                                      char *err, size_t err_size, int *http_status_out);
static cJSON *remote_cheat_download_work(const char *body_json,
                                          char *err, size_t err_size, int *http_status_out);

/* ---- Public API ---- */

void
cr_source_jobs_init(void) {
  /* g_jobs_lock is statically initialised; nothing extra needed */
  pthread_mutex_lock(&g_jobs_lock);
  memset(g_jobs, 0, sizeof(g_jobs));
  g_next_job_id = 1;
  pthread_mutex_unlock(&g_jobs_lock);
}

void
cr_source_jobs_cleanup(void) {
  pthread_mutex_lock(&g_jobs_lock);
  for (int i = 0; i < CR_JOB_MAX; i++) {
    if (g_jobs[i].used && g_jobs[i].result) {
      cJSON_Delete(g_jobs[i].result);
    }
    memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
  }
  pthread_mutex_unlock(&g_jobs_lock);
}

/* cr_source_job_start: parses body_json (a cJSON object already parsed by caller),
   allocates a slot, spawns a thread, returns 0 on success with *out_job_id set.
   Returns non-zero on error (caller should send appropriate HTTP error). */

static void *source_job_thread(void *arg);

int
cr_source_job_start(cJSON *req, int *out_job_id) {
  if (!req || !out_job_id) return -1;

  cJSON *type_j = cJSON_GetObjectItem(req, "type");
  if (!cJSON_IsString(type_j) || !type_j->valuestring) return -1;

  cr_job_type_t type;
  if      (!strcmp(type_j->valuestring, "cheat_find"))     type = CR_JOB_CHEAT_FIND;
  else if (!strcmp(type_j->valuestring, "cheat_download")) type = CR_JOB_CHEAT_DOWNLOAD;
  else return -2; /* unknown type */

  char title_id[16]     = {0};
  char version[64]      = {0};
  char body_copy[16384] = {0};

  if (type == CR_JOB_CHEAT_FIND) {
    cJSON *tid_j = cJSON_GetObjectItem(req, "titleId");
    if (!cJSON_IsString(tid_j) || !tid_j->valuestring ||
        !title_id_normalize(tid_j->valuestring, title_id)) {
      return -3; /* invalid titleId */
    }
    cJSON *ver_j = cJSON_GetObjectItem(req, "version");
    if (cJSON_IsString(ver_j) && ver_j->valuestring)
      snprintf(version, sizeof(version), "%s", ver_j->valuestring);
  } else {
    /* For download jobs, re-serialise the request into body_copy */
    char *s = cJSON_PrintUnformatted(req);
    if (s) {
      snprintf(body_copy, sizeof(body_copy), "%s", s);
      free(s);
    }
  }

  pthread_mutex_lock(&g_jobs_lock);
  time_t now = time(NULL);
  /* Expire old finished jobs */
  for (int i = 0; i < CR_JOB_MAX; i++) {
    if (g_jobs[i].used && g_jobs[i].state != CR_JOB_RUNNING &&
        now - g_jobs[i].created_at > CR_JOB_EXPIRE_SEC) {
      if (g_jobs[i].result) { cJSON_Delete(g_jobs[i].result); }
      memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
    }
  }
  cr_source_job_t *slot = NULL;
  for (int i = 0; i < CR_JOB_MAX; i++) {
    if (!g_jobs[i].used) { slot = &g_jobs[i]; break; }
  }
  if (!slot) {
    pthread_mutex_unlock(&g_jobs_lock);
    return -4; /* queue full */
  }
  memset(slot, 0, sizeof(*slot));
  slot->id          = g_next_job_id++;
  if (g_next_job_id <= 0) g_next_job_id = 1;
  slot->type        = type;
  slot->state       = CR_JOB_PENDING;
  slot->http_status = 200;
  slot->created_at  = now;
  slot->used        = 1;
  if (type == CR_JOB_CHEAT_FIND) {
    snprintf(slot->title_id, sizeof(slot->title_id), "%s", title_id);
    snprintf(slot->version,  sizeof(slot->version),  "%s", version);
  } else {
    snprintf(slot->body_json, sizeof(slot->body_json), "%s", body_copy);
  }
  int job_id = slot->id;
  pthread_mutex_unlock(&g_jobs_lock);

  int *id_arg = malloc(sizeof(int));
  if (!id_arg) {
    pthread_mutex_lock(&g_jobs_lock);
    for (int i = 0; i < CR_JOB_MAX; i++) {
      if (g_jobs[i].used && g_jobs[i].id == job_id) { memset(&g_jobs[i], 0, sizeof(g_jobs[i])); break; }
    }
    pthread_mutex_unlock(&g_jobs_lock);
    return -5; /* oom */
  }
  *id_arg = job_id;
  pthread_t tid;
  if (pthread_create(&tid, NULL, source_job_thread, id_arg) != 0) {
    free(id_arg);
    pthread_mutex_lock(&g_jobs_lock);
    for (int i = 0; i < CR_JOB_MAX; i++) {
      if (g_jobs[i].used && g_jobs[i].id == job_id) { memset(&g_jobs[i], 0, sizeof(g_jobs[i])); break; }
    }
    pthread_mutex_unlock(&g_jobs_lock);
    return -6; /* thread create failed */
  }
  pthread_detach(tid);
  *out_job_id = job_id;
  return 0;
}

/* cr_source_job_status_json: returns a heap-allocated cJSON object with job status.
   Caller must cJSON_Delete it. Returns NULL on OOM. */
cJSON *
cr_source_job_status_json(int job_id) {
  pthread_mutex_lock(&g_jobs_lock);
  cr_job_state_t state = CR_JOB_PENDING;
  int http_status = 200;
  char error[128] = {0};
  char *result_txt = NULL;
  int found = 0;
  for (int i = 0; i < CR_JOB_MAX; i++) {
    if (g_jobs[i].used && g_jobs[i].id == job_id) {
      found = 1;
      state = g_jobs[i].state;
      http_status = g_jobs[i].http_status;
      if (state == CR_JOB_DONE && g_jobs[i].result)
        result_txt = cJSON_PrintUnformatted(g_jobs[i].result);
      else if (state == CR_JOB_FAILED)
        snprintf(error, sizeof(error), "%s", g_jobs[i].error);
      break;
    }
  }
  pthread_mutex_unlock(&g_jobs_lock);

  cJSON *out = cJSON_CreateObject();
  if (!out) { free(result_txt); return NULL; }

  if (!found) {
    cJSON_AddBoolToObject(out, "ok", 0);
    cJSON_AddStringToObject(out, "error", "job_not_found");
    cJSON_AddStringToObject(out, "message", "Job not found or expired.");
    cJSON_AddNumberToObject(out, "notFound", 1);
    return out;
  }

  if (state == CR_JOB_PENDING || state == CR_JOB_RUNNING) {
    cJSON_AddStringToObject(out, "state", state == CR_JOB_PENDING ? "pending" : "running");
    return out;
  }

  if (state == CR_JOB_DONE) {
    cJSON_AddStringToObject(out, "state", "done");
    cJSON_AddNumberToObject(out, "httpStatus", http_status);
    if (result_txt) {
      cJSON *r = cJSON_Parse(result_txt);
      if (r) cJSON_AddItemToObject(out, "result", r);
      free(result_txt);
    }
    return out;
  }

  /* FAILED */
  free(result_txt);
  static const struct { const char *code; const char *msg; } err_map[] = {
    { "network_unavailable", "Network unavailable. Check PS5 internet connection or use local files." },
    { "network_error",       "Network request failed. Check PS5 internet connection or try again." },
    { "rate_limited",        "GitHub rate limit hit. Try again later or use local files." },
    { "file_too_large",      "Remote file exceeds max size." },
    { "source_not_found",    "Remote source was not found." },
    { "save_failed",         "Could not save file." },
    { "download_failed",     "GitHub request failed." },
    { NULL, NULL }
  };
  const char *msg = error;
  for (int i = 0; err_map[i].code; i++) {
    if (!strcmp(error, err_map[i].code)) { msg = err_map[i].msg; break; }
  }
  cJSON_AddStringToObject(out, "state", "failed");
  cJSON_AddNumberToObject(out, "httpStatus", http_status);
  cJSON_AddStringToObject(out, "error", error);
  cJSON_AddStringToObject(out, "message", msg);
  return out;
}

int
cr_source_jobs_is_busy(void) {
  int busy = 0;
  pthread_mutex_lock(&g_jobs_lock);
  for (int i = 0; i < CR_JOB_MAX; i++) {
    if (g_jobs[i].used && g_jobs[i].state == CR_JOB_RUNNING) {
      busy = 1;
      break;
    }
  }
  pthread_mutex_unlock(&g_jobs_lock);
  return busy;
}

/* ---- Job worker thread ---- */

static void *
source_job_thread(void *arg) {
  int job_id = *(int *)arg;
  free(arg);

  pthread_mutex_lock(&g_jobs_lock);
  int slot_idx = -1;
  for (int i = 0; i < CR_JOB_MAX; i++) {
    if (g_jobs[i].used && g_jobs[i].id == job_id) { slot_idx = i; break; }
  }
  if (slot_idx < 0) { pthread_mutex_unlock(&g_jobs_lock); return NULL; }
  g_jobs[slot_idx].state = CR_JOB_RUNNING;
  cr_job_type_t type = g_jobs[slot_idx].type;
  char title_id[16], version[64], body_copy[16384];
  snprintf(title_id,  sizeof(title_id),  "%s", g_jobs[slot_idx].title_id);
  snprintf(version,   sizeof(version),   "%s", g_jobs[slot_idx].version);
  snprintf(body_copy, sizeof(body_copy), "%s", g_jobs[slot_idx].body_json);
  pthread_mutex_unlock(&g_jobs_lock);

  char err[128] = {0};
  int http_status = 200;
  cJSON *result = NULL;

  cr_log("info", "remote.job", "start id=%d type=%d", job_id, (int)type);

  switch (type) {
    case CR_JOB_CHEAT_FIND:
      result = remote_cheat_find_work(title_id, version, err, sizeof(err), &http_status);
      break;
    case CR_JOB_CHEAT_DOWNLOAD:
      result = remote_cheat_download_work(body_copy, err, sizeof(err), &http_status);
      break;
    default:
      snprintf(err, sizeof(err), "unknown_job_type");
      http_status = 500;
      break;
  }

  pthread_mutex_lock(&g_jobs_lock);
  for (int i = 0; i < CR_JOB_MAX; i++) {
    if (g_jobs[i].used && g_jobs[i].id == job_id) {
      g_jobs[i].http_status = http_status;
      if (result) {
        g_jobs[i].result = result;
        g_jobs[i].state  = CR_JOB_DONE;
        cr_log("info", "remote.job", "done id=%d http=%d", job_id, http_status);
      } else {
        snprintf(g_jobs[i].error, sizeof(g_jobs[i].error), "%s", err[0] ? err : "unknown_error");
        g_jobs[i].state = CR_JOB_FAILED;
        cr_log("warn", "remote.job", "failed id=%d err=%s", job_id, g_jobs[i].error);
      }
      break;
    }
  }
  pthread_mutex_unlock(&g_jobs_lock);
  return NULL;
}

/* ============================================================
   Internal work functions (moved from cr_api.c)
   ============================================================ */

static cJSON *
remote_cheat_find_work(const char *title_id, const char *version,
                        char *err, size_t err_size, int *http_status_out) {
  int sources_enabled = 1, download_enabled = 1, ttl_sec = 21600;
  cfg_get_cheat_remote_opts(&sources_enabled, &download_enabled, &ttl_sec, NULL);
  if (!sources_enabled || !download_enabled) {
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", 1);
    cJSON_AddStringToObject(out, "titleId", title_id);
    cJSON_AddStringToObject(out, "version", version);
    cJSON_AddArrayToObject(out, "candidates");
    if (http_status_out) *http_status_out = 200;
    return out;
  }
  source_config_model_t model;
  source_model_load(&model);
  remote_candidate_t candidates[MAX_REMOTE_CANDIDATES];
  int cand_n = 0, attempted_sources = 0, network_failures = 0, rate_limited_failures = 0;
  for (int i = 0; i < model.cheat_count; i++) {
    remote_source_t *src = &model.cheat_sources[i];
    if (!src->enabled || strcasecmp(src->type, "github") != 0) continue;
    attempted_sources++;
    cr_log("info", "cheats.remote", "searching source=%s title=%s version=%s",
           src->name, title_id, version[0] ? version : "unknown");
    cJSON *entries = NULL;
    char lerr[96] = {0};
    if (source_load_cheat_entries(src, ttl_sec, &entries, lerr, sizeof(lerr)) != 0 || !entries) {
      if (!strcmp(lerr, "network_error")) network_failures++;
      else if (!strcmp(lerr, "rate_limited")) rate_limited_failures++;
      cr_log("warn", "cheats.remote", "source load failed source=%s reason=%s", src->name, lerr);
      continue;
    }
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, entries) {
      cJSON *path_j   = cJSON_GetObjectItem(it, "path");
      cJSON *name_j   = cJSON_GetObjectItem(it, "name");
      cJSON *format_j = cJSON_GetObjectItem(it, "format");
      cJSON *dl_j     = cJSON_GetObjectItem(it, "downloadUrl");
      cJSON *size_j   = cJSON_GetObjectItem(it, "size");
      if (!cJSON_IsString(path_j) || !path_j->valuestring ||
          !cJSON_IsString(name_j) || !name_j->valuestring) continue;
      char cand_title[10] = {0};
      if (!extract_title_id_from_candidate_path(path_j->valuestring, cand_title)) continue;
      if (strcmp(cand_title, title_id) != 0) continue;
      const char *fname = name_j->valuestring;
      int kind = recognised_cheat_extension(fname);
      if (!kind) continue;
      char cand_ver[64] = {0};
      extract_version_from_filename(fname, cand_ver, sizeof(cand_ver));
      int score = cheat_remote_match_score(version, cand_ver);
      int idx = candidate_find_path(candidates, cand_n, path_j->valuestring);
      if (idx < 0) {
        if (cand_n >= MAX_REMOTE_CANDIDATES) continue;
        idx = cand_n++;
        memset(&candidates[idx], 0, sizeof(candidates[idx]));
        snprintf(candidates[idx].source,    sizeof(candidates[idx].source),    "%s", src->name);
        snprintf(candidates[idx].source_id, sizeof(candidates[idx].source_id), "%s", src->id);
        snprintf(candidates[idx].filename,  sizeof(candidates[idx].filename),  "%s", fname);
        snprintf(candidates[idx].path,      sizeof(candidates[idx].path),      "%s", path_j->valuestring);
      }
      candidates[idx].score = score;
      candidates[idx].size  = cJSON_IsNumber(size_j) ? size_j->valueint : 0;
      snprintf(candidates[idx].format, sizeof(candidates[idx].format), "%s",
               cJSON_IsString(format_j) && format_j->valuestring && format_j->valuestring[0]
                 ? format_j->valuestring : (kind == 1 ? "json" : (kind == 2 ? "shn" : "mc4")));
      snprintf(candidates[idx].version, sizeof(candidates[idx].version), "%s", cand_ver);
      if (cJSON_IsString(dl_j) && dl_j->valuestring && dl_j->valuestring[0])
        snprintf(candidates[idx].download_url, sizeof(candidates[idx].download_url), "%s", dl_j->valuestring);
      else
        build_github_raw_url(src, candidates[idx].path, candidates[idx].download_url, sizeof(candidates[idx].download_url));
    }
    cJSON_Delete(entries);
  }
  if (cand_n > 1) qsort(candidates, (size_t)cand_n, sizeof(candidates[0]), candidate_cmp_desc);
  if (cand_n == 0 && attempted_sources > 0 && rate_limited_failures == attempted_sources) {
    if (err) snprintf(err, err_size, "rate_limited");
    if (http_status_out) *http_status_out = 429;
    return NULL;
  }
  if (cand_n == 0 && attempted_sources > 0 && network_failures == attempted_sources) {
    if (err) snprintf(err, err_size, "network_unavailable");
    if (http_status_out) *http_status_out = 503;
    return NULL;
  }
  cr_log("info", "cheats.remote", "candidates title=%s count=%d", title_id, cand_n);
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "ok", 1);
  cJSON_AddStringToObject(out, "titleId", title_id);
  cJSON_AddStringToObject(out, "version", version);
  cJSON *arr = cJSON_AddArrayToObject(out, "candidates");
  for (int i = 0; i < cand_n; i++) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "source",      candidates[i].source);
    cJSON_AddStringToObject(e, "sourceId",    candidates[i].source_id);
    cJSON_AddStringToObject(e, "format",      candidates[i].format);
    cJSON_AddStringToObject(e, "filename",    candidates[i].filename);
    cJSON_AddStringToObject(e, "version",     candidates[i].version);
    cJSON_AddNumberToObject(e, "score",       candidates[i].score);
    cJSON_AddStringToObject(e, "downloadUrl", candidates[i].download_url);
    cJSON_AddStringToObject(e, "path",        candidates[i].path);
    cJSON_AddNumberToObject(e, "size",        candidates[i].size);
    cJSON_AddItemToArray(arr, e);
  }
  if (http_status_out) *http_status_out = 200;
  return out;
}

static cJSON *
remote_cheat_download_work(const char *body_json,
                            char *err, size_t err_size, int *http_status_out) {
  int sources_enabled = 1, download_enabled = 1, max_bytes = 1048576;
  cfg_get_cheat_remote_opts(&sources_enabled, &download_enabled, NULL, &max_bytes);
  if (!sources_enabled || !download_enabled) {
    if (err) snprintf(err, err_size, "remote_download_disabled");
    if (http_status_out) *http_status_out = 400;
    return NULL;
  }
  cJSON *req = NULL;
  if (parse_json_from_body(body_json, &req) != 0) {
    if (err) snprintf(err, err_size, "invalid_json");
    if (http_status_out) *http_status_out = 400;
    return NULL;
  }
  cJSON *source_j    = cJSON_GetObjectItem(req, "source");
  cJSON *path_j      = cJSON_GetObjectItem(req, "path");
  cJSON *format_j    = cJSON_GetObjectItem(req, "format");
  cJSON *title_j     = cJSON_GetObjectItem(req, "titleId");
  cJSON *overwrite_j = cJSON_GetObjectItem(req, "overwrite");
  int overwrite = cJSON_IsTrue(overwrite_j) ? 1 : 0;
  if (!cJSON_IsString(source_j) || !source_j->valuestring ||
      !cJSON_IsString(path_j)   || !path_j->valuestring   ||
      !cJSON_IsString(format_j) || !format_j->valuestring) {
    cJSON_Delete(req);
    if (err) snprintf(err, err_size, "missing_fields");
    if (http_status_out) *http_status_out = 400;
    return NULL;
  }
  source_config_model_t model;
  source_model_load(&model);
  const remote_source_t *src = find_source_by_name_or_id(model.cheat_sources, model.cheat_count, source_j->valuestring);
  if (!src || !src->enabled || strcasecmp(src->type, "github") != 0) {
    cJSON_Delete(req);
    if (err) snprintf(err, err_size, "source_not_found");
    if (http_status_out) *http_status_out = 404;
    return NULL;
  }
  const char *repo_path = path_j->valuestring;
  if (!is_safe_repo_rel_path(repo_path) ||
      strncmp(repo_path, src->path, strlen(src->path)) != 0 ||
      repo_path[strlen(src->path)] != '/') {
    cJSON_Delete(req);
    if (err) snprintf(err, err_size, "invalid_path");
    if (http_status_out) *http_status_out = 400;
    return NULL;
  }
  const char *filename = path_basename_ptr(repo_path);
  if (!is_safe_filename(filename)) {
    cJSON_Delete(req);
    if (err) snprintf(err, err_size, "invalid_filename");
    if (http_status_out) *http_status_out = 400;
    return NULL;
  }
  int kind = recognised_cheat_extension(filename);
  const char *fmt = kind == 1 ? "json" : (kind == 2 ? "shn" : (kind == 3 ? "mc4" : ""));
  if (!fmt[0] || strcasecmp(fmt, format_j->valuestring) != 0) {
    cJSON_Delete(req);
    if (err) snprintf(err, err_size, "invalid_format");
    if (http_status_out) *http_status_out = 400;
    return NULL;
  }
  if (cJSON_IsString(title_j) && title_j->valuestring && title_j->valuestring[0]) {
    char want_title[10] = {0}, cand_title[10] = {0};
    if (!title_id_normalize(title_j->valuestring, want_title) ||
        !extract_title_id_from_candidate_path(repo_path, cand_title) ||
        strcmp(want_title, cand_title) != 0) {
      cJSON_Delete(req);
      if (err) snprintf(err, err_size, "title_mismatch");
      if (http_status_out) *http_status_out = 400;
      return NULL;
    }
  }
  char dst_dir[512];
  snprintf(dst_dir, sizeof(dst_dir), "%s/%s", CHEATRUNNER_CHEATS_DIR, fmt);
  ensure_dir_recursive(dst_dir);
  char final_path[768];
  snprintf(final_path, sizeof(final_path), "%s/%s", dst_dir, filename);
  struct stat stb;
  if (stat(final_path, &stb) == 0 && !overwrite) {
    cJSON_Delete(req);
    if (http_status_out) *http_status_out = 409;
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", 0);
    cJSON_AddStringToObject(out, "error", "file_exists");
    cJSON_AddStringToObject(out, "message", "Cheat file already exists.");
    cJSON_AddStringToObject(out, "existingPath", final_path);
    return out;
  }
  char url[1024];
  build_github_raw_url(src, repo_path, url, sizeof(url));
  cr_log("info", "cheats.remote", "downloading source=%s path=%s", src->name, repo_path);
  cJSON_Delete(req);
  int dl_status = -1;
  uint8_t *data = NULL;
  size_t len = 0;
  int frc = http_fetch_bytes_checked(url, (size_t)max_bytes, &dl_status, &data, &len);
  if (frc == -2) {
    if (err) snprintf(err, err_size, "file_too_large");
    if (http_status_out) *http_status_out = 400;
    return NULL;
  }
  if (frc != 0) {
    cr_log("warn", "cheats.remote", "download failed reason=network_error");
    if (err) snprintf(err, err_size, "network_unavailable");
    if (http_status_out) *http_status_out = 503;
    return NULL;
  }
  if (dl_status == 403 && body_looks_rate_limited(data, len)) {
    free(data);
    cr_log("warn", "cheats.remote", "download failed reason=rate_limited");
    if (err) snprintf(err, err_size, "rate_limited");
    if (http_status_out) *http_status_out = 429;
    return NULL;
  }
  if (dl_status != 200) {
    free(data);
    cr_log("warn", "cheats.remote", "download failed reason=http_%d", dl_status);
    if (err) snprintf(err, err_size, "download_failed");
    if (http_status_out) *http_status_out = 502;
    return NULL;
  }
  if (write_file_atomic(final_path, data, len) != 0) {
    free(data);
    cr_log("warn", "cheats.remote", "download failed reason=write_failed");
    if (err) snprintf(err, err_size, "save_failed");
    if (http_status_out) *http_status_out = 500;
    return NULL;
  }
  free(data);
  cr_log("info", "cheats.remote", "saved %s", final_path);
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "ok", 1);
  cJSON_AddStringToObject(out, "savedPath", final_path);
  cJSON_AddStringToObject(out, "message", "Cheat downloaded.");
  if (http_status_out) *http_status_out = 200;
  return out;
}


