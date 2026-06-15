#ifndef CR_SOURCE_JOBS_H
#define CR_SOURCE_JOBS_H

#include <pthread.h>
#include <stddef.h>
#include <time.h>

#include "third_party/cJSON.h"

typedef enum {
  CR_JOB_CHEAT_FIND = 1,
  CR_JOB_CHEAT_DOWNLOAD
} cr_job_type_t;

typedef enum {
  CR_JOB_PENDING = 0,
  CR_JOB_RUNNING,
  CR_JOB_DONE,
  CR_JOB_FAILED
} cr_job_state_t;

#define CR_JOB_MAX 16
#define CR_JOB_EXPIRE_SEC 300

typedef struct {
  int id;
  cr_job_type_t type;
  cr_job_state_t state;
  char title_id[16];
  char version[64];
  char body_json[16384];
  cJSON *result;
  char error[128];
  int http_status;
  time_t created_at;
  int used;
  size_t dl_recv;   /* bytes received so far (download jobs only) */
  size_t dl_total;  /* Content-Length from server; 0 if unknown    */
} cr_source_job_t;

extern pthread_mutex_t g_jobs_lock;
extern cr_source_job_t g_jobs[CR_JOB_MAX];
extern int g_next_job_id;

/* Public API */
void   cr_source_jobs_init(void);
void   cr_source_jobs_cleanup(void);
int    cr_source_job_start(cJSON *body_json, int *out_job_id);
cJSON *cr_source_job_status_json(int job_id);
int    cr_source_jobs_is_busy(void);

#endif /* CR_SOURCE_JOBS_H */
