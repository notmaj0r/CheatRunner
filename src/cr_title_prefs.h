#ifndef CR_TITLE_PREFS_H
#define CR_TITLE_PREFS_H
#include <stddef.h>
void title_prefs_load(void);
void title_prefs_save(void);
int  title_prefs_get_addr_mode(const char *title_id, char *out, size_t out_sz);
void title_prefs_set_addr_mode(const char *title_id, const char *mode);
void title_prefs_clear(const char *title_id);
#endif
