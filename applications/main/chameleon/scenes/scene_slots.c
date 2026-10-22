#include "../chameleon_app.h"

/* Slots: list the 8 slots with their active/HF state; selecting one sets it as
 * the device's active slot. Slot indices are 0-based on the wire. */

static void chameleon_slots_submenu_cb(void* context, uint32_t index) {
    ChameleonApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void chameleon_slots_rebuild(ChameleonApp* app) {
    Submenu* m = app->submenu;
    uint32_t selected = submenu_get_selected_item(m);
    submenu_reset(m);
    submenu_set_header(m, "Select slot");
    for(uint8_t i = 0; i < CHAMELEON_SLOT_COUNT; i++) {
        char label[32];
        const char* mark = (app->active_slot == (int8_t)i) ? "*" : "";
        const char* hf = chameleon_tag_type_name(app->slot_hf_type[i]);
        const char* lf = chameleon_tag_type_name(app->slot_lf_type[i]);
        if(!app->slot_info_valid) {
            snprintf(label, sizeof(label), "%u%s", i + 1, mark);
        } else if(!hf && !lf) {
            snprintf(label, sizeof(label), "%u%s empty", i + 1, mark);
        } else {
            snprintf(
                label, sizeof(label), "%u%s %s/%s", i + 1, mark, hf ? hf : "-", lf ? lf : "-");
        }
        submenu_add_item(m, label, i, chameleon_slots_submenu_cb, app);
    }
    submenu_set_selected_item(m, selected < CHAMELEON_SLOT_COUNT ? selected : 0);
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewSubmenu);
}

void chameleon_app_scene_slots_on_enter(void* context) {
    ChameleonApp* app = context;
    chameleon_slots_rebuild(app);
    /* Refresh cached state from the device. */
    chameleon_ble_send(app->ble, CHAMELEON_CMD_GET_ACTIVE_SLOT, NULL, 0);
}

bool chameleon_app_scene_slots_on_event(void* context, SceneManagerEvent event) {
    ChameleonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint32_t e = event.event;
        if(e < CHAMELEON_SLOT_COUNT) {
            app->selected_slot = (int8_t)e;
            scene_manager_next_scene(app->scene_manager, ChameleonAppSceneSlotAction);
            consumed = true;
        } else if(e == ChameleonCustomEventBleResponse) {
            uint16_t cmd = 0, status = 0, len = 0;
            uint8_t data[CHAMELEON_RESP_MAX];
            if(chameleon_ble_take_response(app->ble, &cmd, &status, data, &len, sizeof(data))) {
                if(cmd == CHAMELEON_CMD_GET_ACTIVE_SLOT) {
                    if(status == CHAMELEON_STATUS_SUCCESS && len >= 1 &&
                       data[0] < CHAMELEON_SLOT_COUNT) {
                        app->active_slot = (int8_t)data[0];
                    }
                    chameleon_ble_send(app->ble, CHAMELEON_CMD_GET_SLOT_INFO, NULL, 0);
                } else if(cmd == CHAMELEON_CMD_GET_SLOT_INFO) {
                    /* 8 records of (hf u16 BE, lf u16 BE); 0 = empty. */
                    if(status == CHAMELEON_STATUS_SUCCESS && len >= CHAMELEON_SLOT_COUNT * 4) {
                        for(uint8_t i = 0; i < CHAMELEON_SLOT_COUNT; i++) {
                            app->slot_hf_type[i] =
                                ((uint16_t)data[i * 4] << 8) | data[i * 4 + 1];
                            app->slot_lf_type[i] =
                                ((uint16_t)data[i * 4 + 2] << 8) | data[i * 4 + 3];
                        }
                        app->slot_info_valid = true;
                    }
                    chameleon_slots_rebuild(app);
                }
            }
            consumed = true;
        } else if(e == ChameleonCustomEventBleDisconnected) {
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, ChameleonAppSceneConnect);
            consumed = true;
        }
    }
    return consumed;
}

void chameleon_app_scene_slots_on_exit(void* context) {
    ChameleonApp* app = context;
    submenu_reset(app->submenu);
}
