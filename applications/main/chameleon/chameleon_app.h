#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/popup.h>
#include <notification/notification.h>

#include "helpers/chameleon_ble.h"
#include "scenes/scenes.h"

typedef enum {
    ChameleonViewSubmenu,
    ChameleonViewWidget,
    ChameleonViewPopup,
} ChameleonView;

/* Custom view-dispatcher events. BLE events are offset well above any submenu
 * index so the two never collide. */
typedef enum {
    ChameleonCustomEventBleConnected = 1000,
    ChameleonCustomEventBleConnectFailed,
    ChameleonCustomEventBleDisconnected,
    ChameleonCustomEventBleResponse,
    ChameleonCustomEventRetry,
    ChameleonCustomEventSave,
    ChameleonCustomEventSaveDone,
    ChameleonCustomEventWrite,
    ChameleonCustomEventWriteGo,
} ChameleonCustomEvent;

typedef struct ChameleonApp {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Widget* widget;
    Popup* popup;
    NotificationApp* notifications;

    ChameleonBle* ble;

    bool ble_started; /* the BLE worker has been started at least once */

    /* Cached device state. */
    int8_t active_slot; /* 0..7, -1 unknown */
    uint16_t slot_hf_type[8]; /* per-slot HF tag type (0 = empty) */
    uint16_t slot_lf_type[8]; /* per-slot LF tag type (0 = empty) */
    bool slot_info_valid; /* slot_*_type caches have been read */
    int8_t selected_slot; /* slot the user picked */
    bool read_lf; /* the Read scene operates on LF (true) or HF (false) */

    /* Last successful read, used by the Save-to-slot scene. */
    bool read_valid;
    bool read_was_lf;
    uint8_t read_uid[10];
    uint8_t read_uidlen;
    uint8_t read_atqa[2];
    uint8_t read_sak;
    uint8_t read_lfid[5];
} ChameleonApp;

/* Run the Wipe LED override while scanning/connecting (reuses the per-app
 * WS2812 override, as the IR Learn and NFC read screens do). */
void chameleon_led_scan_start(ChameleonApp* app);
void chameleon_led_reset(ChameleonApp* app);

/* Start a scan/connect attempt. The first call starts the worker; later calls
 * tear the previous attempt down (handing the radio back) and start fresh. Used
 * by the Connect scene on enter, on Retry, and after a disconnect. */
void chameleon_app_start_connect(ChameleonApp* app);
