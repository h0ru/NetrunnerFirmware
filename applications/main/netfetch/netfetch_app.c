/**
 * @file netfetch_app.c
 * @brief "Netfetch" — a fastfetch-style system info screen for Netrunner.
 *
 * Replaces the Power Profiler with a single static snapshot of the things that
 * actually matter at a glance: device, firmware, CPU, RAM, internal disk, SD
 * card and battery (with the live draw in mA from the BQ27220 fuel gauge).
 *
 * Static by design: everything is sampled once on entry so the screen is a
 * stable "card" you can read, photograph or compare. Press Back to exit, or OK
 * to re-sample.
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_power.h>
#include <furi_hal_version.h>

#include <gui/gui.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <toolbox/version.h>

#define TAG "Netfetch"

#define NF_ROWS      7
#define NF_VAL_LEN   28
#define NF_X_ICON    1
#define NF_X_LABEL   11
#define NF_X_VALUE   40
#define NF_Y_FIRST   9 /* baseline of the first row */
#define NF_Y_PITCH   8

typedef enum {
    NfIconDev,
    NfIconOs,
    NfIconCpu,
    NfIconMem,
    NfIconSd,
    NfIconBat,
    NfIconUp,
} NfIcon;

typedef struct {
    NfIcon icon;
    const char* label;
    char value[NF_VAL_LEN];
} NfRow;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;
    NfRow rows[NF_ROWS];
} Netfetch;

/* ------------------------------------------------------------------ */
/* Formatting helpers                                                  */
/* ------------------------------------------------------------------ */

/* Short human size: "7.0G" / "4.3M" / "512K" / "0". */
static void nf_fmt_size(uint64_t b, char* out, size_t n) {
    if(b >= (1024ULL * 1024 * 1024)) {
        snprintf(out, n, "%.1fG", (double)b / (1024.0 * 1024.0 * 1024.0));
    } else if(b >= (1024ULL * 1024)) {
        snprintf(out, n, "%.1fM", (double)b / (1024.0 * 1024.0));
    } else if(b >= 1024ULL) {
        snprintf(out, n, "%.0fK", (double)b / 1024.0);
    } else {
        snprintf(out, n, "%lluB", (unsigned long long)b);
    }
}

/* "used/total pct%" into dst; total==0 -> fallback. */
static void nf_fmt_usage(uint64_t total, uint64_t used, const char* empty, char* dst, size_t n) {
    if(total == 0) {
        snprintf(dst, n, "%s", empty);
        return;
    }
    char u[12], t[12];
    nf_fmt_size(used, u, sizeof(u));
    nf_fmt_size(total, t, sizeof(t));
    unsigned pct = (unsigned)((used * 100ULL) / total);
    snprintf(dst, n, "%s/%s %u%%", u, t, pct);
}

/* ------------------------------------------------------------------ */
/* Data gathering (static snapshot)                                    */
/* ------------------------------------------------------------------ */

static void netfetch_sample(Netfetch* app) {
    /* Fixed rows / labels */
    app->rows[0].icon = NfIconDev;
    app->rows[0].label = "DEV";
    app->rows[1].icon = NfIconOs;
    app->rows[1].label = "OS";
    app->rows[2].icon = NfIconCpu;
    app->rows[2].label = "CPU";
    app->rows[3].icon = NfIconMem;
    app->rows[3].label = "MEM";
    app->rows[4].icon = NfIconSd;
    app->rows[4].label = "SD";
    app->rows[5].icon = NfIconBat;
    app->rows[5].label = "BAT";
    app->rows[6].icon = NfIconUp;
    app->rows[6].label = "UP";

    /* DEV + CPU: board constants (single-board firmware). */
    snprintf(app->rows[0].value, NF_VAL_LEN, "T-Embed CC1101");
    snprintf(app->rows[2].value, NF_VAL_LEN, "ESP32-S3 240M");

    /* OS: firmware version. */
    const Version* v = version_get();
    snprintf(
        app->rows[1].value, NF_VAL_LEN, "Netrunner %s", v ? version_get_version(v) : "?");

    /* MEM: FURI heap used/total. */
    size_t heap_total = memmgr_get_total_heap();
    size_t heap_free = memmgr_get_free_heap();
    size_t heap_used = (heap_total > heap_free) ? (heap_total - heap_free) : 0;
    nf_fmt_usage(heap_total, heap_used, "n/a", app->rows[3].value, NF_VAL_LEN);

    /* SD: the removable card (/ext) — the device's real user storage. On this
     * port the "internal" path "/int" is only a folder on this same SD card
     * (storage remaps it), so there is no separate internal filesystem to show
     * as a distinct disk — the SD is the disk. */
    Storage* storage = furi_record_open(RECORD_STORAGE);
    uint64_t ex_total = 0, ex_free = 0;
    storage_common_fs_info(storage, "/ext", &ex_total, &ex_free);
    furi_record_close(RECORD_STORAGE);

    nf_fmt_usage(
        ex_total, (ex_total > ex_free) ? ex_total - ex_free : 0, "no card", app->rows[4].value,
        NF_VAL_LEN);

    /* BAT: percent + live draw (mA). Negative current = discharging. */
    int pct = furi_hal_power_get_pct();
    int ma = (int)(furi_hal_power_get_battery_current(FuriHalPowerICFuelGauge) * 1000.0f);
    if(furi_hal_power_is_charging()) {
        snprintf(app->rows[5].value, NF_VAL_LEN, "%d%% chg %dmA", pct, ma < 0 ? -ma : ma);
    } else {
        snprintf(app->rows[5].value, NF_VAL_LEN, "%d%% %dmA", pct, ma);
    }

    /* UP: uptime since boot. */
    uint32_t up = furi_get_tick() / furi_kernel_get_tick_frequency();
    if(up >= 86400) {
        snprintf(
            app->rows[6].value, NF_VAL_LEN, "%lud %luh", (unsigned long)(up / 86400),
            (unsigned long)((up % 86400) / 3600));
    } else {
        snprintf(
            app->rows[6].value, NF_VAL_LEN, "%luh %lum", (unsigned long)(up / 3600),
            (unsigned long)((up % 3600) / 60));
    }
}

/* ------------------------------------------------------------------ */
/* Row icons (drawn in code, 7x7, top-left origin)                     */
/* ------------------------------------------------------------------ */

static void nf_draw_icon(Canvas* c, NfIcon icon, int x, int y) {
    switch(icon) {
    case NfIconDev: /* monitor */
        canvas_draw_frame(c, x, y, 7, 5);
        canvas_draw_line(c, x + 3, y + 5, x + 3, y + 5);
        canvas_draw_line(c, x + 2, y + 6, x + 4, y + 6);
        break;
    case NfIconOs: /* document */
        canvas_draw_frame(c, x, y, 6, 7);
        canvas_draw_line(c, x + 2, y + 2, x + 3, y + 2);
        canvas_draw_line(c, x + 2, y + 4, x + 3, y + 4);
        break;
    case NfIconCpu: /* chip */
        canvas_draw_frame(c, x + 1, y + 1, 5, 5);
        canvas_draw_dot(c, x, y + 2);
        canvas_draw_dot(c, x, y + 4);
        canvas_draw_dot(c, x + 6, y + 2);
        canvas_draw_dot(c, x + 6, y + 4);
        canvas_draw_dot(c, x + 2, y);
        canvas_draw_dot(c, x + 4, y);
        canvas_draw_dot(c, x + 2, y + 6);
        canvas_draw_dot(c, x + 4, y + 6);
        break;
    case NfIconMem: /* RAM stick */
        canvas_draw_frame(c, x, y + 1, 7, 5);
        canvas_draw_dot(c, x + 2, y);
        canvas_draw_dot(c, x + 4, y);
        canvas_draw_line(c, x + 2, y + 2, x + 2, y + 4);
        canvas_draw_line(c, x + 4, y + 2, x + 4, y + 4);
        break;
    case NfIconUp: /* clock */
        canvas_draw_circle(c, x + 3, y + 3, 3);
        canvas_draw_line(c, x + 3, y + 3, x + 3, y + 1);
        canvas_draw_line(c, x + 3, y + 3, x + 5, y + 3);
        break;
    case NfIconSd: /* memory card */
        canvas_draw_frame(c, x, y, 6, 7);
        canvas_draw_line(c, x + 4, y, x + 5, y + 1);
        canvas_draw_line(c, x + 1, y + 2, x + 1, y + 3);
        canvas_draw_line(c, x + 3, y + 2, x + 3, y + 3);
        break;
    case NfIconBat: /* battery */
        canvas_draw_frame(c, x, y + 1, 6, 5);
        canvas_draw_box(c, x + 6, y + 2, 1, 3);
        canvas_draw_box(c, x + 1, y + 2, 3, 3);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void netfetch_draw_callback(Canvas* canvas, void* ctx) {
    Netfetch* app = ctx;
    if(furi_mutex_acquire(app->mutex, 100) != FuriStatusOk) return;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    for(size_t i = 0; i < NF_ROWS; i++) {
        int y = NF_Y_FIRST + (int)i * NF_Y_PITCH; /* text baseline */
        nf_draw_icon(canvas, app->rows[i].icon, NF_X_ICON, y - 7);
        canvas_draw_str(canvas, NF_X_LABEL, y, app->rows[i].label);
        canvas_draw_str(canvas, NF_X_VALUE, y, app->rows[i].value);
    }

    furi_mutex_release(app->mutex);
}

static void netfetch_input_callback(InputEvent* input_event, void* ctx) {
    Netfetch* app = ctx;
    furi_message_queue_put(app->queue, input_event, FuriWaitForever);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int32_t netfetch_app(void* p) {
    UNUSED(p);

    Netfetch* app = malloc(sizeof(Netfetch));
    memset(app, 0, sizeof(Netfetch));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    netfetch_sample(app);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, netfetch_draw_callback, app);
    view_port_input_callback_set(app->view_port, netfetch_input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }
        if(event.type != InputTypeShort && event.type != InputTypeLong) continue;

        if(event.key == InputKeyBack) {
            running = false;
        } else if(event.key == InputKeyOk) {
            /* Re-sample on demand (battery/RAM move). */
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            netfetch_sample(app);
            furi_mutex_release(app->mutex);
            view_port_update(app->view_port);
        }
    }

    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
