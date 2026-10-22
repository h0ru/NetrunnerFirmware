#include <furi.h>
#include <furi_hal.h>

#include <gui/gui.h>
#include <gui/elements.h>
#include <datetime/datetime.h>
#include <wifi/wlan_hal.h>

#include "clock_app.h"

/* Layout follows Bruce's ClockMenu: the current time fills the screen. On the
 * first launch (or whenever the clock is unset) a chooser offers two ways to
 * set it -- WiFi auto-sync over NTP, or a manual time entry. OK on the clock
 * face reopens that chooser; Back exits. */

#define CLOCK_VALID_YEAR 2025 /* earlier means the clock was never set */
#define CLOCK_WIFI_TIMEOUT_MS 30000UL

static const char* const clock_weekday_text[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

static bool clock_time_is_set(void) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    return dt.year >= CLOCK_VALID_YEAR;
}

static void clock_input_callback(InputEvent* input_event, void* context) {
    furi_assert(context);
    FuriMessageQueue* queue = context;
    ClockMsg msg = {.type = ClockMsgInput, .input = *input_event};
    furi_message_queue_put(queue, &msg, FuriWaitForever);
}

/* One tick per second: used both to refresh the clock face and to poll for the
 * WiFi/NTP result while the "Connecting" screen is up. */
static void clock_tick(void* context) {
    FuriMessageQueue* queue = context;
    ClockMsg msg = {.type = ClockMsgTick};
    furi_message_queue_put(queue, &msg, 0);
}

static void clock_format_date(char* text, size_t text_size, const DateTime* dt) {
    const char* weekday =
        (dt->weekday >= 1 && dt->weekday <= 7) ? clock_weekday_text[dt->weekday - 1] : "";

    switch(furi_hal_rtc_get_locale_dateformat()) {
    case FuriHalRtcLocaleDateFormatMDY:
        snprintf(text, text_size, "%s %02u/%02u/%04u", weekday, dt->month, dt->day, dt->year);
        break;
    case FuriHalRtcLocaleDateFormatYMD:
        snprintf(text, text_size, "%s %04u-%02u-%02u", weekday, dt->year, dt->month, dt->day);
        break;
    default:
        snprintf(text, text_size, "%s %02u.%02u.%04u", weekday, dt->day, dt->month, dt->year);
        break;
    }
}

/* ------------------------------- clock face ------------------------------- */

static void clock_draw_face(Canvas* canvas) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);

    char big[12];
    char small[12];
    if(furi_hal_rtc_get_locale_timeformat() == FuriHalRtcLocaleTimeFormat12h) {
        const uint8_t hour12 = dt.hour % 12 == 0 ? 12 : dt.hour % 12;
        snprintf(big, sizeof(big), "%u:%02u", hour12, dt.minute);
        snprintf(small, sizeof(small), "%02u %s", dt.second, dt.hour < 12 ? "AM" : "PM");
    } else {
        snprintf(big, sizeof(big), "%02u:%02u", dt.hour, dt.minute);
        snprintf(small, sizeof(small), "%02u", dt.second);
    }

    char line[40];
    clock_format_date(line, sizeof(line), &dt);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignTop, line);

    /* Big HH:MM and small seconds, centered together. */
    canvas_set_font(canvas, FontBigNumbers);
    const uint16_t big_w = canvas_string_width(canvas, big);
    canvas_set_font(canvas, FontSecondary);
    const uint16_t small_w = small[0] ? canvas_string_width(canvas, small) + 2 : 0;
    const int32_t x = (128 - (int32_t)(big_w + small_w)) / 2;

    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str(canvas, x, 42, big);
    if(small[0]) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, x + big_w + 2, 42, small);
    }

    /* Hint on the base: the center button reopens the set-the-clock chooser. */
    canvas_set_font(canvas, FontSecondary);
    elements_button_center(canvas, "Set Time");
}

/* -------------------------------- chooser --------------------------------- */

static const char* const clock_chooser_rows[] = {"WiFi auto-sync", "Set time manually"};
#define CLOCK_CHOOSER_COUNT 2

static void clock_draw_chooser(Canvas* canvas, const ClockState* s) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 4, AlignCenter, AlignTop, "Set the clock");

    canvas_set_font(canvas, FontSecondary);
    if(s->wifi_failed) {
        canvas_draw_str_aligned(canvas, 64, 17, AlignCenter, AlignTop, "WiFi got no time");
    }

    for(uint8_t i = 0; i < CLOCK_CHOOSER_COUNT; i++) {
        const uint8_t y = 28 + i * 15;
        const bool selected = s->chooser_index == i;
        if(selected) {
            canvas_draw_box(canvas, 8, y, 112, 13);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str_aligned(canvas, 64, y + 6, AlignCenter, AlignCenter, clock_chooser_rows[i]);
        if(selected) canvas_set_color(canvas, ColorBlack);
    }
}

/* ------------------------------- wifi wait -------------------------------- */

static void clock_draw_wifi_wait(Canvas* canvas) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, "Connecting WiFi");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Waiting for network time");
    elements_button_center(canvas, "Cancel");
}

/* ---------------------------- manual time entry --------------------------- */

#define MANUAL_BLOCK_W 28
#define MANUAL_STEP    40
#define MANUAL_ORIGIN_X 12
#define MANUAL_BLOCK_Y 22

static void clock_draw_manual_block(Canvas* canvas, int32_t x, const char* text, bool selected) {
    if(selected) {
        canvas_draw_rframe(canvas, x, MANUAL_BLOCK_Y, MANUAL_BLOCK_W, 20, 3);
    }
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(
        canvas, x + MANUAL_BLOCK_W / 2, MANUAL_BLOCK_Y + 10, AlignCenter, AlignCenter, text);
}

static void clock_draw_manual(Canvas* canvas, const ClockState* s) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 1, AlignCenter, AlignTop, "Set Time");

    char field[3][4];
    snprintf(field[0], sizeof(field[0]), "%02u", s->manual_dt.hour);
    snprintf(field[1], sizeof(field[1]), "%02u", s->manual_dt.minute);
    snprintf(field[2], sizeof(field[2]), "%02u", s->manual_dt.second);

    for(uint8_t i = 0; i < 3; i++) {
        clock_draw_manual_block(
            canvas, MANUAL_ORIGIN_X + i * MANUAL_STEP, field[i], s->manual_field == i);
    }

    /* Colons as stacked dots between the blocks. */
    canvas_set_color(canvas, ColorBlack);
    for(uint8_t i = 1; i <= 2; i++) {
        const int32_t cx = MANUAL_ORIGIN_X + i * MANUAL_STEP - 6;
        canvas_draw_box(canvas, cx, MANUAL_BLOCK_Y + 6, 2, 2);
        canvas_draw_box(canvas, cx, MANUAL_BLOCK_Y + 12, 2, 2);
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: next   Back: save");
}

/* -------------------------------- render ---------------------------------- */

static void clock_render_callback(Canvas* canvas, void* context) {
    ClockState* state = context;
    if(furi_mutex_acquire(state->mutex, 200) != FuriStatusOk) return;
    const ClockState snapshot = *state;
    furi_mutex_release(state->mutex);

    canvas_clear(canvas);
    switch(snapshot.screen) {
    case ClockScreenChooser:
        clock_draw_chooser(canvas, &snapshot);
        break;
    case ClockScreenWifiWait:
        clock_draw_wifi_wait(canvas);
        break;
    case ClockScreenManual:
        clock_draw_manual(canvas, &snapshot);
        break;
    case ClockScreenClock:
        clock_draw_face(canvas);
        break;
    }
}

/* -------------------------------- helpers --------------------------------- */

static void clock_enter_manual(ClockState* s) {
    furi_hal_rtc_get_datetime(&s->manual_dt);
    if(!datetime_validate_datetime(&s->manual_dt)) {
        /* Unset clock reads back as 1970, which fails validation; start from a
         * valid placeholder date so the entered time can be applied. */
        s->manual_dt.year = 2024;
        s->manual_dt.month = 1;
        s->manual_dt.day = 1;
        s->manual_dt.weekday = 1;
        s->manual_dt.hour = 0;
        s->manual_dt.minute = 0;
        s->manual_dt.second = 0;
    }
    s->manual_field = 0;
    s->screen = ClockScreenManual;
}

static void clock_start_wifi(ClockState* s) {
    s->wifi_failed = false;
    s->screen = ClockScreenWifiWait;
    s->wifi_deadline_tick = furi_get_tick() + furi_ms_to_ticks(CLOCK_WIFI_TIMEOUT_MS);
    /* Enabling the user radio connects to the saved AP; each successful
     * connection performs one SNTP update into the RTC (see wlan_hal). */
    wlan_hal_set_user_enabled(true);
}

/* Returns true if the app should exit. Caller holds the mutex. */
static bool clock_handle_input(ClockState* s, const InputEvent* input) {
    if(input->type != InputTypeShort && input->type != InputTypeRepeat) return false;

    switch(s->screen) {
    case ClockScreenChooser:
        if(input->key == InputKeyUp || input->key == InputKeyDown) {
            s->chooser_index ^= 1U;
        } else if(input->key == InputKeyOk) {
            if(s->chooser_index == 0) {
                clock_start_wifi(s);
            } else {
                clock_enter_manual(s);
            }
        } else if(input->key == InputKeyBack) {
            return true;
        }
        break;

    case ClockScreenWifiWait:
        if(input->key == InputKeyOk || input->key == InputKeyBack) {
            /* Cancel the wait; the radio keeps whatever state it reached. */
            s->screen = ClockScreenChooser;
        }
        break;

    case ClockScreenManual: {
        uint8_t* value = (s->manual_field == 0) ? &s->manual_dt.hour :
                         (s->manual_field == 1) ? &s->manual_dt.minute :
                                                  &s->manual_dt.second;
        const uint8_t wrap = (s->manual_field == 0) ? 24U : 60U;
        switch(input->key) {
        case InputKeyUp:
            *value = (uint8_t)((*value + 1U) % wrap);
            break;
        case InputKeyDown:
            *value = (uint8_t)((*value + wrap - 1U) % wrap);
            break;
        case InputKeyLeft:
            if(s->manual_field > 0) s->manual_field--;
            break;
        case InputKeyRight:
            if(s->manual_field < 2) s->manual_field++;
            break;
        case InputKeyOk:
            s->manual_field = (s->manual_field + 1U) % 3U;
            break;
        case InputKeyBack:
            furi_hal_rtc_set_datetime(&s->manual_dt);
            s->screen = ClockScreenClock;
            break;
        default:
            break;
        }
        break;
    }

    case ClockScreenClock:
        if(input->key == InputKeyOk) {
            s->wifi_failed = false;
            s->screen = ClockScreenChooser;
        } else if(input->key == InputKeyBack) {
            return true;
        }
        break;
    }
    return false;
}

/* Caller holds the mutex. */
static void clock_handle_tick(ClockState* s) {
    if(s->screen != ClockScreenWifiWait) return;

    if(clock_time_is_set()) {
        s->screen = ClockScreenClock;
    } else if((int32_t)(furi_get_tick() - s->wifi_deadline_tick) >= 0) {
        s->wifi_failed = true;
        s->screen = ClockScreenChooser;
    }
}

int32_t clock_app(void* p) {
    UNUSED(p);

    ClockState* state = calloc(1, sizeof(ClockState));
    if(!state) return 255;

    state->queue = furi_message_queue_alloc(8, sizeof(ClockMsg));
    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    state->screen = clock_time_is_set() ? ClockScreenClock : ClockScreenChooser;

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, clock_render_callback, state);
    view_port_input_callback_set(view_port, clock_input_callback, state->queue);

    FuriTimer* timer = furi_timer_alloc(clock_tick, FuriTimerTypePeriodic, state->queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    furi_timer_start(timer, furi_kernel_get_tick_frequency());

    ClockMsg msg;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(state->queue, &msg, FuriWaitForever) != FuriStatusOk) continue;

        furi_mutex_acquire(state->mutex, FuriWaitForever);
        if(msg.type == ClockMsgInput) {
            if(clock_handle_input(state, &msg.input)) running = false;
        } else {
            clock_handle_tick(state);
        }
        furi_mutex_release(state->mutex);

        view_port_update(view_port);
    }

    furi_timer_stop(timer);
    furi_timer_free(timer);
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_message_queue_free(state->queue);
    furi_mutex_free(state->mutex);
    free(state);

    return 0;
}
