#pragma once
#include "../view.h"
#include "../canvas.h"
#include "../icon.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Loading anonymous structure */
typedef struct Loading Loading;

/** Allocate and initialize
 *
 * This View used to show system is doing some processing
 *
 * @return     Loading View instance
 */
Loading* loading_alloc(void);

/** Deinitialize and free Loading View
 *
 * @param      instance  Loading instance
 */
void loading_free(Loading* instance);

/** Get Loading view
 *
 * @param      instance  Loading instance
 *
 * @return     View instance that can be used for embedding
 */
View* loading_get_view(Loading* instance);

/** Use a specific icon on this Loading view, overriding the running app's icon
 *
 * Takes effect immediately, also while the view is on screen.
 *
 * @param      instance  Loading instance
 * @param      icon      Icon (may be animated), or NULL to go back to the
 *                       running app's icon / the default hourglass
 */
void loading_set_icon(Loading* instance, const Icon* icon);

/** Set the icon of the app that is currently running
 *
 * The loader calls this when it starts and stops an app. Every loading
 * indicator (the Loading view, file browser, archive) then shows the running
 * app's menu icon instead of the generic hourglass.
 *
 * @param      icon      app icon, or NULL when no app is running
 */
void loading_set_app_icon(const Icon* icon);

/** Get the icon of the app that is currently running, or NULL */
const Icon* loading_get_app_icon(void);

/** Draw the loading indicator centered at (cx, cy)
 *
 * Draws the running app's icon at 2x, or the default 24x24 hourglass when no
 * app icon is set. For views that draw a static loading frame themselves.
 */
void loading_draw_indicator(Canvas* canvas, int32_t cx, int32_t cy);

#ifdef __cplusplus
}
#endif
