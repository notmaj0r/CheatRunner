#pragma once

#include <stddef.h>
#include <stdint.h>

uint8_t *http_get_url(const char *agent, const char *url, size_t *len);
int http_get_url_ex(const char *agent, const char *url, size_t max_bytes,
                    int *status_out, uint8_t **data_out, size_t *len_out);
int http_get_url_ex_timeout(const char *agent, const char *url, size_t max_bytes, int timeout_ms,
                            int *status_out, uint8_t **data_out, size_t *len_out);
