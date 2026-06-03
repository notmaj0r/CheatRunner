#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/cJSON.h"
#include "cr_paths.h"
#include "cr_log.h"
#include "cr_titles.h"
#include "cr_title_prefs.h"

#define TITLE_PREFS_MAX 128

typedef struct {
    char title_id[10];
    char addr_mode[16];
} title_pref_t;

static title_pref_t    g_title_prefs[TITLE_PREFS_MAX];
static int             g_title_prefs_n = 0;
static pthread_mutex_t g_title_prefs_lock = PTHREAD_MUTEX_INITIALIZER;

static void
title_prefs_save_locked(void) {
    size_t cap = (size_t)g_title_prefs_n * 64 + 64;
    char *buf = malloc(cap);
    if (!buf) return;
    size_t pos = 0;
    int rc = snprintf(buf + pos, cap - pos, "[");
    if (rc > 0) pos += (size_t)rc;
    for (int i = 0; i < g_title_prefs_n; i++) {
        int r = snprintf(buf + pos, cap - pos,
                         "%s{\"titleId\":\"%s\",\"addrMode\":\"%s\"}",
                         i > 0 ? "," : "",
                         g_title_prefs[i].title_id,
                         g_title_prefs[i].addr_mode);
        if (r > 0) pos += (size_t)r;
    }
    int r2 = snprintf(buf + pos, cap - pos, "]");
    if (r2 > 0) pos += (size_t)r2;
    write_file_atomic(CHEATRUNNER_TITLE_PREFS_PATH, (const uint8_t *)buf, pos);
    free(buf);
}

void
title_prefs_load(void) {
    pthread_mutex_lock(&g_title_prefs_lock);
    g_title_prefs_n = 0;

    char *txt = NULL;
    if (read_file_text(CHEATRUNNER_TITLE_PREFS_PATH, &txt) != 0 || !txt) {
        pthread_mutex_unlock(&g_title_prefs_lock);
        return;
    }

    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        pthread_mutex_unlock(&g_title_prefs_lock);
        return;
    }

    int loaded = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (g_title_prefs_n >= TITLE_PREFS_MAX) break;
        cJSON *tid_j  = cJSON_GetObjectItem(item, "titleId");
        cJSON *mode_j = cJSON_GetObjectItem(item, "addrMode");
        if (!cJSON_IsString(tid_j) || !cJSON_IsString(mode_j)) continue;
        char norm[10] = {0};
        if (!title_id_normalize(tid_j->valuestring, norm)) continue;
        snprintf(g_title_prefs[g_title_prefs_n].title_id, sizeof(g_title_prefs[0].title_id), "%s", norm);
        snprintf(g_title_prefs[g_title_prefs_n].addr_mode, sizeof(g_title_prefs[0].addr_mode), "%s", mode_j->valuestring);
        g_title_prefs_n++;
        loaded++;
    }
    cJSON_Delete(root);
    pthread_mutex_unlock(&g_title_prefs_lock);
    if (loaded > 0)
        cr_log("info", "title_prefs", "loaded %d title preference(s) from disk", loaded);
}

void
title_prefs_save(void) {
    pthread_mutex_lock(&g_title_prefs_lock);
    title_prefs_save_locked();
    pthread_mutex_unlock(&g_title_prefs_lock);
}

int
title_prefs_get_addr_mode(const char *title_id, char *out, size_t out_sz) {
    if (!title_id || !out || out_sz == 0) return 0;
    char norm[10] = {0};
    if (!title_id_normalize(title_id, norm)) return 0;
    pthread_mutex_lock(&g_title_prefs_lock);
    for (int i = 0; i < g_title_prefs_n; i++) {
        if (strcmp(g_title_prefs[i].title_id, norm) == 0) {
            snprintf(out, out_sz, "%s", g_title_prefs[i].addr_mode);
            pthread_mutex_unlock(&g_title_prefs_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_title_prefs_lock);
    return 0;
}

void
title_prefs_set_addr_mode(const char *title_id, const char *mode) {
    if (!title_id || !mode) return;
    /* if mode is empty or "auto", clear the preference */
    if (!mode[0] || !strcmp(mode, "auto")) {
        title_prefs_clear(title_id);
        return;
    }
    char norm[10] = {0};
    if (!title_id_normalize(title_id, norm)) return;
    pthread_mutex_lock(&g_title_prefs_lock);
    /* Upsert */
    for (int i = 0; i < g_title_prefs_n; i++) {
        if (strcmp(g_title_prefs[i].title_id, norm) == 0) {
            snprintf(g_title_prefs[i].addr_mode, sizeof(g_title_prefs[0].addr_mode), "%s", mode);
            title_prefs_save_locked();
            pthread_mutex_unlock(&g_title_prefs_lock);
            return;
        }
    }
    if (g_title_prefs_n < TITLE_PREFS_MAX) {
        snprintf(g_title_prefs[g_title_prefs_n].title_id, sizeof(g_title_prefs[0].title_id), "%s", norm);
        snprintf(g_title_prefs[g_title_prefs_n].addr_mode, sizeof(g_title_prefs[0].addr_mode), "%s", mode);
        g_title_prefs_n++;
    }
    title_prefs_save_locked();
    pthread_mutex_unlock(&g_title_prefs_lock);
}

void
title_prefs_clear(const char *title_id) {
    if (!title_id) return;
    char norm[10] = {0};
    if (!title_id_normalize(title_id, norm)) return;
    pthread_mutex_lock(&g_title_prefs_lock);
    int i = 0;
    while (i < g_title_prefs_n) {
        if (strcmp(g_title_prefs[i].title_id, norm) == 0) {
            for (int j = i; j < g_title_prefs_n - 1; j++)
                g_title_prefs[j] = g_title_prefs[j+1];
            g_title_prefs_n--;
        } else {
            i++;
        }
    }
    title_prefs_save_locked();
    pthread_mutex_unlock(&g_title_prefs_lock);
}
