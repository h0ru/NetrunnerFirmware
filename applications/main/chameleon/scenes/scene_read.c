#include "../chameleon_app.h"

/* Continuous HF (ISO14443A) or LF (EM410x) read, chosen by app->read_lf. The
 * device is put in reader mode once, then scanned repeatedly until a card/tag
 * is found or the user leaves. On a hit, the result is shown with a Scan-again
 * action. (Save and emulate-from-read are Phase 2.) */

static bool s_scanning;
static bool s_lf;
static char s_line1[40];
static char s_line2[40];
static bool s_writing;
static bool s_confirm; /* showing the "place a T5577" confirmation */
static int s_wstep; /* 0 = set reader mode sent, 1 = write sent */
static int s_wtries; /* write attempts spent (for transient-failure retry) */
static char s_status[28]; /* write outcome shown as the result title */

#define CHAMELEON_WRITE_MAX_TRIES 8 /* ~1s of retries while the card is held */

static uint16_t chameleon_read_scan_cmd(void) {
    return s_lf ? CHAMELEON_CMD_EM410X_SCAN : CHAMELEON_CMD_HF14A_SCAN;
}

static void chameleon_read_result_button_cb(GuiButtonType type, InputType input, void* context) {
    ChameleonApp* app = context;
    if(input != InputTypeShort) return;
    if(type == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, ChameleonCustomEventRetry);
    } else if(type == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, ChameleonCustomEventSave);
    } else if(type == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(app->view_dispatcher, ChameleonCustomEventWrite);
    }
}

static void chameleon_read_confirm_button_cb(GuiButtonType type, InputType input, void* context) {
    ChameleonApp* app = context;
    if(input == InputTypeShort && type == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, ChameleonCustomEventWriteGo);
    }
}

/* A transient popup (no auto-dismiss) shown while the write is in flight. */
static void chameleon_read_show_busy(ChameleonApp* app, const char* h, const char* t) {
    Popup* popup = app->popup;
    popup_reset(popup);
    popup_set_header(popup, h, 64, 20, AlignCenter, AlignCenter);
    if(t) popup_set_text(popup, t, 64, 40, AlignCenter, AlignCenter);
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewPopup);
}

/* Confirmation so the user can position the card before the one-shot write. The
 * Chameleon writes with its own antenna, so no separate writer is needed.
 * LF -> blank T5577; HF -> Gen2/CUID magic Mifare (block 0 is overwritten). */
static void chameleon_read_show_confirm(ChameleonApp* app) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 4, AlignCenter, AlignTop, FontPrimary,
        s_lf ? "Write to T5577" : "Write to magic card");
    widget_add_string_element(
        app->widget, 64, 28, AlignCenter, AlignTop, FontSecondary,
        s_lf ? "Hold a blank T5577 on\nthe Chameleon" :
               "Hold a Gen2 magic card\non the Chameleon");
    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "Write", chameleon_read_confirm_button_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewWidget);
}

static void chameleon_read_show_scanning(ChameleonApp* app) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 24, AlignCenter, AlignTop, FontPrimary,
        s_lf ? "Reading LF..." : "Reading HF...");
    widget_add_string_element(
        app->widget, 64, 48, AlignCenter, AlignTop, FontSecondary, "Present a card to read");
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewWidget);
}

/* Failure screen: "Write failed" + a human reason + the raw status code (so an
 * unexpected code can be reported). Overwrites the read-data lines; the card
 * data is still cached, so Rescan or Write both still work. */
static void chameleon_read_show_write_fail(ChameleonApp* app, uint16_t status);

static void chameleon_read_show_result(ChameleonApp* app) {
    widget_reset(app->widget);
    const char* title = s_status[0] ? s_status : (s_lf ? "Tag found" : "Card found");
    widget_add_string_element(app->widget, 64, 3, AlignCenter, AlignTop, FontPrimary, title);
    /* A divider under the title separates it from the data block and keeps the
     * two FontSecondary lines clear of the button row at the bottom. */
    widget_add_line_element(app->widget, 0, 17, 127, 17);
    widget_add_string_element(app->widget, 64, 22, AlignCenter, AlignTop, FontSecondary, s_line1);
    widget_add_string_element(app->widget, 64, 34, AlignCenter, AlignTop, FontSecondary, s_line2);
    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Save", chameleon_read_result_button_cb, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "Rescan", chameleon_read_result_button_cb, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "Write", chameleon_read_result_button_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewWidget);
}

static void chameleon_read_show_write_fail(ChameleonApp* app, uint16_t status) {
    const char* reason = chameleon_status_reason(status);
    strlcpy(s_status, "Write failed", sizeof(s_status));
    if(reason) {
        strlcpy(s_line1, reason, sizeof(s_line1));
    } else {
        snprintf(s_line1, sizeof(s_line1), "Status 0x%02X", (unsigned)(status & 0xFF));
    }
    if(status == CHAMELEON_STATUS_TIMEOUT) {
        s_line2[0] = '\0';
    } else {
        snprintf(s_line2, sizeof(s_line2), "Code 0x%02X", (unsigned)(status & 0xFF));
    }
    chameleon_read_show_result(app);
}

static bool chameleon_read_parse_hf(ChameleonApp* app, const uint8_t* d, uint16_t len) {
    if(len < 8) return false;
    uint8_t uidlen = d[0];
    if(uidlen != 4 && uidlen != 7 && uidlen != 10) return false;
    if((uint16_t)(1 + uidlen + 3) > len) return false;
    char uid[32] = "";
    for(uint8_t i = 0; i < uidlen; i++) {
        char b[4];
        snprintf(b, sizeof(b), "%02X", d[1 + i]);
        strlcat(uid, b, sizeof(uid));
    }
    const uint8_t* atqa = &d[1 + uidlen];
    uint8_t sak = d[1 + uidlen + 2];
    snprintf(s_line1, sizeof(s_line1), "UID: %s", uid);
    snprintf(s_line2, sizeof(s_line2), "SAK: %02X  ATQA: %02X%02X", sak, atqa[0], atqa[1]);
    /* Stash for Save-to-slot. */
    app->read_was_lf = false;
    app->read_uidlen = uidlen;
    memcpy(app->read_uid, &d[1], uidlen);
    app->read_atqa[0] = atqa[0];
    app->read_atqa[1] = atqa[1];
    app->read_sak = sak;
    app->read_valid = true;
    return true;
}

static bool chameleon_read_parse_lf(ChameleonApp* app, const uint8_t* d, uint16_t len) {
    if(len < 5) return false;
    snprintf(
        s_line1, sizeof(s_line1), "ID: %02X%02X%02X%02X%02X", d[0], d[1], d[2], d[3], d[4]);
    strlcpy(s_line2, "EM410x (125 kHz)", sizeof(s_line2));
    app->read_was_lf = true;
    memcpy(app->read_lfid, d, 5);
    app->read_valid = true;
    return true;
}

static void chameleon_read_begin(ChameleonApp* app) {
    s_scanning = true;
    s_writing = false;
    s_confirm = false;
    s_status[0] = '\0';
    s_line1[0] = s_line2[0] = '\0';
    chameleon_led_scan_start(app);
    chameleon_read_show_scanning(app);
    /* Ensure reader mode, then scan (chained in the response handler). */
    uint8_t reader = CHAMELEON_MODE_READER;
    chameleon_ble_send(app->ble, CHAMELEON_CMD_CHANGE_DEVICE_MODE, &reader, 1);
}

/* Send the one-shot write for the current read (LF T5577 or HF magic block 0). */
static void chameleon_read_send_write(ChameleonApp* app) {
    if(s_lf) {
        chameleon_ble_write_t5577_em410x(app->ble, app->read_lfid);
    } else {
        chameleon_ble_write_mifare_block0(
            app->ble, app->read_uid, app->read_uidlen, app->read_sak, app->read_atqa);
    }
}

void chameleon_app_scene_read_on_enter(void* context) {
    ChameleonApp* app = context;
    s_lf = app->read_lf;
    chameleon_read_begin(app);
}

bool chameleon_app_scene_read_on_event(void* context, SceneManagerEvent event) {
    ChameleonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == ChameleonCustomEventRetry) {
            chameleon_read_begin(app);
            consumed = true;
        } else if(event.event == ChameleonCustomEventSave) {
            if(app->read_valid) {
                s_scanning = false;
                chameleon_led_reset(app);
                scene_manager_next_scene(app->scene_manager, ChameleonAppSceneSave);
            }
            consumed = true;
        } else if(event.event == ChameleonCustomEventWrite) {
            if(app->read_valid &&
               (s_lf || app->read_uidlen == 4 || app->read_uidlen == 7)) {
                /* Ask the user to position the card first (one-shot write).
                 * HF is supported for 4- and 7-byte-UID Gen2 magic cards. */
                s_scanning = false;
                chameleon_led_reset(app);
                s_confirm = true;
                chameleon_read_show_confirm(app);
            } else if(app->read_valid) {
                /* 10-byte UIDs don't map to a magic-card block 0. */
                strlcpy(s_status, "Need 4/7-byte UID", sizeof(s_status));
                chameleon_read_show_result(app);
            }
            consumed = true;
        } else if(event.event == ChameleonCustomEventWriteGo) {
            /* Confirmed: reader mode, then the one-shot write. */
            s_confirm = false;
            s_writing = true;
            s_wstep = 0;
            s_wtries = 0;
            chameleon_read_show_busy(app, "Writing...", s_lf ? "Writing to T5577" : "Writing to card");
            uint8_t reader = CHAMELEON_MODE_READER;
            chameleon_ble_send(app->ble, CHAMELEON_CMD_CHANGE_DEVICE_MODE, &reader, 1);
            consumed = true;
        } else if(event.event == ChameleonCustomEventBleResponse) {
            uint16_t cmd = 0, status = 0, len = 0;
            uint8_t data[CHAMELEON_RESP_MAX];
            if(!chameleon_ble_take_response(app->ble, &cmd, &status, data, &len, sizeof(data))) {
                consumed = true;
            } else if(s_writing) {
                /* Step 0 is the mode change (SUCCESS); step 1 is the write,
                 * which reports a tag-ok code (HF 0x00 / LF 0x40). */
                bool ok = status == CHAMELEON_STATUS_SUCCESS ||
                          status == CHAMELEON_STATUS_LF_TAG_OK ||
                          status == CHAMELEON_STATUS_HF_TAG_OK;
                if(s_wstep == 0) {
                    if(!ok) {
                        s_writing = false;
                        chameleon_read_show_write_fail(app, status);
                    } else {
                        s_wstep = 1;
                        /* Let the reader field settle before the write so the
                         * card is powered (otherwise the write sees no tag). */
                        furi_delay_ms(60);
                        chameleon_read_send_write(app);
                    }
                } else if(ok) {
                    s_writing = false;
                    strlcpy(s_status, s_lf ? "Written to T5577" : "Written to card",
                            sizeof(s_status));
                    chameleon_read_show_result(app);
                } else if(
                    chameleon_status_write_transient(status) &&
                    s_wtries < CHAMELEON_WRITE_MAX_TRIES) {
                    /* No tag / no reply: keep trying while the user holds the
                     * card against the Chameleon instead of failing instantly. */
                    s_wtries++;
                    furi_delay_ms(120);
                    chameleon_read_send_write(app);
                } else {
                    s_writing = false;
                    chameleon_read_show_write_fail(app, status);
                }
                consumed = true;
            } else {
                if(cmd == CHAMELEON_CMD_CHANGE_DEVICE_MODE) {
                    if(s_scanning) chameleon_ble_send(app->ble, chameleon_read_scan_cmd(), NULL, 0);
                } else if(cmd == CHAMELEON_CMD_HF14A_SCAN || cmd == CHAMELEON_CMD_EM410X_SCAN) {
                    bool ok = false;
                    if(s_lf && status == CHAMELEON_STATUS_LF_TAG_OK) {
                        ok = chameleon_read_parse_lf(app, data, len);
                    } else if(!s_lf && status == CHAMELEON_STATUS_HF_TAG_OK) {
                        ok = chameleon_read_parse_hf(app, data, len);
                    }
                    if(ok) {
                        s_scanning = false;
                        s_status[0] = '\0'; /* fresh read: normal title */
                        chameleon_led_reset(app);
                        chameleon_read_show_result(app);
                    } else if(s_scanning) {
                        /* Nothing in range yet: keep scanning. */
                        chameleon_ble_send(app->ble, chameleon_read_scan_cmd(), NULL, 0);
                    }
                }
            }
            consumed = true;
        } else if(event.event == ChameleonCustomEventBleDisconnected) {
            s_scanning = false;
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, ChameleonAppSceneConnect);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        /* Back on the write-confirm screen, or mid-write retry, cancels and
         * returns to the result instead of leaving the Read scene. */
        if(s_confirm || s_writing) {
            s_confirm = false;
            s_writing = false;
            s_status[0] = '\0';
            chameleon_read_show_result(app);
            consumed = true;
        }
    }
    return consumed;
}

void chameleon_app_scene_read_on_exit(void* context) {
    ChameleonApp* app = context;
    s_scanning = false;
    chameleon_led_reset(app);
    widget_reset(app->widget);
}
