#include <errno.h>
#include <fcntl.h>
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
#include "cr_fan.h"
#include "cr_tile_pkg.h"

#ifndef CHEATRUNNER_VERSION
#define CHEATRUNNER_VERSION "0.1"
#endif

/* Async-signal-safe crash handler: writes a one-line entry to the crash log
 * using only open/write/close/_exit (no malloc, no stdio). */
static void
crash_sighandler(int sig) {
  static const char path[] = CHEATRUNNER_CRASH_LOG_PATH;
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) {
    static const char hdr[]  = "signal=";
    static const char tail[] = " CheatRunner crashed\n";
    int s = (sig < 0 ? 0 : (sig > 99 ? 99 : sig));
    char nb[2];
    nb[0] = (char)('0' + s / 10);
    nb[1] = (char)('0' + s % 10);
    (void)write(fd, hdr,  sizeof(hdr) - 1);
    (void)write(fd, nb,   2);
    (void)write(fd, tail, sizeof(tail) - 1);
    close(fd);
  }
  _exit(sig);
}

int
main(void) {
  pid_t old_pid;
  signal(SIGPIPE, SIG_IGN);
  {
    struct sigaction sa;
    sa.sa_handler = crash_sighandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
  }

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
   * On PS5, SYS_thr_set_name updates ki_comm (kinfo_proc offset 447) in
   * addition to ki_tdname (offset 394) — both elfldr and elf-arsenal rely on
   * this behaviour.  We search offset 447 for "CheatRunner.elf" which is what
   * we set via thr_set_name above.
   *
   * EPERM means the old instance is escalated and we are not — break fast so
   * we surface the error instead of spinning then flooding bind() logs.
   * ESRCH means the process vanished between find_pid and kill — treat as gone.
   *
   * Cap at 60 spins (~9s) to guard against looping forever on a zombie.
   * Zombies hold no sockets, so the port will be free even if we time out. */
  {
    int kill_spins = 0;
    while ((old_pid = find_pid_by_name("CheatRunner.elf")) > 0 && kill_spins < 60) {
      int kerr = kill(old_pid, SIGKILL);
      if (kerr != 0) {
        int e = errno;
        cr_log("warn", "boot", "SIGKILL pid=%d errno=%d", old_pid, e);
        if (e == EPERM) {
          cr_log("error", "boot",
                 "cannot kill old instance (EPERM) — escalation may have failed");
          break;
        }
        if (e == ESRCH) break;  /* already dead */
      }
      usleep(150 * 1000);
      kill_spins++;
    }
    if (old_pid > 0 && kill_spins >= 60) {
      cr_log("warn", "boot", "old instance pid=%d still visible after 9s (zombie?)", old_pid);
    }
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
  fan_init();
  cr_tile_autoinstall_init();
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
