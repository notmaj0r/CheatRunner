#include <string.h>
#include "cr_api_internal.h"
#include "cr_api_sources.h"

int
cr_api_sources_handle(int fd, const char *method, const char *path,
                       const char *query, const char *body, size_t body_len) {
  (void)body_len;
  int is_post = !strcmp(method, "POST");

  if (!strcmp(path, "/api/cheats/sources")) {
    handle_api_cheats_sources(fd);
    return 1;
  }
  if (!strcmp(path, "/api/sources/jobs/start")) {
    if (!is_post) {
      http_send_json(fd, 405, "{\"ok\":false,\"error\":\"method_not_allowed\",\"message\":\"Use POST.\"}");
      return 1;
    }
    handle_api_sources_jobs_start(fd, body ? body : "");
    return 1;
  }
  if (!strcmp(path, "/api/sources/jobs/status")) {
    handle_api_sources_jobs_status(fd, query);
    return 1;
  }
  if (!strcmp(path, "/api/cheats/remote/find")) {
    handle_api_cheats_remote_find(fd, query);
    return 1;
  }
  if (!strcmp(path, "/api/cheats/remote/download")) {
    if (!is_post) {
      http_send_json(fd, 405, "{\"ok\":false,\"error\":\"method_not_allowed\",\"message\":\"Use POST.\"}");
      return 1;
    }
    handle_api_cheats_remote_download(fd, body ? body : "");
    return 1;
  }
  if (!strcmp(path, "/api/cheats/repo/download")) {
    handle_api_cheats_repo_download(fd, query);
    return 1;
  }
  if (!strcmp(path, "/api/cheats/repo/download/status")) {
    handle_api_cheats_repo_download_status(fd);
    return 1;
  }
  return 0;
}
