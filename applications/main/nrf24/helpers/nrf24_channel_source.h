#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Unified jammer "channel source": every jam method reduces to producing a set
 * of NRF24 channels and then jamming it with the shared engine. The four
 * sources differ only in how that channel set is derived. */
typedef enum {
    Nrf24SourceProtocol = 0, /* curated protocol preset (BLE/BT/WiFi-band/…) */
    Nrf24SourceManual, /* user-picked start channel + span (manual frequency) */
    Nrf24SourceWifi, /* range derived from a scanned WiFi AP */
    Nrf24SourceActivity, /* top-N channels from an RPD activity scan */
    Nrf24SourceCount,
} Nrf24JamSource;

/* Manual source bounds: NRF24 channel 0..125 maps to 2400..2525 MHz. Span is
 * the number of contiguous channels jammed from the start channel upward. */
#define NRF24_MANUAL_CH_MAX   125
#define NRF24_MANUAL_SPAN_MAX 40

#define NRF24_ACTIVITY_TARGETS 8
#define NRF24_ACTIVITY_CH_MIN  0
#define NRF24_ACTIVITY_CH_MAX  79

/* Live selection + scan-cache state for the unified jammer. Lives in Nrf24App
 * so it survives the scan / config sub-scenes (which tear the jam scene down
 * and back up via the scene manager). */
typedef struct {
    uint8_t source; /* Nrf24JamSource */
    uint8_t protocol; /* Nrf24JamPreset — Protocol selection */
    uint8_t manual_ch; /* Manual: start channel 0..NRF24_MANUAL_CH_MAX */
    uint8_t manual_span; /* Manual: contiguous channel count 1..NRF24_MANUAL_SPAN_MAX */
    uint8_t wifi_index; /* index into Nrf24App.wifi_aps — WiFi selection */
    bool wifi_scanned; /* WiFi scan cached this session */
    bool activity_wide; /* Activity: false = Top-N, true = Wide sweep */
    bool activity_scanned; /* Activity scan cached this session */
    uint8_t activity_count; /* number of Top-N channels (0 = none → Wide) */
    uint8_t activity_ch[NRF24_ACTIVITY_TARGETS];
    uint16_t activity_hits[NRF24_ACTIVITY_TARGETS];
    bool resume_running; /* restore run-state after a scan/config sub-scene */
} Nrf24JamState;

/* Forward decl — the source helpers operate on the whole app (channel set,
 * WiFi AP list, jam state). */
typedef struct Nrf24App Nrf24App;

void nrf24_jam_state_init(Nrf24JamState* st);

/* "Protocol" / "Manual" / "WiFi" / "Activity". */
const char* nrf24_source_type_label(uint8_t source);

/* Human selection label for the current source, e.g. "BLE", "CH 50",
 * "MyAP", "Smart 8ch" / "Wide". */
void nrf24_source_selection_label(Nrf24App* app, char* buf, size_t cap);

/* Fill out[] with the channel set for the current source + selection.
 * Returns the channel count (0 if nothing to jam, e.g. WiFi with no AP). */
size_t nrf24_source_fill_channels(Nrf24App* app, uint8_t* out, size_t cap);


/* Up/Down (plain encoder rotation): walk a single flat list of every selectable
 * target — each Protocol preset individually, then the WiFi and Activity scan
 * sources, wrapping around. dir = +1 (next) / -1 (prev). This lets the user
 * reach every preset (e.g. "BT Classic 2") just by turning the encoder, without
 * the hold-and-turn gesture. */
void nrf24_source_step(Nrf24App* app, int dir);

/* True if the current source still needs a scan before it can produce
 * channels (WiFi without APs, Activity without a scan). */
bool nrf24_source_needs_scan(Nrf24App* app);
