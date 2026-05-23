#ifndef CR_REPO_MIRROR_H
#define CR_REPO_MIRROR_H

#include <pthread.h>
#include <time.h>

#define REPO_MIRROR_MISSING_MAX      20
#define REPO_MIRROR_MISSING_NAME_MAX 96

typedef enum {
  REPO_MIRROR_IDLE    = 0,
  REPO_MIRROR_RUNNING = 1,
  REPO_MIRROR_DONE    = 2,
  REPO_MIRROR_ERROR   = 3,
} repo_mirror_state_t;

typedef struct {
  pthread_mutex_t     lock;
  repo_mirror_state_t state;
  char  source[32];
  int   total;
  int   downloaded;
  int   skipped;
  int   failed;
  int   verified;
  int   missing_count;
  char  missing[REPO_MIRROR_MISSING_MAX][REPO_MIRROR_MISSING_NAME_MAX];
  char  current[128];
  char  error[256];
  char  mode[32];      /* "git_tree" or "contents_fallback" */
  char  warning[64];   /* e.g. "git_tree_truncated" */
  int   truncated;
  int   complete;
  int   source_idx;    /* current source index when source=all */
  int   source_total;  /* total sources when source=all */
  time_t started_at;
  time_t finished_at;
} repo_mirror_progress_t;

extern repo_mirror_progress_t g_repo_mirror;

/* Start a background download for 'source' (hencollection / ps5cheats / goldhen / all).
 * If overwrite=0, existing files are skipped.
 * Returns 0 if the thread was started, -1 if already running or source invalid. */
int repo_mirror_start(const char *source, int overwrite);

/* Serialise current progress to buf as a JSON object. */
void repo_mirror_status_json(char *buf, size_t buf_size);

#endif /* CR_REPO_MIRROR_H */
