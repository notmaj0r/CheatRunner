#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cr_log.h"
#include "cr_activity.h"
#include "cr_notifications.h"
#include "cr_shutdown.h"

volatile int g_shutdown_requested = 0;
extern volatile int g_game_monitor_running;
extern int g_http_listen_fd;

static void
close_fd_safe(int *fd) {
  if (!fd || *fd < 0) {
    return;
  }
  shutdown(*fd, SHUT_RDWR);
  close(*fd);
  *fd = -1;
}

void
cheatrunner_close_servers(void) {
  close_fd_safe(&g_http_listen_fd);
}

static void *
delayed_shutdown_thread(void *arg) {
  int delay_ms = arg ? *(int *)arg : 700;
  free(arg);
  if (delay_ms < 0) {
    delay_ms = 0;
  }
  usleep((useconds_t)delay_ms * 1000);
  cr_log("info", "dev", "shutdown requested; closing CheatRunner");
  g_shutdown_requested = 1;
  g_game_monitor_running = 0;
  cr_log("info", "dev", "closing HTTP socket");
  cheatrunner_close_servers();
  activity_save();
  notifications_save();
  cr_log("info", "dev", "CheatRunner exiting for reload");
  kill(getpid(), SIGKILL);
  /* Guaranteed exit: if self-SIGKILL is somehow refused/deferred (PS5 credential
   * quirk), fall through to _exit so the process always dies — otherwise the
   * thread would just return and CheatRunner would keep running ("shutdown
   * didn't work"). */
  _exit(0);
  return NULL;
}

void
cheatrunner_request_shutdown(int delay_ms) {
  int *p = malloc(sizeof(*p));
  if (!p) {
    g_shutdown_requested = 1;
    g_game_monitor_running = 0;
    cheatrunner_close_servers();
    kill(getpid(), SIGKILL);
    _exit(0);
    return;
  }
  *p = delay_ms;
  pthread_t t;
  if (pthread_create(&t, NULL, delayed_shutdown_thread, p) == 0) {
    pthread_detach(t);
    return;
  }
  free(p);
  g_shutdown_requested = 1;
  g_game_monitor_running = 0;
  cheatrunner_close_servers();
  kill(getpid(), SIGKILL);
  _exit(0);
}
