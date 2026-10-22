#pragma once

#include <gui/view.h>
#include "desktop_events.h"
#include "../desktop_settings.h"

#define HINT_TIMEOUT 2

typedef struct NotificationApp NotificationApp;

typedef struct DesktopLockMenuView DesktopLockMenuView;

typedef void (*DesktopLockMenuViewCallback)(DesktopEvent event, void* context);

struct DesktopLockMenuView {
    View* view;
    DesktopLockMenuViewCallback callback;
    void* context;
    /* Brightness/Volume are adjusted live from inside the view, mirroring
     * Netrunner's Control Centre. NULL is tolerated (sliders read as empty). */
    NotificationApp* notification;
};

/* Control Centre model: Netrunner's fixed tile grid, adapted to the wheel. */
typedef struct {
    uint8_t idx; // selected item
    bool adjust; // slider adjust sub-mode
    // Toggle states, filled by desktop_lock_menu_set_states():
    bool bt_on;
    bool wifi_on;
    bool wake_on;
    bool dark_on;
    bool qnetrunner_on;
} DesktopLockMenuViewModel;

void desktop_lock_menu_set_callback(
    DesktopLockMenuView* lock_menu,
    DesktopLockMenuViewCallback callback,
    void* context);

View* desktop_lock_menu_get_view(DesktopLockMenuView* lock_menu);

/** Give the view the notification service so the Brightness/Volume controls can
 *  read and adjust the live values. Safe to pass NULL. */
void desktop_lock_menu_set_notification(
    DesktopLockMenuView* lock_menu,
    NotificationApp* notification);

/** Draw a short message over the menu until the next key press. Used for
 *  failures that would otherwise be invisible (e.g. a WiFi start failure).
 *  Pass NULL hint for a message that needs no follow-up. */
void desktop_lock_menu_show_message(
    DesktopLockMenuView* lock_menu,
    const char* text,
    const char* hint);

/** Refresh the toggle states drawn on the Control Center tiles. */
void desktop_lock_menu_set_states(
    DesktopLockMenuView* lock_menu,
    bool bt_on,
    bool wifi_on,
    bool wake_on,
    bool dark_on,
    bool qnetrunner_on);

/** Redraw live status values (WiFi connection and battery) without rebuilding
 *  the tile list or disturbing wheel/slider state. */
void desktop_lock_menu_refresh_live(DesktopLockMenuView* lock_menu);

DesktopLockMenuView* desktop_lock_menu_alloc(void);
void desktop_lock_menu_free(DesktopLockMenuView* lock_menu);
