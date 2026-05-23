#ifndef CR_REMOTE_SOURCES_H
#define CR_REMOTE_SOURCES_H

#include <stddef.h>
#include <stdint.h>
#include "third_party/cJSON.h"

#define MAX_REMOTE_SOURCES 8
#define MAX_REMOTE_CANDIDATES 64
#define MAX_SOURCE_INDEX_ENTRIES 4096

typedef struct remote_source {
  char id[64];
  char name[64];
  char type[16];
  char owner[64];
  char repo[128];
  char branch[64];
  char path[256];
  int enabled;
} remote_source_t;

typedef struct source_config_model {
  int cheat_count;
  remote_source_t cheat_sources[MAX_REMOTE_SOURCES];
} source_config_model_t;

typedef struct remote_candidate {
  char source[64];
  char source_id[64];
  char owner[64];
  char repo[128];
  char branch[64];
  char format[16];
  char filename[256];
  char version[64];
  char path[512];
  char download_url[1024];
  int score;
  int size;
} remote_candidate_t;

void source_model_load(source_config_model_t *m);
int body_looks_rate_limited(const uint8_t *data, size_t len);
int http_fetch_bytes_checked(const char *url, size_t max_bytes, int *status_out, uint8_t **data_out, size_t *len_out);
void build_github_raw_url(const remote_source_t *src, const char *path, char *out, size_t out_size);
void cfg_get_cheat_remote_opts(int *sources_enabled, int *download_enabled, int *ttl_sec, int *max_bytes);
int source_load_cheat_entries(const remote_source_t *src, int ttl_sec, cJSON **entries_out, char *err, size_t err_size);
int parse_version_triplet(const char *s, int *a, int *b, int *c);
void extract_version_from_filename(const char *filename, char *out, size_t out_size);
int cheat_remote_match_score(const char *want_version, const char *cand_version);
int candidate_cmp_desc(const void *a, const void *b);
int candidate_find_path(remote_candidate_t *arr, int n, const char *path);
const remote_source_t *find_source_by_name_or_id(remote_source_t *sources, int count, const char *name_or_id);
int extract_title_id_from_candidate_path(const char *path, char out[10]);

#endif
