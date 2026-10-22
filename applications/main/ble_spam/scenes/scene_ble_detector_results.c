#include "../ble_spam_app.h"
#include "../ble_detector_embed.h"

/* Live results for the embedded BLE Detector. Hosted by the Bluetooth app's own
 * view_dispatcher: the radio comes up on enter and is released on exit, and the
 * 250 ms scene tick refreshes the list. */

void ble_spam_scene_ble_detector_results_on_enter(void* context) {
    BleSpamApp* app = context;
    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewBleDetector);
    ble_detector_start(app->ble_detector, app->ble_detector_kind);
}

bool ble_spam_scene_ble_detector_results_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeTick) {
        ble_detector_tick(app->ble_detector);
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case BleDetectorEventUp:
            ble_detector_nav_up(app->ble_detector);
            consumed = true;
            break;
        case BleDetectorEventDown:
            ble_detector_nav_down(app->ble_detector);
            consumed = true;
            break;
        case BleDetectorEventToggle:
            ble_detector_toggle(app->ble_detector);
            consumed = true;
            break;
        default:
            break;
        }
    }
    return consumed;
}

void ble_spam_scene_ble_detector_results_on_exit(void* context) {
    BleSpamApp* app = context;
    ble_detector_stop(app->ble_detector);
}
