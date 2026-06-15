#ifndef CR_FAN_H
#define CR_FAN_H

#include <stddef.h>

/* CPU/SoC fan threshold control.
 *
 * Reads console temperatures and pins a fan-on threshold via the
 * /dev/icc_fan ioctl (requires kstuff to be loaded for the device to exist).
 * A watcher thread re-applies the pinned threshold periodically because the
 * firmware resets fan state on every game/app launch. The pinned value is
 * persisted to disk so it survives a payload redeploy. */

/* Load the persisted threshold and start the re-apply watcher. Call once at
 * boot. Safe to call when /dev/icc_fan is unavailable (no-op until a set). */
void fan_init(void);

/* HTTP dispatcher — returns 1 if it handled the path, 0 otherwise.
 * Routes: /api/fan, /api/fan/temp, /api/fan/set */
int cr_api_fan_handle(int fd, const char *method, const char *path,
                      const char *query, const char *body, size_t body_len);

#endif /* CR_FAN_H */
