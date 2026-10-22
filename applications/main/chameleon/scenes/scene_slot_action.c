#include "../chameleon_app.h"

/* Action menu for a selected slot: Emulate (set active + emulator mode) or
 * Write to a physical card. Write is LF-only (EM410x -> T5577); HF card writing
 * needs magic-card block writes and isn't supported here. */

typedef enum {
    ActNone,
    ActEmulate,
    ActWrite,
} SlotAct;

/* Emulate: SET_ACTIVE_SLOT -> CHANGE_DEVICE_MODE(emulator).
 * Write (LF): SET_ACTIVE_SLOT -> reader mode -> GET_EMU_ID -> WRITE_T55XX. */
static SlotAct s_act;
static int s_step;
static bool s_write_hf; /* ActWrite target: HF (Gen2 magic) vs LF (T5577) */
static uint8_t s_lfid[5];
static uint8_t s_uid[10];
static uint8_t s_uidlen;
static uint8_t s_sak;
static uint8_t s_atqa[2];

enum { MenuEmulate, MenuWrite };

static void chameleon_slot_action_submenu_cb(void* context, uint32_t index) {
    ChameleonApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void chameleon_slot_action_popup_cb(void* context) {
    ChameleonApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, ChameleonCustomEventSaveDone);
}

static void chameleon_slot_action_confirm_cb(GuiButtonType type, InputType input, void* context) {
    ChameleonApp* app = context;
    if(input == InputTypeShort && type == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, ChameleonCustomEventWriteGo);
    }
}

/* Let the user position the card before the one-shot write. */
static void chameleon_slot_action_show_confirm(ChameleonApp* app) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 4, AlignCenter, AlignTop, FontPrimary,
        s_write_hf ? "Write to magic card" : "Write to T5577");
    widget_add_string_element(
        app->widget, 64, 28, AlignCenter, AlignTop, FontSecondary,
        s_write_hf ? "Hold a Gen2 magic card\non the Chameleon" :
                     "Hold a blank T5577 on\nthe Chameleon");
    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "Write", chameleon_slot_action_confirm_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewWidget);
}

static void chameleon_slot_action_show_menu(ChameleonApp* app) {
    s_act = ActNone;
    Submenu* m = app->submenu;
    submenu_reset(m);
    char header[24];
    snprintf(header, sizeof(header), "Slot %d", app->selected_slot + 1);
    submenu_set_header(m, header);
    submenu_add_item(m, "Emulate", MenuEmulate, chameleon_slot_action_submenu_cb, app);
    submenu_add_item(m, "Write to card", MenuWrite, chameleon_slot_action_submenu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewSubmenu);
}

static void
    chameleon_slot_action_show_status(ChameleonApp* app, const char* header, const char* text,
                                      bool auto_return) {
    Popup* popup = app->popup;
    popup_reset(popup);
    popup_set_header(popup, header, 64, 20, AlignCenter, AlignCenter);
    if(text) popup_set_text(popup, text, 64, 40, AlignCenter, AlignCenter);
    if(auto_return) {
        popup_set_context(popup, app);
        popup_set_callback(popup, chameleon_slot_action_popup_cb);
        popup_set_timeout(popup, 1500);
        popup_enable_timeout(popup);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewPopup);
}

static bool chameleon_slot_ok(uint16_t status) {
    return status == CHAMELEON_STATUS_SUCCESS || status == CHAMELEON_STATUS_LF_TAG_OK ||
           status == CHAMELEON_STATUS_HF_TAG_OK;
}

/* Issue the command for the current (s_act, s_step). */
static void chameleon_slot_action_send(ChameleonApp* app) {
    uint8_t slot = (uint8_t)app->selected_slot;
    if(s_act == ActEmulate) {
        if(s_step == 0) {
            chameleon_ble_send(app->ble, CHAMELEON_CMD_SET_ACTIVE_SLOT, &slot, 1);
        } else {
            uint8_t mode = CHAMELEON_MODE_EMULATOR;
            chameleon_ble_send(app->ble, CHAMELEON_CMD_CHANGE_DEVICE_MODE, &mode, 1);
        }
    } else { /* ActWrite: set active -> reader -> read slot data -> write card */
        switch(s_step) {
        case 0:
            chameleon_ble_send(app->ble, CHAMELEON_CMD_SET_ACTIVE_SLOT, &slot, 1);
            break;
        case 1: {
            uint8_t mode = CHAMELEON_MODE_READER;
            chameleon_ble_send(app->ble, CHAMELEON_CMD_CHANGE_DEVICE_MODE, &mode, 1);
            break;
        }
        case 2:
            chameleon_ble_send(
                app->ble,
                s_write_hf ? CHAMELEON_CMD_HF14A_GET_ANTICOLL : CHAMELEON_CMD_EM410X_GET_EMU_ID,
                NULL, 0);
            break;
        case 3:
            /* Let the reader field settle so the card is powered for the write. */
            furi_delay_ms(60);
            if(s_write_hf) {
                chameleon_ble_write_mifare_block0(
                    app->ble, s_uid, s_uidlen, s_sak, s_atqa);
            } else {
                chameleon_ble_write_t5577_em410x(app->ble, s_lfid);
            }
            break;
        }
    }
}

void chameleon_app_scene_slot_action_on_enter(void* context) {
    ChameleonApp* app = context;
    chameleon_slot_action_show_menu(app);
}

bool chameleon_app_scene_slot_action_on_event(void* context, SceneManagerEvent event) {
    ChameleonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint32_t e = event.event;
        if(e == ChameleonCustomEventSaveDone) {
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, ChameleonAppSceneSlots);
            consumed = true;
        } else if(e == ChameleonCustomEventBleDisconnected) {
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, ChameleonAppSceneConnect);
            consumed = true;
        } else if(s_act == ActNone && e == MenuEmulate) {
            s_act = ActEmulate;
            s_step = 0;
            chameleon_slot_action_show_status(app, "Starting...", NULL, false);
            chameleon_slot_action_send(app);
            consumed = true;
        } else if(s_act == ActNone && e == MenuWrite) {
            /* LF (T5577) takes precedence; otherwise an HF (Gen2 magic) write. */
            if(app->slot_lf_type[app->selected_slot] != 0) {
                s_act = ActWrite;
                s_write_hf = false;
                s_step = 0;
                chameleon_slot_action_show_confirm(app);
            } else if(app->slot_hf_type[app->selected_slot] != 0) {
                s_act = ActWrite;
                s_write_hf = true;
                s_step = 0;
                chameleon_slot_action_show_confirm(app);
            } else {
                chameleon_slot_action_show_status(app, "Slot empty", "Nothing to write", true);
            }
            consumed = true;
        } else if(e == ChameleonCustomEventWriteGo && s_act == ActWrite) {
            chameleon_slot_action_show_status(
                app, "Writing...", s_write_hf ? "Writing to card" : "Writing to T5577", false);
            chameleon_slot_action_send(app);
            consumed = true;
        } else if(e == ChameleonCustomEventBleResponse && s_act != ActNone) {
            uint16_t cmd = 0, status = 0, len = 0;
            uint8_t data[CHAMELEON_RESP_MAX];
            if(chameleon_ble_take_response(app->ble, &cmd, &status, data, &len, sizeof(data))) {
                if(!chameleon_slot_ok(status)) {
                    s_act = ActNone;
                    /* Show the device's reason so the failure is diagnosable
                     * (no card, auth = not gen2, wrong mode, no reply...). */
                    const char* r = chameleon_status_reason(status);
                    char reason[24];
                    if(!r) {
                        snprintf(reason, sizeof(reason), "Status 0x%02X",
                                 (unsigned)(status & 0xFF));
                        r = reason;
                    }
                    chameleon_slot_action_show_status(app, "Failed", r, true);
                } else if(s_act == ActEmulate) {
                    if(s_step == 0) {
                        s_step = 1;
                        chameleon_slot_action_send(app);
                    } else {
                        s_act = ActNone;
                        app->active_slot = app->selected_slot;
                        char sub[24];
                        snprintf(sub, sizeof(sub), "Slot %d", app->selected_slot + 1);
                        chameleon_slot_action_show_status(app, "Emulating", sub, true);
                    }
                } else { /* ActWrite */
                    if(s_step == 2) {
                        /* Slot data reply: capture the id/UID, then write. */
                        bool got = false;
                        if(s_write_hf) {
                            /* uidlen | uid | atqa(2) | sak(1) | ... */
                            uint8_t ul = len ? data[0] : 0;
                            if((ul == 4 || ul == 7) && len >= (uint16_t)(1 + ul + 3)) {
                                memcpy(s_uid, &data[1], ul);
                                s_uidlen = ul;
                                s_atqa[0] = data[1 + ul];
                                s_atqa[1] = data[1 + ul + 1];
                                s_sak = data[1 + ul + 2];
                                got = true;
                            }
                        } else if(len >= 5) {
                            memcpy(s_lfid, data, 5);
                            got = true;
                        }
                        if(got) {
                            s_step = 3;
                            chameleon_slot_action_send(app);
                        } else {
                            s_act = ActNone;
                            chameleon_slot_action_show_status(
                                app, "Write failed", "Need 4/7-byte UID", true);
                        }
                    } else if(s_step == 3) {
                        s_act = ActNone;
                        chameleon_slot_action_show_status(
                            app, "Written", s_write_hf ? "to card" : "to T5577", true);
                    } else {
                        s_step++;
                        chameleon_slot_action_send(app);
                    }
                }
            }
            consumed = true;
        }
    }
    return consumed;
}

void chameleon_app_scene_slot_action_on_exit(void* context) {
    ChameleonApp* app = context;
    submenu_reset(app->submenu);
    popup_reset(app->popup);
}
