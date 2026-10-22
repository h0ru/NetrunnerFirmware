#include "nrf24_channel_source.h"
#include "nrf24_jam_presets.h"
#include "../nrf24_app.h"

#include <stdio.h>
#include <string.h>

void nrf24_jam_state_init(Nrf24JamState* st) {
    memset(st, 0, sizeof(*st));
    st->source = Nrf24SourceProtocol;
    /* Default to BT Classic (nRF ch 2..80 = the whole 2402–2480 MHz BT/BLE band).
     * Combined with the CW default strategy this is Bruce's "Bluetooth" mode: a
     * continuous carrier swept lucklessly across the entire band, which is what
     * actually disrupts real devices. The narrow BLE-Adv preset (3 channels) is
     * still selectable but only covers 1/3 of each advertising channel over time. */
    st->protocol = Nrf24JamPresetBluetooth;
    /* Manual defaults to a single channel mid-band (ch 45 = 2445 MHz). */
    st->manual_ch = 45;
    st->manual_span = 1;
    /* wifi/activity start unscanned; everything else zeroed. */
}

const char* nrf24_source_type_label(uint8_t source) {
    switch(source) {
    case Nrf24SourceProtocol:
        return "Protocol";
    case Nrf24SourceManual:
        return "Manual";
    case Nrf24SourceWifi:
        return "WiFi";
    case Nrf24SourceActivity:
        return "Activity";
    default:
        return "?";
    }
}

/* WiFi channel center frequency in MHz (2.4 GHz band). */
static uint16_t wifi_channel_center_mhz(uint8_t wifi_ch) {
    if(wifi_ch == 14) return 2484;
    if(wifi_ch < 1) wifi_ch = 1;
    if(wifi_ch > 13) wifi_ch = 13;
    return (uint16_t)(2412 + (wifi_ch - 1) * 5);
}

static uint8_t wifi_selected_channel(Nrf24App* app) {
    if(app->wifi_ap_count == 0 || app->wifi_aps == NULL) return 0;
    uint8_t idx = app->jam.wifi_index;
    if(idx >= app->wifi_ap_count) idx = 0;
    return app->wifi_aps[idx].primary;
}

void nrf24_source_selection_label(Nrf24App* app, char* buf, size_t cap) {
    Nrf24JamState* st = &app->jam;
    switch(st->source) {
    case Nrf24SourceProtocol:
        snprintf(buf, cap, "%s", nrf24_jam_preset_short((Nrf24JamPreset)st->protocol));
        break;
    case Nrf24SourceManual:
        if(st->manual_span <= 1) {
            snprintf(buf, cap, "CH%u %uMHz", st->manual_ch, 2400u + st->manual_ch);
        } else {
            uint8_t end = st->manual_ch + st->manual_span - 1;
            if(end > NRF24_MANUAL_CH_MAX) end = NRF24_MANUAL_CH_MAX;
            snprintf(buf, cap, "CH%u-%u", st->manual_ch, end);
        }
        break;
    case Nrf24SourceWifi:
        if(app->wifi_ap_count == 0) {
            if(cap) buf[0] = '\0';
        } else {
            uint8_t idx = st->wifi_index < app->wifi_ap_count ? st->wifi_index : 0;
            const char* ssid = (const char*)app->wifi_aps[idx].ssid;
            if(ssid[0] == '\0') ssid = "<hidden>";
            /* Cap at the width that fits next to the strategy badge. */
            snprintf(buf, cap, "%.14s", ssid);
        }
        break;
    case Nrf24SourceActivity:
        if(!st->activity_scanned) {
            if(cap) buf[0] = '\0';
        } else if(st->activity_wide || st->activity_count == 0) {
            snprintf(buf, cap, "Wide");
        } else {
            snprintf(buf, cap, "Smart %uch", st->activity_count);
        }
        break;
    default:
        snprintf(buf, cap, "?");
        break;
    }
}

size_t nrf24_source_fill_channels(Nrf24App* app, uint8_t* out, size_t cap) {
    if(out == NULL || cap == 0) return 0;
    Nrf24JamState* st = &app->jam;

    switch(st->source) {
    case Nrf24SourceProtocol:
        return nrf24_jam_preset_fill_channels((Nrf24JamPreset)st->protocol, out, cap);

    case Nrf24SourceManual: {
        uint8_t span = st->manual_span ? st->manual_span : 1;
        size_t n = 0;
        for(uint8_t i = 0; i < span; i++) {
            uint16_t ch = (uint16_t)st->manual_ch + i;
            if(ch > NRF24_MANUAL_CH_MAX || n >= cap) break;
            out[n++] = (uint8_t)ch;
        }
        return n;
    }

    case Nrf24SourceWifi: {
        if(app->wifi_ap_count == 0) return 0;
        uint16_t center = wifi_channel_center_mhz(wifi_selected_channel(app));
        int min_mhz = (int)center - 10;
        int max_mhz = (int)center + 10;
        if(min_mhz < 2400) min_mhz = 2400;
        if(max_mhz > 2524) max_mhz = 2524;
        uint8_t nrf_min = (uint8_t)(min_mhz - 2400);
        uint8_t nrf_max = (uint8_t)(max_mhz - 2400);
        size_t n = 0;
        for(uint8_t ch = nrf_min; ch <= nrf_max && n < cap; ch++) {
            out[n++] = ch;
        }
        return n;
    }

    case Nrf24SourceActivity:
        if(st->activity_wide || st->activity_count == 0) {
            /* Wide sweep over the scanned band. */
            size_t n = 0;
            for(uint8_t ch = NRF24_ACTIVITY_CH_MIN; ch <= NRF24_ACTIVITY_CH_MAX && n < cap; ch++) {
                out[n++] = ch;
            }
            return n;
        } else {
            size_t n = st->activity_count;
            if(n > cap) n = cap;
            memcpy(out, st->activity_ch, n);
            return n;
        }

    default:
        return 0;
    }
}


void nrf24_source_step(Nrf24App* app, int dir) {
    Nrf24JamState* st = &app->jam;

    /* One flat list: [Protocol preset 0 .. N-1] → WiFi → Activity → wrap.
     * Stepping past the last preset moves into the WiFi/Activity scan sources;
     * stepping back before preset 0 wraps around to Activity. */
    /* Flat order: [Protocol 0..N-1] → Manual → WiFi → Activity → wrap. */
    if(dir > 0) {
        switch(st->source) {
        case Nrf24SourceProtocol:
            if(st->protocol + 1 < Nrf24JamPresetCount) {
                st->protocol++;
            } else {
                st->source = Nrf24SourceManual;
            }
            break;
        case Nrf24SourceManual:
            st->source = Nrf24SourceWifi;
            break;
        case Nrf24SourceWifi:
            st->source = Nrf24SourceActivity;
            break;
        default: /* Activity → back to first preset */
            st->source = Nrf24SourceProtocol;
            st->protocol = 0;
            break;
        }
    } else {
        switch(st->source) {
        case Nrf24SourceProtocol:
            if(st->protocol > 0) {
                st->protocol--;
            } else {
                st->source = Nrf24SourceActivity;
            }
            break;
        case Nrf24SourceManual:
            st->source = Nrf24SourceProtocol;
            st->protocol = (uint8_t)(Nrf24JamPresetCount - 1);
            break;
        case Nrf24SourceActivity:
            st->source = Nrf24SourceWifi;
            break;
        default: /* WiFi → back to Manual */
            st->source = Nrf24SourceManual;
            break;
        }
    }
}

bool nrf24_source_needs_scan(Nrf24App* app) {
    Nrf24JamState* st = &app->jam;
    if(st->source == Nrf24SourceWifi) return !st->wifi_scanned;
    if(st->source == Nrf24SourceActivity) return !st->activity_scanned;
    return false;
}
