#ifndef CR_LAUNCH_H
#define CR_LAUNCH_H

#include <pthread.h>
#include <stdint.h>

typedef struct launch_status_state {
  int busy;
  char phase[32];
  char title_id[16];
  char message[160];
  int last_ok;
  char method[40];
  int rc;
  int verified;
  char hex[16];
  int foreground_user_valid;
} launch_status_state_t;

typedef struct launch_worker_request {
  char title_id[16];
  char args[512];
} launch_worker_request_t;

extern pthread_mutex_t g_launch_status_lock;
extern launch_status_state_t g_launch_status;
extern volatile uint64_t g_last_launch_verified_at_ms;

void set_launch_status(int busy, const char *phase, const char *title_id, const char *message, int last_ok);
void set_launch_status_ex(int busy, const char *phase, const char *title_id, const char *message, int last_ok,
                          const char *method, int rc, int verified);
void *launch_worker_thread(void *arg);

#endif
