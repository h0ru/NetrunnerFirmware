#include "../chameleon_app.h"
#include <assets_icons.h>

/* Connect screen: scan for and connect to a ChameleonUltra over BLE. The whole
 * lifecycle lives in chameleon_ble; this scene shows status and reacts to the
 * Connected / ConnectFailed events. */

static void chameleon_scene_connect_show_searching(ChameleonApp* app) {
    widget_reset(app->widget);
    /* Same Bluetooth mark the firmware uses for BLE (also this app's menu icon),
     * animated as the scan indicator. */
    widget_add_icon_animation_element(app->widget, 57, 14, &A_Bluetooth_14);
    widget_add_string_element(
        app->widget, 64, 36, AlignCenter, AlignTop, FontPrimary, "Searching...");
    widget_add_string_element(
        app->widget, 64, 60, AlignCenter, AlignBottom, FontSecondary, "Power on the Chameleon");
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewWidget);
}

static void chameleon_scene_connect_retry_cb(GuiButtonType type, InputType input, void* context) {
    ChameleonApp* app = context;
    if(input == InputTypeShort && type == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, ChameleonCustomEventRetry);
    }
}

static void chameleon_scene_connect_show_error(ChameleonApp* app) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 16, AlignCenter, AlignTop, FontPrimary, "Not connected");
    widget_add_string_element(
        app->widget, 64, 36, AlignCenter, AlignTop, FontSecondary,
        chameleon_ble_last_error(app->ble));
    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "Retry", chameleon_scene_connect_retry_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewWidget);
}

void chameleon_app_scene_connect_on_enter(void* context) {
    ChameleonApp* app = context;
    chameleon_scene_connect_show_searching(app);
    chameleon_led_scan_start(app);
    chameleon_app_start_connect(app);
}

bool chameleon_app_scene_connect_on_event(void* context, SceneManagerEvent event) {
    ChameleonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case ChameleonCustomEventBleConnected:
            chameleon_led_reset(app);
            scene_manager_next_scene(app->scene_manager, ChameleonAppSceneMain);
            consumed = true;
            break;
        case ChameleonCustomEventBleConnectFailed:
            chameleon_led_reset(app);
            chameleon_scene_connect_show_error(app);
            consumed = true;
            break;
        case ChameleonCustomEventRetry:
            chameleon_scene_connect_show_searching(app);
            chameleon_led_scan_start(app);
            chameleon_app_start_connect(app);
            consumed = true;
            break;
        default:
            break;
        }
    }
    return consumed;
}

void chameleon_app_scene_connect_on_exit(void* context) {
    ChameleonApp* app = context;
    chameleon_led_reset(app);
    widget_reset(app->widget);
}
