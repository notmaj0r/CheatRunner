#include <string.h>
#include "cr_api_internal.h"
#include "cr_api_dashboard.h"

/* Dashboard HTML is defined in cr_api.c as g_dashboard_html */
extern const char g_dashboard_html[];

int
cr_api_dashboard_handle(int fd, const char *method, const char *path,
                         const char *query, const char *body, size_t body_len) {
  (void)method; (void)query; (void)body; (void)body_len;
  if (!strcmp(path, "/") || !strcmp(path, "/index.html") ||
      !strcmp(path, "/launcher.html")) {
    http_send_response(fd, 200, "text/html; charset=utf-8",
      (const uint8_t *)g_dashboard_html, strlen(g_dashboard_html));
    return 1;
  }
  if (!strcmp(path, "/CheatRunner.png") || !strcmp(path, "/favicon.png") ||
      !strcmp(path, "/icon.png")        || !strcmp(path, "/apple-touch-icon.png")) {
    http_send_png_asset(fd);
    return 1;
  }
  if (!strcmp(path, "/manifest.json")) {
    static const char manifest[] =
      "{\"name\":\"CheatRunner\",\"short_name\":\"CheatRunner\","
      "\"description\":\"PS5 Web Launcher & Cheat Trainer\","
      "\"theme_color\":\"#E11D48\",\"background_color\":\"#050505\","
      "\"display\":\"standalone\","
      "\"icons\":[{\"src\":\"/CheatRunner.png\",\"sizes\":\"192x192\",\"type\":\"image/png\"},"
      "{\"src\":\"/CheatRunner.png\",\"sizes\":\"512x512\",\"type\":\"image/png\","
      "\"purpose\":\"any maskable\"}]}";
    http_send_response(fd, 200, "application/manifest+json",
      (const uint8_t *)manifest, sizeof(manifest) - 1);
    return 1;
  }
  return 0;
}
