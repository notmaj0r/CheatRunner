#ifndef CR_TITLES_H
#define CR_TITLES_H

#include <stddef.h>

typedef struct game_entry {
  char title_id[16];
  char title_name[256];
  char content_id[96];
  char name_source[32];
  int  name_resolved;
} game_entry_t;

int is_valid_title_id(const char *title_id);
int is_game_title_id(const char *title_id);
int cr_title_name_is_unresolved(const char *title_id, const char *name);
int cr_title_id_normalize(const char *in, char out[10]);
int title_id_normalize(const char *in, char out[10]);
int find_title_id_in_string(const char *s, char out[10]);
int extract_title_id_prefix(const char *filename, char *out, size_t out_size);
int case_contains(const char *haystack, const char *needle);

const char *platform_for_title_id(const char *title_id);
int cr_title_is_known_media_app(const char *title_id, const char *name);

int read_param_value_by_title_id(const char *title_id, const char *key, char *out, size_t out_size);
int read_param_value_from_sfo(const char *sfo_path, const char *key, char *out, size_t out_size);
int load_title_meta(const char *dir_path, const char *fallback_id, char *title_id, size_t id_size,
                    char *title_name, size_t name_size);

#endif /* CR_TITLES_H */
