#ifndef CR_PROFILE_H
#define CR_PROFILE_H

#include <stddef.h>

/* Read/change foreground user's name (sceUserServiceSetUserName) and avatar (builds 4 DXT5 .dds sizes into the profile cache). */
int cr_api_profile_handle(int fd, const char *method, const char *path,
                          const char *query, const char *body, size_t body_len);

#endif /* CR_PROFILE_H */
