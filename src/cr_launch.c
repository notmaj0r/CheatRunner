#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cr_config.h"
#include "cr_game_monitor.h"
#include "cr_launch.h"
#include "cr_log.h"
#include "cr_notifications.h"
#include "cr_paths.h"
#include "cr_titles.h"
#include "ps5sdk_compat.h"

pthread_mutex_t g_launch_status_lock = PTHREAD_MUTEX_INITIALIZER;
launch_status_state_t g_launch_status = {
    .busy = 0,
    .phase = "idle",
    .title_id = "",
    .message = "ready",
    .last_ok = 1,
    .method = "",
    .rc = 0,
    .verified = 0,
    .hex = "0x00000000",
};
volatile uint64_t g_last_launch_verified_at_ms = 0;

static int wait_until_title_running(const char *title_id, int timeout_ms);
static int
lncutil_initialize_once(void) {
  static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
  static int initialized = 0;
  static int init_rc = 0;

  pthread_mutex_lock(&init_lock);
  if (!initialized) {
    init_rc = sceLncUtilInitialize();
    initialized = 1;
    if (init_rc < 0) {
      cr_log("warn", "launch", "sceLncUtilInitialize failed rc=%d (0x%08X); trying launch anyway", init_rc, (uint32_t)init_rc);
    } else {
      cr_log("info", "launch", "sceLncUtilInitialize ok rc=%d", init_rc);
    }
  }
  pthread_mutex_unlock(&init_lock);
  return init_rc;
}

static int
launch_rc_is_async_verifiable(int rc) {
  return rc == 0 || (uint32_t)rc == 0x80940005u;
}

static int
launch_title_with_lncutil(const char *title_id, app_launch_ctx_t *ctx, int has_ctx, const char **argv) {
#if CHEATRUNNER_HAVE_SCE_LNCUTIL
  int rc;

  lncutil_initialize_once();

  if (has_ctx) {
    cr_log("info", "launch", "trying sceLncUtilLaunchApp %s with foreground user=0x%08X", title_id, ctx->user_id);
    rc = sceLncUtilLaunchApp(title_id, argv, ctx);
    if (rc >= 0) {
      cr_log("info", "launch", "sceLncUtilLaunchApp submitted %s rc=%d", title_id, rc);
    } else {
      cr_log("warn", "launch", "sceLncUtilLaunchApp failed %s with ctx rc=%d (0x%08X)", title_id, rc, (uint32_t)rc);
    }
    if (rc >= 0 || launch_rc_is_async_verifiable(rc)) {
      return rc;
    }
  }

  cr_log("info", "launch", "trying sceLncUtilLaunchApp %s without ctx", title_id);
  rc = sceLncUtilLaunchApp(title_id, argv, NULL);
  if (rc >= 0) {
    cr_log("info", "launch", "sceLncUtilLaunchApp submitted %s rc=%d", title_id, rc);
  } else {
    cr_log("warn", "launch", "sceLncUtilLaunchApp failed %s without ctx rc=%d (0x%08X)", title_id, rc, (uint32_t)rc);
  }
  return rc;
#else
  (void)ctx;
  (void)has_ctx;
  cr_log("warn", "launch", "sceLncUtilLaunchApp unavailable in this SDK build; skipping LncUtil path for %s", title_id);
  return -38;
#endif
}

static int
launch_title_with_systemservice(const char *title_id, app_launch_ctx_t *ctx, int has_ctx, const char **argv) {
  int rc;

  if (has_ctx) {
    cr_log("info", "launch", "trying sceSystemServiceLaunchApp %s with foreground user=0x%08X", title_id, ctx->user_id);
    rc = sceSystemServiceLaunchApp(title_id, (char **)argv, ctx);
    if (rc >= 0) {
      cr_log("info", "launch", "sceSystemServiceLaunchApp submitted %s rc=%d", title_id, rc);
    } else {
      cr_log("warn", "launch", "sceSystemServiceLaunchApp failed %s with ctx rc=%d (0x%08X)", title_id, rc, (uint32_t)rc);
    }
    if (rc >= 0 || launch_rc_is_async_verifiable(rc)) {
      return rc;
    }
  }

  cr_log("info", "launch", "trying sceSystemServiceLaunchApp %s without ctx", title_id);
  rc = sceSystemServiceLaunchApp(title_id, (char **)argv, NULL);
  if (rc >= 0) {
    cr_log("info", "launch", "sceSystemServiceLaunchApp submitted %s rc=%d", title_id, rc);
  } else {
    cr_log("warn", "launch", "sceSystemServiceLaunchApp failed %s without ctx rc=%d (0x%08X)", title_id, rc, (uint32_t)rc);
  }
  return rc;
}

static int
launch_title(const char *title_id, const char *args, const char **method_out, int *rc_out, int *verified_out,
             int *fguser_out, char *msg_out, size_t msg_out_size) {
  app_launch_ctx_t ctx;
  uint32_t user_id = 0xFFFFFFFFu;
  int has_ctx = 0;
  int lnc_rc;
  int sys_rc;
  int verify_timeout = 30000;
  const char *argv_ptrs[16] = {0};
  char argv_buf[16][96];
  int argc = 0;

  pthread_mutex_lock(&g_cfg_lock);
  verify_timeout = g_cfg.launch_wait_timeout_ms;
  pthread_mutex_unlock(&g_cfg_lock);
  if (msg_out && msg_out_size > 0) {
    msg_out[0] = '\0';
  }
  if (method_out) {
    *method_out = "";
  }
  if (rc_out) {
    *rc_out = 0;
  }
  if (verified_out) {
    *verified_out = 0;
  }
  if (fguser_out) {
    *fguser_out = 0;
  }

  if (!is_valid_title_id(title_id)) {
    cr_log("error", "launch", "invalid title id: %s", title_id ? title_id : "(null)");
    return -1;
  }
  if (args && args[0]) {
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", args);
    char *save = NULL;
    char *tok = strtok_r(copy, " ", &save);
    while (tok && argc < 15) {
      snprintf(argv_buf[argc], sizeof(argv_buf[argc]), "%s", tok);
      argv_ptrs[argc] = argv_buf[argc];
      argc++;
      tok = strtok_r(NULL, " ", &save);
    }
    argv_ptrs[argc] = NULL;
    cr_log("info", "launch", "launch args parsed argc=%d raw=\"%s\"", argc, args);
  }

  memset(&ctx, 0, sizeof(ctx));
  ctx.structsize = sizeof(ctx);
  {
    char uid_cfg[32] = "auto";
    pthread_mutex_lock(&g_cfg_lock);
    snprintf(uid_cfg, sizeof(uid_cfg), "%s", g_cfg.launch_user_id);
    pthread_mutex_unlock(&g_cfg_lock);
    if (strcasecmp(uid_cfg, "none") == 0) {
      cr_log("info", "launch", "launch_user_id=none: skipping user context");
    } else if (strcasecmp(uid_cfg, "auto") == 0 || uid_cfg[0] == '\0') {
      if (sceUserServiceGetForegroundUser(&user_id) == 0 && user_id != 0xFFFFFFFFu && (int32_t)user_id != -1) {
        ctx.user_id = user_id;
        has_ctx = 1;
      } else {
        cr_log("warn", "launch", "sceUserServiceGetForegroundUser failed or invalid (id=0x%08X); launching without user context", user_id);
#if CHEATRUNNER_HAVE_SCE_USER_LIST
        {
          uint32_t uid_list[16] = {0};
          size_t uid_actual = 0;
          if (sceUserServiceGetLoginUserIdList(uid_list, 16, &uid_actual) == 0 && uid_actual == 1 &&
              uid_list[0] != 0xFFFFFFFFu && (int32_t)uid_list[0] != -1) {
            ctx.user_id = uid_list[0];
            has_ctx = 1;
            cr_log("info", "launch", "using logged-in user 0x%08X for launch ctx", ctx.user_id);
          } else if (uid_actual > 1) {
            cr_log("warn", "launch", "multiple logged-in users (%zu); skipping user ctx fallback", uid_actual);
          } else {
            cr_log("warn", "launch", "logged-in user fallback returned no valid user");
          }
        }
#else
        cr_log("info", "launch", "logged-in user fallback unavailable in this SDK");
#endif
      }
    } else {
      char *end = NULL;
      unsigned long parsed = strtoul(uid_cfg, &end, 16);
      if (end && *end == '\0' && parsed != 0 && (uint32_t)parsed != 0xFFFFFFFFu) {
        user_id = (uint32_t)parsed;
        ctx.user_id = user_id;
        has_ctx = 1;
        cr_log("info", "launch", "using configured launch_user_id=0x%08X", user_id);
      } else {
        cr_log("warn", "launch", "launch_user_id=%s: invalid hex; falling back to auto", uid_cfg);
        if (sceUserServiceGetForegroundUser(&user_id) == 0 && user_id != 0xFFFFFFFFu && (int32_t)user_id != -1) {
          ctx.user_id = user_id;
          has_ctx = 1;
        }
      }
    }
  }
  if (fguser_out) {
    *fguser_out = has_ctx;
  }

  /*
   * The launch worker is responsible for closing/waiting on the current BigApp.
   * Do not kill the current app again here; doing so can race the actual launch.
   *
   * sceSystemServiceLaunchApp frequently returns 0x80940005 for mounted, fake-installed,
   * or otherwise non-standard titles. Try the lower-level LncUtil path first, then keep
   * SystemService as a compatibility fallback for titles that prefer it.
   */
  lnc_rc = launch_title_with_lncutil(title_id, &ctx, has_ctx, argc > 0 ? argv_ptrs : NULL);
  if (rc_out) {
    *rc_out = lnc_rc;
  }
  if (method_out) {
    *method_out = "sceLncUtilLaunchApp";
  }
  if (launch_rc_is_async_verifiable(lnc_rc)) {
    set_launch_status_ex(1, "verifying_lnc", title_id, "Verifying launch...", 0, "sceLncUtilLaunchApp", lnc_rc, 0);
    // 0x80940005 on the LncUtil path: cap the wait to 5s so we fall through to
    // SystemService quickly when the title never appears. rc==0 (clean submit)
    // still uses the full verify_timeout.
    int lnc_verify_ms = ((uint32_t)lnc_rc == 0x80940005u)
                        ? (verify_timeout < 5000 ? verify_timeout : 5000)
                        : verify_timeout;
    if ((uint32_t)lnc_rc == 0x80940005u) {
      cr_log("warn", "launch", "launch returned 0x80940005; verifying async BigApp state (quick check %dms)", lnc_verify_ms);
    }
    if (wait_until_title_running(title_id, lnc_verify_ms) == 0) {
      if ((uint32_t)lnc_rc == 0x80940005u) {
        cr_log("info", "launch",
               "treating rc=0x80940005 as success because title is running (method=sceLncUtilLaunchApp title=%s)",
               title_id);
      }
      if (verified_out) {
        *verified_out = 1;
      }
      if (msg_out && msg_out_size > 0) {
        snprintf(msg_out, msg_out_size,
                 (uint32_t)lnc_rc == 0x80940005u ? "Launch returned 0x80940005 but title is running" : "Game is running");
      }
      return 0;
    }
    if ((uint32_t)lnc_rc == 0x80940005u) {
      cr_log("warn", "launch", "quick verify (%dms) found nothing after LncUtil 0x80940005; trying SystemService", lnc_verify_ms);
    }
  }

  set_launch_status_ex(1, "launching_system", title_id, "Trying SystemService launch...", 0, "sceSystemServiceLaunchApp", 0, 0);
  sys_rc = launch_title_with_systemservice(title_id, &ctx, has_ctx, argc > 0 ? argv_ptrs : NULL);
  if (rc_out) {
    *rc_out = sys_rc;
  }
  if (method_out) {
    *method_out = "sceSystemServiceLaunchApp";
  }
  if (launch_rc_is_async_verifiable(sys_rc)) {
    set_launch_status_ex(1, "verifying_system", title_id, "Verifying launch...", 0, "sceSystemServiceLaunchApp", sys_rc, 0);
    int sys_verify_ms = ((uint32_t)sys_rc == 0x80940005u)
                        ? (verify_timeout < 5000 ? verify_timeout : 5000)
                        : verify_timeout;
    if ((uint32_t)sys_rc == 0x80940005u) {
      cr_log("warn", "launch", "launch returned 0x80940005; verifying async BigApp state (quick check %dms)", sys_verify_ms);
    }
    if (wait_until_title_running(title_id, sys_verify_ms) == 0) {
      if ((uint32_t)sys_rc == 0x80940005u) {
        cr_log("info", "launch",
               "treating rc=0x80940005 as success because title is running (method=sceSystemServiceLaunchApp title=%s)",
               title_id);
      }
      if (verified_out) {
        *verified_out = 1;
      }
      if (msg_out && msg_out_size > 0) {
        snprintf(msg_out, msg_out_size,
                 (uint32_t)sys_rc == 0x80940005u ? "Launch returned 0x80940005 but title is running" : "Game is running");
      }
      return 0;
    }
  }

  cr_log("error", "launch", "all launch methods failed %s lnc=%d (0x%08X) system=%d (0x%08X)",
         title_id, lnc_rc, (uint32_t)lnc_rc, sys_rc, (uint32_t)sys_rc);

  /* Task 5: if both methods returned 0x80940005 do a final longer wait before giving up. */
  if ((uint32_t)lnc_rc == 0x80940005u && (uint32_t)sys_rc == 0x80940005u) {
    cr_log("info", "launch", "final async verify %s for up to %dms after 0x80940005", title_id, verify_timeout);
    if (wait_until_title_running(title_id, verify_timeout) == 0) {
      cr_log("info", "launch", "final async verify succeeded for %s", title_id);
      if (verified_out) {
        *verified_out = 1;
      }
      if (msg_out && msg_out_size > 0) {
        snprintf(msg_out, msg_out_size, "Launch returned 0x80940005 but title appeared after final async verify");
      }
      return 0;
    }
    cr_log("warn", "launch", "final verify timeout %s", title_id);

    /* Task 2: immediate check — game_monitor may have detected the title
       at the exact moment the verify timeout fired */
    {
      running_game_state_t gm_imm;
      running_state_get(&gm_imm);
      if (gm_imm.running && strcmp(gm_imm.title_id, title_id) == 0) {
        cr_log("info", "launch", "game monitor detected %s at verify timeout; treating as success", title_id);
        if (verified_out) *verified_out = 1;
        if (msg_out && msg_out_size > 0)
          snprintf(msg_out, msg_out_size, "Game monitor detected the title running at final verify timeout");
        return 0;
      }
    }

    /* Task 2: grace window — poll game_monitor for up to launch_post_timeout_grace_ms */
    {
      int grace_ms = 3000;
      pthread_mutex_lock(&g_cfg_lock);
      grace_ms = g_cfg.launch_post_timeout_grace_ms;
      pthread_mutex_unlock(&g_cfg_lock);
      if (grace_ms > 0) {
        cr_log("info", "launch", "entering %dms grace window after final verify timeout for %s", grace_ms, title_id);
        int waited = 0;
        while (waited < grace_ms) {
          usleep(250 * 1000);
          waited += 250;
          running_game_state_t gm_gw;
          running_state_get(&gm_gw);
          if (gm_gw.running && strcmp(gm_gw.title_id, title_id) == 0) {
            cr_log("info", "launch", "monitor detected %s in grace window (+%dms); treating as success", title_id, waited);
            if (verified_out) *verified_out = 1;
            if (msg_out && msg_out_size > 0)
              snprintf(msg_out, msg_out_size, "Game monitor detected the title running after final verify timeout");
            return 0;
          }
        }
        cr_log("warn", "launch", "grace window expired for %s", title_id);
      }
    }
  }

  if (msg_out && msg_out_size > 0) {
    if ((uint32_t)lnc_rc == 0x80940005u && (uint32_t)sys_rc == 0x80940005u) {
      snprintf(msg_out, msg_out_size,
               "Launch returned 0x80940005 and the title did not appear as BigApp.%s",
               has_ctx ? "" : " Foreground user context was unavailable.");
    } else {
      snprintf(msg_out, msg_out_size, "All launch methods failed (lnc=0x%08X sys=0x%08X)",
               (uint32_t)lnc_rc, (uint32_t)sys_rc);
    }
  }
  return sys_rc < 0 ? sys_rc : lnc_rc;
}

static void
set_launch_status_locked(int busy, const char *phase, const char *title_id, const char *message, int last_ok,
                         const char *method, int rc, int verified) {
  g_launch_status.busy = busy;
  snprintf(g_launch_status.phase, sizeof(g_launch_status.phase), "%s", phase ? phase : "idle");
  snprintf(g_launch_status.title_id, sizeof(g_launch_status.title_id), "%s", title_id ? title_id : "");
  snprintf(g_launch_status.message, sizeof(g_launch_status.message), "%s", message ? message : "");
  g_launch_status.last_ok = last_ok;
  snprintf(g_launch_status.method, sizeof(g_launch_status.method), "%s", method ? method : "");
  g_launch_status.rc = rc;
  g_launch_status.verified = verified ? 1 : 0;
  snprintf(g_launch_status.hex, sizeof(g_launch_status.hex), "0x%08X", (uint32_t)rc);
}

void
set_launch_status(int busy, const char *phase, const char *title_id, const char *message, int last_ok) {
  pthread_mutex_lock(&g_launch_status_lock);
  set_launch_status_locked(busy, phase, title_id, message, last_ok, g_launch_status.method, g_launch_status.rc,
                           g_launch_status.verified);
  pthread_mutex_unlock(&g_launch_status_lock);
}

void
set_launch_status_ex(int busy, const char *phase, const char *title_id, const char *message, int last_ok,
                     const char *method, int rc, int verified) {
  pthread_mutex_lock(&g_launch_status_lock);
  set_launch_status_locked(busy, phase, title_id, message, last_ok, method, rc, verified);
  pthread_mutex_unlock(&g_launch_status_lock);
}

static int
wait_until_no_bigapp(int timeout_ms) {
  int step = 100;
  int waited = 0;
  while (waited < timeout_ms) {
    if (sceSystemServiceGetAppIdOfRunningBigApp() <= 0) {
      return 0;
    }
    usleep(step * 1000);
    waited += step;
  }
  return -1;
}

static int
wait_until_title_running(const char *title_id, int timeout_ms) {
  int step = 250;
  int waited = 0;
  int last_app_id = -9999;
  char last_title[16] = {0};
  pid_t last_pid = -1;
  cr_log("info", "launch", "verifying title %s for up to %dms", title_id ? title_id : "(null)", timeout_ms);
  while (waited < timeout_ms) {
    pid_t pid = -1;
    intptr_t base = 0;
    int app_id = 0;
    char cur_title[16] = {0};
    int rc = get_running_game_ex(&pid, cur_title, sizeof(cur_title), &base, &app_id);
    if (rc == 0) {
      if (app_id != last_app_id || strcmp(cur_title, last_title) != 0 || pid != last_pid) {
        cr_log("info", "launch", "current bigapp=%s appId=0x%X pid=%d", cur_title, app_id, (int)pid);
        last_app_id = app_id;
        last_pid = pid;
        snprintf(last_title, sizeof(last_title), "%s", cur_title);
      }
    } else if (last_app_id != 0) {
      cr_log("info", "launch", "current bigapp=none");
      last_app_id = 0;
      last_pid = -1;
      last_title[0] = '\0';
    }
    if (rc == 0 && title_id && !strcmp(cur_title, title_id)) {
      cr_log("info", "launch", "verify success %s appId=0x%X pid=%d", title_id, app_id, (int)pid);
      return 0;
    }
    usleep(step * 1000);
    waited += step;
  }
  cr_log("warn", "launch", "verify timeout %s after %dms", title_id ? title_id : "(null)", timeout_ms);
  return -1;
}

void *
launch_worker_thread(void *arg) {
  launch_worker_request_t req = *(launch_worker_request_t *)arg;
  free(arg);

  set_launch_status_ex(1, "killing_current", req.title_id, "Closing current game...", 0, "", 0, 0);
  cr_log("info", "launch", "request title=%s args=\"%s\"", req.title_id, req.args[0] ? req.args : "");
  /* Resolve user context before killing current game so we can abort cleanly */
  {
    char uid_cfg[32] = "auto";
    pthread_mutex_lock(&g_cfg_lock);
    snprintf(uid_cfg, sizeof(uid_cfg), "%s", g_cfg.launch_user_id);
    pthread_mutex_unlock(&g_cfg_lock);
    if (strcasecmp(uid_cfg, "auto") == 0 || uid_cfg[0] == '\0') {
      uint32_t fguser = 0xFFFFFFFFu;
      if (sceUserServiceGetForegroundUser(&fguser) != 0 || fguser == 0xFFFFFFFFu || (int32_t)fguser == -1) {
        cr_log("warn", "launch", "auto user context: no foreground user (id=0x%08X); will attempt without user context", fguser);
      }
    }
  }
  if (g_cfg.launch_kill_current) {
    int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
    cr_log("info", "launch", "current bigapp appId=0x%X", app_id > 0 ? app_id : 0);
    if (app_id > 0) {
      sceSystemServiceKillApp(app_id, -1, 0, 0);
      set_launch_status_ex(1, "waiting_for_close", req.title_id, "Waiting current game close...", 0, "", 0, 0);
      if (wait_until_no_bigapp(g_cfg.launch_wait_timeout_ms) != 0) {
        set_launch_status_ex(0, "failed", req.title_id, "Timeout waiting game close", 0, "", -1, 0);
        notification_add("launch_fail", "Launch failed for %s (close timeout)", req.title_id);
        cr_log("error", "launch", "close timeout for %s", req.title_id);
        return NULL;
      }
      if (g_cfg.launch_kill_delay_ms > 0) {
        usleep((useconds_t)g_cfg.launch_kill_delay_ms * 1000);
      }
    }
  }

  set_launch_status_ex(1, "launching_lnc", req.title_id, "Trying LNC launch...", 0, "sceLncUtilLaunchApp", 0, 0);
  const char *method = "";
  int method_rc = 0;
  int verified = 0;
  int fguser_valid = 0;
  char launch_msg[160] = {0};
  int rc = launch_title(req.title_id, req.args[0] ? req.args : NULL, &method, &method_rc, &verified, &fguser_valid,
                        launch_msg, sizeof(launch_msg));
  pthread_mutex_lock(&g_launch_status_lock);
  g_launch_status.foreground_user_valid = fguser_valid;
  pthread_mutex_unlock(&g_launch_status_lock);
  if (rc != 0) {
    set_launch_status_ex(0, "failed", req.title_id,
                         launch_msg[0] ? launch_msg : "Launch syscall failed", 0, method, method_rc, verified);
    notification_add("launch_fail", "Launch failed for %s (rc=%d)", req.title_id, rc);
    cr_log("error", "launch", "launch syscall failed %s rc=%d (0x%08X)", req.title_id, rc, (uint32_t)rc);
    return NULL;
  }

  g_last_launch_verified_at_ms = now_ms();
  set_launch_status_ex(0, "ready", req.title_id, launch_msg[0] ? launch_msg : "Game is running", 1, method, method_rc, verified);
  notification_add("launch", "Game launched: %s", req.title_id);
  cr_log("info", "launch", "game launched %s method=%s rc=%d (0x%08X) verified=%d", req.title_id,
         method ? method : "", method_rc, (uint32_t)method_rc, verified);
  return NULL;
}
