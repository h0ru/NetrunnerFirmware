#include "../ble_spam_app.h"
#include "../ble_detector_embed.h"

/* Scan-kind menu for the embedded BLE Detector. Selecting a kind drops into the
 * results scene; no app switch, so no main-menu flash. */

static void ble_spam_scene_ble_detector_menu_cb(void* context, uint32_t index) {
    BleSpamApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void ble_spam_scene_ble_detector_on_enter(void* context) {
    BleSpamApp* app = context;
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "BLE Detector");
    ble_detector_build_menu(app->submenu, ble_spam_scene_ble_detector_menu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewSubmenu);
}

bool ble_spam_scene_ble_detector_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        /* The custom event is the selected DetectorKind index. */
        app->ble_detector_kind = event.event;
        scene_manager_next_scene(app->scene_manager, BleSpamSceneBleDetectorResults);
        consumed = true;
    }
    return consumed;
}

void ble_spam_scene_ble_detector_on_exit(void* context) {
    BleSpamApp* app = context;
    submenu_reset(app->submenu);
}
