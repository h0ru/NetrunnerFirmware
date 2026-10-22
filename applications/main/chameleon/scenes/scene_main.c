#include "../chameleon_app.h"

enum {
    SubmenuDeviceInfo,
    SubmenuSlots,
    SubmenuReader,
};

static void chameleon_scene_main_submenu_cb(void* context, uint32_t index) {
    ChameleonApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void chameleon_app_scene_main_on_enter(void* context) {
    ChameleonApp* app = context;
    Submenu* m = app->submenu;
    submenu_reset(m);
    submenu_set_header(m, "Chameleon");
    submenu_add_item(m, "Device info", SubmenuDeviceInfo, chameleon_scene_main_submenu_cb, app);
    submenu_add_item(m, "Slots", SubmenuSlots, chameleon_scene_main_submenu_cb, app);
    submenu_add_item(m, "Reader", SubmenuReader, chameleon_scene_main_submenu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewSubmenu);
}

bool chameleon_app_scene_main_on_event(void* context, SceneManagerEvent event) {
    ChameleonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case SubmenuDeviceInfo:
            scene_manager_next_scene(app->scene_manager, ChameleonAppSceneDeviceInfo);
            consumed = true;
            break;
        case SubmenuSlots:
            scene_manager_next_scene(app->scene_manager, ChameleonAppSceneSlots);
            consumed = true;
            break;
        case SubmenuReader:
            scene_manager_next_scene(app->scene_manager, ChameleonAppSceneReader);
            consumed = true;
            break;
        case ChameleonCustomEventBleDisconnected:
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, ChameleonAppSceneConnect);
            consumed = true;
            break;
        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        /* Root of the usable UI: Back exits the app rather than returning to the
         * Connect scene (which would start a fresh scan). */
        scene_manager_stop(app->scene_manager);
        view_dispatcher_stop(app->view_dispatcher);
        consumed = true;
    }
    return consumed;
}

void chameleon_app_scene_main_on_exit(void* context) {
    ChameleonApp* app = context;
    submenu_reset(app->submenu);
}
