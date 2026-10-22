#include "widget_element_i.h"
#include "../../icon_animation.h"
#include "../../canvas.h"

/* Animated counterpart of widget_element_icon: draws a multi-frame Icon that
 * advances on its own timer, so a widget-based screen can show a GIF (e.g. the
 * NFC scan animation) alongside the usual string/button elements. The timer is
 * tied to the widget's view by widget_add_icon_animation_element() so each frame
 * change triggers a redraw. */

typedef struct {
    uint8_t x;
    uint8_t y;
    IconAnimation* icon_animation;
} GuiIconAnimationModel;

static void gui_icon_animation_draw(Canvas* canvas, WidgetElement* element) {
    furi_assert(canvas);
    furi_assert(element);
    GuiIconAnimationModel* model = element->model;

    if(model->icon_animation) {
        canvas_draw_icon_animation(canvas, model->x, model->y, model->icon_animation);
    }
}

static void gui_icon_animation_free(WidgetElement* element) {
    furi_assert(element);
    GuiIconAnimationModel* model = element->model;

    icon_animation_stop(model->icon_animation);
    icon_animation_free(model->icon_animation);
    free(model);
    free(element);
}

WidgetElement* widget_element_icon_animation_create(uint8_t x, uint8_t y, const Icon* icon) {
    furi_assert(icon);

    GuiIconAnimationModel* model = malloc(sizeof(GuiIconAnimationModel));
    model->x = x;
    model->y = y;
    model->icon_animation = icon_animation_alloc(icon);

    WidgetElement* element = malloc(sizeof(WidgetElement));
    element->parent = NULL;
    element->input = NULL;
    element->draw = gui_icon_animation_draw;
    element->free = gui_icon_animation_free;
    element->model = model;

    return element;
}

IconAnimation* widget_element_icon_animation_get_animation(WidgetElement* element) {
    furi_assert(element);
    GuiIconAnimationModel* model = element->model;
    return model->icon_animation;
}
