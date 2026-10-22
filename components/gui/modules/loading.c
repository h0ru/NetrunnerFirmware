#include "loading.h"

#include "../icon_animation.h"
#include "../icon_animation_i.h"
#include "../icon_i.h"
#include "../elements.h"
#include "../canvas.h"
#include "../canvas_i.h"
#include "../view.h"
#include <input.h>

#include <furi.h>
#include <assets_icons.h>
#include <stdint.h>

/* App menu icons are 14x14 (FAP icons 10x10); doubled they read as a loading
 * animation at roughly the size of the 24x24 hourglass they replace. */
#define LOADING_APP_ICON_SCALE 2

/* Set by the loader around each app launch; see loading_set_app_icon(). */
static const Icon* volatile loading_app_icon = NULL;

struct Loading {
    View* view;
};

typedef struct {
    IconAnimation* icon;
    const Icon* override_icon;
    bool active;
} LoadingModel;

void loading_set_app_icon(const Icon* icon) {
    loading_app_icon = icon;
}

const Icon* loading_get_app_icon(void) {
    return loading_app_icon;
}

static void loading_draw_frame_scaled(
    Canvas* canvas,
    int32_t cx,
    int32_t cy,
    uint16_t width,
    uint16_t height,
    const uint8_t* frame_data) {
    uint8_t* bitmap = NULL;
    compress_icon_decode(canvas->compress_icon, frame_data, &bitmap);

    const int32_t scale = LOADING_APP_ICON_SCALE;
    const int32_t x0 = cx - (width * scale) / 2;
    const int32_t y0 = cy - (height * scale) / 2;
    const size_t row_bytes = (width + 7) / 8;

    for(uint16_t y = 0; y < height; y++) {
        for(uint16_t x = 0; x < width; x++) {
            if(bitmap[y * row_bytes + x / 8] & (1 << (x % 8))) {
                canvas_draw_box(canvas, x0 + x * scale, y0 + y * scale, scale, scale);
            }
        }
    }
}

void loading_draw_indicator(Canvas* canvas, int32_t cx, int32_t cy) {
    furi_check(canvas);
    const Icon* icon = loading_app_icon;

    if(!icon) {
        canvas_draw_icon(canvas, cx - 24 / 2, cy - 24 / 2, &A_Loading_24);
        return;
    }

    /* No IconAnimation here: the caller redraws on its own schedule, so pick
     * the frame from the clock and the icon animates whenever it is redrawn. */
    uint32_t frame = 0;
    if(icon->frame_count > 1 && icon->frame_rate) {
        frame = (furi_get_tick() * icon->frame_rate / furi_kernel_get_tick_frequency()) %
                icon->frame_count;
    }
    loading_draw_frame_scaled(canvas, cx, cy, icon->width, icon->height, icon->frames[frame]);
}

static void loading_draw_callback(Canvas* canvas, void* _model) {
    LoadingModel* model = (LoadingModel*)_model;

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 0, 0, canvas_width(canvas), canvas_height(canvas));
    canvas_set_color(canvas, ColorBlack);

    const IconAnimation* animation = model->icon;
    if(animation->icon != &A_Loading_24) {
        loading_draw_frame_scaled(
            canvas,
            canvas_width(canvas) / 2,
            canvas_height(canvas) / 2,
            animation->icon->width,
            animation->icon->height,
            icon_animation_get_data(animation));
        return;
    }

    uint8_t x = canvas_width(canvas) / 2 - 24 / 2;
    uint8_t y = canvas_height(canvas) / 2 - 24 / 2;

    canvas_draw_icon(canvas, x, y, &A_Loading_24);

    canvas_draw_icon_animation(canvas, x, y, model->icon);
}

static bool loading_input_callback(InputEvent* event, void* context) {
    UNUSED(event);
    furi_assert(context);
    return true;
}

/* Swap the animation to the icon this view should show now. Called with the
 * model locked. */
static void loading_apply_icon(Loading* instance, LoadingModel* model) {
    const Icon* wanted = model->override_icon;
    if(!wanted) wanted = loading_app_icon;
    if(!wanted) wanted = &A_Loading_24;

    if(model->icon->icon != wanted) {
        icon_animation_free(model->icon);
        model->icon = icon_animation_alloc(wanted);
    }

    /* using Loading View in conjunction with several
     * Stack View obligates to reassign
     * Update callback, as it can be rewritten
     */
    view_tie_icon_animation(instance->view, model->icon);
    if(model->active) icon_animation_start(model->icon);
}

static void loading_enter_callback(void* context) {
    furi_assert(context);
    Loading* instance = context;
    LoadingModel* model = view_get_model(instance->view);
    model->active = true;
    loading_apply_icon(instance, model);
    view_commit_model(instance->view, false);
}

static void loading_exit_callback(void* context) {
    furi_assert(context);
    Loading* instance = context;
    LoadingModel* model = view_get_model(instance->view);
    model->active = false;
    icon_animation_stop(model->icon);
    view_commit_model(instance->view, false);
}

Loading* loading_alloc(void) {
    Loading* instance = malloc(sizeof(Loading));
    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(LoadingModel));
    LoadingModel* model = view_get_model(instance->view);
    model->icon = icon_animation_alloc(&A_Loading_24);
    model->override_icon = NULL;
    model->active = false;
    view_tie_icon_animation(instance->view, model->icon);
    view_commit_model(instance->view, false);

    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, loading_draw_callback);
    view_set_input_callback(instance->view, loading_input_callback);
    view_set_enter_callback(instance->view, loading_enter_callback);
    view_set_exit_callback(instance->view, loading_exit_callback);

    return instance;
}

void loading_free(Loading* instance) {
    furi_check(instance);

    LoadingModel* model = view_get_model(instance->view);
    icon_animation_free(model->icon);
    view_commit_model(instance->view, false);

    furi_assert(instance);
    view_free(instance->view);
    free(instance);
}

View* loading_get_view(Loading* instance) {
    furi_check(instance);
    return instance->view;
}

void loading_set_icon(Loading* instance, const Icon* icon) {
    furi_check(instance);
    LoadingModel* model = view_get_model(instance->view);
    model->override_icon = icon;
    loading_apply_icon(instance, model);
    view_commit_model(instance->view, true);
}
