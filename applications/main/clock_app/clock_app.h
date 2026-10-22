#pragma once

#include <input/input.h>

#define TAG "Clock"

typedef enum {
    ClockMsgTick,
    ClockMsgInput,
} ClockMsgType;

typedef struct {
    ClockMsgType type;
    InputEvent input;
} ClockMsg;

/* Which screen is showing. On first launch (clock never set) the chooser is
 * shown; once the time is valid the clock face is the default. */
typedef enum {
    ClockScreenChooser,
    ClockScreenWifiWait,
    ClockScreenManual,
    ClockScreenClock,
} ClockScreen;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ClockScreen screen;
    uint8_t chooser_index; /* 0 = WiFi, 1 = manual */
    uint8_t manual_field; /* 0 = hours, 1 = minutes, 2 = seconds */
    DateTime manual_dt;
    uint32_t wifi_deadline_tick; /* give up waiting for NTP at this tick */
    bool wifi_failed; /* show the "no time" hint after a timeout */
} ClockState;
