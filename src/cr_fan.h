#ifndef CR_FAN_H
#define CR_FAN_H

#include <stddef.h>

/* Pins a fan-on threshold via /dev/icc_fan (needs kstuff); a watcher thread re-applies it since firmware resets fan state on every app launch. */

/* Loads the persisted threshold and starts the re-apply watcher; call once at boot, no-op if /dev/icc_fan is unavailable. */
void fan_init(void);

/* HTTP dispatcher — returns 1 if it handled the path, 0 otherwise.
 * Routes: /api/fan, /api/fan/temp, /api/fan/set */
int cr_api_fan_handle(int fd, const char *method, const char *path,
                      const char *query, const char *body, size_t body_len);

#endif /* CR_FAN_H */
