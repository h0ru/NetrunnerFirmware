#include "../infrared_app_i.h"
#include <dolphin/dolphin.h>

void infrared_scene_learn_on_enter(void* context) {
    InfraredApp* infrared = context;
    Widget* widget = infrared->widget;
    InfraredWorker* worker = infrared->worker;

    infrared_worker_rx_set_received_signal_callback(
        worker, infrared_signal_received_callback, context);
    infrared_worker_rx_start(worker);
    infrared_play_notification_message(infrared, InfraredNotificationMessageBlinkStartRead);
    infrared_play_notification_message(infrared, InfraredNotificationMessageLedLearn);

    /* Capture screen in the NFC module's visual language: the animated IR scan
     * GIF centered up top, the action word below it, and a hint at the bottom.
     * Replaces the old full-width I_InfraredLearnShort_128x31 illustration. */
    widget_reset(widget);
    widget_add_icon_animation_element(widget, 50, 2, &A_Infrared_scan_28x28);
    widget_add_string_element(widget, 64, 36, AlignCenter, AlignTop, FontPrimary, "Learning");
    widget_add_string_element(
        widget, 64, 60, AlignCenter, AlignBottom, FontSecondary, "Point remote at IR port");

    view_dispatcher_switch_to_view(infrared->view_dispatcher, InfraredViewWidget);
}

bool infrared_scene_learn_on_event(void* context, SceneManagerEvent event) {
    InfraredApp* infrared = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InfraredCustomEventTypeSignalReceived) {
            /* The worker also forwards undecoded bursts (noise, partial presses,
             * unknown protocols) on its receive timeout. In the normal decoded
             * Learn mode those are unsuccessful attempts, not a real capture, so
             * ignore them and keep scanning. Only a decoded signal -- or, in the
             * explicit RAW mode, any capture -- is an effective result that
             * stops the scan and advances to the Save/Continue screen. */
            const bool is_effective_result = infrared->app_state.is_learning_raw ||
                                             !infrared_signal_is_raw(infrared->current_signal);
            if(is_effective_result) {
                infrared_play_notification_message(infrared, InfraredNotificationMessageSuccess);
                scene_manager_next_scene(infrared->scene_manager, InfraredSceneLearnSuccess);
                dolphin_deed(DolphinDeedIrLearnSuccess);
            }
            consumed = true;
        }
    }

    return consumed;
}

void infrared_scene_learn_on_exit(void* context) {
    InfraredApp* infrared = context;
    infrared_worker_rx_set_received_signal_callback(infrared->worker, NULL, NULL);
    infrared_worker_rx_stop(infrared->worker);
    infrared_play_notification_message(infrared, InfraredNotificationMessageBlinkStop);
    infrared_play_notification_message(infrared, InfraredNotificationMessageLedReset);
    widget_reset(infrared->widget);
}
