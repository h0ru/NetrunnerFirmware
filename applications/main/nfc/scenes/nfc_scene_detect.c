#include "../nfc_app_i.h"
#include <dolphin/dolphin.h>

/* The "Read" screen: the nfc_scanner (PN532) detects a card and the scene
 * advances to NfcSceneRead, or to NfcSceneSelectProtocol when more than one
 * protocol is detected. */

static void nfc_scene_detect_build_view(NfcApp* instance) {
    widget_reset(instance->widget);
    /* Animated NFC scan GIF, centered, with the action word below it. */
    widget_add_icon_animation_element(instance->widget, 50, 2, &A_NFC_scan_28x28);
    widget_add_string_element(
        instance->widget, 64, 36, AlignCenter, AlignTop, FontPrimary, "Reading");
    widget_add_string_element(
        instance->widget, 64, 60, AlignCenter, AlignBottom, FontSecondary, "Hold card to Device");
    view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewWidget);
}

void nfc_scene_detect_scan_callback(NfcScannerEvent event, void* context) {
    furi_assert(context);

    NfcApp* instance = context;

    if(event.type == NfcScannerEventTypeDetected) {
        nfc_detected_protocols_set(
            instance->detected_protocols, event.data.protocols, event.data.protocol_num);
        view_dispatcher_send_custom_event(instance->view_dispatcher, NfcCustomEventWorkerExit);
    }
}

void nfc_scene_detect_on_enter(void* context) {
    NfcApp* instance = context;

    /* Detecting a card needs no SD parser plugins. Load them in the Read
     * scene after detection instead of blocking entry to the scanner. */

    nfc_detected_protocols_reset(instance->detected_protocols);
    nfc_scene_detect_build_view(instance);

    instance->scanner = nfc_scanner_alloc(instance->nfc);
    nfc_scanner_start(instance->scanner, nfc_scene_detect_scan_callback, instance);

    nfc_blink_detect_start(instance);
}

bool nfc_scene_detect_on_event(void* context, SceneManagerEvent event) {
    NfcApp* instance = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == NfcCustomEventWorkerExit) {
            if(nfc_detected_protocols_get_num(instance->detected_protocols) > 1) {
                notification_message(instance->notifications, &sequence_single_vibro);
                scene_manager_next_scene(instance->scene_manager, NfcSceneSelectProtocol);
            } else {
                scene_manager_next_scene(instance->scene_manager, NfcSceneRead);
            }
            consumed = true;
        }
    }

    return consumed;
}

void nfc_scene_detect_on_exit(void* context) {
    NfcApp* instance = context;

    nfc_scanner_stop(instance->scanner);
    nfc_scanner_free(instance->scanner);
    widget_reset(instance->widget);
    popup_reset(instance->popup);

    nfc_blink_stop(instance);
}
