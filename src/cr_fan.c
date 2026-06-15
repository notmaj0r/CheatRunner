#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "third_party/cJSON.h"
#include "cr_api_internal.h"
#include "cr_config.h"
#include "cr_fan.h"
#include "cr_log.h"
#include "cr_paths.h"

/* Provided by libkernel (kernel_sys). Declared locally — the SDK headers
 * CheatRunner pulls in don't expose these. */
int sceKernelGetCpuTemperature(int *out_celsius);
int sceKernelGetSocSensorTemperature(int sensor_id, int *out_celsius);

#define ICC_FAN_DEVICE           "/dev/icc_fan"
#define ICC_FAN_THRESHOLD_IOCTL  0xC01C8F07ul
#define ICC_FAN_GET_MANUAL_DUTY  0xC0068F06ul

#define FAN_REAPPLY_SEC  15

static void
fan_get_range(int *min_c, int *max_c) {
  pthread_mutex_lock(&g_cfg_lock);
  *min_c = g_cfg.fan_min_c;
  *max_c = g_cfg.fan_max_c;
  pthread_mutex_unlock(&g_cfg_lock);
}

static atomic_int g_pinned_threshold_c  = 0;  /* 0 = nothing pinned yet */
static atomic_int g_watcher_started      = 0;

/* ----------------------------------------------------------------------- */
/* Persistence                                                             */

static void
fan_save_threshold(int temp_c) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return;
  cJSON_AddNumberToObject(root, "thresholdC", temp_c);
  char *txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!txt) return;
  write_file_atomic(CHEATRUNNER_FAN_PATH, (const uint8_t *)txt, strlen(txt));
  free(txt);
}

static int
fan_load_threshold(void) {
  char *txt = NULL;
  int val = 0;
  if (read_file_text(CHEATRUNNER_FAN_PATH, &txt) == 0 && txt) {
    cJSON *root = cJSON_Parse(txt);
    if (root) {
      cJSON *t = cJSON_GetObjectItem(root, "thresholdC");
      if (cJSON_IsNumber(t)) val = (int)t->valuedouble;
      cJSON_Delete(root);
    }
  }
  free(txt);
  int min_c, max_c;
  fan_get_range(&min_c, &max_c);
  if (val && (val < min_c || val > max_c)) val = 0;
  return val;
}

/* ----------------------------------------------------------------------- */
/* Device access                                                           */

/* Apply a fan-on threshold via the icc_fan ioctl. Returns the ioctl result
 * (0 on success). On failure, *errno_out (if non-NULL) carries errno. */
static int
fan_set_threshold(int temp_c, int *errno_out, unsigned char duty_out[6]) {
  int min_c, max_c;
  fan_get_range(&min_c, &max_c);
  if (temp_c < min_c) temp_c = min_c;
  if (temp_c > max_c) temp_c = max_c;
  if (errno_out) *errno_out = 0;

  int fd = open(ICC_FAN_DEVICE, O_RDONLY);
  if (fd < 0) {
    if (errno_out) *errno_out = errno;
    return -1;
  }

  /* Threshold byte sits at offset 5 of the 10-byte ioctl buffer. */
  unsigned char data[10] = {0, 0, 0, 0, 0, (unsigned char)temp_c, 0, 0, 0, 0};
  int rc = ioctl(fd, ICC_FAN_THRESHOLD_IOCTL, data);
  int eno = errno;

  if (duty_out) {
    unsigned char duty[6] = {0};
    if (ioctl(fd, ICC_FAN_GET_MANUAL_DUTY, duty) == 0) {
      memcpy(duty_out, duty, 6);
    }
  }

  close(fd);
  if (rc < 0 && errno_out) *errno_out = eno;
  return rc;
}

/* ----------------------------------------------------------------------- */
/* Re-apply watcher                                                        */

static void *
fan_watcher_thread_fn(void *arg) {
  (void)arg;
  syscall(SYS_thr_set_name, -1, "cr-fan-watcher");

  for (;;) {
    for (int i = 0; i < FAN_REAPPLY_SEC; i++) sleep(1);

    int t = atomic_load(&g_pinned_threshold_c);
    int _min, _max; fan_get_range(&_min, &_max);
    if (t < _min || t > _max) continue;  /* nothing pinned */

    int eno = 0;
    if (fan_set_threshold(t, &eno, NULL) != 0) {
      /* Device briefly unavailable (e.g. just after a launch) — the next
       * tick retries. Log at debug so we don't spam the log panel. */
      cr_log("debug", "fan", "re-apply failed errno=%d", eno);
    }
  }
  return NULL;
}

void
fan_init(void) {
  int saved = fan_load_threshold();
  if (saved) {
    atomic_store(&g_pinned_threshold_c, saved);
    /* Apply immediately at boot; the watcher keeps it pinned afterwards. */
    fan_set_threshold(saved, NULL, NULL);
    cr_log("info", "fan", "restored pinned threshold %dC", saved);
  }

  if (atomic_exchange(&g_watcher_started, 1)) return;
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &attr, fan_watcher_thread_fn, NULL) != 0) {
    cr_log("warn", "fan", "could not start re-apply watcher");
  }
  pthread_attr_destroy(&attr);
}

/* ----------------------------------------------------------------------- */
/* HTTP handlers                                                           */

static void
send_obj(int fd, int status, cJSON *root) {
  if (!root) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}"); return; }
  char *txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!txt) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}"); return; }
  http_send_json(fd, status, txt);
  free(txt);
}

static int
temp_in_range(int c) { return c >= 0 && c <= 130; }

static void
fan_temp_request(int fd) {
  int cpu_c = -1, soc_c = -1, m2_c = -1;
  int cpu_ok = (sceKernelGetCpuTemperature(&cpu_c) == 0) && temp_in_range(cpu_c);
  int soc_ok = (sceKernelGetSocSensorTemperature(0, &soc_c) == 0) && temp_in_range(soc_c);

  /* Channel 2 is the M.2 NVMe sensor on most revisions; an empty slot reads
   * ~0, so require >= 20C before trusting it. */
  int m2_ok = 0;
  {
    int v = -1;
    if (sceKernelGetSocSensorTemperature(2, &v) == 0 && v >= 20 && v <= 130) {
      m2_c = v;
      m2_ok = 1;
    }
  }

  int hottest = -1;
  if (cpu_ok && cpu_c > hottest) hottest = cpu_c;
  if (soc_ok && soc_c > hottest) hottest = soc_c;
  if (m2_ok  && m2_c  > hottest) hottest = m2_c;

  int threshold = atomic_load(&g_pinned_threshold_c);

  cJSON *r = cJSON_CreateObject();
  if (!r) { http_send_json(fd, 500, "{\"ok\":false}"); return; }
  cJSON_AddBoolToObject(r, "ok", (cpu_ok || soc_ok));
  if (cpu_ok) cJSON_AddNumberToObject(r, "cpuC", cpu_c);
  if (soc_ok) cJSON_AddNumberToObject(r, "socC", soc_c);
  if (m2_ok)  cJSON_AddNumberToObject(r, "m2C", m2_c);
  if (hottest >= 0) cJSON_AddNumberToObject(r, "hottestC", hottest);
  int min_c, max_c;
  fan_get_range(&min_c, &max_c);
  cJSON_AddNumberToObject(r, "thresholdC", threshold);
  cJSON_AddNumberToObject(r, "thresholdF", (threshold * 9 / 5) + 32);
  cJSON_AddNumberToObject(r, "minC", min_c);
  cJSON_AddNumberToObject(r, "maxC", max_c);
  cJSON_AddNumberToObject(r, "warmC", 65);
  cJSON_AddNumberToObject(r, "hotC", 80);
  send_obj(fd, 200, r);
}

static void
fan_set_request(int fd, const char *query) {
  char tmp[16] = {0};
  if (query_value(query, "temp", tmp, sizeof(tmp)) != 0 || !tmp[0]) {
    http_send_json(fd, 400, "{\"ok\":false,\"error\":\"missing 'temp' (Celsius)\"}");
    return;
  }
  int temp = atoi(tmp);
  int min_c, max_c;
  fan_get_range(&min_c, &max_c);
  if (temp < min_c || temp > max_c) {
    char err[96];
    snprintf(err, sizeof(err),
             "{\"ok\":false,\"error\":\"temp must be %d..%d Celsius\"}",
             min_c, max_c);
    http_send_json(fd, 400, err);
    return;
  }

  int eno = 0;
  unsigned char duty[6] = {0};
  int rc = fan_set_threshold(temp, &eno, duty);

  cJSON *r = cJSON_CreateObject();
  if (!r) { http_send_json(fd, 500, "{\"ok\":false}"); return; }
  if (rc != 0) {
    char err[256];
    snprintf(err, sizeof(err),
             "/dev/icc_fan ioctl failed (rc=%d errno=%d). Make sure kstuff is "
             "loaded so /dev/icc_fan is accessible.", rc, eno);
    cJSON_AddBoolToObject(r, "ok", 0);
    cJSON_AddStringToObject(r, "error", err);
    cJSON_AddNumberToObject(r, "errno", eno);
    send_obj(fd, 500, r);
    return;
  }

  /* Pin + persist so the watcher keeps re-applying it and it survives a
   * payload redeploy. */
  atomic_store(&g_pinned_threshold_c, temp);
  fan_save_threshold(temp);
  cr_log("info", "fan", "pinned fan-on threshold to %dC", temp);

  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddNumberToObject(r, "thresholdC", temp);
  cJSON_AddNumberToObject(r, "thresholdF", (temp * 9 / 5) + 32);
  cJSON_AddNumberToObject(r, "manualDuty", duty[0]);
  cJSON_AddBoolToObject(r, "pinned", 1);
  cJSON_AddNumberToObject(r, "reapplyEverySeconds", FAN_REAPPLY_SEC);
  send_obj(fd, 200, r);
}

static void
fan_info_request(int fd) {
  cJSON *r = cJSON_CreateObject();
  if (!r) { http_send_json(fd, 500, "{\"ok\":false}"); return; }
  int min_c, max_c;
  fan_get_range(&min_c, &max_c);
  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddNumberToObject(r, "minC", min_c);
  cJSON_AddNumberToObject(r, "maxC", max_c);
  cJSON_AddNumberToObject(r, "thresholdC", atomic_load(&g_pinned_threshold_c));
  cJSON_AddStringToObject(r, "device", ICC_FAN_DEVICE);
  send_obj(fd, 200, r);
}

int
cr_api_fan_handle(int fd, const char *method, const char *path,
                  const char *query, const char *body, size_t body_len) {
  (void)method; (void)body; (void)body_len;
  if (!strcmp(path, "/api/fan/temp")) { fan_temp_request(fd); return 1; }
  if (!strcmp(path, "/api/fan/set"))  { fan_set_request(fd, query); return 1; }
  if (!strcmp(path, "/api/fan"))      { fan_info_request(fd); return 1; }
  return 0;
}
