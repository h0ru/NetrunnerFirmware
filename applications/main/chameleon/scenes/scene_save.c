#include "../chameleon_app.h"
#include <storage/storage.h>
#include <netrunner_format.h>

/* Save the last read card/tag to either the Chameleon Ultra (a slot) or the
 * T-Embed's SD card (a native .nfc / .rfid file the NFC app / Archive can open).
 * Only the UID is available from a scan, so both are UID-level saves. After a
 * save the scene returns to the Read scene to keep scanning. */

#define CHAMELEON_SD_NFC_FOLDER   EXT_PATH("nfc")
#define CHAMELEON_SD_LFRFID_FOLDER EXT_PATH("lfrfid")

typedef enum {
    SaveStateDest, /* choosing destination */
    SaveStateSlot, /* choosing a Chameleon slot */
} SaveState;

typedef enum {
    StepSetActive,
    StepSetType,
    StepEnable,
    StepSetData,
    StepSave,
} SaveStep;

static SaveState s_state;
static SaveStep s_step;
static uint8_t s_slot;
static bool s_saving;

enum { DestChameleon, DestTEmbed };

/* ---- helpers -------------------------------------------------------------- */

static void chameleon_save_hexstr(const uint8_t* d, uint8_t n, char* out, size_t cap) {
    out[0] = '\0';
    for(uint8_t i = 0; i < n; i++) {
        char b[4];
        snprintf(b, sizeof(b), "%02X", d[i]);
        strlcat(out, b, cap);
    }
}

static void chameleon_save_submenu_cb(void* context, uint32_t index) {
    ChameleonApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void chameleon_save_popup_cb(void* context) {
    ChameleonApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, ChameleonCustomEventSaveDone);
}

static void chameleon_save_show_dest(ChameleonApp* app) {
    s_state = SaveStateDest;
    Submenu* m = app->submenu;
    submenu_reset(m);
    submenu_set_header(m, "Save to");
    submenu_add_item(m, "Chameleon Ultra", DestChameleon, chameleon_save_submenu_cb, app);
    submenu_add_item(m, "T-Embed (SD)", DestTEmbed, chameleon_save_submenu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewSubmenu);
}

static void chameleon_save_show_slots(ChameleonApp* app) {
    s_state = SaveStateSlot;
    Submenu* m = app->submenu;
    submenu_reset(m);
    submenu_set_header(m, app->read_was_lf ? "Save LF to slot" : "Save HF to slot");
    for(uint8_t i = 0; i < CHAMELEON_SLOT_COUNT; i++) {
        char label[32];
        const char* mark = (app->active_slot == (int8_t)i) ? "*" : "";
        uint16_t t = app->read_was_lf ? app->slot_lf_type[i] : app->slot_hf_type[i];
        const char* name = app->slot_info_valid ? chameleon_tag_type_name(t) : NULL;
        if(app->slot_info_valid)
            snprintf(label, sizeof(label), "%u%s %s", i + 1, mark, name ? name : "empty");
        else
            snprintf(label, sizeof(label), "Slot %u%s", i + 1, mark);
        submenu_add_item(m, label, i, chameleon_save_submenu_cb, app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewSubmenu);
}

static void chameleon_save_show_result(ChameleonApp* app, const char* header, const char* text) {
    Popup* popup = app->popup;
    popup_reset(popup);
    popup_set_header(popup, header, 64, 20, AlignCenter, AlignCenter);
    if(text) popup_set_text(popup, text, 64, 40, AlignCenter, AlignCenter);
    popup_set_context(popup, app);
    popup_set_callback(popup, chameleon_save_popup_cb);
    popup_set_timeout(popup, 1500);
    popup_enable_timeout(popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewPopup);
}

/* ---- T-Embed (SD) save ---------------------------------------------------- */

static bool chameleon_save_to_sd(ChameleonApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    NetrunnerFormat* ff = netrunner_format_file_alloc(storage);
    char hex[24];
    char path[96];
    bool ok = false;

    do {
        if(app->read_was_lf) {
            /* Short default name: LF_ + last 2 id bytes. */
            chameleon_save_hexstr(&app->read_lfid[3], 2, hex, sizeof(hex));
            storage_simply_mkdir(storage, CHAMELEON_SD_LFRFID_FOLDER);
            snprintf(path, sizeof(path), "%s/LF_%s.rfid", CHAMELEON_SD_LFRFID_FOLDER, hex);
            if(!netrunner_format_file_open_always(ff, path)) break;
            if(!netrunner_format_write_header_cstr(ff, "Netrunner RFID key", 1)) break;
            if(!netrunner_format_write_string_cstr(ff, "Key type", "EM4100")) break;
            if(!netrunner_format_write_hex(ff, "Data", app->read_lfid, 5)) break;
        } else {
            /* Short default name: HF_ + last 2 UID bytes. */
            uint8_t off = app->read_uidlen >= 2 ? app->read_uidlen - 2 : 0;
            chameleon_save_hexstr(&app->read_uid[off], app->read_uidlen >= 2 ? 2 : app->read_uidlen,
                                  hex, sizeof(hex));
            storage_simply_mkdir(storage, CHAMELEON_SD_NFC_FOLDER);
            snprintf(path, sizeof(path), "%s/HF_%s.nfc", CHAMELEON_SD_NFC_FOLDER, hex);
            /* ATQA is stored most-significant-byte first in the file. */
            const uint8_t atqa_msb[2] = {app->read_atqa[1], app->read_atqa[0]};
            if(!netrunner_format_file_open_always(ff, path)) break;
            if(!netrunner_format_write_header_cstr(ff, "Netrunner NFC device", 4)) break;
            if(!netrunner_format_write_string_cstr(ff, "Device type", "ISO14443-3A")) break;
            if(!netrunner_format_write_hex(ff, "UID", app->read_uid, app->read_uidlen)) break;
            if(!netrunner_format_write_hex(ff, "ATQA", atqa_msb, 2)) break;
            if(!netrunner_format_write_hex(ff, "SAK", &app->read_sak, 1)) break;
        }
        ok = true;
    } while(false);

    netrunner_format_free(ff);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

/* ---- Chameleon slot write sequence ---------------------------------------- */

static void chameleon_save_send_step(ChameleonApp* app) {
    switch(s_step) {
    case StepSetActive: {
        uint8_t arg = s_slot;
        chameleon_ble_send(app->ble, CHAMELEON_CMD_SET_ACTIVE_SLOT, &arg, 1);
        break;
    }
    case StepSetType: {
        uint16_t type = app->read_was_lf ? CHAMELEON_TAG_EM410X :
                        (app->read_sak == 0x00) ? CHAMELEON_TAG_NTAG_215 :
                                                   CHAMELEON_TAG_MIFARE_1K;
        uint8_t arg[3] = {s_slot, (uint8_t)(type >> 8), (uint8_t)(type & 0xFF)};
        chameleon_ble_send(app->ble, CHAMELEON_CMD_SET_SLOT_TAG_TYPE, arg, sizeof(arg));
        break;
    }
    case StepEnable: {
        uint8_t arg[3] = {
            s_slot, app->read_was_lf ? CHAMELEON_SENSE_LF : CHAMELEON_SENSE_HF, 1};
        chameleon_ble_send(app->ble, CHAMELEON_CMD_SET_SLOT_ENABLE, arg, sizeof(arg));
        break;
    }
    case StepSetData:
        if(app->read_was_lf) {
            chameleon_ble_send(app->ble, CHAMELEON_CMD_EM410X_SET_EMU_ID, app->read_lfid, 5);
        } else {
            uint8_t arg[1 + 10 + 2 + 1 + 1];
            uint8_t n = 0;
            arg[n++] = app->read_uidlen;
            memcpy(&arg[n], app->read_uid, app->read_uidlen);
            n += app->read_uidlen;
            arg[n++] = app->read_atqa[0];
            arg[n++] = app->read_atqa[1];
            arg[n++] = app->read_sak;
            arg[n++] = 0; /* ATS length */
            chameleon_ble_send(app->ble, CHAMELEON_CMD_HF14A_SET_ANTICOLL, arg, n);
        }
        break;
    case StepSave:
        chameleon_ble_send(app->ble, CHAMELEON_CMD_SLOT_DATA_SAVE, NULL, 0);
        break;
    }
}

/* ---- scene ---------------------------------------------------------------- */

void chameleon_app_scene_save_on_enter(void* context) {
    ChameleonApp* app = context;
    s_saving = false;
    chameleon_save_show_dest(app);
}

bool chameleon_app_scene_save_on_event(void* context, SceneManagerEvent event) {
    ChameleonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint32_t e = event.event;
        if(e == ChameleonCustomEventSaveDone) {
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, ChameleonAppSceneRead);
            consumed = true;
        } else if(e == ChameleonCustomEventBleDisconnected) {
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, ChameleonAppSceneConnect);
            consumed = true;
        } else if(s_state == SaveStateDest && e == DestChameleon) {
            chameleon_save_show_slots(app);
            chameleon_ble_send(app->ble, CHAMELEON_CMD_GET_SLOT_INFO, NULL, 0);
            consumed = true;
        } else if(s_state == SaveStateDest && e == DestTEmbed) {
            bool ok = chameleon_save_to_sd(app);
            chameleon_save_show_result(
                app, ok ? "Saved to SD" : "Save failed",
                ok ? (app->read_was_lf ? "/lfrfid" : "/nfc") : "SD error");
            consumed = true;
        } else if(s_state == SaveStateSlot && e < CHAMELEON_SLOT_COUNT && !s_saving) {
            s_slot = (uint8_t)e;
            s_step = StepSetActive;
            s_saving = true;
            chameleon_save_show_result(app, "Saving...", NULL);
            /* The popup has a timeout; cancel auto-return while writing. */
            popup_disable_timeout(app->popup);
            chameleon_save_send_step(app);
            consumed = true;
        } else if(e == ChameleonCustomEventBleResponse) {
            uint16_t cmd = 0, status = 0, len = 0;
            uint8_t data[CHAMELEON_RESP_MAX];
            if(chameleon_ble_take_response(app->ble, &cmd, &status, data, &len, sizeof(data))) {
                if(cmd == CHAMELEON_CMD_GET_SLOT_INFO) {
                    if(status == CHAMELEON_STATUS_SUCCESS && len >= CHAMELEON_SLOT_COUNT * 4) {
                        for(uint8_t i = 0; i < CHAMELEON_SLOT_COUNT; i++) {
                            app->slot_hf_type[i] = ((uint16_t)data[i * 4] << 8) | data[i * 4 + 1];
                            app->slot_lf_type[i] =
                                ((uint16_t)data[i * 4 + 2] << 8) | data[i * 4 + 3];
                        }
                        app->slot_info_valid = true;
                        if(s_state == SaveStateSlot && !s_saving) chameleon_save_show_slots(app);
                    }
                } else if(s_saving) {
                    if(status != CHAMELEON_STATUS_SUCCESS) {
                        s_saving = false;
                        chameleon_save_show_result(app, "Save failed", "Device error");
                    } else if(s_step == StepSave) {
                        s_saving = false;
                        app->active_slot = (int8_t)s_slot;
                        app->slot_info_valid = false;
                        char sub[24];
                        snprintf(sub, sizeof(sub), "Slot %u", s_slot + 1);
                        chameleon_save_show_result(app, "Saved", sub);
                    } else {
                        s_step++;
                        chameleon_save_send_step(app);
                    }
                }
            }
            consumed = true;
        }
    }
    return consumed;
}

void chameleon_app_scene_save_on_exit(void* context) {
    ChameleonApp* app = context;
    submenu_reset(app->submenu);
    popup_reset(app->popup);
    widget_reset(app->widget);
}
