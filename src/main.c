#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "jb.h"
#include "priv_bootstrap.h"
#include "ps5sdk_compat.h"
#include "cr_activity.h"
#include "cr_config.h"
#include "cr_game_monitor.h"
#include "cr_http.h"
#include "cr_launch.h"
#include "cr_log.h"
#include "cr_notifications.h"
#include "cr_paths.h"
#include "cr_shutdown.h"
#include "cr_cheats.h"
#include "cr_addr_cache.h"
#include "cr_title_prefs.h"
#include "cr_favorites.h"

#ifndef CHEATRUNNER_VERSION
#define CHEATRUNNER_VERSION "0.1"
#endif

int
main(void) {
  pid_t old_pid;
  signal(SIGPIPE, SIG_IGN);

  syscall(SYS_thr_set_name, -1, "CheatRunner.elf");

  cr_log_klog_banner();

  puts(".------------------------------------.");
  puts("|            CheatRunner             |");
  puts("|       PS5 web launcher + cheats    |");
  puts("'------------------------------------'");
  log_msg("version: %s", CHEATRUNNER_VERSION);

#ifdef __SCE__
  if (jb_escalate_pid(getpid()) != 0) {
    cr_log("warn", "core", "jb_escalate_pid failed; privilege may be incomplete");
  }
  cr_priv_init();
  {
    const cr_priv_status_t *ps = cr_priv_get();
    cr_log("info", "priv",
           "uid=%d gid=%d euid=%d egid=%d uid_ok=%d sandbox=%d authid_ok=%d caps_ok=%d kernel_rw=%d mprotect=%d can_patch=%d",
           ps->uid, ps->gid, ps->euid, ps->egid, ps->uid_ok, ps->sandbox_ok, ps->authid_ok, ps->caps_ok,
           ps->kernel_rw_ok, ps->kernel_mprotect_ok, ps->can_patch_game);
    if (!ps->can_patch_game) {
      cr_log("warn", "priv", "privilege bootstrap incomplete; cheat writes may fail");
      for (int i = 0; i < ps->n_warnings; i++) {
        cr_log("warn", "priv", "  %s", ps->warnings[i]);
      }
    }
  }
#endif

  /* Kill any previous CheatRunner instance before binding the HTTP port.
   *
   * This MUST run AFTER jb_escalate_pid/cr_priv_init: a previously-running
   * CheatRunner has escalated itself (uid 0 + a privileged authid), and an
   * un-escalated newcomer cannot SIGKILL it — kill() returns EPERM, the old
   * process keeps listening on the HTTP port, and our bind() then fails with
   * EADDRINUSE forever. With privileges in hand the kill actually lands.
   *
   * Bounded SIGKILL-then-verify loop: a SIGKILL'd process lingers as a ZOMBIE
   * in the process list until reaped, so an unbounded `while (find_pid > 0)`
   * could spin forever. Cap the wait at ~6s. We re-issue the signal each spin
   * and log if the target refuses to die so the cause is visible in the log
   * instead of surfacing only as a flood of bind() failures later. */
  for (int spin = 0; spin < 60; spin++) {
    old_pid = find_pid_by_name("CheatRunner.elf");
    if (old_pid <= 0) {
      break;
    }
    if (kill(old_pid, SIGKILL) != 0 && spin == 0) {
      cr_log("warn", "boot", "could not signal previous instance pid=%d errno=%d", old_pid, errno);
    }
    usleep(100 * 1000);
  }

  {
    int urc = sceUserServiceInitialize(NULL);
    if (urc != 0 && urc != (int)0x80960003) {
      cr_log("warn", "core", "sceUserServiceInitialize rc=0x%08X", (uint32_t)urc);
    }
  }

  ensure_data_dirs();
  config_load();
  notifications_load();
  activity_load();
  crash_suspects_load();
  addr_cache_load();
  title_prefs_load();
  favorites_load();
  rpc_refresh_title_and_notify();
  set_launch_status_ex(0, "idle", "", "ready", 1, "", 0, 0);
  notify("CheatRunner v" CHEATRUNNER_VERSION " by maj0r");
  notification_add("boot", "CheatRunner v" CHEATRUNNER_VERSION " started");
  cr_log("info", "boot", "version %s", CHEATRUNNER_VERSION);

  pthread_t http_thread;
  pthread_t monitor_thread;
  if (pthread_create(&http_thread, NULL, http_server_thread, NULL) != 0) {
    log_msg("error: could not start HTTP thread");
    notify("CheatRunner HTTP thread failed");
    return 1;
  }
  if (pthread_create(&monitor_thread, NULL, game_monitor_thread, NULL) != 0) {
    log_msg("error: could not start game monitor thread");
    notify("CheatRunner game monitor thread failed");
    return 1;
  }

  pthread_join(http_thread, NULL);
  g_game_monitor_running = 0;
  pthread_join(monitor_thread, NULL);
  return 0;
}
