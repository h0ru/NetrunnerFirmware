#include "power_menu.h"
#include <furi.h>
#include <gui/elements.h>
#include <gui/icon.h>
#include <assets_icons.h>

#define POWER_MENU_COUNT 3

/* Option highlighted when the System panel opens. Index into power_menu_items:
 * 0 = Settings, 1 = Reboot (middle), 2 = Power Off. Change to
 * PowerMenuActionPowerOff to open on Power Off instead. */
#define POWER_MENU_DEFAULT_SELECTED PowerMenuActionReboot

/* Uniform framed cells so all three options share one look. Centers are spread
 * evenly across the 128px display; every box is the same size and the selected
 * one is drawn filled (inverted icon). */
static const uint8_t power_menu_cell_x[POWER_MENU_COUNT] = {24, 64, 104};
#define POWER_MENU_BOX_W   30
#define POWER_MENU_BOX_H   28
#define POWER_MENU_BOX_R   3

/* Vertical rhythm, balanced on the 64px screen (measured with the real fonts):
 *   picker:  4px top | title caps 4..11 | 6px | boxes 18..45 | 6px | label 52..58 | 5px
 *   confirm: 4px top | title caps 4..11 | 7px | icon 19..38  | 8px | buttons 47..60 | 3px
 * Previously the picker sat 1px from the top with 6px spare at the bottom, and the
 * confirm icon had 6px above it but 11px below. */
#define POWER_MENU_TITLE_Y         4
#define POWER_MENU_BOX_CY          32 /* vertical center of the box row */
#define POWER_MENU_LABEL_Y         52
#define POWER_MENU_CONFIRM_ICON_CY 29
#define POWER_MENU_BUTTON_Y        47
#define POWER_MENU_BUTTON_H        14

typedef struct {
    const char* name; /* label shown under the icon row */
    const char* confirm; /* header shown on the confirm prompt */
    const Icon* icon;
} PowerMenuItem;

static const PowerMenuItem power_menu_items[POWER_MENU_COUNT] = {
    {"Settings", "Settings?", &I_SystemSettings_20x20},
    {"Reboot", "Reboot?", &I_SystemReboot_20x20},
    {"Power Off", "Power Off?", &I_SystemPowerOff_20x20},
};

struct PowerMenu {
    View* view;
    PowerMenuCallback callback;
    void* context;
};

typedef struct {
    uint8_t selected;
    bool confirm;
    bool confirm_yes; /* true: Confirm highlighted, false: Cancel highlighted */
} PowerMenuModel;

static void power_menu_draw_icon_centered(Canvas* canvas, const Icon* icon, int cx, int cy) {
    int w = icon_get_width(icon);
    int h = icon_get_height(icon);
    canvas_draw_icon(canvas, cx - w / 2, cy - h / 2, icon);
}

static void power_menu_draw_button(
    Canvas* canvas,
    int x,
    int y,
    int w,
    int h,
    const char* text,
    bool selected) {
    if(selected) {
        canvas_draw_rbox(canvas, x, y, w, h, POWER_MENU_BOX_R);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_rframe(canvas, x, y, w, h, POWER_MENU_BOX_R);
    }
    canvas_draw_str_aligned(canvas, x + w / 2, y + h / 2, AlignCenter, AlignCenter, text);
    canvas_set_color(canvas, ColorBlack);
}

static void power_menu_draw_callback(Canvas* canvas, void* _model) {
    furi_assert(_model);
    PowerMenuModel* model = _model;
    const PowerMenuItem* item = &power_menu_items[model->selected];

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    if(!model->confirm) {
        /* Icon picker: three identically framed cells, the selected one filled.
         * Rotate (Up/Down) to move, press OK to open the confirm prompt. */
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(
            canvas, 64, POWER_MENU_TITLE_Y, AlignCenter, AlignTop, "System");

        for(uint8_t i = 0; i < POWER_MENU_COUNT; i++) {
            int bx = power_menu_cell_x[i] - POWER_MENU_BOX_W / 2;
            int by = POWER_MENU_BOX_CY - POWER_MENU_BOX_H / 2;
            bool sel = (i == model->selected);

            if(sel) {
                canvas_draw_rbox(canvas, bx, by, POWER_MENU_BOX_W, POWER_MENU_BOX_H, POWER_MENU_BOX_R);
                canvas_set_color(canvas, ColorWhite);
            } else {
                canvas_draw_rframe(
                    canvas, bx, by, POWER_MENU_BOX_W, POWER_MENU_BOX_H, POWER_MENU_BOX_R);
            }
            power_menu_draw_icon_centered(
                canvas, power_menu_items[i].icon, power_menu_cell_x[i], POWER_MENU_BOX_CY);
            canvas_set_color(canvas, ColorBlack);
        }

        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas, 64, POWER_MENU_LABEL_Y, AlignCenter, AlignTop, item->name);
    } else {
        /* Confirm prompt. Rotate to move between Cancel/Confirm, OK to activate,
         * Back to cancel. */
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(
            canvas, 64, POWER_MENU_TITLE_Y, AlignCenter, AlignTop, item->confirm);

        power_menu_draw_icon_centered(canvas, item->icon, 64, POWER_MENU_CONFIRM_ICON_CY);

        canvas_set_font(canvas, FontSecondary);
        power_menu_draw_button(
            canvas,
            6,
            POWER_MENU_BUTTON_Y,
            54,
            POWER_MENU_BUTTON_H,
            "Cancel",
            !model->confirm_yes);
        power_menu_draw_button(
            canvas,
            68,
            POWER_MENU_BUTTON_Y,
            54,
            POWER_MENU_BUTTON_H,
            "Confirm",
            model->confirm_yes);
    }
}

static bool power_menu_input_callback(InputEvent* event, void* context) {
    PowerMenu* power_menu = context;
    bool consumed = false;
    bool fire = false;
    PowerMenuAction action = PowerMenuActionSettings;

    PowerMenuModel* model = view_get_model(power_menu->view);
    if(event->type == InputTypeShort) {
        if(!model->confirm) {
            /* The T-Embed encoder emits Up/Down on rotation; Left/Right only
             * while the knob is held. Accept both so either works. */
            if(event->key == InputKeyUp || event->key == InputKeyLeft) {
                model->selected = (model->selected + POWER_MENU_COUNT - 1) % POWER_MENU_COUNT;
                consumed = true;
            } else if(event->key == InputKeyDown || event->key == InputKeyRight) {
                model->selected = (model->selected + 1) % POWER_MENU_COUNT;
                consumed = true;
            } else if(event->key == InputKeyOk) {
                model->confirm = true;
                model->confirm_yes = true; /* default highlight on Confirm */
                consumed = true;
            }
            /* Back is left unconsumed so the view dispatcher exits the app. */
        } else {
            if(event->key == InputKeyUp || event->key == InputKeyDown ||
               event->key == InputKeyLeft || event->key == InputKeyRight) {
                model->confirm_yes = !model->confirm_yes;
                consumed = true;
            } else if(event->key == InputKeyOk) {
                if(model->confirm_yes) {
                    action = (PowerMenuAction)model->selected;
                    fire = true;
                } else {
                    model->confirm = false; /* Cancel */
                }
                consumed = true;
            } else if(event->key == InputKeyBack) {
                model->confirm = false;
                consumed = true;
            }
        }
    }
    view_commit_model(power_menu->view, consumed);

    if(fire && power_menu->callback) {
        power_menu->callback(action, power_menu->context);
    }

    return consumed;
}

PowerMenu* power_menu_alloc(void) {
    PowerMenu* power_menu = malloc(sizeof(PowerMenu));

    power_menu->view = view_alloc();
    view_allocate_model(power_menu->view, ViewModelTypeLocking, sizeof(PowerMenuModel));
    view_set_context(power_menu->view, power_menu);
    view_set_draw_callback(power_menu->view, power_menu_draw_callback);
    view_set_input_callback(power_menu->view, power_menu_input_callback);

    return power_menu;
}

void power_menu_free(PowerMenu* power_menu) {
    furi_assert(power_menu);
    view_free(power_menu->view);
    free(power_menu);
}

View* power_menu_get_view(PowerMenu* power_menu) {
    furi_assert(power_menu);
    return power_menu->view;
}

void power_menu_set_callback(PowerMenu* power_menu, PowerMenuCallback callback, void* context) {
    furi_assert(power_menu);
    power_menu->callback = callback;
    power_menu->context = context;
}

void power_menu_reset(PowerMenu* power_menu) {
    furi_assert(power_menu);
    with_view_model(
        power_menu->view,
        PowerMenuModel * model,
        {
            model->selected = POWER_MENU_DEFAULT_SELECTED;
            model->confirm = false;
            model->confirm_yes = true;
        },
        true);
}
