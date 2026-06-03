#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

#include "third_party/cJSON.h"
#include "cr_paths.h"
#include "cr_log.h"
#include "cr_addr_cache.h"

#define ADDR_CACHE_MAX 512

typedef struct {
    char     key[384];
    intptr_t addr;
    uint8_t  orig_bytes[128];
    size_t   orig_len;
    time_t   learned_at;
} addr_cache_slot_t;

static addr_cache_slot_t g_addr_cache[ADDR_CACHE_MAX];
static int               g_addr_cache_n = 0;
static pthread_mutex_t   g_addr_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static void
make_key(char *key, size_t sz, const char *path, time_t mtime, int mod_idx, int entry_idx) {
    snprintf(key, sz, "%s|%lld|%d|%d", path, (long long)mtime, mod_idx, entry_idx);
}

/* Decode a hex string into bytes. Returns number of bytes decoded, or -1 on error. */
static int
hex_decode(const char *hex, uint8_t *out, size_t out_max) {
    if (!hex) return 0;
    size_t n = 0;
    while (hex[0] && hex[1] && n < out_max) {
        char h[3] = { hex[0], hex[1], '\0' };
        out[n++] = (uint8_t)strtoul(h, NULL, 16);
        hex += 2;
    }
    return (int)n;
}

/* Encode bytes as hex string. out must be at least len*2+1 bytes. */
static void
hex_encode(const uint8_t *bytes, size_t len, char *out) {
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i*2]   = hex_chars[(bytes[i] >> 4) & 0xf];
        out[i*2+1] = hex_chars[bytes[i] & 0xf];
    }
    out[len*2] = '\0';
}

void
addr_cache_load(void) {
    pthread_mutex_lock(&g_addr_cache_lock);
    g_addr_cache_n = 0;

    char *txt = NULL;
    if (read_file_text(CHEATRUNNER_ADDR_CACHE_PATH, &txt) != 0 || !txt) {
        pthread_mutex_unlock(&g_addr_cache_lock);
        return;
    }

    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        pthread_mutex_unlock(&g_addr_cache_lock);
        return;
    }

    int loaded = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (g_addr_cache_n >= ADDR_CACHE_MAX) break;
        cJSON *k_j    = cJSON_GetObjectItem(item, "k");
        cJSON *addr_j = cJSON_GetObjectItem(item, "addr");
        cJSON *orig_j = cJSON_GetObjectItem(item, "orig");
        cJSON *ts_j   = cJSON_GetObjectItem(item, "ts");
        if (!cJSON_IsString(k_j) || !cJSON_IsString(addr_j) || !cJSON_IsString(orig_j))
            continue;
        addr_cache_slot_t *s = &g_addr_cache[g_addr_cache_n];
        snprintf(s->key, sizeof(s->key), "%s", k_j->valuestring);
        s->addr = (intptr_t)strtoull(addr_j->valuestring, NULL, 16);
        int nb = hex_decode(orig_j->valuestring, s->orig_bytes, sizeof(s->orig_bytes));
        if (nb < 0) nb = 0;
        s->orig_len   = (size_t)nb;
        s->learned_at = cJSON_IsNumber(ts_j) ? (time_t)(long long)ts_j->valuedouble : 0;
        g_addr_cache_n++;
        loaded++;
    }
    cJSON_Delete(root);
    pthread_mutex_unlock(&g_addr_cache_lock);
    if (loaded > 0)
        cr_log("info", "addr_cache", "loaded %d entr%s from disk", loaded, loaded == 1 ? "y" : "ies");
}

/* Internal: must be called with g_addr_cache_lock held. */
static void
addr_cache_save_locked(void) {
    /* Worst-case bytes per entry: escaped key (key[384] can double to ~766 when
     * every char needs a backslash) + orig hex (128 bytes → 256 chars) + the
     * fixed JSON scaffolding/addr/ts (~80).  Under-sizing here makes the bounded
     * snprintf truncate mid-entry, producing invalid JSON that fails to parse on
     * the next load and silently drops the whole learned-address cache. */
    size_t cap = (size_t)g_addr_cache_n * 1200 + 64;
    if (cap < 4) cap = 4;
    char *buf = malloc(cap);
    if (!buf) return;

    size_t pos = 0;
    int rc_sn = snprintf(buf + pos, cap - pos, "[");
    if (rc_sn > 0) pos += (size_t)rc_sn;

    for (int i = 0; i < g_addr_cache_n; i++) {
        addr_cache_slot_t *s = &g_addr_cache[i];
        char orig_hex[257] = {0};
        hex_encode(s->orig_bytes, s->orig_len, orig_hex);
        char addr_hex[32];
        snprintf(addr_hex, sizeof(addr_hex), "0x%lx", (long)s->addr);
        /* Escape key for JSON — replace backslashes and quotes */
        char esc_key[768];
        size_t ek = 0;
        for (size_t j = 0; s->key[j] && ek + 4 < sizeof(esc_key); j++) {
            char c = s->key[j];
            if (c == '"' || c == '\\') { esc_key[ek++] = '\\'; }
            esc_key[ek++] = c;
        }
        esc_key[ek] = '\0';
        int r = snprintf(buf + pos, cap - pos,
                         "%s{\"k\":\"%s\",\"addr\":\"%s\",\"orig\":\"%s\",\"ts\":%lld}",
                         i > 0 ? "," : "",
                         esc_key, addr_hex, orig_hex, (long long)s->learned_at);
        if (r > 0) pos += (size_t)r;
    }
    int r2 = snprintf(buf + pos, cap - pos, "]");
    if (r2 > 0) pos += (size_t)r2;

    write_file_atomic(CHEATRUNNER_ADDR_CACHE_PATH, (const uint8_t *)buf, pos);
    free(buf);
}

void
addr_cache_save(void) {
    pthread_mutex_lock(&g_addr_cache_lock);
    addr_cache_save_locked();
    pthread_mutex_unlock(&g_addr_cache_lock);
}

int
addr_cache_get(const char *path, time_t mtime, int mod_idx, int entry_idx, addr_cache_entry_t *out) {
    char key[384];
    make_key(key, sizeof(key), path, mtime, mod_idx, entry_idx);

    pthread_mutex_lock(&g_addr_cache_lock);
    for (int i = 0; i < g_addr_cache_n; i++) {
        if (strcmp(g_addr_cache[i].key, key) == 0) {
            if (out) {
                out->addr     = g_addr_cache[i].addr;
                out->orig_len = g_addr_cache[i].orig_len;
                memcpy(out->orig_bytes, g_addr_cache[i].orig_bytes, g_addr_cache[i].orig_len);
            }
            pthread_mutex_unlock(&g_addr_cache_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_addr_cache_lock);
    return 0;
}

void
addr_cache_set(const char *path, time_t mtime, int mod_idx, int entry_idx, intptr_t addr, const uint8_t *orig_bytes, size_t orig_len) {
    char key[384];
    make_key(key, sizeof(key), path, mtime, mod_idx, entry_idx);

    if (orig_len > 128) orig_len = 128;

    pthread_mutex_lock(&g_addr_cache_lock);

    /* Upsert: find existing slot */
    for (int i = 0; i < g_addr_cache_n; i++) {
        if (strcmp(g_addr_cache[i].key, key) == 0) {
            g_addr_cache[i].addr = addr;
            if (orig_bytes && orig_len > 0) {
                memcpy(g_addr_cache[i].orig_bytes, orig_bytes, orig_len);
                g_addr_cache[i].orig_len = orig_len;
            }
            g_addr_cache[i].learned_at = time(NULL);
            addr_cache_save_locked();
            pthread_mutex_unlock(&g_addr_cache_lock);
            return;
        }
    }

    /* Evict oldest if full */
    if (g_addr_cache_n >= ADDR_CACHE_MAX) {
        int oldest = 0;
        for (int i = 1; i < g_addr_cache_n; i++) {
            if (g_addr_cache[i].learned_at < g_addr_cache[oldest].learned_at)
                oldest = i;
        }
        /* Shift down */
        for (int i = oldest; i < g_addr_cache_n - 1; i++)
            g_addr_cache[i] = g_addr_cache[i+1];
        g_addr_cache_n--;
    }

    addr_cache_slot_t *s = &g_addr_cache[g_addr_cache_n];
    snprintf(s->key, sizeof(s->key), "%s", key);
    s->addr = addr;
    s->orig_len = orig_len;
    if (orig_bytes && orig_len > 0)
        memcpy(s->orig_bytes, orig_bytes, orig_len);
    s->learned_at = time(NULL);
    g_addr_cache_n++;

    addr_cache_save_locked();
    pthread_mutex_unlock(&g_addr_cache_lock);
}

void
addr_cache_clear_for_path(const char *path) {
    if (!path) return;
    char prefix[384];
    snprintf(prefix, sizeof(prefix), "%s|", path);
    size_t plen = strlen(prefix);

    pthread_mutex_lock(&g_addr_cache_lock);
    int i = 0;
    while (i < g_addr_cache_n) {
        if (strncmp(g_addr_cache[i].key, prefix, plen) == 0) {
            for (int j = i; j < g_addr_cache_n - 1; j++)
                g_addr_cache[j] = g_addr_cache[j+1];
            g_addr_cache_n--;
        } else {
            i++;
        }
    }
    addr_cache_save_locked();
    pthread_mutex_unlock(&g_addr_cache_lock);
}
