#include "chameleon_app.h"
#include <notification/notification_app.h> /* LedEffect */
#include <stdlib.h>

/* Wipe LED (red, Normal speed) while scanning/connecting or reading. */
static const NotificationMessage message_chameleon_led_scan = {
    .type = NotificationMessageTypeLedEffectOverride,
    .data.led_effect = {.effect = LedEffectWipe,
                        .speed_tick_ms = 33,
                        .has_color = true,
                        .color = 0xFF0000}, /* red */
};
static const NotificationSequence sequence_chameleon_led_scan = {
    &message_chameleon_led_scan,
    NULL};

static const NotificationMessage message_chameleon_led_reset = {
    .type = NotificationMessageTypeLedEffectOverrideReset,
};
static const NotificationSequence sequence_chameleon_led_reset = {
    &message_chameleon_led_reset,
    NULL};

void chameleon_led_scan_start(ChameleonApp* app) {
    notification_message(app->notifications, &sequence_chameleon_led_scan);
}

void chameleon_led_reset(ChameleonApp* app) {
    notification_message(app->notifications, &sequence_chameleon_led_reset);
}

/* BLE worker -> UI: map transport events to view-dispatcher custom events so
 * all UI work happens on the view_dispatcher thread. */
static void chameleon_ble_event_cb(ChameleonBleEventType type, void* context) {
    ChameleonApp* app = context;
    uint32_t event;
    switch(type) {
    case ChameleonBleEventConnected:
        event = ChameleonCustomEventBleConnected;
        break;
    case ChameleonBleEventConnectFailed:
        event = ChameleonCustomEventBleConnectFailed;
        break;
    case ChameleonBleEventDisconnected:
        event = ChameleonCustomEventBleDisconnected;
        break;
    case ChameleonBleEventResponse:
        event = ChameleonCustomEventBleResponse;
        break;
    default:
        return;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, event);
}

void chameleon_app_start_connect(ChameleonApp* app) {
    if(app->ble_started) {
        /* Previous attempt: tear it down (restores the bt stack) and realloc. */
        chameleon_ble_free(app->ble);
        app->ble = chameleon_ble_alloc();
        chameleon_ble_set_event_callback(app->ble, chameleon_ble_event_cb, app);
    }
    app->active_slot = -1;
    app->selected_slot = -1;
    app->slot_info_valid = false;
    app->read_valid = false;
    chameleon_ble_connect(app->ble);
    app->ble_started = true;
}

static bool chameleon_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    ChameleonApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool chameleon_app_back_event_callback(void* context) {
    furi_assert(context);
    ChameleonApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void chameleon_app_tick_event_callback(void* context) {
    furi_assert(context);
    ChameleonApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static ChameleonApp* chameleon_app_alloc(void) {
    ChameleonApp* app = malloc(sizeof(ChameleonApp));
    memset(app, 0, sizeof(ChameleonApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->active_slot = -1;
    app->selected_slot = -1;
    app->slot_info_valid = false;

    app->scene_manager = scene_manager_alloc(&chameleon_app_scene_handlers, app);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, chameleon_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, chameleon_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, chameleon_app_tick_event_callback, 100);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ChameleonViewSubmenu, submenu_get_view(app->submenu));
    app->widget = widget_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ChameleonViewWidget, widget_get_view(app->widget));
    app->popup = popup_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ChameleonViewPopup, popup_get_view(app->popup));

    app->ble = chameleon_ble_alloc();
    chameleon_ble_set_event_callback(app->ble, chameleon_ble_event_cb, app);

    return app;
}

static void chameleon_app_free(ChameleonApp* app) {
    /* Stop BLE first: this disconnects and hands the radio back to the bt
     * service before we tear down the UI. */
    chameleon_ble_free(app->ble);

    view_dispatcher_remove_view(app->view_dispatcher, ChameleonViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, ChameleonViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, ChameleonViewPopup);
    submenu_free(app->submenu);
    widget_free(app->widget);
    popup_free(app->popup);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    chameleon_led_reset(app);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t chameleon_app(void* args) {
    UNUSED(args);
    ChameleonApp* app = chameleon_app_alloc();
    scene_manager_next_scene(app->scene_manager, ChameleonAppSceneConnect);
    view_dispatcher_run(app->view_dispatcher);
    chameleon_app_free(app);
    return 0;
}
