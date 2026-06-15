#ifndef CR_TILE_PKG_H
#define CR_TILE_PKG_H

/* Spawn the background thread that auto-installs the CheatRunner home-screen
 * tile PKG on first boot.  Safe to call unconditionally — is a no-op when
 * the PKG was not embedded at build time or when the tile is already
 * installed. */
void cr_tile_autoinstall_init(void);

#endif /* CR_TILE_PKG_H */
