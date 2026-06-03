#ifndef CR_STORE_LOOKUP_H
#define CR_STORE_LOOKUP_H

#include <stddef.h>

/* Fetch game name from PlayStation Store product page using contentId.
 * content_id: 36-char PlayStation content ID (e.g. UP9000-CUSA00001_00-...).
 * Returns 1 and fills name_out on success, 0 on failure (network error, not found, parse error).
 * timeout_ms: per-request timeout (0 = library default). */
int cr_store_lookup(const char *content_id, char *name_out, size_t name_size, int timeout_ms);

#endif /* CR_STORE_LOOKUP_H */
