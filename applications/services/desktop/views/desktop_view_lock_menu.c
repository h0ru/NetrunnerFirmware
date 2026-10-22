#include <furi.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <notification/notification_messages_notes.h>
#include <notification/notification_app.h>
#include <furi_hal_power.h>
#include <furi_hal_rtc.h>
#include <wifi/wlan_hal.h>
#include <stdio.h>

#include "../desktop_i.h"
#include "../helpers/qnetrunner_bridge.h"
#include "desktop_view_lock_menu.h"

/* The Control Center uses Netrunner's fixed eight-tile composition only. */

/* The two sliders the Control Center can adjust. */
typedef enum {
    CcBrightness,
    CcVolume,
} CcId;

/* Exact control order and geometry from official Netrunner's Control Center.
 * The input policy is adapted for the T-Embed wheel. Slider positions are
 * computed relative to NetrunnerCcBrightness so the toggle count can change. */
typedef enum {
    NetrunnerCcWebFs,
    NetrunnerCcSettings,
    NetrunnerCcQnetrunner,
    NetrunnerCcPassport,
    NetrunnerCcWifi,
    NetrunnerCcBluetooth,
    NetrunnerCcBrightness,
    NetrunnerCcVolume,
    NetrunnerCcCount,
} NetrunnerCcId;

#define CC_BRIGHTNESS_MIN 0.20f

/* Slider glyphs sit at the bottom of the track (as on phone control centres),
 * 2px above the inner edge, on a solid band the width of the track so the fill
 * edge never cuts through them (see cc_draw_netrunner). */
#define CC_SLIDER_ICON_BOTTOM_PAD 2

/* Draw needs the notification service for the slider values; there is only ever
 * one lock-menu instance, so a file static mirrors the struct field. */
static NotificationApp* s_notification = NULL;

/* Short preview used after each volume step. */
static const NotificationSequence cc_sequence_volume_preview = {
    &message_note_c5,
    &message_delay_100,
    &message_sound_off,
    NULL,
};

/* Transient overlay message (e.g. a WiFi start failure). */
#define LOCK_MENU_MESSAGE_LEN 40
static char s_message[LOCK_MENU_MESSAGE_LEN];
static char s_hint[LOCK_MENU_MESSAGE_LEN];
static bool s_has_message = false;

/* Slider value in 0..1. Reads the live notification settings. */
static float cc_slider_value(CcId id) {
    if(!s_notification) return 0.0f;
    if(id == CcBrightness) return s_notification->settings.display_brightness;
    if(id == CcVolume) return s_notification->settings.speaker_volume;
    return 0.0f;
}

static void cc_slider_adjust(DesktopLockMenuView* lock_menu, CcId id, float delta) {
    NotificationApp* n = lock_menu->notification;
    if(!n) return;
    float* v = NULL;
    if(id == CcBrightness) {
        v = &n->settings.display_brightness;
    } else if(id == CcVolume) {
        v = &n->settings.speaker_volume;
    }
    if(!v) return;

    float nv = *v + delta;
    const float minimum = id == CcBrightness ? CC_BRIGHTNESS_MIN : 0.0f;
    if(nv < minimum) nv = minimum;
    if(nv > 1.0f) nv = 1.0f;
    *v = nv;

    /* Apply/preview every step immediately, matching Sound & Display settings. */
    if(id == CcBrightness) {
        notification_message(n, &sequence_display_backlight_force_on);
    } else if(id == CcVolume) {
        notification_message(n, &cc_sequence_volume_preview);
    }
}

void desktop_lock_menu_set_callback(
    DesktopLockMenuView* lock_menu,
    DesktopLockMenuViewCallback callback,
    void* context) {
    if(!lock_menu || !callback) return;
    lock_menu->callback = callback;
    lock_menu->context = context;
}

void desktop_lock_menu_set_notification(
    DesktopLockMenuView* lock_menu,
    NotificationApp* notification) {
    if(!lock_menu) return;
    lock_menu->notification = notification;
    s_notification = notification;

    /* A zero backlight makes a wheel-only device effectively impossible to
     * recover. Match Sound & Display's 20% floor and repair older saved values
     * as soon as the Control Centre opens. */
    if(notification && notification->settings.display_brightness < CC_BRIGHTNESS_MIN) {
        notification->settings.display_brightness = CC_BRIGHTNESS_MIN;
        notification_message(notification, &sequence_display_backlight_force_on);
        notification_message_save_settings(notification);
    }
}

void desktop_lock_menu_show_message(
    DesktopLockMenuView* lock_menu,
    const char* text,
    const char* hint) {
    if(!lock_menu || !text) return;

    strlcpy(s_message, text, LOCK_MENU_MESSAGE_LEN);
    strlcpy(s_hint, hint ? hint : "", LOCK_MENU_MESSAGE_LEN);
    s_has_message = true;

    with_view_model(lock_menu->view, DesktopLockMenuViewModel * model, { UNUSED(model); }, true);
}

void desktop_lock_menu_set_states(
    DesktopLockMenuView* lock_menu,
    bool bt_on,
    bool wifi_on,
    bool wake_on,
    bool dark_on,
    bool qnetrunner_on) {
    if(!lock_menu) return;
    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            model->bt_on = bt_on;
            model->wifi_on = wifi_on;
            model->wake_on = wake_on;
            model->dark_on = dark_on;
            model->qnetrunner_on = qnetrunner_on;
            /* Keep the selection valid across a rebuild (a toggle can add or
             * drop a tile). */
            if(model->idx >= NetrunnerCcCount) model->idx = NetrunnerCcCount - 1;
        },
        true);
}

void desktop_lock_menu_refresh_live(DesktopLockMenuView* lock_menu) {
    if(!lock_menu) return;
    with_view_model(lock_menu->view, DesktopLockMenuViewModel * model, { UNUSED(model); }, true);
}

static void cc_draw_netrunner(Canvas* canvas, const DesktopLockMenuViewModel* m) {
    /* This is the official Netrunner 3x2 + two vertical-slider composition. */
    const int8_t slider_total = 58;
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontBatteryPercent);

    for(uint8_t i = 0; i < NetrunnerCcCount; ++i) {
        const bool selected = m->idx == i;
        const bool toggle = i < NetrunnerCcBrightness;
        int8_t x;
        int8_t y;
        int8_t w;
        int8_t h;
        bool enabled = false;
        uint8_t value = 0;
        const Icon* icon = NULL;

        if(toggle) {
            x = 2 + 32 * (i / 2);
            y = 2 + 32 * (i % 2);
            w = 28;
            h = 28;
        } else {
            x = 98 + 16 * (i - NetrunnerCcBrightness);
            y = 2;
            w = 12;
            h = 60;
        }

        switch(i) {
        case NetrunnerCcWebFs:
            icon = &I_CC_WebFs_16x16;
            break;
        case NetrunnerCcSettings:
            icon = &I_CC_Settings_16x16;
            break;
        case NetrunnerCcQnetrunner:
            icon = &I_CC_PcBridge_16x16;
            enabled = m->qnetrunner_on;
            break;
        case NetrunnerCcPassport:
            icon = &I_CC_Passport_16x16;
            break;
        case NetrunnerCcWifi:
            /* WiFi and BLE are mutually exclusive; toggling one turns the other off. */
            icon = &I_CC_Wifi_16x16;
            enabled = m->wifi_on;
            break;
        case NetrunnerCcBluetooth:
            icon = &I_CC_Bluetooth_16x16;
            enabled = m->bt_on;
            break;
        case NetrunnerCcBrightness:
            icon = &I_CC_Brightness_8x8;
            value = slider_total - (uint8_t)(cc_slider_value(CcBrightness) * slider_total);
            break;
        case NetrunnerCcVolume:
            icon = furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode) ? &I_CC_Muted_8x8 :
                                                                        &I_CC_Volume_8x8;
            value = slider_total - (uint8_t)(cc_slider_value(CcVolume) * slider_total);
            break;
        default:
            break;
        }

        if(selected) {
            elements_bold_rounded_frame(canvas, x - 1, y - 1, w + 1, h + 1);
        } else {
            canvas_draw_rframe(canvas, x, y, w, h, 5);
        }

        if(toggle) {
            if(enabled) {
                canvas_draw_rbox(canvas, x, y, w, h, 5);
                canvas_set_color(canvas, ColorWhite);
            }
            canvas_draw_icon(
                canvas,
                x + (w - icon_get_width(icon)) / 2,
                y + (h - icon_get_height(icon)) / 2,
                icon);
            if(enabled) canvas_set_color(canvas, ColorBlack);
        } else {
            const int8_t icon_h = icon_get_height(icon);
            const int8_t icon_x = x + (w - icon_get_width(icon)) / 2;
            const int8_t icon_y = y + h - 1 - CC_SLIDER_ICON_BOTTOM_PAD - icon_h;
            const int8_t fill_top = y + 1 + value;

            canvas_set_color(canvas, ColorXOR);
            canvas_draw_box(canvas, x + 1, fill_top, w - 2, h - 2 - value);

            /* Draw the glyph on a solid band: white on black when the fill
             * reaches the glyph's centre row, black on white otherwise. XORing
             * the fill over the glyph instead left it half inverted whenever
             * the level edge crossed it (low volume). */
            const bool icon_in_fill = fill_top <= icon_y + icon_h / 2;
            canvas_set_color(canvas, icon_in_fill ? ColorBlack : ColorWhite);
            canvas_draw_box(canvas, x + 1, icon_y, w - 2, icon_h);
            canvas_set_color(canvas, icon_in_fill ? ColorWhite : ColorBlack);
            canvas_draw_icon(canvas, icon_x, icon_y, icon);

            canvas_set_color(canvas, selected ? ColorBlack : ColorWhite);
            canvas_draw_dot(canvas, x + 1, y + 1);
            canvas_draw_dot(canvas, x + 1, y + h - 2);
            canvas_draw_dot(canvas, x + w - 2, y + 1);
            canvas_draw_dot(canvas, x + w - 2, y + h - 2);
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rframe(canvas, x, y, w, h, 5);

            if(selected && m->adjust) {
                /* Wheel-only affordance: upstream uses directional buttons;
                 * the inner line marks that rotation now adjusts the slider.
                 * It stops 1px above the glyph, which is wider than the
                 * frame's interior and would otherwise merge with it. */
                canvas_draw_rframe(canvas, x + 2, y + 2, w - 4, icon_y - 1 - (y + 2), 3);
            }
        }
    }
}

void desktop_lock_menu_draw_callback(Canvas* canvas, void* model) {
    DesktopLockMenuViewModel* m = model;

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    cc_draw_netrunner(canvas, m);

    if(s_has_message) {
        const bool have_hint = s_hint[0] != '\0';
        const uint8_t h = have_hint ? 42 : 32;
        const uint8_t y = 10;

        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 6, y, 116, h);
        canvas_set_color(canvas, ColorBlack);
        elements_frame(canvas, 6, y, 116, h);

        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, y + 11, AlignCenter, AlignCenter, s_message);
        if(have_hint) {
            canvas_draw_str_aligned(canvas, 64, y + 22, AlignCenter, AlignCenter, s_hint);
        }
        canvas_draw_str_aligned(canvas, 64, y + h - 9, AlignCenter, AlignCenter, "Press any key");
    }
}

View* desktop_lock_menu_get_view(DesktopLockMenuView* lock_menu) {
    furi_check(lock_menu);
    return lock_menu->view;
}

static DesktopEvent cc_netrunner_event(NetrunnerCcId id) {
    switch(id) {
    case NetrunnerCcWebFs:
        return DesktopLockMenuEventWebFs;
    case NetrunnerCcSettings:
        return DesktopLockMenuEventOpenSettings;
    case NetrunnerCcQnetrunner:
        return DesktopLockMenuEventQnetrunnerToggle;
    case NetrunnerCcPassport:
        return DesktopLockMenuEventOpenPassport;
    case NetrunnerCcWifi:
        return DesktopLockMenuEventWifiScan;
    case NetrunnerCcBluetooth:
        return DesktopLockMenuEventBluetoothToggle;
    default:
        return 0;
    }
}

static bool cc_netrunner_input(DesktopLockMenuView* lock_menu, InputEvent* event) {
    const bool nav = event->type == InputTypeShort || event->type == InputTypeRepeat;
    bool consumed = false;
    uint8_t idx = 0;
    bool in_adjust = false;
    bool wifi_on = false;

    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            if(model->idx >= NetrunnerCcCount) model->idx = NetrunnerCcCount - 1;
            if(nav && (event->key == InputKeyUp || event->key == InputKeyDown)) {
                if(model->adjust) {
                    cc_slider_adjust(
                        lock_menu,
                        model->idx == NetrunnerCcBrightness ? CcBrightness : CcVolume,
                        event->key == InputKeyUp ? 0.05f : -0.05f);
                } else if(event->key == InputKeyUp) {
                    model->idx = model->idx == 0 ? NetrunnerCcCount - 1 : model->idx - 1;
                } else {
                    model->idx = model->idx + 1 >= NetrunnerCcCount ? 0 : model->idx + 1;
                }
                consumed = true;
            }
            idx = model->idx;
            in_adjust = model->adjust;
            wifi_on = model->wifi_on;
        },
        consumed);

    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        if(idx >= NetrunnerCcBrightness) {
            bool leaving = false;
            with_view_model(
                lock_menu->view,
                DesktopLockMenuViewModel * model,
                {
                    model->adjust = !model->adjust;
                    leaving = !model->adjust;
                },
                true);
            if(leaving && lock_menu->notification) {
                notification_message_save_settings(lock_menu->notification);
            }
        } else {
            const DesktopEvent desktop_event = cc_netrunner_event((NetrunnerCcId)idx);
            if(desktop_event) lock_menu->callback(desktop_event, lock_menu->context);
        }
        consumed = true;
    } else if(event->key == InputKeyOk && event->type == InputTypeLong) {
        if(idx == NetrunnerCcBrightness) {
            /* Preserve the two upstream slider-click actions without
             * sacrificing short-OK wheel adjustment. */
            lock_menu->callback(DesktopLockMenuEventOpenDisplay, lock_menu->context);
            consumed = true;
        } else if(idx == NetrunnerCcVolume) {
            lock_menu->callback(DesktopLockMenuEventStealthToggle, lock_menu->context);
            consumed = true;
        } else if(idx == NetrunnerCcWifi) {
            /* Short-press opens the network picker; a hold is the quick way to
             * turn WiFi off. Off-only: a no-op when it is already off, so a
             * hold never starts the radio. */
            if(wifi_on) lock_menu->callback(DesktopLockMenuEventWifiToggle, lock_menu->context);
            consumed = true;
        }
    } else if(event->key == InputKeyBack && event->type == InputTypeShort && in_adjust) {
        with_view_model(
            lock_menu->view, DesktopLockMenuViewModel * model, { model->adjust = false; }, true);
        if(lock_menu->notification) notification_message_save_settings(lock_menu->notification);
        consumed = true;
    }

    return consumed;
}

bool desktop_lock_menu_input_callback(InputEvent* event, void* context) {
    if(!event || !context) return false;

    DesktopLockMenuView* lock_menu = context;

    /* The overlay swallows the dismissing press so it does not also actuate a
     * tile underneath. */
    if(s_has_message) {
        if(event->type == InputTypeShort || event->type == InputTypeLong) {
            s_has_message = false;
            with_view_model(
                lock_menu->view, DesktopLockMenuViewModel * model, { UNUSED(model); }, true);
        }
        return true;
    }

    return cc_netrunner_input(lock_menu, event);
}

DesktopLockMenuView* desktop_lock_menu_alloc(void) {
    DesktopLockMenuView* lock_menu = malloc(sizeof(DesktopLockMenuView));
    lock_menu->callback = NULL;
    lock_menu->context = NULL;
    lock_menu->notification = NULL;
    lock_menu->view = view_alloc();
    view_allocate_model(lock_menu->view, ViewModelTypeLocking, sizeof(DesktopLockMenuViewModel));
    view_set_context(lock_menu->view, lock_menu);
    view_set_draw_callback(lock_menu->view, (ViewDrawCallback)desktop_lock_menu_draw_callback);
    view_set_input_callback(lock_menu->view, desktop_lock_menu_input_callback);

    return lock_menu;
}

void desktop_lock_menu_free(DesktopLockMenuView* lock_menu_view) {
    furi_check(lock_menu_view);
    view_free(lock_menu_view->view);
    free(lock_menu_view);
}
