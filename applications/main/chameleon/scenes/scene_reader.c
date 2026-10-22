#include "../chameleon_app.h"

/* Reader: pick the frequency to read. HF is the left button, LF the right
 * button; pressing one sets the mode and goes to the Reading screen. */

enum { ReaderPickHf, ReaderPickLf };

static void chameleon_reader_button_cb(GuiButtonType type, InputType input, void* context) {
    ChameleonApp* app = context;
    if(input != InputTypeShort) return;
    if(type == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, ReaderPickHf);
    } else if(type == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(app->view_dispatcher, ReaderPickLf);
    }
}

void chameleon_app_scene_reader_on_enter(void* context) {
    ChameleonApp* app = context;
    Widget* w = app->widget;
    widget_reset(w);
    widget_add_string_element(w, 64, 8, AlignCenter, AlignTop, FontPrimary, "Reader");
    widget_add_string_element(
        w, 64, 30, AlignCenter, AlignTop, FontSecondary, "Choose the frequency");
    widget_add_string_element(w, 2, 44, AlignLeft, AlignTop, FontSecondary, "13.56MHz");
    widget_add_string_element(w, 110, 44, AlignCenter, AlignTop, FontSecondary, "125kHz");
    widget_add_button_element(w, GuiButtonTypeLeft, "HF", chameleon_reader_button_cb, app);
    widget_add_button_element(w, GuiButtonTypeRight, "LF", chameleon_reader_button_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, ChameleonViewWidget);
}

bool chameleon_app_scene_reader_on_event(void* context, SceneManagerEvent event) {
    ChameleonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == ReaderPickHf || event.event == ReaderPickLf) {
            app->read_lf = (event.event == ReaderPickLf);
            scene_manager_next_scene(app->scene_manager, ChameleonAppSceneRead);
            consumed = true;
        } else if(event.event == ChameleonCustomEventBleDisconnected) {
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, ChameleonAppSceneConnect);
            consumed = true;
        }
    }
    return consumed;
}

void chameleon_app_scene_reader_on_exit(void* context) {
    ChameleonApp* app = context;
    widget_reset(app->widget);
}
