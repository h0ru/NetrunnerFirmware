#include "../nfc_app_i.h"

/* "Place blank Card" — second step of the Clone flow. Reached automatically
 * from NfcSceneReadSuccess once a writable card has been read in clone_mode.
 *
 * OK -> Write (NfcSceneWrite): transfer the read data onto a blank card. Write
 * is entered via scene_manager_next_scene(), so it returns here on completion —
 * the user can clone several blanks in a row. Back leaves the Clone flow
 * straight to the NFC main menu (skipping the ReadSuccess scene, which would
 * otherwise immediately re-route back here). */

static void nfc_scene_clone_place_blank_button_callback(
    GuiButtonType result,
    InputType type,
    void* context) {
    NfcApp* instance = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(instance->view_dispatcher, GuiButtonTypeCenter);
    }
}

static void nfc_scene_clone_place_blank_build_view(NfcApp* instance) {
    widget_reset(instance->widget);
    widget_add_icon_animation_element(instance->widget, 8, 12, &A_NFC_scan_28x28);
    widget_add_string_multiline_element(
        instance->widget,
        94,
        8,
        AlignCenter,
        AlignTop,
        FontSecondary,
        "Place blank\nCard");
    widget_add_button_element(
        instance->widget,
        GuiButtonTypeCenter,
        "Write",
        nfc_scene_clone_place_blank_button_callback,
        instance);
    view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewWidget);
}

void nfc_scene_clone_place_blank_on_enter(void* context) {
    NfcApp* instance = context;
    nfc_scene_clone_place_blank_build_view(instance);
}

bool nfc_scene_clone_place_blank_on_event(void* context, SceneManagerEvent event) {
    NfcApp* instance = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == GuiButtonTypeCenter) {
            dolphin_deed(DolphinDeedNfcEmulate);
            scene_manager_next_scene(instance->scene_manager, NfcSceneWrite);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        /* Skip the ReadSuccess scene (it auto-routes back here in clone_mode) */
        consumed = scene_manager_search_and_switch_to_previous_scene(
            instance->scene_manager, NfcSceneStart);
    }

    return consumed;
}

void nfc_scene_clone_place_blank_on_exit(void* context) {
    NfcApp* instance = context;
    widget_reset(instance->widget);
}
