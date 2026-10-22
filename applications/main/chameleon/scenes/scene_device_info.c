#include "../chameleon_app.h"

/* Device info: read app version then battery, proving request/response framing. */

static char s_version[32];
static char s_battery[32];

static void chameleon_device_info_refresh(ChameleonApp* app) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 2, AlignCenter, AlignTop, FontPrimary, "Device info");
    widget_add_string_element(app->widget, 64, 26, AlignCenter, AlignTop, FontSecondary, s_version);
    widget_add_string_element(app->widget, 64, 42, AlignCenter, AlignTop, FontSecondary, s_battery);
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewWidget);
}

void chameleon_app_scene_device_info_on_enter(void* context) {
    ChameleonApp* app = context;
    strlcpy(s_version, "Version: reading...", sizeof(s_version));
    strlcpy(s_battery, "Battery: ...", sizeof(s_battery));
    chameleon_device_info_refresh(app);
    chameleon_ble_send(app->ble, CHAMELEON_CMD_GET_APP_VERSION, NULL, 0);
}

bool chameleon_app_scene_device_info_on_event(void* context, SceneManagerEvent event) {
    ChameleonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == ChameleonCustomEventBleResponse) {
            uint16_t cmd = 0, status = 0, len = 0;
            uint8_t data[CHAMELEON_RESP_MAX];
            if(chameleon_ble_take_response(app->ble, &cmd, &status, data, &len, sizeof(data))) {
                if(cmd == CHAMELEON_CMD_GET_APP_VERSION) {
                    if(status == CHAMELEON_STATUS_SUCCESS && len >= 2) {
                        snprintf(
                            s_version, sizeof(s_version), "Version: %u.%u", data[1], data[0]);
                    } else {
                        strlcpy(s_version, "Version: n/a", sizeof(s_version));
                    }
                    chameleon_device_info_refresh(app);
                    chameleon_ble_send(app->ble, CHAMELEON_CMD_GET_BATTERY_INFO, NULL, 0);
                } else if(cmd == CHAMELEON_CMD_GET_BATTERY_INFO) {
                    if(status == CHAMELEON_STATUS_SUCCESS && len >= 3) {
                        uint16_t mv = ((uint16_t)data[0] << 8) | data[1];
                        snprintf(
                            s_battery, sizeof(s_battery), "Battery: %u%%  (%umV)", data[2], mv);
                    } else {
                        strlcpy(s_battery, "Battery: n/a", sizeof(s_battery));
                    }
                    chameleon_device_info_refresh(app);
                }
            }
            consumed = true;
        } else if(event.event == ChameleonCustomEventBleDisconnected) {
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, ChameleonAppSceneConnect);
            consumed = true;
        }
    }
    return consumed;
}

void chameleon_app_scene_device_info_on_exit(void* context) {
    ChameleonApp* app = context;
    widget_reset(app->widget);
}
