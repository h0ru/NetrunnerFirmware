/**
 * @file ble_detector_embed.h
 * @brief BLE Detector embedded as a native part of the Bluetooth app.
 *
 * The detector used to be a separate app; launching it from the Bluetooth menu
 * meant an app-to-app switch that flashed the main menu in between. Here the
 * same scanning/parsing logic is hosted directly by the Bluetooth app's own
 * view_dispatcher, so it opens as a plain scene with no app switch and no flash.
 */
#pragma once

#include <gui/view.h>
#include <gui/modules/submenu.h>

typedef struct BleDetector BleDetector;

/* Custom events the results view raises on the hosting view_dispatcher. Chosen
 * above the other BleSpam custom-event ranges (100/200) to avoid clashes. */
typedef enum {
    BleDetectorEventUp = 300,
    BleDetectorEventDown,
    BleDetectorEventToggle,
} BleDetectorEvent;

BleDetector* ble_detector_alloc(void);
void ble_detector_free(BleDetector* d);

/* The results View, owned by the detector. The host adds it to its
 * view_dispatcher and sets its context to that dispatcher (so the view's input
 * callback can raise BleDetectorEvent_* on it). */
View* ble_detector_get_view(BleDetector* d);

/* Fill a submenu with the scan-kind entries (index = DetectorKind). */
void ble_detector_build_menu(Submenu* submenu, SubmenuItemCallback cb, void* ctx);

/* Begin scanning for the chosen kind. Brings up the radio on first use.
 * Returns false if the radio could not be started. */
bool ble_detector_start(BleDetector* d, uint32_t kind);
/* Stop scanning and release the radio, restoring the previous Bt stack. */
void ble_detector_stop(BleDetector* d);

/* Periodic refresh (call from the scene Tick). */
void ble_detector_tick(BleDetector* d);

/* Results navigation (call from the scene's custom-event handler). */
void ble_detector_nav_up(BleDetector* d);
void ble_detector_nav_down(BleDetector* d);
void ble_detector_toggle(BleDetector* d);
