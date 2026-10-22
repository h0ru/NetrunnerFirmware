#include <furi.h>
#include <gui/scene_manager.h>

#include <btshim.h>
#include <wlan_hal.h>
#include <wifi.h>

#include "../desktop_i.h"
#include "../views/desktop_view_lock_menu.h"
#include "../helpers/qnetrunner_bridge.h"

#include <notification/notification_messages.h>
#include <loader/loader.h>
#include <netrunner/settings.h>
#include <furi_hal_power.h>
#include "desktop_scene.h"

#include "sdkconfig.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

void desktop_scene_lock_menu_callback(DesktopEvent event, void* context) {
    Desktop* desktop = (Desktop*)context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, event);
}

/* Wake mode starts off; the Control Centre tile owns exactly one insomnia
 * count and only acquires it when toggled on. */
static bool s_wake_mode = false;

static bool desktop_lock_menu_bt_enabled(void) {
    Bt* bt = furi_record_open(RECORD_BT);
    BtSettings settings;
    bt_get_settings(bt, &settings);
    furi_record_close(RECORD_BT);
    return settings.enabled;
}

static bool desktop_lock_menu_wifi_active(void) {
    if(wlan_hal_is_held_after_update()) return false;
    return wlan_hal_is_user_enabled() || wlan_hal_is_boot_time_sync_active();
}

static bool desktop_lock_menu_bt_active(void) {
    return desktop_lock_menu_bt_enabled();
}

static bool desktop_lock_menu_apply_bt_enabled(bool enabled) {
    Bt* bt = furi_record_open(RECORD_BT);
    const bool result = bt_set_enabled(bt, enabled);
    furi_record_close(RECORD_BT);
    return result;
}

static bool desktop_lock_menu_set_bt_enabled(Desktop* desktop, bool enabled) {
    UNUSED(desktop);
    return desktop_lock_menu_apply_bt_enabled(enabled);
}

static bool desktop_lock_menu_set_wifi_enabled(Desktop* desktop, bool enabled) {
    if(!enabled) {
        wlan_hal_set_user_enabled(false);
        desktop_set_wifi_icon_state(desktop, false);
        return true;
    }

    if(wlan_hal_set_user_enabled(true)) {
        desktop_set_wifi_icon_state(desktop, true);
        return true;
    }

    desktop_set_wifi_icon_state(desktop, false);
    return false;
}

/* The Control Centre has one radio mode at a time. Release the active stack
 * first so its controller/host memory is available to the new stack. */
static bool desktop_lock_menu_switch_to_wifi(Desktop* desktop) {
    const bool bt_was_enabled = desktop_lock_menu_bt_enabled();
    if(bt_was_enabled && !desktop_lock_menu_set_bt_enabled(desktop, false)) return false;

    if(desktop_lock_menu_set_wifi_enabled(desktop, true)) return true;

    if(bt_was_enabled) desktop_lock_menu_set_bt_enabled(desktop, true);
    return false;
}

static bool desktop_lock_menu_switch_to_bt(Desktop* desktop) {
    const bool wifi_was_enabled = desktop_lock_menu_wifi_active();
    if(wifi_was_enabled && !desktop_lock_menu_set_wifi_enabled(desktop, false)) return false;

    if(desktop_lock_menu_set_bt_enabled(desktop, true)) return true;

    if(wifi_was_enabled) desktop_lock_menu_set_wifi_enabled(desktop, true);
    return false;
}

/* Rebuild the menu from the live toggle states (used on enter and after a
 * toggle, so the Enable/Disable labels track reality). */
void desktop_scene_lock_menu_refresh(Desktop* desktop) {
    desktop_lock_menu_set_states(
        desktop->lock_menu,
        desktop_lock_menu_bt_active(),
        desktop_lock_menu_wifi_active(),
        s_wake_mode,
        netrunner_settings.dark_mode,
        qnetrunner_bridge_is_active());
}

void desktop_scene_lock_menu_on_enter(void* context) {
    Desktop* desktop = (Desktop*)context;

    /* The Netrunner Control Center owns the full canvas. */
    gui_set_status_bar_hidden(desktop->gui, true);
    desktop_lock_menu_set_callback(desktop->lock_menu, desktop_scene_lock_menu_callback, desktop);
    desktop_lock_menu_set_notification(desktop->lock_menu, desktop->notification);
    desktop_scene_lock_menu_refresh(desktop);

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdLockMenu);
}

bool desktop_scene_lock_menu_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = (Desktop*)context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case DesktopLockMenuEventBluetoothToggle:
            if(desktop_lock_menu_bt_enabled()) {
                desktop_lock_menu_set_bt_enabled(desktop, false);
            } else if(!desktop_lock_menu_switch_to_bt(desktop)) {
                desktop_lock_menu_show_message(
                    desktop->lock_menu, "Bluetooth start failed", "WiFi was restored");
            }
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;
        case DesktopLockMenuEventWifiToggle:
            if(desktop_lock_menu_wifi_active()) {
                desktop_lock_menu_set_wifi_enabled(desktop, false);
            } else if(!desktop_lock_menu_switch_to_wifi(desktop)) {
                desktop_lock_menu_show_message(
                    desktop->lock_menu, "WiFi start failed", "Bluetooth was restored");
            }
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;
        case DesktopLockMenuEventWifiScan:
            /* Open the WiFi app straight on its "Select Wifi" scan-and-connect
             * screen. The app starts the radio (and suspends BLE) itself, so no
             * pre-toggle is needed here. */
            loader_start_detached_with_gui_error(desktop->loader, "wlan", "connect");
            consumed = true;
            break;

        case DesktopLockMenuEventWakeToggle:
            s_wake_mode = !s_wake_mode;
            if(s_wake_mode) {
                furi_hal_power_insomnia_enter();
            } else {
                furi_hal_power_insomnia_exit();
            }
            notification_message(
                desktop->notification,
                s_wake_mode ? &sequence_display_backlight_enforce_on :
                              &sequence_display_backlight_enforce_auto);
            desktop_set_wake_icon_state(desktop, s_wake_mode);
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;



        case DesktopLockMenuEventDarkModeToggle:
            /* canvas.c inverts the whole UI on this flag; persist it. */
            netrunner_settings.dark_mode = !netrunner_settings.dark_mode;
            netrunner_settings_save();
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;

        case DesktopLockMenuEventStealthToggle:
            desktop_set_stealth_mode_state(
                desktop, !furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode));
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;

        case DesktopLockMenuEventLock:
            desktop_lock(desktop);
            consumed = true;
            break;

        case DesktopLockMenuEventOpenSettings:
            /* Match Netrunner: open the native Settings list. Its Desktop entry
             * owns the Current/Netrunner Control Center selector. */
            loader_show_settings(desktop->loader);
            consumed = true;
            break;

        case DesktopLockMenuEventOpenApps:
            loader_start_detached_with_gui_error(
                desktop->loader, LOADER_APPLICATIONS_NAME, NULL);
            consumed = true;
            break;

        case DesktopLockMenuEventOpenDisplay:
            loader_start_detached_with_gui_error(desktop->loader, "notification_settings", NULL);
            consumed = true;
            break;

        case DesktopLockMenuEventOpenPower:
            loader_start_detached_with_gui_error(desktop->loader, "power_settings", NULL);
            consumed = true;
            break;

        case DesktopLockMenuEventOpenPassport:
            loader_start_detached_with_gui_error(desktop->loader, "passport", NULL);
            consumed = true;
            break;

        case DesktopLockMenuEventQnetrunnerToggle:
            if(qnetrunner_bridge_is_active()) {
                qnetrunner_bridge_restore_native_serial();
            } else if(!qnetrunner_bridge_start()) {
                desktop_lock_menu_show_message(
                    desktop->lock_menu, "qNetrunner start failed", NULL);
            }
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;

        case DesktopLockMenuEventWebFs:
            /* Web-Filesystem lives in the WiFi app; launch it into that flow. */
            loader_start_detached_with_gui_error(desktop->loader, "wlan", "webfs");
            consumed = true;
            break;

        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        /* WiFi association and battery state change without a key event. */
        desktop_lock_menu_refresh_live(desktop->lock_menu);
        consumed = true;
    }

    return consumed;
}

void desktop_scene_lock_menu_on_exit(void* context) {
    Desktop* desktop = (Desktop*)context;
    gui_set_status_bar_hidden(desktop->gui, false);
}
