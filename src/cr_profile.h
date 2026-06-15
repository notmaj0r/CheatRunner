#ifndef CR_PROFILE_H
#define CR_PROFILE_H

#include <stddef.h>

/* PS5 user profile — read/change the foreground user's name and avatar.
 *
 * Name change goes through sceUserServiceSetUserName (no external payload).
 * Avatar change decodes an uploaded image, builds the four DXT5 .dds sizes
 * the firmware expects, and writes them (plus online.json) into the user's
 * profile cache at /system_data/priv/cache/profile/0x<uid>/.
 *
 * Routes (dispatcher returns 1 if handled):
 *   GET  /api/profile                  - whoami (foreground user id + name)
 *   POST /api/profile/name?name=...    - set the foreground user's name
 *   POST /api/profile/avatar?mode=...  - body = raw image; build preview sizes
 *   POST /api/profile/avatar/apply     - apply built sizes to the user
 *   GET  /api/profile/avatar/preview?size=N - serve a built PNG preview
 */
int cr_api_profile_handle(int fd, const char *method, const char *path,
                          const char *query, const char *body, size_t body_len);

#endif /* CR_PROFILE_H */
