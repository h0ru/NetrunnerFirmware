#include "nrf24_jam_view.h"
#include "../helpers/nrf24_jam_config.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <assets_icons.h>
#include <stdio.h>

/* Spectrum-analyzer band: one bar per active channel across the 2400–2525 MHz
 * axis. Idle → short stubs (shows the target set at a glance). Running → lively
 * bars whose heights animate off hop_count, with the channel currently being
 * swept drawn full-height, so it reads instantly as "on air". */
static void nrf24_jam_draw_spectrum(Canvas* canvas, const Nrf24JamModel* model) {
    const int ax = 2, aw = 124; /* axis x / width */
    const int base = 51; /* baseline y */
    const int top = 35; /* tallest a bar may reach */
    const int maxh = base - top; /* 16 px of headroom */

    canvas_draw_line(canvas, ax, base, ax + aw - 1, base); /* baseline axis */

    for(uint8_t i = 0; i < model->band_count; i++) {
        uint8_t c = model->channels[i];
        if(c > 125) continue;
        int x = ax + 1 + (c * (aw - 2)) / 125;
        int h;
        if(model->running) {
            if(c == model->channel) {
                h = maxh; /* the live, currently-swept channel */
            } else {
                uint8_t r = (uint8_t)(c * 37u + model->hop_count * 13u);
                h = 4 + (r % (maxh - 3)); /* animated activity */
            }
        } else {
            h = 3; /* idle stub */
        }
        canvas_draw_line(canvas, x, base - 1, x, base - h);
    }
}

static void nrf24_jam_draw_callback(Canvas* canvas, void* _model) {
    Nrf24JamModel* model = _model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    if(!model->hardware_ok) {
        canvas_draw_icon(canvas, 60, 18, &I_Quest_7x8);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "NRF24 not found");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignCenter, "Check the module");
        return;
    }

    char line[40];

    /* ── Header (y0): attack mode · power, right-aligned ── */
    canvas_set_font(canvas, FontSecondary);
    snprintf(
        line, sizeof(line), "%s/%s", nrf24_jam_strategy_label(model->strategy),
        nrf24_jam_pa_label(model->pa_level));
    canvas_draw_str_aligned(canvas, 127, 1, AlignRight, AlignTop, line);

    /* ── Title (y0, left): what we're aimed at ── */
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 0, 0, AlignLeft, AlignTop, model->selection_label);

    /* ── State chip (y13, right): the single most important readout ── */
    const char* state = model->show_scan ? "SCAN" : (!model->can_jam ? "IDLE" : (model->running ? "JAMMING" : "READY"));
    canvas_set_font(canvas, FontSecondary);
    int cw = canvas_string_width(canvas, state) + 8;
    int cx = 127 - cw;
    if(model->running) {
        canvas_draw_box(canvas, cx, 13, cw, 12);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, cx + cw / 2, 15, AlignCenter, AlignTop, state);
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_rframe(canvas, cx, 13, cw, 12, 2);
        canvas_draw_str_aligned(canvas, cx + cw / 2, 15, AlignCenter, AlignTop, state);
    }

    /* ── Frequency readout (y15, left): the actual span on air ── */
    if(model->show_scan) {
        canvas_draw_str_aligned(canvas, 0, 15, AlignLeft, AlignTop, "Press Scan");
    } else if(!model->can_jam) {
        canvas_draw_str_aligned(canvas, 0, 15, AlignLeft, AlignTop, "No target");
    } else if(model->band_count <= 3) {
        int n = 0;
        for(uint8_t i = 0; i < model->band_count; i++) {
            n += snprintf(
                line + n, (int)sizeof(line) - n, (i ? " %u" : "%u"), 2400u + model->channels[i]);
        }
        snprintf(line + n, (int)sizeof(line) - n, " MHz");
        canvas_draw_str_aligned(canvas, 0, 15, AlignLeft, AlignTop, line);
    } else {
        uint16_t lo = 255, hi = 0;
        for(uint8_t i = 0; i < model->band_count; i++) {
            if(model->channels[i] < lo) lo = model->channels[i];
            if(model->channels[i] > hi) hi = model->channels[i];
        }
        snprintf(line, sizeof(line), "%u-%u MHz", 2400u + lo, 2400u + hi);
        canvas_draw_str_aligned(canvas, 0, 15, AlignLeft, AlignTop, line);
    }

    /* ── Spectrum band (y35–51) ── */
    if(model->show_scan) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignTop, "Scan for nearby targets");
    } else if(!model->can_jam) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignTop, "Pick a target with < >");
    } else {
        nrf24_jam_draw_spectrum(canvas, model);
    }

    /* ── Buttons: < Prev | Run/Stop/Scan | Next > ── */
    elements_button_left(canvas, "Prev");
    elements_button_right(canvas, "Next");
    elements_button_center(
        canvas, model->show_scan ? "Scan" : (model->running ? "Stop" : "Run"));
}

static bool nrf24_jam_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;

    switch(event->key) {
    case InputKeyOk:
        /* Short and long are mutually exclusive in the input stack (a long press
         * never also emits a short), so these two actions never both fire. */
        if(event->type == InputTypeShort) {
            view_dispatcher_send_custom_event(vd, Nrf24JamEventToggle);
            return true;
        }
        if(event->type == InputTypeLong) {
            view_dispatcher_send_custom_event(vd, Nrf24JamEventConfig);
            return true;
        }
        return false;
    case InputKeyLeft:
    case InputKeyUp:
        /* Previous preset / source (Bruce's Prev). */
        if(event->type != InputTypeShort) return false;
        view_dispatcher_send_custom_event(vd, Nrf24JamEventSourcePrev);
        return true;
    case InputKeyRight:
    case InputKeyDown:
        /* Next preset / source (Bruce's Next). */
        if(event->type != InputTypeShort) return false;
        view_dispatcher_send_custom_event(vd, Nrf24JamEventSourceNext);
        return true;
    default:
        return false;
    }
}

View* nrf24_jam_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(Nrf24JamModel));
    view_set_draw_callback(view, nrf24_jam_draw_callback);
    view_set_input_callback(view, nrf24_jam_input_callback);
    return view;
}

void nrf24_jam_view_free(View* view) {
    view_free(view);
}
