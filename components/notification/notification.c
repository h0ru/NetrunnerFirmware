/**
 * @file notification.c
 * @brief ESP32 notification service with STM32-style backlight handling.
 */

#include <furi.h>
#include <furi_hal_display.h>
#include <furi_hal_light.h>
#include <furi_hal_power.h>
#include <furi_hal_rtc.h>

/* Idle behaviour is two-stage: the backlight drops to a dim-but-readable level
 * first, and only if nothing happens after that does the device sleep. */
#define NOTIFICATION_IDLE_DIM_DELAY_MS   30000UL
#define NOTIFICATION_IDLE_SLEEP_DELAY_MS 240000UL /* after the 120 s backlight timeout */
#define NOTIFICATION_IDLE_DIM_BACKLIGHT  8
#include <furi_hal_speaker.h>
#include <esp_random.h>
#include <input.h>
#include <saved_struct.h>
#include <storage/storage.h>
#include <dolphin/dolphin.h>
#include "notification_app.h"
#include "notification_messages.h"

#define TAG "NotificationSrv"

#define NOTIFICATION_EVENT_COMPLETE 0x00000001U
#define NOTIFICATION_SD_WAIT_MS     5000U /* grace period for the SD card at boot */

#define NOTIFICATION_SETTINGS_PATH    INT_PATH(".notification.settings")
#define NOTIFICATION_SETTINGS_MAGIC   0x42

/* App-LED-effects master switch, persisted on its own so adding it needs no
 * NotificationSettings version bump (which would reset users' saved settings). */
#define LED_APP_EFFECTS_PATH    INT_PATH(".led_app_effects")
#define LED_APP_EFFECTS_MAGIC   0x42
#define LED_APP_EFFECTS_VERSION 1
#define LCD_COLOR_ORDER_PATH    INT_PATH(".lcd_color_order")
#define LCD_COLOR_ORDER_MAGIC   0x42
#define LCD_COLOR_ORDER_VERSION 1
#define NOTIFICATION_SETTINGS_VERSION_0C 0x0C /* 0x0C: added display_sleep_delay_ms */
#define NOTIFICATION_SETTINGS_VERSION_0D 0x0D /* 0x0D: same layout, UI colors reset to Netrunner red */
#define NOTIFICATION_SETTINGS_VERSION    0x0E /* 0x0E: same layout, 120 s backlight default */
#define NOTIFICATION_DISPLAY_OFF_DELAY_MS 120000UL /* factory backlight timeout */

typedef enum {
    NotificationLayerMessage,
    InternalLayerMessage,
    SaveSettingsMessage,
    DisplayDimMessage,
} NotificationAppMessageType;

typedef struct {
    const NotificationSequence* sequence;
    NotificationAppMessageType type;
    FuriEventFlag* back_event;
} NotificationAppMessage;

static bool lcd_backlight_is_on = false;

static uint8_t notification_clamp_u8(float value) {
    if(value <= 0.0f) return 0;
    if(value >= 255.0f) return UINT8_MAX;

    return (uint8_t)value;
}

static uint8_t notification_scale_display_brightness(
    NotificationApp* app,
    uint8_t value,
    float display_brightness_setting) {
    return notification_clamp_u8(value * display_brightness_setting * app->current_night_shift);
}

static uint8_t notification_settings_get_display_brightness(NotificationApp* app, uint8_t value) {
    return notification_clamp_u8(value * app->settings.display_brightness);
}

static void notification_apply_internal_display_layer(NotificationApp* app, uint8_t layer_value) {
    furi_assert(app);

    app->display.value[LayerInternal] = layer_value;

    if(app->display.index == LayerInternal) {
        furi_hal_display_set_backlight(app->display.value[LayerInternal]);
    }
}

static void notification_apply_notification_display_layer(NotificationApp* app, uint8_t layer_value) {
    furi_assert(app);

    app->display.index = LayerNotification;
    app->display.value[LayerNotification] = layer_value;

    if(lcd_backlight_is_on) return;

    furi_hal_display_set_backlight(app->display.value[LayerNotification]);
}

static void notification_reset_notification_display_layer(NotificationApp* app) {
    furi_assert(app);

    app->display.value[LayerNotification] = 0;
    app->display.index = LayerInternal;

    furi_hal_display_set_backlight(app->display.value[LayerInternal]);
}

/* Idle delays are user settings now. The stored value is the delay from the
 * last activity; the sleep timer is armed when the dim fires, so it needs
 * the remainder rather than the whole. */
static uint32_t notification_dim_delay(NotificationApp* app) {
    uint32_t d = app->settings.display_off_delay_ms;
    return d ? d : NOTIFICATION_IDLE_DIM_DELAY_MS;
}

/** @return 0 when sleep is disabled. */
static uint32_t notification_sleep_remainder(NotificationApp* app) {
    const uint32_t sleep = app->settings.display_sleep_delay_ms;
    if(sleep == 0) return 0; /* Never */

    const uint32_t dim = notification_dim_delay(app);
    /* A sleep delay at or before the dim would blank the screen the instant
     * it dimmed; give it one dim period of daylight instead. */
    return (sleep > dim) ? (sleep - dim) : dim;
}

static void notification_reset_notification_layer(
    NotificationApp* app,
    bool reset_display,
    float display_brightness_setting) {
    if(!reset_display) return;

    if(display_brightness_setting != app->settings.display_brightness) {
        furi_hal_display_set_backlight(
            notification_clamp_u8(
                app->settings.display_brightness * 255.0f * app->current_night_shift));
    }

    /* Activity: back to full brightness and restart the idle sequence.
     * Setting the display-off delay to OFF disables dimming and sleep too. */
    if(app->display_sleep_timer && furi_timer_is_running(app->display_sleep_timer)) {
        furi_timer_stop(app->display_sleep_timer);
    }
    app->display_idle_dim = false;

    // A display wake lock holds the backlight on and prevents idle sleep.
    if(app->settings.display_off_delay_ms > 0 && app->display_led_lock_count == 0) {
        furi_timer_start(app->display_timer, furi_ms_to_ticks(notification_dim_delay(app)));
    }
}

static void notification_idle_timers_stop(NotificationApp* app) {
    if(furi_timer_is_running(app->display_timer)) {
        furi_timer_stop(app->display_timer);
    }
    if(app->display_sleep_timer && furi_timer_is_running(app->display_sleep_timer)) {
        furi_timer_stop(app->display_sleep_timer);
    }
    app->display_idle_dim = false;
}

/* Stage two: nothing since the display dimmed, so power down. */
static void notification_display_sleep_timer(void* context) {
    furi_assert(context);
    NotificationApp* app = context;

    /* A timer callback may already be queued when Wake mode acquires its lock.
     * Do not let that stale callback dim or shut down the device. */
    if(app->display_led_lock_count > 0) return;

    /* Apps that must keep running hold insomnia; do not sleep under them. */
    if(!furi_hal_power_sleep_available()) {
        furi_timer_start(
            app->display_sleep_timer, furi_ms_to_ticks(notification_sleep_remainder(app)));
        return;
    }

    app->display_idle_dim = false;
    /* This is the last high-level point before idle deep sleep. Persist the
     * Dolphin state; the HAL records sleep duration independently in RTC slow
     * memory. Special boot modes intentionally omit Dolphin. */
    if(furi_record_exists(RECORD_DOLPHIN)) {
        Dolphin* dolphin = furi_record_open(RECORD_DOLPHIN);
        dolphin_prepare_for_sleep(dolphin);
        furi_record_close(RECORD_DOLPHIN);
    }
    /* Only idle sleep resumes; a deliberate power off ends in the same deep
     * sleep and would otherwise be indistinguishable at wake. */
    furi_hal_rtc_arm_resume();
    furi_hal_power_shutdown();
}

/* Stage one: dim rather than blanking, so the screen is still readable. */
static void notification_display_timer(void* context) {
    furi_assert(context);
    NotificationApp* app = context;

    if(app->display_led_lock_count > 0) return;

    app->display_idle_dim = true;
    /* The backlight is no longer at its normal on level, and
     * notification_apply_notification_display_layer() returns early while this
     * flag is set. Leaving it true would strand the display at the dim level,
     * because the next input could not write full brightness back. */
    lcd_backlight_is_on = false;
    furi_hal_display_set_backlight(NOTIFICATION_IDLE_DIM_BACKLIGHT);

    const uint32_t sleep_in = notification_sleep_remainder(app);
    if(app->display_sleep_timer && sleep_in > 0) {
        furi_timer_start(app->display_sleep_timer, furi_ms_to_ticks(sleep_in));
    }
}

static void night_shift_demo_timer_callback(void* context) {
    furi_assert(context);
    NotificationApp* app = context;
    notification_message(app, &sequence_display_backlight_force_on);
}

void night_shift_timer_start(NotificationApp* app) {
    if(app->settings.night_shift != 1.0f) {
        if(furi_timer_is_running(app->night_shift_timer)) {
            furi_timer_stop(app->night_shift_timer);
        }
        furi_timer_start(app->night_shift_timer, furi_ms_to_ticks(1000));
    }
}

void night_shift_timer_stop(NotificationApp* app) {
    if(furi_timer_is_running(app->night_shift_timer)) {
        furi_timer_stop(app->night_shift_timer);
    }
}

static void night_shift_timer_callback(void* context) {
    furi_assert(context);
    NotificationApp* app = context;
    DateTime current_date_time;

    furi_hal_rtc_get_datetime(&current_date_time);
    uint32_t time = current_date_time.hour * 60 + current_date_time.minute;

    if((time > app->settings.night_shift_end) && (time < app->settings.night_shift_start)) {
        app->current_night_shift = 1.0f;
    } else {
        app->current_night_shift = app->settings.night_shift;
    }
}

static void notification_process_notification_message(
    NotificationApp* app,
    NotificationAppMessage* message) {
    uint32_t notification_message_index = 0;
    const NotificationMessage* notification_message = (*message->sequence)[notification_message_index];

    bool reset_notifications = true;
    float display_brightness_setting = app->settings.display_brightness;
    float speaker_volume_setting = app->settings.speaker_volume;
    bool force_volume = false;
    bool reset_display = false;

    while(notification_message != NULL) {
        switch(notification_message->type) {
        case NotificationMessageTypeLedDisplayBacklight:
            if(notification_message->data.led.value > 0x00) {
                notification_apply_notification_display_layer(
                    app,
                    notification_scale_display_brightness(
                        app,
                        notification_message->data.led.value,
                        display_brightness_setting));
                reset_display = true;
                lcd_backlight_is_on = true;
            } else {
                reset_display = false;
                notification_reset_notification_display_layer(app);
                lcd_backlight_is_on = false;

                notification_idle_timers_stop(app);
            }
            break;
        case NotificationMessageTypeLedDisplayBacklightForceOn:
            lcd_backlight_is_on = false;
            notification_apply_notification_display_layer(
                app,
                notification_scale_display_brightness(
                    app,
                    notification_message->data.led.value,
                    display_brightness_setting));
            reset_display = true;
            lcd_backlight_is_on = true;
            break;
        case NotificationMessageTypeLedDisplayBacklightEnforceOn:
            if(app->display_led_lock_count < UINT8_MAX) {
                const bool first_lock = app->display_led_lock_count == 0;
                app->display_led_lock_count++;
                if(!first_lock) break;
                // Wake mode: cancel any pending dim and sleep.
                notification_idle_timers_stop(app);
                notification_apply_internal_display_layer(
                    app,
                    notification_scale_display_brightness(
                        app,
                        notification_message->data.led.value,
                        display_brightness_setting));
                lcd_backlight_is_on = true;
            }
            break;
        case NotificationMessageTypeLedDisplayBacklightEnforceAuto:
            if(app->display_led_lock_count > 0) {
                app->display_led_lock_count--;
                if(app->display_led_lock_count > 0) break;
                // Leaving Wake mode restarts the idle sequence from now.
                if(app->settings.display_off_delay_ms > 0) {
                    furi_timer_start(
                        app->display_timer, furi_ms_to_ticks(notification_dim_delay(app)));
                }
                notification_apply_internal_display_layer(
                    app,
                    notification_scale_display_brightness(
                        app,
                        notification_message->data.led.value,
                        display_brightness_setting));
            } else {
                FURI_LOG_E(TAG, "Incorrect BacklightEnforceAuto usage");
            }
            break;
        case NotificationMessageTypeLedRed:
        case NotificationMessageTypeLedGreen:
        case NotificationMessageTypeLedBlue:
        case NotificationMessageTypeLedBlinkStart:
        case NotificationMessageTypeLedBlinkStop:
        case NotificationMessageTypeLedBlinkColor:
            /* WS2812 ring is driven exclusively by the user's ambient color/effect
             * settings. Per-app notification flashes are deliberately ignored here
             * — at full PWM on 8 LEDs they were strobing painfully under high-rate
             * callers (e.g. SubGHz frequency hopper). The visual signal in those
             * apps already comes from on-screen UI; no LED override needed. */
            break;
        case NotificationMessageTypeLedEffectOverride:
            if(app->led_app_effects) {
                app->led_effect_override_active = true;
                app->led_effect_override = notification_message->data.led_effect.effect;
                app->led_effect_override_speed =
                    notification_message->data.led_effect.speed_tick_ms;
                app->led_effect_override_has_color =
                    notification_message->data.led_effect.has_color;
                app->led_effect_override_color = notification_message->data.led_effect.color;
            } else {
                app->led_effect_override_active = false; /* feature disabled */
            }
            notification_apply_led_color(app);
            break;
        case NotificationMessageTypeLedEffectOverrideReset:
            app->led_effect_override_active = false;
            notification_apply_led_color(app);
            break;
        case NotificationMessageTypeSoundOn:
            if(!furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode) || force_volume) {
                float vol = notification_message->data.sound.volume * speaker_volume_setting;
                if(furi_hal_speaker_is_mine() || furi_hal_speaker_acquire(30)) {
                    furi_hal_speaker_start(
                        notification_message->data.sound.frequency, vol);
                }
            }
            break;
        case NotificationMessageTypeSoundOff:
            if(furi_hal_speaker_is_mine()) {
                furi_hal_speaker_stop();
                furi_hal_speaker_release();
            }
            break;
        case NotificationMessageTypeDelay:
            furi_delay_ms(notification_message->data.delay.length);
            break;
        case NotificationMessageTypeDoNotReset:
            reset_notifications = false;
            break;
        case NotificationMessageTypeForceSpeakerVolumeSetting:
            speaker_volume_setting = notification_message->data.forced_settings.speaker_volume;
            force_volume = true;
            break;
        case NotificationMessageTypeForceDisplayBrightnessSetting:
            display_brightness_setting =
                notification_message->data.forced_settings.display_brightness;
            break;
        default:
            break;
        }

        notification_message_index++;
        notification_message = (*message->sequence)[notification_message_index];
    }

    if(reset_notifications) {
        notification_reset_notification_layer(app, reset_display, display_brightness_setting);
    }
}

static void notification_process_internal_message(
    NotificationApp* app,
    NotificationAppMessage* message) {
    uint32_t notification_message_index = 0;
    const NotificationMessage* notification_message = (*message->sequence)[notification_message_index];

    while(notification_message != NULL) {
        switch(notification_message->type) {
        case NotificationMessageTypeLedDisplayBacklight:
            notification_apply_internal_display_layer(
                app,
                notification_settings_get_display_brightness(
                    app, notification_message->data.led.value));
            break;
        default:
            break;
        }

        notification_message_index++;
        notification_message = (*message->sequence)[notification_message_index];
    }
}

static void input_event_callback(const void* value, void* context) {
    furi_assert(value);
    furi_assert(context);

    NotificationApp* app = context;
    notification_message(app, &sequence_display_backlight_on);
    /* Keep the WS2812 ring alive on user input. If the idle-off timer already
     * darkened it, re-light fully (resets phase + restarts the effect). If it's
     * still lit, only re-arm the idle-off timer — restarting here would reset a
     * running animation back to frame 0 on every encoder tick. */
    if(app->led_idle_off) {
        notification_apply_led_color(app);
    } else if(app->led_off_timer && app->settings.led_off_delay_ms > 0) {
        furi_timer_start(app->led_off_timer, furi_ms_to_ticks(app->settings.led_off_delay_ms));
    }
}

static void notification_message_send(
    NotificationApp* app,
    NotificationAppMessageType type,
    const NotificationSequence* sequence,
    FuriEventFlag* back_event) {
    furi_assert(app);
    furi_assert(sequence);

    NotificationAppMessage message = {
        .sequence = sequence,
        .type = type,
        .back_event = back_event,
    };

    furi_check(furi_message_queue_put(app->queue, &message, FuriWaitForever) == FuriStatusOk);
}

static bool notification_save_settings(NotificationApp* app);

#define UI_COLOR_SPECTRUM_INDEX   10
#define UI_COLOR_DEFAULT_BG_INDEX 2 /* Red */

/* A corrupt or stale index used to leave the screen on whatever color was
 * last pushed (or on the orange custom default) while the settings list showed
 * a different name. Pull anything out of range back to the red/black default. */
static void notification_sanitize_ui_colors(NotificationSettings* s) {
    if(s->ui_color_index > UI_COLOR_CUSTOM_INDEX ||
       (s->ui_color_index == UI_COLOR_CUSTOM_INDEX && !s->ui_custom_color_set)) {
        s->ui_color_index = UI_COLOR_DEFAULT_BG_INDEX;
    }
    if(s->ui_fg_color_index > UI_COLOR_SPECTRUM_INDEX) s->ui_fg_color_index = 0;
    if(s->ui_bg_color_count < 1 || s->ui_bg_color_count > FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS) {
        s->ui_bg_color_count = 1;
    }
    for(uint8_t i = 0; i < FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS; i++) {
        if(s->ui_bg_stop_index[i] > UI_COLOR_CUSTOM_INDEX) {
            s->ui_bg_stop_index[i] = UI_COLOR_DEFAULT_BG_INDEX;
        }
    }
}

/* persist_migration: write a migrated older file back as the current version.
 * Only pass true once the SD card has had its chance to mount -- otherwise the
 * save lands in NVS and the next load would migrate that copy over the card. */
static bool notification_load_settings(NotificationApp* app, bool persist_migration) {
    NotificationSettings tmp;
    bool ok = saved_struct_load(
        NOTIFICATION_SETTINGS_PATH,
        &tmp,
        sizeof(NotificationSettings),
        NOTIFICATION_SETTINGS_MAGIC,
        NOTIFICATION_SETTINGS_VERSION);
    bool migrated = false;
    bool from_0c = false;
    if(!ok) {
        ok = saved_struct_load(
            NOTIFICATION_SETTINGS_PATH,
            &tmp,
            sizeof(NotificationSettings),
            NOTIFICATION_SETTINGS_MAGIC,
            NOTIFICATION_SETTINGS_VERSION_0D);
        if(!ok) {
            ok = from_0c = saved_struct_load(
                NOTIFICATION_SETTINGS_PATH,
                &tmp,
                sizeof(NotificationSettings),
                NOTIFICATION_SETTINGS_MAGIC,
                NOTIFICATION_SETTINGS_VERSION_0C);
        }
        migrated = ok;
    }
    if(from_0c) {
        /* Red is the Netrunner factory default from 0x0D onward; settings
         * saved by older builds (often the old default) are reset once.
         * Users can still pick another color afterwards. */
        tmp.ui_color_index = UI_COLOR_DEFAULT_BG_INDEX;
        tmp.ui_fg_color_index = 0; /* Black */
        tmp.ui_bg_color_count = 1; /* flat background, gradient off */
    }
    if(migrated) {
        /* 120 s backlight is the factory default from 0x0E onward; apply it
         * once to older saves. Keep deep sleep after it unless set to Never. */
        tmp.display_off_delay_ms = NOTIFICATION_DISPLAY_OFF_DELAY_MS;
        if(tmp.display_sleep_delay_ms != 0 &&
           tmp.display_sleep_delay_ms < NOTIFICATION_IDLE_SLEEP_DELAY_MS) {
            tmp.display_sleep_delay_ms = NOTIFICATION_IDLE_SLEEP_DELAY_MS;
        }
    }
    if(ok) {
        notification_sanitize_ui_colors(&tmp);
        app->settings = tmp;
        FURI_LOG_I(
            TAG,
            "LOAD ok: brightness=%.2f vol=%.2f delay_ms=%u",
            (double)app->settings.display_brightness,
            (double)app->settings.speaker_volume,
            (unsigned)app->settings.display_off_delay_ms);
    } else {
        FURI_LOG_W(TAG, "LOAD failed — using defaults");
    }
    /* Sanity: never let a 0ms delay sneak through and instantly blank the screen. */
    if(app->settings.display_off_delay_ms < 2000) app->settings.display_off_delay_ms = 2000;
    if(migrated && persist_migration) notification_save_settings(app);
    return ok;
}

static bool notification_save_settings(NotificationApp* app) {
    bool ok = saved_struct_save(
        NOTIFICATION_SETTINGS_PATH,
        &app->settings,
        sizeof(NotificationSettings),
        NOTIFICATION_SETTINGS_MAGIC,
        NOTIFICATION_SETTINGS_VERSION);
    if(ok) {
        FURI_LOG_I(
            TAG,
            "SAVE ok: brightness=%.2f vol=%.2f",
            (double)app->settings.display_brightness,
            (double)app->settings.speaker_volume);
    } else {
        FURI_LOG_E(TAG, "SAVE failed");
    }
    return ok;
}

/* ──────────────────────────── LED Effect Engine ──────────────────────────── */

#define LED_EFFECT_TICK_MS 33  /* ~30 FPS */
static uint32_t led_effect_phase = 0;

static inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

/* Unpack settings.led_color into channels scaled by settings.led_brightness.
 * Applies cubic gamma (k -> k^3) because WS2812 LEDs on the T-Embed Plus are
 * unusually bright at low PWM duty — quadratic gamma was perceptually still
 * too bright at low slider values. Cubic mapping: 20% slider drives ~0.8%
 * (channel ~2/255), 30% drives ~2.7% (channel ~6/255), 50% drives 12.5%,
 * 100% stays 100%. Anything below ~15% slider clamps to 0 after uint8 cast. */
/* Effect/speed/color actually in force: the app override when set, else the
 * user's saved settings. Keeps the override non-persistent and in one place. */
static uint8_t notification_active_led_effect(NotificationApp* app) {
    return app->led_effect_override_active ? app->led_effect_override :
                                             app->settings.led_effect;
}

static uint32_t notification_active_led_speed(NotificationApp* app) {
    if(app->led_effect_override_active && app->led_effect_override_speed) {
        return app->led_effect_override_speed;
    }
    return app->settings.led_speed_tick_ms;
}

static void notification_unpack_color(NotificationApp* app, uint8_t* r, uint8_t* g, uint8_t* b) {
    uint32_t c = (app->led_effect_override_active && app->led_effect_override_has_color) ?
                     app->led_effect_override_color :
                     app->settings.led_color;
    float k = clamp01(app->settings.led_brightness);
    k = k * k * k;  /* cubic gamma correction */
    *r = (uint8_t)(((c >> 16) & 0xFF) * k);
    *g = (uint8_t)(((c >> 8) & 0xFF) * k);
    *b = (uint8_t)((c & 0xFF) * k);
}

/* HSV→RGB at S=255, used for rainbow. h is 0-255 (one full hue cycle).
 * v is the value (brightness) 0-255. */
static void hsv_to_rgb(uint8_t h, uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b) {
    uint8_t region = h / 43;       /* 0..5 */
    uint8_t remainder = (h - region * 43) * 6;
    uint8_t p = 0;
    uint8_t q = (uint8_t)((v * (uint16_t)(255 - remainder)) / 255);
    uint8_t t = (uint8_t)((v * (uint16_t)remainder) / 255);
    switch(region) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

static void render_breathing(NotificationApp* app) {
    /* 60-tick cycle (~2s at 30fps). Triangle wave 0..255..0. */
    uint32_t p = led_effect_phase % 60;
    uint16_t scale = (p < 30) ? (p * 255 / 30) : ((59 - p) * 255 / 30);
    uint8_t r, g, b;
    notification_unpack_color(app, &r, &g, &b);
    r = (uint8_t)((r * scale) / 255);
    g = (uint8_t)((g * scale) / 255);
    b = (uint8_t)((b * scale) / 255);
    furi_hal_light_set_rgb_all(r, g, b);
}

static void render_chase(NotificationApp* app) {
    uint16_t n = furi_hal_light_pixel_count();
    if(n == 0) return;
    /* Advance one pixel every 4 ticks (~133ms). */
    uint16_t head = (led_effect_phase / 4) % n;
    uint8_t r, g, b;
    notification_unpack_color(app, &r, &g, &b);
    for(uint16_t i = 0; i < n; i++) {
        if(i == head) {
            furi_hal_light_set_pixel(i, r, g, b);
        } else {
            /* Dim trail: 1/8th brightness on the rest. */
            furi_hal_light_set_pixel(i, r >> 3, g >> 3, b >> 3);
        }
    }
    furi_hal_light_refresh();
}

static void render_rainbow(NotificationApp* app) {
    uint16_t n = furi_hal_light_pixel_count();
    if(n == 0) return;
    /* Hue advances 1 unit per 2 ticks (~66ms) → ~17s full rainbow cycle. */
    float k = clamp01(app->settings.led_brightness);
    k = k * k * k; /* match cubic gamma in notification_unpack_color */
    uint8_t v = (uint8_t)(255 * k);
    uint8_t base = (uint8_t)((led_effect_phase / 2) & 0xFF);
    /* Each LED offset around the ring so the rainbow appears to rotate. */
    for(uint16_t i = 0; i < n; i++) {
        uint8_t hue = (uint8_t)(base + (i * 256) / n);
        uint8_t r, g, b;
        hsv_to_rgb(hue, v, &r, &g, &b);
        furi_hal_light_set_pixel(i, r, g, b);
    }
    furi_hal_light_refresh();
}

/* Strobe: sharp on/off flashes. 2 ticks on, 4 ticks off. */
static void render_strobe(NotificationApp* app) {
    bool on = (led_effect_phase % 6) < 2;
    if(on) {
        uint8_t r, g, b;
        notification_unpack_color(app, &r, &g, &b);
        furi_hal_light_set_rgb_all(r, g, b);
    } else {
        furi_hal_light_set_rgb_all(0, 0, 0);
    }
}

/* Cylon (Larson Scanner): single bright pixel sweeps back and forth, with a
 * dim trail on the immediate neighbors for a smear effect. */
static void render_cylon(NotificationApp* app) {
    uint16_t n = furi_hal_light_pixel_count();
    if(n < 2) return;
    /* Triangle wave: head walks 0..n-1..0..n-1 with one-tick-per-step granularity
     * adjusted by /3 so it's not too fast at default speed. */
    uint16_t period = 2 * (n - 1);
    uint16_t step = (uint16_t)((led_effect_phase / 3) % period);
    uint16_t head = (step < n) ? step : (uint16_t)(period - step);
    uint8_t r, g, b;
    notification_unpack_color(app, &r, &g, &b);
    for(uint16_t i = 0; i < n; i++) {
        int diff = (int)i - (int)head;
        if(diff < 0) diff = -diff;
        if(diff == 0) {
            furi_hal_light_set_pixel(i, r, g, b);
        } else if(diff == 1) {
            furi_hal_light_set_pixel(i, r >> 2, g >> 2, b >> 2); /* 25% trail */
        } else {
            furi_hal_light_set_pixel(i, 0, 0, 0);
        }
    }
    furi_hal_light_refresh();
}

/* Sparkle: each tick, every pixel has a small chance of flashing on for one
 * frame; otherwise dark. Creates a randomized twinkle on a dark base. */
static void render_sparkle(NotificationApp* app) {
    uint16_t n = furi_hal_light_pixel_count();
    if(n == 0) return;
    uint8_t r, g, b;
    notification_unpack_color(app, &r, &g, &b);
    uint32_t rnd = esp_random();
    for(uint16_t i = 0; i < n; i++) {
        /* Pull 4 bits of randomness per pixel (16 values), threshold 2 → ~12.5% on. */
        uint8_t roll = (uint8_t)((rnd >> (i * 4)) & 0xF);
        if(roll < 2) {
            furi_hal_light_set_pixel(i, r, g, b);
        } else {
            furi_hal_light_set_pixel(i, 0, 0, 0);
        }
    }
    furi_hal_light_refresh();
}

/* Wipe: paint pixels around the ring one at a time, then erase them one at a
 * time. Two-phase loop. */
static void render_wipe(NotificationApp* app) {
    uint16_t n = furi_hal_light_pixel_count();
    if(n == 0) return;
    uint16_t period = 2 * n;
    uint16_t step = (uint16_t)((led_effect_phase / 4) % period); /* one slot per 4 ticks */
    uint8_t r, g, b;
    notification_unpack_color(app, &r, &g, &b);
    for(uint16_t i = 0; i < n; i++) {
        bool lit;
        if(step < n) {
            /* Fill phase: 0..step are lit */
            lit = (i <= step);
        } else {
            /* Clear phase: 0..(step-n) are dark, rest still lit */
            lit = (i > (uint16_t)(step - n));
        }
        if(lit) {
            furi_hal_light_set_pixel(i, r, g, b);
        } else {
            furi_hal_light_set_pixel(i, 0, 0, 0);
        }
    }
    furi_hal_light_refresh();
}

static void notification_led_effect_tick(void* context) {
    furi_assert(context);
    NotificationApp* app = context;
    led_effect_phase++;
    switch(notification_active_led_effect(app)) {
        case LedEffectBreathing: render_breathing(app); break;
        case LedEffectChase:     render_chase(app);     break;
        case LedEffectRainbow:   render_rainbow(app);   break;
        case LedEffectStrobe:    render_strobe(app);    break;
        case LedEffectCylon:     render_cylon(app);     break;
        case LedEffectSparkle:   render_sparkle(app);   break;
        case LedEffectWipe:      render_wipe(app);      break;
        default: break; /* Solid/Off don't tick */
    }
}

/* Push the current settings to the WS2812 ring. Static effects (Solid/Off)
 * push once and stop the effect timer. Animated effects start the timer.
 * Also (re)starts the idle-off timer. */
void notification_apply_led_color(NotificationApp* app) {
    if(!app) return;
    /* About to re-light the ring — clear the idle-off marker. */
    app->led_idle_off = false;
    /* Always stop the effect timer; we'll restart for animated effects. */
    if(app->led_effect_timer && furi_timer_is_running(app->led_effect_timer)) {
        furi_timer_stop(app->led_effect_timer);
    }

    LedEffect eff = (LedEffect)notification_active_led_effect(app);
    uint8_t r, g, b;
    switch(eff) {
        case LedEffectOff:
            furi_hal_light_set_rgb_all(0, 0, 0);
            break;
        case LedEffectBreathing:
        case LedEffectChase:
        case LedEffectRainbow:
        case LedEffectStrobe:
        case LedEffectCylon:
        case LedEffectSparkle:
        case LedEffectWipe:
            led_effect_phase = 0;
            notification_led_effect_tick(app);          /* render frame 0 immediately */
            if(app->led_effect_timer) {
                uint32_t tick = notification_active_led_speed(app);
                if(tick == 0) tick = LED_EFFECT_TICK_MS; /* sane fallback */
                furi_timer_start(app->led_effect_timer, furi_ms_to_ticks(tick));
            }
            break;
        case LedEffectSolid:
        default:
            notification_unpack_color(app, &r, &g, &b);
            furi_hal_light_set_rgb_all(r, g, b);
            break;
    }

    /* Re-arm idle-off timer */
    if(app->led_off_timer) {
        if(app->settings.led_off_delay_ms > 0) {
            furi_timer_start(app->led_off_timer, furi_ms_to_ticks(app->settings.led_off_delay_ms));
        } else if(furi_timer_is_running(app->led_off_timer)) {
            furi_timer_stop(app->led_off_timer);
        }
    }
}

/* Idle-off timer callback: dark-out the WS2812 ring AND stop any running
 * effect. The next user input event re-applies color/effect via
 * notification_apply_led_color. */
static void notification_led_off_timer_cb(void* context) {
    furi_assert(context);
    NotificationApp* app = context;
    if(app->led_effect_timer && furi_timer_is_running(app->led_effect_timer)) {
        furi_timer_stop(app->led_effect_timer);
    }
    furi_hal_light_set_rgb_all(0, 0, 0);
    app->led_idle_off = true;
}

/* ─────────────────────────── UI color (LCD foreground) ─────────────────────
 * The ST7789 render path in furi_hal_display reads a single uint16_t fg_color
 * for every drawn pixel. Updating it at runtime recolors the entire UI on the
 * next frame — no full redraw needed. The byte order is RGB565 byte-swapped
 * for the S3 SPI peripheral (see board_lilygo_t_embed_cc1101.h:58). */

#define UI_COLOR_TICK_MS 50  /* Spectrum hue increments every 50ms → ~13s rotation */

/* Pack RGB888 (8-bit per channel) into RGB565 with the byte swap the S3 SPI
 * peripheral expects. */
/* The accent-colour table below needs a constant expression, so it uses the
 * macro form of the shared packer. */
#define UI_RGB(r, g, b) FURI_HAL_DISPLAY_RGB565_SWAP(r, g, b)

/* UI accent color presets. Order matches the labels in notification_settings_app.c.
 * Used by both ui_color_index (Background = fg_color) and ui_fg_color_index
 * (Foreground = bg_color). The last index (UI_COLOR_SPECTRUM_INDEX) is a
 * sentinel — the actual color is driven by the spectrum timer, not the table.
 *
 * Defaults: Background = Red (idx 2, the Netrunner accent), Foreground = Black
 * (idx 0) — a black-on-red UI. */
const uint16_t ui_color_value[UI_COLOR_SPECTRUM_INDEX] = {
    UI_RGB(0x00, 0x00, 0x00), /* Black */
    UI_RGB(0xFF, 0xA5, 0x00), /* Orange */
    UI_RGB(0xFF, 0x1F, 0x3D), /* Red — Netrunner accent #ff1f3d (matches the web UI) */
    UI_RGB(0x00, 0xFF, 0x00), /* Green */
    UI_RGB(0x00, 0x00, 0xFF), /* Blue */
    UI_RGB(0x00, 0xFF, 0xFF), /* Cyan */
    UI_RGB(0xFF, 0x1D, 0xCE), /* Magenta — matches LED preset */
    UI_RGB(0xFF, 0xFF, 0x00), /* Yellow */
    UI_RGB(0xFF, 0xFF, 0xFF), /* White */
    UI_RGB(0x8F, 0x00, 0xFF), /* Purple — matches LED preset */
};

static uint8_t ui_spectrum_hue = 0;

static void notification_ui_spectrum_tick(void* context) {
    furi_assert(context);
    NotificationApp* app = context;
    uint8_t r, g, b;
    /* Background (fg_color): hue at the current phase. */
    if(app->settings.ui_color_index == UI_COLOR_SPECTRUM_INDEX) {
        hsv_to_rgb(ui_spectrum_hue, 255, &r, &g, &b);
        furi_hal_display_set_fg_color(furi_hal_display_color565_swap(r, g, b));
    }
    /* Foreground (bg_color): same hue offset by 180° for visual contrast.
     * If both are Spectrum the UI gets an opposing-hue cycle that stays
     * legible at every phase. */
    if(app->settings.ui_fg_color_index == UI_COLOR_SPECTRUM_INDEX) {
        hsv_to_rgb((uint8_t)(ui_spectrum_hue + 128), 255, &r, &g, &b);
        furi_hal_display_set_bg_color(furi_hal_display_color565_swap(r, g, b));
    }
    ui_spectrum_hue++;
}

/* Palette entries are stored pre-packed as byte-swapped RGB565, but the
 * gradient builder interpolates in RGB888 (blending packed 565 would band
 * badly). Unpack back to 0x00RRGGBB, scaling each channel to full range. */
static uint32_t ui_color_to_rgb888(uint16_t swapped) {
    const uint16_t v = (uint16_t)(((swapped & 0xFF) << 8) | (swapped >> 8));
    const uint32_t r = (((v >> 11) & 0x1F) * 255U) / 31U;
    const uint32_t g = (((v >> 5) & 0x3F) * 255U) / 63U;
    const uint32_t b = ((v & 0x1F) * 255U) / 31U;
    return (r << 16) | (g << 8) | b;
}

/* Resolve one gradient stop to RGB888, honouring the custom-color slot the
 * same way ui_color_index does. Spectrum is animated and has no fixed value,
 * so it falls back to the stop's custom color. */
static uint32_t ui_gradient_stop_rgb888(const NotificationSettings* s, uint8_t stop) {
    const uint8_t idx = s->ui_bg_stop_index[stop];
    if(idx == UI_COLOR_CUSTOM_INDEX || idx >= UI_COLOR_SPECTRUM_INDEX) {
        return s->ui_bg_stop_custom[stop] & 0xFFFFFF;
    }
    return ui_color_to_rgb888(ui_color_value[idx]);
}

/* Push the gradient (or turn it off) based on the current settings. */
void notification_apply_ui_gradient(NotificationApp* app) {
    if(!app) return;
    const NotificationSettings* s = &app->settings;
    uint8_t count = s->ui_bg_color_count;

    if(count < 2) {
        /* One stop = flat background; the HAL falls back to fg_color. */
        furi_hal_display_set_bg_gradient(NULL, 0, 0, false);
        return;
    }
    if(count > FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS) {
        count = FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS;
    }

    uint32_t stops[FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS];
    for(uint8_t i = 0; i < count; i++) {
        stops[i] = ui_gradient_stop_rgb888(s, i);
    }
    furi_hal_display_set_bg_gradient(stops, count, s->ui_bg_mix, s->ui_bg_horizontal);
}

void notification_apply_ui_color(NotificationApp* app) {
    if(!app) return;
    uint8_t bg_idx = app->settings.ui_color_index;
    uint8_t fg_idx = app->settings.ui_fg_color_index;

    /* The gradient overrides the flat background when enabled, so keep it in
     * sync with every color change. */
    notification_apply_ui_gradient(app);

    /* Push static colors immediately; defer animated ones to the timer.
     * The custom index pulls its color from settings.ui_custom_color (0x00RRGGBB)
     * instead of the fixed palette. */
    if(bg_idx == UI_COLOR_CUSTOM_INDEX) {
        uint32_t c = app->settings.ui_custom_color;
        furi_hal_display_set_fg_color(
            furi_hal_display_color565_swap((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
    } else if(bg_idx < UI_COLOR_SPECTRUM_INDEX) {
        furi_hal_display_set_fg_color(ui_color_value[bg_idx]);
    } else if(bg_idx != UI_COLOR_SPECTRUM_INDEX) {
        furi_hal_display_set_fg_color(ui_color_value[UI_COLOR_DEFAULT_BG_INDEX]);
    }
    if(fg_idx < UI_COLOR_SPECTRUM_INDEX) {
        furi_hal_display_set_bg_color(ui_color_value[fg_idx]);
    } else if(fg_idx != UI_COLOR_SPECTRUM_INDEX) {
        furi_hal_display_set_bg_color(ui_color_value[0]);
    }

    /* Run the timer if either color is on Spectrum. */
    bool need_timer =
        (bg_idx == UI_COLOR_SPECTRUM_INDEX) || (fg_idx == UI_COLOR_SPECTRUM_INDEX);

    if(need_timer) {
        if(app->ui_spectrum_timer && !furi_timer_is_running(app->ui_spectrum_timer)) {
            furi_timer_start(app->ui_spectrum_timer, furi_ms_to_ticks(UI_COLOR_TICK_MS));
        }
    } else {
        if(app->ui_spectrum_timer && furi_timer_is_running(app->ui_spectrum_timer)) {
            furi_timer_stop(app->ui_spectrum_timer);
        }
    }
}

static void notification_load_led_app_effects(NotificationApp* app);
static bool notification_load_color_order(NotificationApp* app);

static NotificationApp* notification_app_alloc(void) {
    NotificationApp* app = malloc(sizeof(NotificationApp));

    app->queue = furi_message_queue_alloc(8, sizeof(NotificationAppMessage));
    app->display_timer = furi_timer_alloc(notification_display_timer, FuriTimerTypeOnce, app);
    app->display_sleep_timer =
        furi_timer_alloc(notification_display_sleep_timer, FuriTimerTypeOnce, app);
    app->display_idle_dim = false;
    app->night_shift_timer = furi_timer_alloc(night_shift_timer_callback, FuriTimerTypePeriodic, app);
    app->night_shift_demo_timer = furi_timer_alloc(night_shift_demo_timer_callback, FuriTimerTypeOnce, app);
    app->led_off_timer = furi_timer_alloc(notification_led_off_timer_cb, FuriTimerTypeOnce, app);
    app->led_effect_timer =
        furi_timer_alloc(notification_led_effect_tick, FuriTimerTypePeriodic, app);
    app->ui_spectrum_timer =
        furi_timer_alloc(notification_ui_spectrum_tick, FuriTimerTypePeriodic, app);

    app->settings.display_brightness = 1.0f;
    app->settings.display_off_delay_ms = NOTIFICATION_DISPLAY_OFF_DELAY_MS;
    app->settings.speaker_volume = 1.0f;
    app->settings.vibro_on = true;
    app->settings.night_shift = 1.0f;
    app->settings.night_shift_start = 1020;
    app->settings.night_shift_end = 300;
    app->settings.led_color = 0;  /* Off by default */
    app->settings.led_brightness = 0.30f; /* 30% pre-gamma → ~2.7% drive after cubic gamma; gentle ambient */
    app->settings.led_off_delay_ms = 0; /* Always on by default; user can pick a timeout */
    app->settings.led_effect = (uint8_t)LedEffectSolid; /* Default: static color */
    app->settings.led_speed_tick_ms = 33; /* Default ~30fps */
    app->led_effect_override_active = false;
    app->led_app_effects = true; /* app-driven LED effects on by default */
    app->lcd_bgr_order = true; /* per-unit; BGR matches the board boot default */
    app->settings.ui_color_index = UI_COLOR_DEFAULT_BG_INDEX; /* Red (Netrunner accent) — black-on-red UI */
    app->settings.ui_fg_color_index = 0; /* Default: Black (drawn elements) */
    app->settings.ui_custom_color = UI_CUSTOM_DEFAULT_RGB; /* picker start color */
    app->settings.ui_custom_color_set = false; /* no custom color defined yet */
    /* Gradient off by default so the stock look is unchanged. The stops still
     * get sensible starting colors so turning it on shows something useful
     * rather than four identical blacks. */
    app->settings.ui_bg_color_count = 1;
    app->settings.ui_bg_mix = (uint8_t)FuriHalDisplayGradientLinear;
    app->settings.ui_bg_horizontal = false;
    app->settings.display_sleep_delay_ms = NOTIFICATION_IDLE_SLEEP_DELAY_MS;
    for(uint8_t i = 0; i < FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS; i++) {
        /* Alternate Red / Black so enabling the gradient stays on-theme. */
        app->settings.ui_bg_stop_index[i] = (i % 2 == 0) ? UI_COLOR_DEFAULT_BG_INDEX : 0;
        app->settings.ui_bg_stop_custom[i] = UI_CUSTOM_DEFAULT_RGB;
    }
    app->current_night_shift = 1.0f;

    /* Try to load persisted settings (NVS-backed via saved_struct).
     * Fails silently on first boot or version mismatch — defaults stay in place. */
    notification_load_settings(app, false);
    notification_load_led_app_effects(app);

    /* Push the loaded LED color to the WS2812 ring. */
    notification_apply_led_color(app);

    /* Deliberately NOT applying the UI colour here: at this point /int is not
     * mounted yet, so the saved panel red/blue order is unknown and the display
     * is booted neutral (black). The themed colour is brought up in the service
     * loop below, once the saved order can be read and applied — otherwise it
     * would flash with the wrong order on a unit whose saved order is not the
     * compile-time default. */

    app->display.value[LayerInternal] = 0x00;
    app->display.value[LayerNotification] = 0x00;
    app->display.index = LayerInternal;
    app->display_led_lock_count = 0;

    app->event_record = furi_record_open(RECORD_INPUT_EVENTS);
    furi_pubsub_subscribe(app->event_record, input_event_callback, app);
    notification_message(app, &sequence_display_backlight_on);

    if(app->settings.night_shift != 1.0f) {
        night_shift_timer_start(app);
    } else {
        night_shift_timer_stop(app);
    }

    return app;
}

int32_t notification_srv(void* p) {
    UNUSED(p);

    NotificationApp* app = notification_app_alloc();

    notification_apply_internal_display_layer(app, 0x00);

    furi_record_create(RECORD_NOTIFICATION, app);

    /* Settings on the SD card win over the NVS copy loaded in alloc, so reload
     * once the card is mounted (or after a grace period without one). This
     * used to be a blocking wait before the loop below, which left every
     * queued message -- including the boot "backlight on" -- unprocessed, so
     * without a card the screen stayed black for up to 5 s. Now the queue is
     * served while the card is polled. */
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool sd_reload_pending = true;
    bool colors_pending = true;
    const uint32_t sd_wait_start = furi_get_tick();

    NotificationAppMessage message;
    while(true) {
        const FuriStatus status = furi_message_queue_get(
            app->queue, &message, sd_reload_pending ? furi_ms_to_ticks(100) : FuriWaitForever);

        /* Bring the themed colours up ONLY once the saved panel order has
         * actually been read from /int — never on a guess. The display booted
         * neutral (black); applying the themed colour before the order is known
         * would paint it with the boot-default order, which is exactly the blue
         * themed screen on a unit whose saved order is the opposite. This does
         * NOT wait for the SD card (the order lives in /int, which mounts early
         * and independently). A unit with no saved order file yet falls through
         * to the authoritative SD block below, which applies defaults — it stays
         * black until then rather than risk a wrong-order flash. */
        if(colors_pending && notification_load_color_order(app)) {
            colors_pending = false;
            furi_hal_display_set_bgr_order(app->lcd_bgr_order);
            notification_apply_ui_color(app);
        }

        if(sd_reload_pending &&
           (storage_sd_status(storage) == FSE_OK ||
            furi_get_tick() - sd_wait_start >= furi_ms_to_ticks(NOTIFICATION_SD_WAIT_MS))) {
            sd_reload_pending = false;
            furi_record_close(RECORD_STORAGE);
            storage = NULL;

            /* Authoritative re-apply once storage (incl. SD) is fully ready.
             * The colours_pending block above usually applied the order already;
             * re-reading here keeps it correct even if that ran before /int was
             * mounted, so the per-unit RGB/BGR choice survives a reboot. */
            notification_load_color_order(app);
            furi_hal_display_set_bgr_order(app->lcd_bgr_order);

            if(notification_load_settings(app, true)) {
                notification_apply_led_color(app);
                notification_apply_ui_color(app);
                /* Run it in place: posting to our own queue from the consumer
                 * thread would deadlock if the queue happened to be full. */
                NotificationAppMessage backlight = {
                    .sequence = &sequence_display_backlight_on,
                    .type = NotificationLayerMessage,
                    .back_event = NULL,
                };
                notification_process_notification_message(app, &backlight);
                if(app->settings.night_shift != 1.0f) {
                    night_shift_timer_start(app);
                } else {
                    night_shift_timer_stop(app);
                }
            }
        }

        if(status != FuriStatusOk) {
            furi_check(status == FuriStatusErrorTimeout);
            continue;
        }

        switch(message.type) {
        case NotificationLayerMessage:
            notification_process_notification_message(app, &message);
            break;
        case InternalLayerMessage:
            notification_process_internal_message(app, &message);
            break;
        case SaveSettingsMessage:
            notification_save_settings(app);
            break;
        case DisplayDimMessage:
            if(app->display_led_lock_count == 0) {
                notification_idle_timers_stop(app);
                notification_display_timer(app);
            }
            break;
        }

        if(message.back_event != NULL) {
            furi_event_flag_set(message.back_event, NOTIFICATION_EVENT_COMPLETE);
        }
    }

    return 0;
}

void notification_message(NotificationApp* app, const NotificationSequence* sequence) {
    notification_message_send(app, NotificationLayerMessage, sequence, NULL);
}

void notification_message_block(NotificationApp* app, const NotificationSequence* sequence) {
    FuriEventFlag* back_event = furi_event_flag_alloc();

    notification_message_send(app, NotificationLayerMessage, sequence, back_event);
    furi_event_flag_wait(
        back_event, NOTIFICATION_EVENT_COMPLETE, FuriFlagWaitAny, FuriWaitForever);
    furi_event_flag_free(back_event);
}

void notification_display_dim(NotificationApp* app) {
    furi_assert(app);
    notification_message_send(app, DisplayDimMessage, &sequence_empty, NULL);
}

void notification_internal_message(NotificationApp* app, const NotificationSequence* sequence) {
    notification_message_send(app, InternalLayerMessage, sequence, NULL);
}

void notification_internal_message_block(
    NotificationApp* app,
    const NotificationSequence* sequence) {
    FuriEventFlag* back_event = furi_event_flag_alloc();

    notification_message_send(app, InternalLayerMessage, sequence, back_event);
    furi_event_flag_wait(
        back_event, NOTIFICATION_EVENT_COMPLETE, FuriFlagWaitAny, FuriWaitForever);
    furi_event_flag_free(back_event);
}

static void notification_load_led_app_effects(NotificationApp* app) {
    bool enabled = app->led_app_effects; /* keep current default on failure */
    saved_struct_load(
        LED_APP_EFFECTS_PATH,
        &enabled,
        sizeof(enabled),
        LED_APP_EFFECTS_MAGIC,
        LED_APP_EFFECTS_VERSION);
    app->led_app_effects = enabled;
}

void notification_led_app_effects_set(NotificationApp* app, bool enabled) {
    furi_assert(app);
    app->led_app_effects = enabled;
    bool v = enabled;
    saved_struct_save(
        LED_APP_EFFECTS_PATH, &v, sizeof(v), LED_APP_EFFECTS_MAGIC, LED_APP_EFFECTS_VERSION);
    /* If turned off while an override happens to be live, drop back to ambient. */
    if(!enabled && app->led_effect_override_active) {
        app->led_effect_override_active = false;
        notification_apply_led_color(app);
    }
}

/* Returns true if the saved order file was actually read (i.e. /int is mounted
 * and a value exists) — used at boot as a "/int is ready" signal. */
static bool notification_load_color_order(NotificationApp* app) {
    bool bgr = app->lcd_bgr_order; /* keep current default on failure */
    bool loaded = saved_struct_load(
        LCD_COLOR_ORDER_PATH, &bgr, sizeof(bgr), LCD_COLOR_ORDER_MAGIC, LCD_COLOR_ORDER_VERSION);
    app->lcd_bgr_order = bgr;
    return loaded;
}

void notification_set_color_order(NotificationApp* app, bool bgr) {
    furi_assert(app);
    app->lcd_bgr_order = bgr;
    bool v = bgr;
    saved_struct_save(
        LCD_COLOR_ORDER_PATH, &v, sizeof(v), LCD_COLOR_ORDER_MAGIC, LCD_COLOR_ORDER_VERSION);
    /* Apply immediately so the user sees the swap take effect live. */
    furi_hal_display_set_bgr_order(bgr);
}

void notification_message_save_settings(NotificationApp* app) {
    furi_assert(app);
    notification_message_send(app, SaveSettingsMessage, &sequence_empty, NULL);
}
