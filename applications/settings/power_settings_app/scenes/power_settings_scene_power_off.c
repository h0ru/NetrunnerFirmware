#include "../power_settings_app.h"

/* The "Turn Off Device?" dialog was replaced by a small icon picker (the
 * "System" panel): Settings / Reboot / Power Off, chosen with Left/Right and
 * OK, each followed by an in-view Cancel/Confirm prompt before it runs. */

void power_settings_scene_power_off_menu_callback(PowerMenuAction action, void* context) {
    furi_assert(context);
    PowerSettingsApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, (uint32_t)action);
}

void power_settings_scene_power_off_on_enter(void* context) {
    PowerSettingsApp* app = context;

    power_menu_set_callback(app->power_menu, power_settings_scene_power_off_menu_callback, app);
    power_menu_reset(app->power_menu);

    view_dispatcher_switch_to_view(app->view_dispatcher, PowerSettingsAppViewPowerMenu);
}

bool power_settings_scene_power_off_on_event(void* context, SceneManagerEvent event) {
    PowerSettingsApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case PowerMenuActionSettings:
            /* Return to the Power settings menu (reuse the caller's scene when
             * we were opened from it, otherwise push it). */
            if(!scene_manager_previous_scene(app->scene_manager)) {
                scene_manager_next_scene(app->scene_manager, PowerSettingsAppSceneStart);
            }
            break;
        case PowerMenuActionReboot:
            power_reboot(app->power, PowerBootModeNormal);
            break;
        case PowerMenuActionPowerOff:
            power_off(app->power);
            break;
        default:
            break;
        }
        consumed = true;
    }
    return consumed;
}

void power_settings_scene_power_off_on_exit(void* context) {
    UNUSED(context);
}
