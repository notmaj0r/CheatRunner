#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "cr_log.h"

pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
log_entry_t     g_logs[MAX_LOG_ENTRIES];
int             g_log_start = 0;
int             g_log_count = 0;
int             g_log_next_seq = 0;
static volatile int g_log_min_level = CR_LOG_LEVEL_INFO;

static int
log_level_from_string(const char *level) {
  if (!level || !level[0]) {
    return CR_LOG_LEVEL_INFO;
  }
  if (strcasecmp(level, "debug") == 0) {
    return CR_LOG_LEVEL_DEBUG;
  }
  if (strcasecmp(level, "info") == 0) {
    return CR_LOG_LEVEL_INFO;
  }
  if (strcasecmp(level, "warn") == 0 || strcasecmp(level, "warning") == 0) {
    return CR_LOG_LEVEL_WARN;
  }
  if (strcasecmp(level, "error") == 0) {
    return CR_LOG_LEVEL_ERROR;
  }
  return CR_LOG_LEVEL_INFO;
}

static const char *
log_level_to_string(int level) {
  switch (level) {
  case CR_LOG_LEVEL_DEBUG: return "debug";
  case CR_LOG_LEVEL_INFO: return "info";
  case CR_LOG_LEVEL_WARN: return "warn";
  case CR_LOG_LEVEL_ERROR: return "error";
  default: return "info";
  }
}

void
log_push(const char *level, const char *tag, const char *message) {
  pthread_mutex_lock(&g_log_lock);
  time_t now = time(NULL);
  if (g_log_count > 0) {
    int prev_idx = (g_log_start + g_log_count - 1) % MAX_LOG_ENTRIES;
    if (!strcmp(g_logs[prev_idx].level, level ? level : "info") &&
        !strcmp(g_logs[prev_idx].tag, tag ? tag : "core") &&
        !strcmp(g_logs[prev_idx].message, message ? message : "") &&
        (now - g_logs[prev_idx].ts) <= 5) {
      pthread_mutex_unlock(&g_log_lock);
      return;
    }
  }
  int idx = (g_log_start + g_log_count) % MAX_LOG_ENTRIES;
  if (g_log_count == MAX_LOG_ENTRIES) {
    idx = g_log_start;
    g_log_start = (g_log_start + 1) % MAX_LOG_ENTRIES;
  } else {
    g_log_count++;
  }
  g_logs[idx].seq = g_log_next_seq++;
  g_logs[idx].ts = now;
  snprintf(g_logs[idx].level, sizeof(g_logs[idx].level), "%s", level ? level : "info");
  snprintf(g_logs[idx].tag, sizeof(g_logs[idx].tag), "%s", tag ? tag : "core");
  snprintf(g_logs[idx].message, sizeof(g_logs[idx].message), "%s", message ? message : "");
  pthread_mutex_unlock(&g_log_lock);
}

void
cr_log_set_level(const char *level) {
  g_log_min_level = log_level_from_string(level);
}

const char *
cr_log_get_level(void) {
  return log_level_to_string(g_log_min_level);
}

int
cr_log_level_enabled(const char *level) {
  int msg_level = log_level_from_string(level);
  return msg_level >= g_log_min_level;
}

void
cr_log(const char *level, const char *tag, const char *fmt, ...) {
  if (!cr_log_level_enabled(level)) {
    return;
  }
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  printf("[%s] %s\n", tag ? tag : "core", buf);
  log_push(level, tag, buf);
}

void
log_msg(const char *fmt, ...) {
  if (!cr_log_level_enabled("info")) {
    return;
  }
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  printf("%s\n", buf);
  log_push("info", "core", buf);
}
