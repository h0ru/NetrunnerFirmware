#include "ble_detector_embed.h"
#include "ble_detector_parse.h"

#include <furi.h>
#include <btshim.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <nimble_glue.h>
#include <string.h>
#include <stdio.h>

#define DETECTOR_RESULTS_CHUNK 32
#define DETECTOR_RAW_CHUNK 96
#define DETECTOR_COUNT_MAX 0xfffeu

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    uint32_t last_seen;
    uint32_t kinds;
    char name[32];
} DetectorRecord;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    uint32_t last_seen;
    uint8_t adv[31];
    uint8_t adv_len;
    uint8_t scan_rsp[31];
    uint8_t scan_rsp_len;
} DetectorRawDevice;

struct BleDetector {
    View* results_view;
    bool radio_ready;
    bool bt_was_enabled;
    bool scanning;
    DetectorKind filter;
    uint16_t selected;
    DetectorRecord* records[DetectorKindCount];
    uint16_t counts[DetectorKindCount];
    uint16_t caps[DetectorKindCount];
    DetectorRawDevice* raw;
    uint16_t raw_count;
    uint16_t raw_cap;
    FuriMutex* raw_mutex;
};

/* Grow a scan list in place on the internal heap. On failure the existing
 * buffer is kept and the caller simply stops recording new devices. */
static bool
    detector_grow(void** buffer, uint16_t* cap, uint16_t needed, size_t item, uint16_t chunk) {
    if(needed <= *cap) return true;
    uint32_t next = *cap ? (uint32_t)*cap * 2u : chunk;
    while(next < needed) next *= 2u;
    if(next > DETECTOR_COUNT_MAX) next = DETECTOR_COUNT_MAX;
    if(needed > next) return false;
    const size_t bytes = (size_t)next * item;
    void* grown = realloc(*buffer, bytes);
    if(!grown) return false;
    memset((uint8_t*)grown + (size_t)*cap * item, 0, bytes - (size_t)*cap * item);
    *buffer = grown;
    *cap = (uint16_t)next;
    return true;
}

static int detector_gap_event(struct ble_gap_event* event, void* context) {
    BleDetector* d = context;
    if(event->type == BLE_GAP_EVENT_DISC) {
        const struct ble_gap_disc_desc* disc = &event->disc;
        uint8_t display_addr[6];
        for(size_t i = 0; i < 6; ++i) display_addr[i] = disc->addr.val[5 - i];
        int found = -1;
        furi_mutex_acquire(d->raw_mutex, FuriWaitForever);
        for(uint16_t i = 0; i < d->raw_count; ++i) {
            if(!memcmp(d->raw[i].addr, display_addr, 6) &&
               d->raw[i].addr_type == disc->addr.type) {
                found = (int)i;
                break;
            }
        }
        if(found < 0) {
            if(!detector_grow(
                   (void**)&d->raw,
                   &d->raw_cap,
                   d->raw_count + 1,
                   sizeof(DetectorRawDevice),
                   DETECTOR_RAW_CHUNK)) {
                furi_mutex_release(d->raw_mutex);
                return 0;
            }
            found = d->raw_count++;
            memset(&d->raw[found], 0, sizeof(DetectorRawDevice));
            memcpy(d->raw[found].addr, display_addr, 6);
            d->raw[found].addr_type = disc->addr.type;
        }
        DetectorRawDevice* raw = &d->raw[found];
        raw->rssi = disc->rssi;
        raw->last_seen = furi_get_tick();
        uint8_t length = disc->length_data > 31 ? 31 : disc->length_data;
        if(disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
            raw->scan_rsp_len = length;
            memcpy(raw->scan_rsp, disc->data, length);
        } else {
            raw->adv_len = length;
            memcpy(raw->adv, disc->data, length);
        }
        furi_mutex_release(d->raw_mutex);
    } else if(event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        d->scanning = false;
    }
    return 0;
}

static bool detector_radio_start(BleDetector* d) {
    Bt* bt = furi_record_open(RECORD_BT);
    d->bt_was_enabled = bt_is_enabled(bt);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    furi_delay_ms(50);
    if(nimble_glue_init("BLE Detector") != ESP_OK) {
        if(d->bt_was_enabled) {
            Bt* restore = furi_record_open(RECORD_BT);
            bt_start_stack(restore);
            furi_record_close(RECORD_BT);
        }
        return false;
    }
    nimble_glue_configure_security(false, false, false, BLE_HS_IO_NO_INPUT_OUTPUT);
    if(nimble_glue_start(NULL, NULL) != ESP_OK) {
        nimble_glue_stop();
        if(d->bt_was_enabled) {
            Bt* restore = furi_record_open(RECORD_BT);
            bt_start_stack(restore);
            furi_record_close(RECORD_BT);
        }
        return false;
    }
    furi_mutex_acquire(d->raw_mutex, FuriWaitForever);
    if(d->raw && d->raw_cap) memset(d->raw, 0, (size_t)d->raw_cap * sizeof(DetectorRawDevice));
    d->raw_count = 0;
    furi_mutex_release(d->raw_mutex);
    return true;
}

static bool detector_scan_start(BleDetector* d) {
    struct ble_gap_disc_params params = {
        .passive = 0,
        .itvl = 0x50,
        .window = 0x30,
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(
        nimble_glue_own_address_type(), BLE_HS_FOREVER, &params, detector_gap_event, d);
    if(rc != 0) return false;
    d->scanning = true;
    return true;
}

static void detector_scan_stop(BleDetector* d) {
    if(!d->scanning) return;
    ble_gap_disc_cancel();
    for(uint8_t i = 0; i < 20 && d->scanning; ++i) furi_delay_ms(10);
    d->scanning = false;
}

static void detector_radio_stop(BleDetector* d) {
    detector_scan_stop(d);
    if(d->radio_ready) {
        nimble_glue_stop();
        if(d->bt_was_enabled) {
            Bt* bt = furi_record_open(RECORD_BT);
            bt_start_stack(bt);
            furi_record_close(RECORD_BT);
        }
    }
    d->radio_ready = false;
    d->bt_was_enabled = false;
}

static const char* detector_distance(int8_t rssi) {
    if(rssi >= -45) return "VERY CLOSE";
    if(rssi >= -60) return "NEAR";
    if(rssi >= -75) return "NEARBY";
    return "FAR";
}

static bool detector_parse_device(DetectorRawDevice* device, DetectorMatch* match) {
    DetectorMatch first = {0};
    DetectorMatch second = {0};
    if(device->adv_len && !ble_detector_parse(device->adv, device->adv_len, &first)) {
        memset(&first, 0, sizeof(first));
    }
    if(device->scan_rsp_len &&
       !ble_detector_parse(device->scan_rsp, device->scan_rsp_len, &second)) {
        memset(&second, 0, sizeof(second));
    }
    memset(match, 0, sizeof(*match));
    match->kinds = first.kinds | second.kinds;
    match->signatures = first.signatures | second.signatures;
    if(first.name[0])
        strncpy(match->name, first.name, sizeof(match->name) - 1);
    else if(second.name[0])
        strncpy(match->name, second.name, sizeof(match->name) - 1);
    return match->kinds != 0;
}

static void detector_store(BleDetector* d, DetectorKind kind, DetectorRawDevice* device, DetectorMatch* match) {
    uint16_t count = d->counts[kind];
    int found = -1;
    for(uint16_t i = 0; i < count; ++i) {
        if(!memcmp(d->records[kind][i].addr, device->addr, 6)) {
            found = (int)i;
            break;
        }
    }
    if(found < 0) {
        if(!detector_grow(
               (void**)&d->records[kind],
               &d->caps[kind],
               count + 1,
               sizeof(DetectorRecord),
               DETECTOR_RESULTS_CHUNK))
            return;
        found = count++;
        memset(&d->records[kind][found], 0, sizeof(DetectorRecord));
        memcpy(d->records[kind][found].addr, device->addr, 6);
        d->records[kind][found].addr_type = device->addr_type;
    }
    DetectorRecord* record = &d->records[kind][found];
    record->rssi = device->rssi;
    record->last_seen = device->last_seen;
    record->kinds = match->kinds;
    if(match->name[0]) strncpy(record->name, match->name, sizeof(record->name) - 1);
    d->counts[kind] = count;
}

static void detector_sync_results(BleDetector* d) {
    if(!d->radio_ready || !d->scanning) return;
    furi_mutex_acquire(d->raw_mutex, FuriWaitForever);
    for(uint16_t i = 0; i < d->raw_count; ++i) {
        DetectorMatch match;
        if(!detector_parse_device(&d->raw[i], &match)) continue;
        for(DetectorKind kind = DetectorNetrunner; kind < DetectorKindCount; ++kind) {
            if(match.kinds & DETECTOR_BIT(kind)) detector_store(d, kind, &d->raw[i], &match);
        }
    }
    furi_mutex_release(d->raw_mutex);
    /* All Scan: one row per detected category. */
    for(DetectorKind kind = DetectorNetrunner; kind < DetectorKindCount; ++kind) {
        uint16_t source_count = d->counts[kind];
        for(uint16_t i = 0; i < source_count; ++i) {
            DetectorRecord* source = &d->records[kind][i];
            uint16_t all_count = d->counts[DetectorAll];
            int found = -1;
            for(uint16_t j = 0; j < all_count; ++j) {
                DetectorRecord* row = &d->records[DetectorAll][j];
                if(row->kinds == DETECTOR_BIT(kind) && !memcmp(row->addr, source->addr, 6)) {
                    found = (int)j;
                    break;
                }
            }
            if(found < 0 &&
               detector_grow(
                   (void**)&d->records[DetectorAll],
                   &d->caps[DetectorAll],
                   all_count + 1,
                   sizeof(DetectorRecord),
                   DETECTOR_RESULTS_CHUNK)) {
                found = all_count++;
                memset(&d->records[DetectorAll][found], 0, sizeof(DetectorRecord));
                memcpy(d->records[DetectorAll][found].addr, source->addr, 6);
                d->records[DetectorAll][found].addr_type = source->addr_type;
            }
            if(found >= 0) {
                DetectorRecord* row = &d->records[DetectorAll][found];
                row->rssi = source->rssi;
                row->last_seen = source->last_seen;
                row->kinds = DETECTOR_BIT(kind);
                strncpy(row->name, source->name, sizeof(row->name) - 1);
            }
            d->counts[DetectorAll] = all_count;
        }
    }
}

static DetectorRecord* detector_selected(BleDetector* d) {
    uint16_t count = d->counts[d->filter];
    return count && d->selected < count ? &d->records[d->filter][d->selected] : NULL;
}

static const char* detector_record_label(BleDetector* d, DetectorRecord* record) {
    if(d->filter != DetectorAll) return detector_labels[d->filter];
    for(DetectorKind kind = DetectorNetrunner; kind < DetectorKindCount; ++kind) {
        if(record->kinds == DETECTOR_BIT(kind)) return detector_labels[kind];
    }
    return "Detected device";
}

static void detector_draw(Canvas* canvas, void* model) {
    BleDetector* d = *(BleDetector**)model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    char line[48];
    snprintf(line, sizeof(line), "THE FOUND: %u", d->counts[d->filter]);
    canvas_draw_str(canvas, 1, 9, line);
    canvas_set_font(canvas, FontSecondary);
    DetectorRecord* record = detector_selected(d);
    canvas_draw_str(
        canvas, 1, 19,
        record ? detector_record_label(d, record) :
                 (d->filter == DetectorAll ? "All Scan" : detector_labels[d->filter]));
    if(record) {
        snprintf(
            line, sizeof(line), "%u/%u %s", d->selected + 1, d->counts[d->filter],
            detector_distance(record->rssi));
        canvas_draw_str(canvas, 1, 29, line);
        snprintf(
            line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X", record->addr[0], record->addr[1],
            record->addr[2], record->addr[3], record->addr[4], record->addr[5]);
        canvas_draw_str(canvas, 1, 40, line);
        snprintf(
            line, sizeof(line), "%d dBm  %lus ago", record->rssi,
            (unsigned long)((furi_get_tick() - record->last_seen) / 1000));
        canvas_draw_str(canvas, 1, 50, line);
        if(record->name[0]) canvas_draw_str(canvas, 1, 60, record->name);
    } else {
        canvas_draw_str(canvas, 1, 35, d->scanning ? "Scanning..." : "Paused");
        canvas_draw_str(canvas, 1, 52, "OK resume  Up/Down browse");
    }
}

static bool detector_input(InputEvent* event, void* context) {
    /* Context is the hosting view_dispatcher (set by the Bluetooth app). */
    ViewDispatcher* vd = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    if(event->key == InputKeyUp || event->key == InputKeyDown || event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(
            vd,
            event->key == InputKeyUp    ? BleDetectorEventUp :
            event->key == InputKeyDown  ? BleDetectorEventDown :
                                          BleDetectorEventToggle);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

BleDetector* ble_detector_alloc(void) {
    BleDetector* d = calloc(1, sizeof(BleDetector));
    furi_check(d);
    d->raw_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_check(d->raw_mutex);
    d->filter = DetectorAll;

    d->results_view = view_alloc();
    view_allocate_model(d->results_view, ViewModelTypeLockFree, sizeof(BleDetector*));
    *(BleDetector**)view_get_model(d->results_view) = d;
    view_commit_model(d->results_view, false);
    view_set_draw_callback(d->results_view, detector_draw);
    view_set_input_callback(d->results_view, detector_input);
    return d;
}

void ble_detector_free(BleDetector* d) {
    if(!d) return;
    detector_radio_stop(d);
    view_free(d->results_view);
    for(DetectorKind kind = DetectorAll; kind < DetectorKindCount; ++kind) free(d->records[kind]);
    free(d->raw);
    furi_mutex_free(d->raw_mutex);
    free(d);
}

View* ble_detector_get_view(BleDetector* d) {
    return d->results_view;
}

void ble_detector_build_menu(Submenu* submenu, SubmenuItemCallback cb, void* ctx) {
    for(DetectorKind kind = DetectorAll; kind < DetectorKindCount; ++kind)
        submenu_add_item(submenu, detector_labels[kind], kind, cb, ctx);
}

bool ble_detector_start(BleDetector* d, uint32_t kind) {
    d->filter = (DetectorKind)kind;
    d->selected = 0;
    if(!d->radio_ready) d->radio_ready = detector_radio_start(d);
    if(d->radio_ready && !d->scanning) detector_scan_start(d);
    return d->radio_ready;
}

void ble_detector_stop(BleDetector* d) {
    detector_radio_stop(d);
}

void ble_detector_tick(BleDetector* d) {
    detector_sync_results(d);
    uint16_t count = d->counts[d->filter];
    if(count && d->selected >= count) d->selected = count - 1;
    view_commit_model(d->results_view, true);
}

void ble_detector_nav_up(BleDetector* d) {
    uint16_t count = d->counts[d->filter];
    if(count) d->selected = d->selected ? d->selected - 1 : count - 1;
    view_commit_model(d->results_view, true);
}

void ble_detector_nav_down(BleDetector* d) {
    uint16_t count = d->counts[d->filter];
    if(count) d->selected = (d->selected + 1) % count;
    view_commit_model(d->results_view, true);
}

void ble_detector_toggle(BleDetector* d) {
    if(d->scanning) {
        detector_scan_stop(d);
    } else if(d->radio_ready) {
        detector_scan_start(d);
    }
    view_commit_model(d->results_view, true);
}
