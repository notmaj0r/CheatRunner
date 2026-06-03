#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cr_api_internal.h"
#include "cr_titles.h"
#include "cr_title_prefs.h"

void
handle_api_title_prefs_get(int fd, const char *query) {
    char raw_tid[32] = {0};
    if (query_value(query, "titleId", raw_tid, sizeof(raw_tid)) != 0 || !raw_tid[0]) {
        http_send_json(fd, 400, "{\"ok\":false,\"error\":\"missing titleId\"}");
        return;
    }
    char title_id[10] = {0};
    if (!title_id_normalize(raw_tid, title_id)) {
        http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid titleId\"}");
        return;
    }
    char mode[16] = {0};
    int found = title_prefs_get_addr_mode(title_id, mode, sizeof(mode));
    char body[128];
    if (found && mode[0]) {
        snprintf(body, sizeof(body), "{\"ok\":true,\"titleId\":\"%s\",\"addrMode\":\"%s\"}", title_id, mode);
    } else {
        snprintf(body, sizeof(body), "{\"ok\":true,\"titleId\":\"%s\",\"addrMode\":null}", title_id);
    }
    http_send_json(fd, 200, body);
}

void
handle_api_title_prefs_set(int fd, const char *query) {
    char raw_tid[32] = {0};
    char mode[32] = {0};
    if (query_value(query, "titleId", raw_tid, sizeof(raw_tid)) != 0 || !raw_tid[0]) {
        http_send_json(fd, 400, "{\"ok\":false,\"error\":\"missing titleId\"}");
        return;
    }
    if (query_value(query, "addrMode", mode, sizeof(mode)) != 0 || !mode[0]) {
        http_send_json(fd, 400, "{\"ok\":false,\"error\":\"missing addrMode\"}");
        return;
    }
    if (strcmp(mode, "absolute") != 0 && strcmp(mode, "relative") != 0 && strcmp(mode, "auto") != 0) {
        http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid addrMode; allowed: absolute relative auto\"}");
        return;
    }
    char title_id[10] = {0};
    if (!title_id_normalize(raw_tid, title_id)) {
        http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid titleId\"}");
        return;
    }
    title_prefs_set_addr_mode(title_id, mode);
    http_send_json(fd, 200, "{\"ok\":true}");
}

void
handle_api_title_prefs_clear(int fd, const char *query) {
    char raw_tid[32] = {0};
    if (query_value(query, "titleId", raw_tid, sizeof(raw_tid)) != 0 || !raw_tid[0]) {
        http_send_json(fd, 400, "{\"ok\":false,\"error\":\"missing titleId\"}");
        return;
    }
    char title_id[10] = {0};
    if (!title_id_normalize(raw_tid, title_id)) {
        http_send_json(fd, 400, "{\"ok\":false,\"error\":\"invalid titleId\"}");
        return;
    }
    title_prefs_clear(title_id);
    http_send_json(fd, 200, "{\"ok\":true}");
}
