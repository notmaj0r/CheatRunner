#ifndef CR_ADDR_CACHE_H
#define CR_ADDR_CACHE_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct {
    intptr_t addr;
    uint8_t  orig_bytes[128];
    size_t   orig_len;
} addr_cache_entry_t;
void addr_cache_load(void);
void addr_cache_save(void);
int  addr_cache_get(const char *path, time_t mtime, int mod_idx, int entry_idx, addr_cache_entry_t *out);
void addr_cache_set(const char *path, time_t mtime, int mod_idx, int entry_idx, intptr_t addr, const uint8_t *orig_bytes, size_t orig_len);
void addr_cache_clear_for_path(const char *path);
#endif
