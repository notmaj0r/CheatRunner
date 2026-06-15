#pragma once

#include <stddef.h>
#include <stdint.h>

uint8_t *http_get_url(const char *agent, const char *url, size_t *len);
int http_get_url_ex(const char *agent, const char *url, size_t max_bytes,
                    int *status_out, uint8_t **data_out, size_t *len_out);
int http_get_url_ex_timeout(const char *agent, const char *url, size_t max_bytes, int timeout_ms,
                            int *status_out, uint8_t **data_out, size_t *len_out);

/* Optional download-progress callback: called after each read chunk.
 * received = bytes read so far; total = Content-Length (0 if unknown). */
typedef void (*http_progress_fn_t)(size_t received, size_t total, void *ud);
int http_get_url_ex_timeout_progress(const char *agent, const char *url, size_t max_bytes,
                                     int timeout_ms, int *status_out, uint8_t **data_out,
                                     size_t *len_out, http_progress_fn_t pfn, void *pud);
