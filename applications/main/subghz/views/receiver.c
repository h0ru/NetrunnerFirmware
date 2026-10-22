#include "receiver.h"
#include "../subghz_i.h"

#include "types.h"
#include <input/input.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <m-array.h>

#define FRAME_HEIGHT 12
#define MAX_LEN_PX   111
#define MENU_ITEMS   4u
#define UNLOCK_CNT   3

/* Live spectrum-analyzer bars on the idle Read screen. */
#define SPECTRUM_BARS    29
#define SPECTRUM_BASE_Y  42 /* baseline the bars grow upward from */
#define SPECTRUM_TOP_Y   14 /* tallest a bar may reach */
#define SPECTRUM_RSSI_FS 42 /* u_rssi value treated as a full-height bar */

// #define SUBGHZ_RAW_THRESHOLD_MIN (-90.0f)

#define FLIP_TIMEOUT (500)

typedef struct {
    FuriString* item_str;
    FuriString* time;
    uint8_t type;
} SubGhzReceiverMenuItem;

ARRAY_DEF(SubGhzReceiverMenuItemArray, SubGhzReceiverMenuItem, M_POD_OPLIST) //-V658

#define M_OPL_SubGhzReceiverMenuItemArray_t() \
    ARRAY_OPLIST(SubGhzReceiverMenuItemArray, M_POD_OPLIST)

struct SubGhzReceiverHistory {
    SubGhzReceiverMenuItemArray_t data;
};

typedef struct SubGhzReceiverHistory SubGhzReceiverHistory;

static const Icon* ReceiverItemIcons[] = {
    [SubGhzProtocolTypeUnknown] = &I_Quest_7x8,
    [SubGhzProtocolTypeStatic] = &I_Static_9x7,
    [SubGhzProtocolTypeDynamic] = &I_Dynamic_9x7,
    [SubGhzProtocolTypeBinRAW] = &I_Raw_9x7,
};

typedef enum {
    SubGhzViewReceiverBarShowDefault,
    SubGhzViewReceiverBarShowLock,
    SubGhzViewReceiverBarShowToUnlockPress,
    SubGhzViewReceiverBarShowUnlock,
} SubGhzViewReceiverBarShow;

struct SubGhzViewReceiver {
    bool lock;
    uint8_t lock_count;
    FuriTimer* timer;
    View* view;
    SubGhzViewReceiverCallback callback;
    void* context;
    FuriTimer* flip_time_timer;
};

typedef struct {
    FuriString* frequency_str;
    FuriString* preset_str;
    FuriString* history_stat_str;
    FuriString* progress_str;
    bool hopping_enabled;
    bool bin_raw_enabled;
    SubGhzReceiverHistory* history;
    uint16_t idx;
    uint16_t list_offset;
    uint16_t history_item;
    SubGhzViewReceiverBarShow bar_show;
    SubGhzViewReceiverMode mode;
    uint8_t u_rssi;

    SubGhzRadioDeviceType device_type;

    bool show_time;

    bool nodraw;

    /* Rolling RSSI history that drives the live spectrum-analyzer bars on the
     * idle Read screen. Each RSSI sample is pushed to the right; the display
     * reads these as bar heights, so stronger/more frequent activity shows as
     * taller bars that scroll across the screen. */
    uint8_t rssi_history[SPECTRUM_BARS];
} SubGhzViewReceiverModel;

void subghz_view_receiver_set_mode(
    SubGhzViewReceiver* subghz_receiver,
    SubGhzViewReceiverMode mode) {
    with_view_model(
        subghz_receiver->view, SubGhzViewReceiverModel * model, { model->mode = mode; }, true);
}

void subghz_receiver_rssi(SubGhzViewReceiver* instance, float rssi) {
    furi_assert(instance);
    with_view_model(
        instance->view,
        SubGhzViewReceiverModel * model,
        {
            if(rssi < SUBGHZ_RAW_THRESHOLD_MIN) {
                model->u_rssi = 0;
            } else {
                model->u_rssi = (uint8_t)(rssi - SUBGHZ_RAW_THRESHOLD_MIN);
            }
            /* Scroll the spectrum history left and append the new sample. */
            for(uint8_t i = 0; i + 1 < SPECTRUM_BARS; i++) {
                model->rssi_history[i] = model->rssi_history[i + 1];
            }
            model->rssi_history[SPECTRUM_BARS - 1] = model->u_rssi;
        },
        true);
}

void subghz_view_receiver_set_lock(SubGhzViewReceiver* subghz_receiver, bool lock) {
    furi_assert(subghz_receiver);
    subghz_receiver->lock_count = 0;
    if(lock == true) {
        subghz_receiver->lock = lock;
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            { model->bar_show = SubGhzViewReceiverBarShowLock; },
            true);
        furi_timer_start(subghz_receiver->timer, 1000);
    } else {
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            { model->bar_show = SubGhzViewReceiverBarShowDefault; },
            true);
    }
}

void subghz_view_receiver_set_callback(
    SubGhzViewReceiver* subghz_receiver,
    SubGhzViewReceiverCallback callback,
    void* context) {
    furi_assert(subghz_receiver);
    furi_assert(callback);
    subghz_receiver->callback = callback;
    subghz_receiver->context = context;
}

static void subghz_view_receiver_update_offset(SubGhzViewReceiver* subghz_receiver) {
    furi_assert(subghz_receiver);

    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            size_t history_item = model->history_item;
            uint16_t bounds = history_item > 3 ? 2 : history_item;

            if(history_item > 3 && model->idx >= (int16_t)(history_item - 1)) {
                model->list_offset = model->idx - 3;
            } else if(model->list_offset < model->idx - bounds) {
                model->list_offset =
                    CLAMP(model->list_offset + 1, (int16_t)(history_item - bounds), 0);
            } else if(model->list_offset > model->idx - bounds) {
                model->list_offset = CLAMP(model->idx - 1, (int16_t)(history_item - bounds), 0);
            }
        },
        true);
}

static void subghz_view_receiver_show_time_moment(void* context) {
    furi_assert(context);
    SubGhzViewReceiver* subghz_receiver = context;
    with_view_model(
        subghz_receiver->view, SubGhzViewReceiverModel * model, { model->show_time = true; }, true);
    furi_timer_start(subghz_receiver->flip_time_timer, FLIP_TIMEOUT);
}

static void subghz_view_receiver_flip_string_callback(void* context) {
    furi_assert(context);
    SubGhzViewReceiver* subghz_receiver = context;
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        { model->show_time = false; },
        true);
}

void subghz_view_receiver_add_item_to_menu(
    SubGhzViewReceiver* subghz_receiver,
    const char* name,
    const char* time,
    uint8_t type) {
    furi_assert(subghz_receiver);
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            SubGhzReceiverMenuItem* item_menu =
                SubGhzReceiverMenuItemArray_push_raw(model->history->data);
            item_menu->time = furi_string_alloc_set(time);
            item_menu->item_str = furi_string_alloc_set(name);
            item_menu->type = type;
            if(model->history_item == 0) {
                view_set_input_mode(subghz_receiver->view, ViewInputModeUpDown);
            }
            if(model->idx == model->history_item - 1) {
                model->history_item++;
                model->idx++;
                subghz_view_receiver_show_time_moment(subghz_receiver);
            } else {
                model->history_item++;
            }
        },
        true);
    subghz_view_receiver_update_offset(subghz_receiver);
}

void subghz_view_receiver_add_data_statusbar(
    SubGhzViewReceiver* subghz_receiver,
    const char* frequency_str,
    const char* preset_str,
    const char* history_stat_str,
    bool hopping_enabled,
    bool bin_raw_enabled) {
    furi_assert(subghz_receiver);
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            furi_string_set(model->frequency_str, frequency_str);
            furi_string_set(model->preset_str, preset_str);
            furi_string_set(model->history_stat_str, history_stat_str);
            model->hopping_enabled = hopping_enabled;
            model->bin_raw_enabled = bin_raw_enabled;
        },
        true);
}

void subghz_view_receiver_add_data_progress(
    SubGhzViewReceiver* subghz_receiver,
    const char* progress_str) {
    furi_assert(subghz_receiver);
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        { furi_string_set(model->progress_str, progress_str); },
        true);
}

void subghz_view_receiver_set_radio_device_type(
    SubGhzViewReceiver* subghz_receiver,
    SubGhzRadioDeviceType device_type) {
    furi_assert(subghz_receiver);
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        { model->device_type = device_type; },
        true);
}

static void subghz_view_receiver_draw_frame(Canvas* canvas, uint16_t idx, bool scrollbar) {
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0 + idx * FRAME_HEIGHT, scrollbar ? 122 : 127, FRAME_HEIGHT);

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_dot(canvas, 0, 0 + idx * FRAME_HEIGHT);
    canvas_draw_dot(canvas, 1, 0 + idx * FRAME_HEIGHT);
    canvas_draw_dot(canvas, 0, (0 + idx * FRAME_HEIGHT) + 1);

    canvas_draw_dot(canvas, 0, (0 + idx * FRAME_HEIGHT) + 11);
    canvas_draw_dot(canvas, scrollbar ? 121 : 126, 0 + idx * FRAME_HEIGHT);
    canvas_draw_dot(canvas, scrollbar ? 121 : 126, (0 + idx * FRAME_HEIGHT) + 11);
}

static void subghz_view_rssi_draw(Canvas* canvas, SubGhzViewReceiverModel* model) {
    for(uint8_t i = 1; i < model->u_rssi; i++) {
        if(i % 5) {
            canvas_draw_dot(canvas, 46 + i, 50);
            canvas_draw_dot(canvas, 47 + i, 51);
            canvas_draw_dot(canvas, 46 + i, 52);
        }
    }
}

/* Live spectrum analyzer: draws the rolling RSSI history as vertical bars with
 * a short dotted reflection, centered in the idle Read screen. */
static void subghz_view_receiver_draw_spectrum(Canvas* canvas, SubGhzViewReceiverModel* model) {
    const uint8_t bw = 2; /* bar width */
    const uint8_t gap = 2; /* gap between bars */
    const uint8_t total_w = SPECTRUM_BARS * (bw + gap) - gap;
    const uint8_t x0 = (128 - total_w) / 2; /* centered horizontally */
    const uint8_t range = SPECTRUM_BASE_Y - SPECTRUM_TOP_Y;

    canvas_set_color(canvas, ColorBlack);
    for(uint8_t i = 0; i < SPECTRUM_BARS; i++) {
        uint16_t v = model->rssi_history[i];
        uint16_t h = (v * range) / SPECTRUM_RSSI_FS;
        if(h > range) h = range;
        const uint8_t bx = x0 + i * (bw + gap);
        if(h > 0) {
            canvas_draw_box(canvas, bx, SPECTRUM_BASE_Y - (uint8_t)h, bw, (uint8_t)h);
        }
        /* dotted reflection under the baseline */
        const uint8_t rh = (uint8_t)(h / 3);
        for(uint8_t yy = 0; yy < rh; yy += 2) {
            canvas_draw_dot(canvas, bx, SPECTRUM_BASE_Y + 2 + yy);
            canvas_draw_dot(canvas, bx + 1, SPECTRUM_BASE_Y + 2 + yy);
        }
    }
}

void subghz_view_receiver_draw(Canvas* canvas, SubGhzViewReceiverModel* model) {
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    if(model->mode == SubGhzViewReceiverModeLive) {
        elements_button_left(canvas, "Config");
        //canvas_draw_line(canvas, 46, 51, 125, 51);
    } else {
        canvas_draw_line(canvas, 2, 52, 125, 52);
        canvas_draw_str(canvas, 3, 62, furi_string_get_cstr(model->progress_str));
    }

    bool scrollbar = model->history_item > 4;
    FuriString* str_buff = furi_string_alloc();

    if(!model->nodraw) {
        SubGhzReceiverMenuItem* item_menu;

        for(size_t i = 0; i < MIN(model->history_item, MENU_ITEMS); ++i) {
            size_t idx = CLAMP((uint16_t)(i + model->list_offset), model->history_item, 0);
            item_menu = SubGhzReceiverMenuItemArray_get(model->history->data, idx);
            if(item_menu == NULL) {
                break;
            }
            if(item_menu->type == 0) {
                break;
            }
            furi_string_set(str_buff, item_menu->item_str);
            if(model->idx == idx) {
                subghz_view_receiver_draw_frame(canvas, i, scrollbar);
                if(model->show_time) {
                    // Show time of signal one moment
                    furi_string_set(str_buff, item_menu->time);
                }
            } else {
                canvas_set_color(canvas, ColorBlack);
            }
            elements_string_fit_width(canvas, str_buff, scrollbar ? MAX_LEN_PX - 7 : MAX_LEN_PX);
            canvas_draw_icon(canvas, 4, 2 + i * FRAME_HEIGHT, ReceiverItemIcons[item_menu->type]);
            canvas_draw_str(canvas, 15, 9 + i * FRAME_HEIGHT, furi_string_get_cstr(str_buff));
            furi_string_reset(str_buff);
        }
        if(scrollbar) {
            elements_scrollbar_pos(canvas, 128, 0, 49, model->idx, model->history_item);
        }
    }
    furi_string_free(str_buff);

    canvas_set_color(canvas, ColorBlack);

    if(model->history_item == 0) {
        /* Idle Read screen: live spectrum analyzer driven by the RSSI history,
         * with a small status word. Replaces the old static dolphin graphic and
         * the large overlapping "scan..." text. */
        subghz_view_receiver_draw_spectrum(canvas, model);
        canvas_set_font(canvas, FontSecondary);
        if(model->mode == SubGhzViewReceiverModeLive) {
            /* Center "REC" button (OK): starts RAW capture. Config stays on the
             * left; the frequency/modulation sit on the top row. */
            elements_button_center(canvas, "REC");
            /* Top-left: "<f> MHz". The "Freq:" label is dropped so the frequency
             * and the top-right "Mod: …" both fit on the 128px-wide top row.
             * (Modulation is drawn top-right in the preset block below.) */
            uint8_t fx = 2;
            canvas_draw_str(canvas, fx, 7, furi_string_get_cstr(model->frequency_str));
            fx += canvas_string_width(canvas, furi_string_get_cstr(model->frequency_str)) + 2;
            canvas_draw_str(canvas, fx, 7, "MHz");
            if(model->bin_raw_enabled) {
                canvas_draw_icon(canvas, 118, 17, &I_Cos_9x7);
            }
        }
    }

    if(model->mode == SubGhzViewReceiverModeLive && model->history_item != 0) {
        /* The idle screen already shows RSSI as the live spectrum bars, so the
         * small dotted RSSI meter is only drawn once a history list is present. */
        subghz_view_rssi_draw(canvas, model);
    }
    switch(model->bar_show) {
    case SubGhzViewReceiverBarShowLock:
        canvas_draw_icon(canvas, 64, 55, &I_Lock_7x8);
        canvas_draw_str(canvas, 74, 62, "Locked");
        break;
    case SubGhzViewReceiverBarShowToUnlockPress:
        canvas_draw_str(canvas, 44, 62, furi_string_get_cstr(model->frequency_str));
#ifdef SUBGHZ_EXT_PRESET_NAME
        if(model->history_item == 0 && model->mode == SubGhzViewReceiverModeLive) {
            canvas_draw_str(
                canvas,
                44 + canvas_string_width(canvas, furi_string_get_cstr(model->frequency_str)) + 1,
                62,
                "MHz");
            const char* str = furi_string_get_cstr(model->preset_str);
            const uint8_t vertical_offset = 7;
            const uint8_t horizontal_offset = 3;
            const uint8_t string_width = canvas_string_width(canvas, str);
            canvas_draw_str(
                canvas,
                canvas_width(canvas) - (string_width + horizontal_offset),
                vertical_offset,
                str);
        } else {
            canvas_draw_str(canvas, 79, 62, furi_string_get_cstr(model->preset_str));
        }
#else
        canvas_draw_str(canvas, 79, 62, furi_string_get_cstr(model->preset_str));
#endif
        canvas_draw_str(canvas, 96, 62, furi_string_get_cstr(model->history_stat_str));
        canvas_set_font(canvas, FontSecondary);
        elements_bold_rounded_frame(canvas, 14, 8, 99, 48);
        elements_multiline_text(canvas, 65, 26, "To unlock\npress:");
        canvas_draw_icon(canvas, 65, 42, &I_Pin_back_arrow_10x8);
        canvas_draw_icon(canvas, 80, 42, &I_Pin_back_arrow_10x8);
        canvas_draw_icon(canvas, 95, 42, &I_Pin_back_arrow_10x8);
        canvas_draw_icon(canvas, 16, 13, &I_WarningDolphin_45x42);
        canvas_draw_dot(canvas, 17, 61);
        break;
    case SubGhzViewReceiverBarShowUnlock:
        canvas_draw_icon(canvas, 64, 55, &I_Unlock_7x8);
        canvas_draw_str(canvas, 74, 62, "Unlocked");
        break;
    default: {
        const bool idle_live =
            (model->history_item == 0 && model->mode == SubGhzViewReceiverModeLive);
        const char* frequency_str = furi_string_get_cstr(model->frequency_str);
        /* On the idle Read screen the frequency/modulation live on the top row
         * (drawn above) and the bottom center is the REC button, so skip the
         * bottom frequency here. */
        if(!idle_live) {
            canvas_draw_str(canvas, 44, 62, frequency_str);
        }
#ifdef SUBGHZ_EXT_PRESET_NAME
        if(idle_live) {
            /* Modulation on the top-right; history counter dropped on idle. */
            const char* str = furi_string_get_cstr(model->preset_str);
            const uint8_t vertical_offset = 7;
            const uint8_t horizontal_offset = 3;
            const uint8_t string_width = canvas_string_width(canvas, str);
            canvas_draw_str(
                canvas,
                canvas_width(canvas) - (string_width + horizontal_offset),
                vertical_offset,
                str);
        } else {
            canvas_draw_str(canvas, 79, 62, furi_string_get_cstr(model->preset_str));
        }
#else
        canvas_draw_str(canvas, 79, 62, furi_string_get_cstr(model->preset_str));
#endif
        if(!idle_live) {
            canvas_draw_str(canvas, 96, 62, furi_string_get_cstr(model->history_stat_str));
        }
    } break;
    }
}

static void subghz_view_receiver_timer_callback(void* context) {
    furi_assert(context);
    SubGhzViewReceiver* subghz_receiver = context;
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        { model->bar_show = SubGhzViewReceiverBarShowDefault; },
        true);
    if(subghz_receiver->lock_count < UNLOCK_CNT) {
        subghz_receiver->callback(
            SubGhzCustomEventViewReceiverOffDisplay, subghz_receiver->context);
    }
    subghz_receiver->lock_count = 0;
}

bool subghz_view_receiver_input(InputEvent* event, void* context) {
    furi_assert(context);
    SubGhzViewReceiver* subghz_receiver = context;

    if(subghz_receiver->lock == true) {
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            { model->bar_show = SubGhzViewReceiverBarShowToUnlockPress; },
            true);
        if(subghz_receiver->lock_count == 0) {
            furi_timer_start(subghz_receiver->timer, 1000);
        }
        if(event->key == InputKeyBack && event->type == InputTypeShort) {
            subghz_receiver->lock_count++;
        }
        if(subghz_receiver->lock_count >= UNLOCK_CNT) {
            subghz_receiver->callback(
                SubGhzCustomEventViewReceiverUnlock, subghz_receiver->context);
            with_view_model(
                subghz_receiver->view,
                SubGhzViewReceiverModel * model,
                { model->bar_show = SubGhzViewReceiverBarShowUnlock; },
                true);
            subghz_receiver->lock = false;
            furi_timer_start(subghz_receiver->timer, 650);
        }

        return true;
    }

    bool consumed = false;
    if(event->key == InputKeyBack && event->type == InputTypeShort) {
        subghz_receiver->callback(SubGhzCustomEventViewReceiverBack, subghz_receiver->context);
        consumed = true;
    } else if(
        event->key == InputKeyUp &&
        (event->type == InputTypeShort || event->type == InputTypeRepeat)) {
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            {
                if(model->idx != 0) model->idx--;
                subghz_view_receiver_show_time_moment(context);
            },
            true);
        consumed = true;
    } else if(
        event->key == InputKeyDown &&
        (event->type == InputTypeShort || event->type == InputTypeRepeat)) {
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            {
                if((model->history_item != 0) && (model->idx != model->history_item - 1)) {
                    model->idx++;
                    subghz_view_receiver_show_time_moment(context);
                }
            },
            true);
        consumed = true;
    } else if(event->key == InputKeyLeft && event->type == InputTypeShort) {
        /* Left (<) opens the radio Config. */
        subghz_receiver->callback(SubGhzCustomEventViewReceiverConfig, subghz_receiver->context);
        consumed = true;
    } else if(event->key == InputKeyRight && event->type == InputTypeLong) {
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            {
                if(model->history_item != 0) {
                    // Callback
                    subghz_receiver->callback(
                        SubGhzCustomEventViewReceiverDeleteItem, subghz_receiver->context);
                }
            },
            false);
        consumed = true;
    } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
        bool rec = false;
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            {
                if(model->history_item != 0) {
                    subghz_receiver->callback(
                        SubGhzCustomEventViewReceiverOK, subghz_receiver->context);
                } else if(model->mode == SubGhzViewReceiverModeLive) {
                    /* No captured signals yet: OK is the center "REC" button and
                     * opens RAW capture. */
                    rec = true;
                }
            },
            false);
        if(rec) {
            subghz_receiver->callback(
                SubGhzCustomEventViewReceiverReadRAW, subghz_receiver->context);
        }
        consumed = true;
    }

    if(consumed) {
        subghz_view_receiver_update_offset(subghz_receiver);
    }
    return consumed;
}

void subghz_view_receiver_enter(void* context) {
    furi_assert(context);
    subghz_view_receiver_show_time_moment(context);
}

void subghz_view_receiver_exit(void* context) {
    furi_assert(context);
    SubGhzViewReceiver* subghz_receiver = context;
    view_set_input_mode(subghz_receiver->view, ViewInputModeLeftRight);
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            furi_string_reset(model->frequency_str);
            furi_string_reset(model->preset_str);
            furi_string_reset(model->history_stat_str);

                for
                    M_EACH(item_menu, model->history->data, SubGhzReceiverMenuItemArray_t) {
                        furi_string_free(item_menu->item_str);
                        furi_string_free(item_menu->time);
                        item_menu->type = 0;
                    }
                SubGhzReceiverMenuItemArray_reset(model->history->data);
                model->idx = 0;
                model->list_offset = 0;
                model->history_item = 0;
                model->nodraw = false;
                model->hopping_enabled = false;
                model->bin_raw_enabled = false;
        },
        false);
    furi_timer_stop(subghz_receiver->timer);
    furi_timer_stop(subghz_receiver->flip_time_timer);
}

SubGhzViewReceiver* subghz_view_receiver_alloc(void) {
    SubGhzViewReceiver* subghz_receiver = malloc(sizeof(SubGhzViewReceiver));

    // View allocation and configuration
    subghz_receiver->view = view_alloc();

    subghz_receiver->lock = false;
    subghz_receiver->lock_count = 0;
    view_allocate_model(
        subghz_receiver->view, ViewModelTypeLocking, sizeof(SubGhzViewReceiverModel));
    view_set_context(subghz_receiver->view, subghz_receiver);
    view_set_draw_callback(subghz_receiver->view, (ViewDrawCallback)subghz_view_receiver_draw);
    view_set_input_callback(subghz_receiver->view, subghz_view_receiver_input);
    view_set_input_mode(subghz_receiver->view, ViewInputModeLeftRight);
    view_set_enter_callback(subghz_receiver->view, subghz_view_receiver_enter);
    view_set_exit_callback(subghz_receiver->view, subghz_view_receiver_exit);

    subghz_receiver->flip_time_timer = furi_timer_alloc(
        subghz_view_receiver_flip_string_callback, FuriTimerTypeOnce, subghz_receiver);

    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            model->frequency_str = furi_string_alloc();
            model->preset_str = furi_string_alloc();
            model->history_stat_str = furi_string_alloc();
            model->progress_str = furi_string_alloc();
            model->bar_show = SubGhzViewReceiverBarShowDefault;
            model->nodraw = false;
            model->history = malloc(sizeof(SubGhzReceiverHistory));
            model->hopping_enabled = false;
            model->bin_raw_enabled = false;
            SubGhzReceiverMenuItemArray_init(model->history->data);
        },
        true);
    subghz_receiver->timer =
        furi_timer_alloc(subghz_view_receiver_timer_callback, FuriTimerTypeOnce, subghz_receiver);
    return subghz_receiver;
}

void subghz_view_receiver_free(SubGhzViewReceiver* subghz_receiver) {
    furi_assert(subghz_receiver);

    furi_timer_free(subghz_receiver->flip_time_timer);

    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            furi_string_free(model->frequency_str);
            furi_string_free(model->preset_str);
            furi_string_free(model->history_stat_str);
            furi_string_free(model->progress_str);
                for
                    M_EACH(item_menu, model->history->data, SubGhzReceiverMenuItemArray_t) {
                        furi_string_free(item_menu->item_str);
                        furi_string_free(item_menu->time);
                        item_menu->type = 0;
                    }
                SubGhzReceiverMenuItemArray_clear(model->history->data);
                free(model->history);
        },
        false);
    furi_timer_free(subghz_receiver->timer);
    view_free(subghz_receiver->view);
    free(subghz_receiver);
}

View* subghz_view_receiver_get_view(SubGhzViewReceiver* subghz_receiver) {
    furi_assert(subghz_receiver);
    return subghz_receiver->view;
}

uint16_t subghz_view_receiver_get_idx_menu(SubGhzViewReceiver* subghz_receiver) {
    furi_assert(subghz_receiver);
    uint16_t idx;
    with_view_model(
        subghz_receiver->view, SubGhzViewReceiverModel * model, { idx = model->idx; }, false);
    return idx;
}

void subghz_view_receiver_delete_item(SubGhzViewReceiver* subghz_receiver, uint16_t idx) {
    furi_assert(subghz_receiver);

    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            if(idx < SubGhzReceiverMenuItemArray_size(model->history->data)) {
                SubGhzReceiverMenuItem* item =
                    SubGhzReceiverMenuItemArray_get(model->history->data, idx);
                furi_string_free(item->item_str);
                furi_string_free(item->time);
                item->type = 0;
                SubGhzReceiverMenuItemArray_remove_v(model->history->data, idx, idx + 1);

                if(model->history_item == 5) {
                    if(model->idx >= 2) {
                        model->idx = model->history_item - 1;
                    }
                }
                model->history_item--;

                if(model->idx && (model->idx > idx || model->idx == model->history_item)) {
                    model->idx--;
                }
            }
        },
        true);
}

void subghz_view_receiver_enable_draw_callback(SubGhzViewReceiver* subghz_receiver) {
    furi_assert(subghz_receiver);

    with_view_model(
        subghz_receiver->view, SubGhzViewReceiverModel * model, { model->nodraw = false; }, true);
}

void subghz_view_receiver_disable_draw_callback(SubGhzViewReceiver* subghz_receiver) {
    furi_assert(subghz_receiver);

    with_view_model(
        subghz_receiver->view, SubGhzViewReceiverModel * model, { model->nodraw = true; }, true);
}

void subghz_view_receiver_set_idx_menu(SubGhzViewReceiver* subghz_receiver, uint16_t idx) {
    furi_assert(subghz_receiver);
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            model->idx = idx;
            if(model->idx > 2) model->list_offset = idx - 2;
        },
        true);
    subghz_view_receiver_update_offset(subghz_receiver);
}
