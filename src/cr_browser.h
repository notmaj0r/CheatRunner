#ifndef CR_BROWSER_H
#define CR_BROWSER_H

#ifndef CHEATRUNNER_HAVE_BROWSER_OPEN
#define CHEATRUNNER_HAVE_BROWSER_OPEN 0
#endif

/*
 * cr_browser — thin wrapper around sceSystemServiceLaunchWebBrowser.
 * Opens the system browser at the given URL.
 * NOTE: This launches the normal system browser, NOT a confirmed in-game overlay.
 *       Overlay behavior is unconfirmed — test on console before enabling.
 */
int cr_browser_open_url(const char *url);

#endif /* CR_BROWSER_H */
