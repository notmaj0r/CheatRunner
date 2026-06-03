#ifndef CR_FAVORITES_H
#define CR_FAVORITES_H

/* Server-side favorites + recents for game tiles, persisted to
 * /data/cheatrunner/favorites.json so they sync across every browser/device
 * that talks to this CheatRunner instance. */

void favorites_load(void);

/* HTTP handlers (routed from cr_api_games_handle). */
void handle_api_favorites_get(int fd);
void handle_api_favorites_set(int fd, const char *query);
void handle_api_favorites_recent(int fd, const char *query);

#endif /* CR_FAVORITES_H */
