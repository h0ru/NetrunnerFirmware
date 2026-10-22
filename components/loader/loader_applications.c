#include "loader.h"
#include "loader_applications.h"
#include <dialogs/dialogs.h>
#include <assets_icons.h>
#include <gui/gui.h>
#include <gui/view_holder.h>
#include <gui/modules/loading.h>
#include <gui/icon_animation.h>
#include <canvas_i.h>
#include <storage/storage.h>
#include <toolbox/path.h>
#include <dolphin/dolphin.h>
#include <esp_rom_sys.h>
#include <esp_heap_caps.h>
#include <netrunner_application/netrunner_application.h>

#define TAG "LoaderApplications"

/* How long a FAP may take to load before the hourglass replaces the frozen
 * frame. Most load faster, so they open with no intermediate screen at all. */
#define LOADER_APPLICATIONS_SPINNER_DELAY_MS 500

struct LoaderApplications {
    FuriThread* thread;
    void (*closed_cb)(void*);
    void* context;
};

static void loader_applications_trace(const char* step) {
    esp_rom_printf(
        "\r\n[LA] %s free=%u\r\n",
        step,
        (unsigned)furi_thread_get_stack_space(furi_thread_get_current_id()));
}

static int32_t loader_applications_thread(void* p);

LoaderApplications* loader_applications_alloc(void (*closed_cb)(void*), void* context) {
    LoaderApplications* loader_applications = malloc(sizeof(LoaderApplications));
    loader_applications->thread =
        furi_thread_alloc_ex_psram(TAG, 4096, loader_applications_thread, (void*)loader_applications);
    loader_applications->closed_cb = closed_cb;
    loader_applications->context = context;
    furi_thread_start(loader_applications->thread);
    return loader_applications;
}

void loader_applications_free(LoaderApplications* loader_applications) {
    furi_assert(loader_applications);
    furi_thread_join(loader_applications->thread);
    furi_thread_free(loader_applications->thread);
    free(loader_applications);
}

#define APP_METADATA_CACHE_SIZE 64

typedef struct {
    FuriString* path;
    NetrunnerApplicationManifest manifest;
    bool valid;
} AppMetadata;

typedef struct {
    FuriString* file_path;
    DialogsApp* dialogs;
    Loader* loader;
    Storage* storage;
    AppMetadata* metadata;
    size_t metadata_next;

    Gui* gui;
    ViewHolder* view_holder;

    /* Covers the screen between the file browser closing and the app drawing
     * its first frame. A full-screen Loading view used to do this, which
     * flashed white for every FAP even when it loaded in a moment. This view
     * instead repeats the last frame that reached the display, so the browser
     * stays put until the app replaces it. Only the GUI thread touches
     * snapshot: it is written by the commit callback and read by draw. */
    View* cover;
    uint8_t* snapshot;
    size_t snapshot_size;
    bool has_snapshot;
    IconAnimation* spinner;
    FuriTimer* spinner_timer;
} LoaderApplicationsApp;

typedef struct {
    LoaderApplicationsApp* app;
    bool show_spinner;
} LoaderApplicationsCoverModel;

static void loader_applications_snapshot_callback(
    uint8_t* data,
    size_t size,
    CanvasOrientation orientation,
    void* context) {
    UNUSED(orientation);
    LoaderApplicationsApp* app = context;
    if(size != app->snapshot_size) return;
    memcpy(app->snapshot, data, size);
    app->has_snapshot = true;
}

static void loader_applications_cover_draw(Canvas* canvas, void* model) {
    LoaderApplicationsCoverModel* cover = model;
    LoaderApplicationsApp* app = cover->app;

    if(!cover->show_spinner && app->has_snapshot &&
       canvas_get_buffer_size(canvas) == app->snapshot_size) {
        memcpy(canvas_get_buffer(canvas), app->snapshot, app->snapshot_size);
        return;
    }

    /* Same look as the Loading module. */
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 0, 0, canvas_width(canvas), canvas_height(canvas));
    canvas_set_color(canvas, ColorBlack);
    const int32_t x = canvas_width(canvas) / 2 - 24 / 2;
    const int32_t y = canvas_height(canvas) / 2 - 24 / 2;
    canvas_draw_icon(canvas, x, y, &A_Loading_24);
    if(cover->show_spinner) canvas_draw_icon_animation(canvas, x, y, app->spinner);
}

static bool loader_applications_cover_input(InputEvent* event, void* context) {
    UNUSED(event);
    UNUSED(context);
    return true;
}

static void loader_applications_set_spinner(LoaderApplicationsApp* app, bool show) {
    with_view_model(
        app->cover, LoaderApplicationsCoverModel * model, { model->show_spinner = show; }, true);
}

static void loader_applications_spinner_timer_callback(void* context) {
    loader_applications_set_spinner(context, true);
}

static LoaderApplicationsApp* loader_applications_app_alloc(void) {
    LoaderApplicationsApp* app = malloc(sizeof(LoaderApplicationsApp)); //-V799
    app->file_path = furi_string_alloc_set(EXT_PATH("apps"));
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->loader = furi_record_open(RECORD_LOADER);
    app->storage = furi_record_open(RECORD_STORAGE);
    /* Bounded, session-only cache. Leave DMA RAM available if PSRAM is full. */
    app->metadata = heap_caps_calloc(
        APP_METADATA_CACHE_SIZE, sizeof(AppMetadata), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_holder = view_holder_alloc();

    app->snapshot_size = gui_get_framebuffer_size(app->gui);
    app->snapshot = malloc(app->snapshot_size);
    app->has_snapshot = false;
    app->spinner = icon_animation_alloc(&A_Loading_24);
    app->spinner_timer = furi_timer_alloc(
        loader_applications_spinner_timer_callback, FuriTimerTypeOnce, app);

    app->cover = view_alloc();
    view_allocate_model(app->cover, ViewModelTypeLocking, sizeof(LoaderApplicationsCoverModel));
    with_view_model(
        app->cover,
        LoaderApplicationsCoverModel * model,
        {
            model->app = app;
            model->show_spinner = false;
        },
        false);
    view_tie_icon_animation(app->cover, app->spinner);
    view_set_draw_callback(app->cover, loader_applications_cover_draw);
    view_set_input_callback(app->cover, loader_applications_cover_input);

    gui_add_framebuffer_callback(app->gui, loader_applications_snapshot_callback, app);
    view_holder_attach_to_gui(app->view_holder, app->gui);

    return app;
} //-V773

static void loader_applications_app_free(LoaderApplicationsApp* app) {
    furi_assert(app);

    view_holder_free(app->view_holder);
    /* Removing the callback takes the canvas lock that commits run under, so
     * no snapshot write can still be in flight once this returns. */
    gui_remove_framebuffer_callback(app->gui, loader_applications_snapshot_callback, app);
    furi_timer_free(app->spinner_timer);
    icon_animation_stop(app->spinner);
    view_free(app->cover);
    icon_animation_free(app->spinner);
    free(app->snapshot);
    furi_record_close(RECORD_GUI);

    furi_record_close(RECORD_LOADER);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    if(app->metadata) {
        for(size_t i = 0; i < APP_METADATA_CACHE_SIZE; i++) {
            if(app->metadata[i].path) furi_string_free(app->metadata[i].path);
        }
        free(app->metadata);
    }
    furi_string_free(app->file_path);
    free(app);
}

static bool loader_applications_item_callback(
    FuriString* path, void* context, uint8_t** icon_ptr, FuriString* item_name) {
    LoaderApplicationsApp* app = context;
    if(!furi_string_end_withi(path, ".fap")) return false;

    AppMetadata uncached = {0};
    AppMetadata* metadata = &uncached;
    if(app->metadata) {
        for(size_t i = 0; i < APP_METADATA_CACHE_SIZE; i++) {
            AppMetadata* cached = &app->metadata[i];
            if(cached->path && furi_string_equal(cached->path, path)) {
                metadata = cached;
                break;
            }
        }
    }
    if(metadata == &uncached) {
        if(app->metadata) {
            metadata = &app->metadata[app->metadata_next++ % APP_METADATA_CACHE_SIZE];
            if(!metadata->path) metadata->path = furi_string_alloc();
            furi_string_set(metadata->path, path);
        }
        metadata->valid = false;
        NetrunnerApplication* fap = netrunner_application_alloc(app->storage, NULL);
        if(fap) {
            if(netrunner_application_preload_manifest(fap, furi_string_get_cstr(path)) ==
               NetrunnerApplicationPreloadStatusSuccess) {
                metadata->manifest = *netrunner_application_get_manifest(fap);
                metadata->valid = true;
            }
            netrunner_application_free(fap);
        }
    }
    if(!metadata->valid) return false;
    furi_string_set_strn(
        item_name, metadata->manifest.name,
        strnlen(metadata->manifest.name, FAP_MANIFEST_MAX_APP_NAME_LENGTH));
    if(!metadata->manifest.has_icon || !*icon_ptr) return false;
    memcpy(*icon_ptr, metadata->manifest.icon, FAP_MANIFEST_MAX_ICON_SIZE);
    return true;
}

static bool loader_applications_select_app(LoaderApplicationsApp* loader_applications_app) {
    loader_applications_trace("select_begin");
    const DialogsFileBrowserOptions browser_options = {
        .extension = ".fap|.js",
        .skip_assets = true,
        .icon = &I_file_10px,
        .hide_ext = true,
        .item_loader_callback = loader_applications_item_callback,
        .item_loader_context = loader_applications_app,
        .base_path = EXT_PATH("apps"),
    };

    bool result = dialog_file_browser_show(
        loader_applications_app->dialogs,
        loader_applications_app->file_path,
        loader_applications_app->file_path,
        &browser_options);
    loader_applications_trace(result ? "select_done_true" : "select_done_false");
    return result;
}

// TODO: aufrufen, sobald JS-Apps im Browser auswaehlbar sind (Hook fehlt noch) -> bewusst ungenutzt
__attribute__((unused)) static void
    loader_applications_show_js_not_supported(LoaderApplicationsApp* app) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "JS not supported", 64, 3, AlignCenter, AlignTop);
    dialog_message_set_text(
        message, "JS apps are visible,\nbut cannot run yet.", 64, 32, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, "Back", NULL, NULL);
    dialog_message_show(app->dialogs, message);
    dialog_message_free(message);
}

#define APPLICATION_STOP_EVENT 1

static void loader_pubsub_callback(const void* message, void* context) {
    const LoaderEvent* event = message;
    const FuriThreadId thread_id = (FuriThreadId)context;

    if(event->type == LoaderEventTypeNoMoreAppsInQueue) {
        furi_thread_flags_set(thread_id, APPLICATION_STOP_EVENT);
    }
}

static void
    loader_applications_start_app(LoaderApplicationsApp* app, const char* name, const char* args) {
    loader_applications_trace("start_app_begin");

    /* External apps earn XP here rather than awarding it themselves -- the deed
     * weights for PluginStart/GameStart/GameWin are 0 for exactly that reason.
     * Games and media are excluded: only tools count as using the device.
     *
     * NOTE this must live in components/loader, the copy that is actually built.
     * The same call in applications/services/loader/ is dead code, which is why
     * launching a FAP awarded nothing at all before this. */
    if(!furi_string_start_with_str(app->file_path, EXT_PATH("apps/Games/")) &&
       !furi_string_start_with_str(app->file_path, EXT_PATH("apps/Media/"))) {
        dolphin_deed(DolphinDeedPluginInternalStart);
    }
    // load app
    FuriThreadId thread_id = furi_thread_get_current_id();
    FuriPubSubSubscription* subscription =
        furi_pubsub_subscribe(loader_get_pubsub(app->loader), loader_pubsub_callback, thread_id);

    /* The cover holds the browser's last frame while the FAP is read and
     * mapped. Only a slow load swaps it for the hourglass. */
    loader_applications_set_spinner(app, false);
    icon_animation_start(app->spinner);
    furi_timer_start(app->spinner_timer, furi_ms_to_ticks(LOADER_APPLICATIONS_SPINNER_DELAY_MS));

    LoaderStatus status = loader_start_with_gui_error(app->loader, name, args);
    loader_applications_trace("start_app_after_loader");

    furi_timer_stop(app->spinner_timer);
    icon_animation_stop(app->spinner);
    loader_applications_set_spinner(app, false);

    if(status == LoaderStatusOk) {
        furi_thread_flags_wait(APPLICATION_STOP_EVENT, FuriFlagWaitAny, FuriWaitForever);
    }

    furi_pubsub_unsubscribe(loader_get_pubsub(app->loader), subscription);
    furi_thread_flags_clear(APPLICATION_STOP_EVENT);
}

static int32_t loader_applications_thread(void* p) {
    LoaderApplications* loader_applications = p;
    loader_applications_trace("thread_start");
    LoaderApplicationsApp* app = loader_applications_app_alloc();
    loader_applications_trace("app_alloc");

    view_holder_set_view(app->view_holder, app->cover);

    while(loader_applications_select_app(app)) {
        if(furi_string_end_with(app->file_path, ".js")) {
            loader_applications_trace("launch_js");
            loader_applications_start_app(
                app, "js_app", furi_string_get_cstr(app->file_path));
        } else {
            loader_applications_trace("launch_fap");
            loader_applications_start_app(app, furi_string_get_cstr(app->file_path), NULL);
        }
    }

    view_holder_set_view(app->view_holder, NULL);
    loader_applications_app_free(app);
    loader_applications_trace("app_free");

    if(loader_applications->closed_cb) {
        loader_applications->closed_cb(loader_applications->context);
    }

    loader_applications_trace("thread_end");
    return 0;
}
