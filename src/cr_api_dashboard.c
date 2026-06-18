#include <string.h>
#include "cr_api_internal.h"
#include "cr_api_dashboard.h"

extern const char g_dashboard_html[];
extern const char g_dashboard_css[];
extern const char g_dashboard_js[];

static void send_asset(int fd, const char *content_type, const char *data) {
  http_send_response(fd, 200, content_type, (const uint8_t *)data, strlen(data));
}

static void send_static_asset(int fd, const char *content_type, const char *data) {
  http_send_response_cached(fd, 200, content_type, (const uint8_t *)data, strlen(data));
}

int
cr_api_dashboard_handle(int fd, const char *method, const char *path,
                         const char *query, const char *body, size_t body_len) {
  (void)method; (void)query; (void)body; (void)body_len;
  if (!strcmp(path, "/") || !strcmp(path, "/index.html") ||
      !strcmp(path, "/launcher.html")) {
    send_asset(fd, "text/html; charset=utf-8", g_dashboard_html);
    return 1;
  }
  if (!strcmp(path, "/dashboard.css")) {
    send_static_asset(fd, "text/css; charset=utf-8", g_dashboard_css);
    return 1;
  }
  if (!strcmp(path, "/dashboard.js")) {
    send_static_asset(fd, "application/javascript; charset=utf-8", g_dashboard_js);
    return 1;
  }
  if (!strcmp(path, "/CheatRunner.png") || !strcmp(path, "/favicon.png") ||
      !strcmp(path, "/icon.png")        || !strcmp(path, "/apple-touch-icon.png")) {
    http_send_png_asset(fd);
    return 1;
  }
  if (!strcmp(path, "/cache.appcache")) {
    static const char appcache[] =
      "CACHE MANIFEST\n"
      "# CheatRunner v0.14\n"
      "# Build: " __DATE__ " " __TIME__ "\n"
      "\n"
      "CACHE:\n"
      "/\n"
      "/dashboard.css\n"
      "/dashboard.js\n"
      "/CheatRunner.png\n"
      "\n"
      "NETWORK:\n"
      "*\n";
    http_send_response(fd, 200, "text/cache-manifest; charset=utf-8",
      (const uint8_t *)appcache, sizeof(appcache) - 1);
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
