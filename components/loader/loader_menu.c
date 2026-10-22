#include <gui.h>
#include <view_dispatcher.h>
#include <menu.h>
#include <submenu.h>
#include <icon_i.h>
#include <assets_icons.h>
#include <applications.h>
#include <archive/helpers/archive_favorites.h>
#include <archive/helpers/archive_files.h>
#include <core/dangerous_defines.h>
#include <stdlib.h>
#include <strings.h>
#include <netrunner_application/netrunner_application.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>
#include <m-list.h>
#include <esp_rom_sys.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "loader.h"
#include "loader_menu.h"

#define TAG "LoaderMenu"

typedef struct LoaderMenuApp LoaderMenuApp;

struct LoaderMenu {
    FuriThread* thread;
    void (*closed_cb)(void*);
    void* context;
    bool start_in_settings;
    /* Set once the menu thread has built its views, so a reload arriving
     * before that is simply dropped — the menu is about to be built anyway. */
    LoaderMenuApp* app;
};

static void loader_menu_trace(const char* step) {
    esp_rom_printf(
        "\r\n[LM] %s free=%u\r\n",
        step,
        (unsigned)furi_thread_get_stack_space(furi_thread_get_current_id()));
}

static void loader_menu_trace_settings_registry(void) {
    esp_rom_printf(
        "\r\n[LM] settings_counts internal=%u external=%u\r\n",
        (unsigned)NETRUNNER_SETTINGS_APPS_COUNT,
        (unsigned)NETRUNNER_EXTSETTINGS_APPS_COUNT);

    if(NETRUNNER_SETTINGS_APPS_COUNT == 0) {
        esp_rom_printf("\r\n[LM] settings_internal none\r\n");
    } else {
        for(size_t i = 0; i < NETRUNNER_SETTINGS_APPS_COUNT; ++i) {
            esp_rom_printf(
                "\r\n[LM] settings_internal[%u] appid=%s name=%s\r\n",
                (unsigned)i,
                NETRUNNER_SETTINGS_APPS[i].appid ? NETRUNNER_SETTINGS_APPS[i].appid : "(null)",
                NETRUNNER_SETTINGS_APPS[i].name ? NETRUNNER_SETTINGS_APPS[i].name : "(null)");
        }
    }

    if(NETRUNNER_EXTSETTINGS_APPS_COUNT == 0) {
        esp_rom_printf("\r\n[LM] settings_external none\r\n");
    } else {
        for(size_t i = 0; i < NETRUNNER_EXTSETTINGS_APPS_COUNT; ++i) {
            esp_rom_printf(
                "\r\n[LM] settings_external[%u] launch=%s name=%s\r\n",
                (unsigned)i,
                NETRUNNER_EXTSETTINGS_APPS[i].path ? NETRUNNER_EXTSETTINGS_APPS[i].path : "(null)",
                NETRUNNER_EXTSETTINGS_APPS[i].name ? NETRUNNER_EXTSETTINGS_APPS[i].name : "(null)");
        }
    }
}

static int32_t loader_menu_thread(void* p);

LoaderMenu* loader_menu_alloc(
    void (*closed_cb)(void*),
    void* context,
    bool start_in_settings) {
    /* app is published by the menu thread. It must be NULL until then so an
     * asynchronous app-close notification can never follow an indeterminate
     * pointer. */
    LoaderMenu* loader_menu = calloc(1, sizeof(LoaderMenu));
    loader_menu->closed_cb = closed_cb;
    loader_menu->context = context;
    loader_menu->start_in_settings = start_in_settings;
    loader_menu->thread =
        furi_thread_alloc_ex_psram(TAG, 4096, loader_menu_thread, loader_menu);
    furi_thread_start(loader_menu->thread);
    return loader_menu;
}

void loader_menu_free(LoaderMenu* loader_menu) {
    furi_assert(loader_menu);
    furi_thread_join(loader_menu->thread);
    furi_thread_free(loader_menu->thread);
    free(loader_menu);
}

typedef enum {
    LoaderMenuViewPrimary,
    LoaderMenuViewSettings,
    LoaderMenuViewMarauder,
} LoaderMenuView;

/* One row of the primary menu. A built-in app points name, launch and icon
 * into flash; a file entry allocates name and launch. The icon is tracked
 * separately because a file entry only owns one when its FAP manifest supplied
 * it — a file with no manifest falls back to a flash icon. launch is stored
 * rather than derived because internal apps are started by display name and
 * external ones by appid. */
typedef struct {
    const char* name;
    const Icon* icon;
    const char* launch;
    bool owns_strings;
    bool owns_icon;
} MenuApp;

LIST_DEF(MenuAppList, MenuApp, M_POD_OPLIST)
#define M_OPL_MenuAppList_t() LIST_OPLIST(MenuAppList)

struct LoaderMenuApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Menu* primary_menu;
    Submenu* settings_menu;
    Submenu* marauder_menu;
    MenuAppList_t apps_list;
    /* Raw contents of the layout file the menu was last built from, empty when
     * the default order is in use. A reload compares against it and rebuilds
     * only on a real change, so exiting an app that touched nothing does not
     * flicker the menu through an empty state. */
    FuriString* layout;
};

static void loader_menu_start(const char* name) {
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_start_with_gui_error(loader, name, NULL);
    furi_record_close(RECORD_LOADER);
}

static void loader_menu_apps_callback(void* context, uint32_t index) {
    LoaderMenuApp* app = context;
    loader_menu_start(MenuAppList_get(app->apps_list, index)->launch);
}

static void loader_menu_pinned_callback(void* context, uint32_t index) {
    UNUSED(context);
    loader_menu_start(NETRUNNER_EXTERNAL_APPS[index].path);
}

static void loader_menu_applications_callback(void* context, uint32_t index) {
    UNUSED(index);
    UNUSED(context);
    loader_menu_trace("apps_callback");
    loader_menu_start(LOADER_APPLICATIONS_NAME);
}

static void
    loader_menu_settings_menu_callback(void* context, InputType input_type, uint32_t index) {
    UNUSED(context);
    if(input_type == InputTypeShort) {
        loader_menu_start((const char*)index);
    } else if(input_type == InputTypeLong) {
        archive_favorites_handle_setting_pin_unpin((const char*)index, NULL);
    }
}

static void loader_menu_switch_to_settings(void* context, uint32_t index) {
    UNUSED(index);
    UNUSED(context);
    /* Settings is the Netrunner tree now: its Root page holds the Interface,
     * Protocols and Misc groups, and launches the standalone settings apps
     * (Power, Storage, ...) from inside them. */
    loader_menu_start("settings");
}

static uint32_t loader_menu_switch_to_primary(void* context) {
    UNUSED(context);
    return LoaderMenuViewPrimary;
}

static uint32_t loader_menu_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

/* ---- Marauder folder --------------------------------------------------------
 * The WiFi/BLE offensive-security tools, launched straight from this menu. As
 * the loader menu never exits like a standalone app does, selecting an entry
 * starts the target app directly -- no app-to-app handoff and no transition
 * flash. Each entry names the target app's registered name and an optional
 * deep-link arg it already understands (see wlan_app.c for the WiFi args). */
typedef struct {
    const char* label;
    const char* app;
    const char* args;
} MarauderFolderEntry;

static const MarauderFolderEntry marauder_folder_entries[] = {
    {"WiFi Tools", "WiFi", NULL},
    {"WiFi Scan", "WiFi", "scan"},
    {"Capture Handshake+PMKID", "WiFi", "handshake"},
    {"Beacon/SSID Spam", "WiFi", "ssidspam"},
    {"Probe Request Flood", "WiFi", "probeflood"},
    {"BLE Scanner", "BLE Detector", NULL},
    {"BLE Spam", "Bluetooth", NULL},
    {"Wardriving", "Wardriving", NULL},
};

static void loader_menu_marauder_callback(void* context, uint32_t index) {
    UNUSED(context);
    if(index >= COUNT_OF(marauder_folder_entries)) return;
    const MarauderFolderEntry* entry = &marauder_folder_entries[index];
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_start_with_gui_error(loader, entry->app, entry->args);
    furi_record_close(RECORD_LOADER);
}

bool loader_menu_load_fap_meta(
    Storage* storage,
    FuriString* path,
    FuriString* name,
    const Icon** icon) {
    *icon = NULL;
    uint8_t* icon_buf = malloc(FAP_MANIFEST_MAX_ICON_SIZE);
    if(!netrunner_application_load_name_and_icon(path, storage, &icon_buf, name)) {
        free(icon_buf);
        return false;
    }

    Icon* loaded = malloc(sizeof(Icon));
    FURI_CONST_ASSIGN(loaded->frame_count, 1);
    FURI_CONST_ASSIGN(loaded->frame_rate, 1);
    FURI_CONST_ASSIGN(loaded->width, 10);
    FURI_CONST_ASSIGN(loaded->height, 10);
    FURI_CONST_ASSIGN_PTR(loaded->frames, malloc(sizeof(const uint8_t*)));
    FURI_CONST_ASSIGN_PTR(loaded->frames[0], icon_buf);
    *icon = loaded;
    return true;
}

void loader_menu_free_fap_icon(const Icon* icon) {
    if(!icon) return;
    free((void*)icon->frames[0]);
    free((void*)icon->frames);
    free((void*)icon);
}

/* Index of the pinned app in NETRUNNER_EXTERNAL_APPS, or the count if this build
 * does not include it. */
/* Dual Boot is hideable from Netrunner settings: it reboots the board into
 * another firmware, which is not something everyone wants one OK press away
 * on a device they hand to someone else. */
static bool loader_menu_entry_hidden(const char* name) {
    if(!name) return false;
    return netrunner_settings.hide_dualboot && strcmp(name, "Dual Boot") == 0;
}

static size_t loader_menu_pinned_index(void) {
    for(size_t i = 0; i < NETRUNNER_EXTERNAL_APPS_COUNT; i++) {
        if(!strcmp(NETRUNNER_EXTERNAL_APPS[i].path, MAINMENU_PINNED_APPID)) return i;
    }
    return NETRUNNER_EXTERNAL_APPS_COUNT;
}

static void loader_menu_add_app_entry(
    LoaderMenuApp* app,
    const char* name,
    const Icon* icon,
    const char* launch,
    bool owns_strings,
    bool owns_icon) {
    MenuAppList_push_back(app->apps_list, (MenuApp){name, icon, launch, owns_strings, owns_icon});
    menu_add_item(
        app->primary_menu,
        name,
        icon,
        MenuAppList_size(app->apps_list) - 1,
        loader_menu_apps_callback,
        app);
}

static const Icon* loader_menu_get_file_icon(const char* path) {
    const char* extension = strrchr(path, '.');
    if(extension && !strcasecmp(extension, ".js")) return &I_js_script_10px;
    return &I_file_10px;
}

/* Marks the built-in app called `name` as accounted for by the layout file.
 * Shown and removed entries both count: either way the file knew about it, so
 * it is not new and must not be appended. */
static void loader_menu_mark_seen(FuriString* name, bool* seen_internal, bool* seen_external) {
    for(size_t i = 0; i < NETRUNNER_APPS_COUNT; i++) {
        if(furi_string_equal(name, NETRUNNER_APPS[i].name)) {
            seen_internal[i] = true;
            return;
        }
    }
    for(size_t i = 0; i < NETRUNNER_EXTERNAL_APPS_COUNT; i++) {
        if(furi_string_equal(name, NETRUNNER_EXTERNAL_APPS[i].name)) {
            seen_external[i] = true;
            return;
        }
    }
}

static void loader_menu_find_add_app(
    LoaderMenuApp* app,
    Storage* storage,
    FuriString* line,
    size_t pinned) {
    if(furi_string_start_with(line, "/")) {
        const char* launch = strdup(furi_string_get_cstr(line));
        const Icon* icon;
        /* line doubles as the label output: on success it becomes the manifest
         * name, otherwise it still holds the path we fall back to. */
        const bool owns_icon = loader_menu_load_fap_meta(storage, line, line, &icon);
        if(!owns_icon) icon = loader_menu_get_file_icon(launch);
        loader_menu_add_app_entry(
            app, strdup(furi_string_get_cstr(line)), icon, launch, true, owns_icon);
        return;
    }

    for(size_t i = 0; i < NETRUNNER_APPS_COUNT; i++) {
        if(furi_string_equal(line, NETRUNNER_APPS[i].name)) {
            loader_menu_add_app_entry(
                app, NETRUNNER_APPS[i].name, NETRUNNER_APPS[i].icon, NETRUNNER_APPS[i].name, false, false);
            return;
        }
    }

    for(size_t i = 0; i < NETRUNNER_EXTERNAL_APPS_COUNT; i++) {
        if(i == pinned) continue;
        /* A saved custom menu must not resurrect a hidden entry. */
        if(loader_menu_entry_hidden(NETRUNNER_EXTERNAL_APPS[i].name)) continue;
        if(furi_string_equal(line, NETRUNNER_EXTERNAL_APPS[i].name)) {
            loader_menu_add_app_entry(
                app,
                NETRUNNER_EXTERNAL_APPS[i].name,
                NETRUNNER_EXTERNAL_APPS[i].icon,
                NETRUNNER_EXTERNAL_APPS[i].path,
                false,
                false);
            return;
        }
    }
}

/* One sortable main-menu entry: an app (kind 0) or the Settings folder (kind 1). */
typedef struct {
    const char* name;
    const Icon* icon;
    const char* launch; /* app launch name/path; NULL for Settings */
    int kind; /* 0 = app, 1 = Settings */
} LoaderMenuSortEntry;

static int loader_menu_sort_cmp(const void* a, const void* b) {
    const LoaderMenuSortEntry* ea = a;
    const LoaderMenuSortEntry* eb = b;
    return strcasecmp(ea->name, eb->name);
}

/* Default main-menu order: everything except the fixed "Apps" header (kept at
 * the top) is sorted alphabetically by display name, so internal apps, menu
 * apps and the Settings folder interleave into one clean A→Z list regardless of
 * their internal type or `order=`. A saved layout (the Mainmenu editor) still
 * wins — this only applies when there is no layout file. */
static void loader_menu_build_default(LoaderMenuApp* app, size_t pinned) {
    size_t cap = NETRUNNER_APPS_COUNT + NETRUNNER_EXTERNAL_APPS_COUNT + 1;
    LoaderMenuSortEntry* entries = malloc(sizeof(LoaderMenuSortEntry) * cap);
    size_t n = 0;

    for(size_t i = 0; i < NETRUNNER_APPS_COUNT; i++) {
        entries[n++] = (LoaderMenuSortEntry){
            NETRUNNER_APPS[i].name, NETRUNNER_APPS[i].icon, NETRUNNER_APPS[i].name, 0};
    }
    for(size_t i = 0; i < NETRUNNER_EXTERNAL_APPS_COUNT; i++) {
        if(i == pinned) continue;
        if(loader_menu_entry_hidden(NETRUNNER_EXTERNAL_APPS[i].name)) continue;
        entries[n++] = (LoaderMenuSortEntry){
            NETRUNNER_EXTERNAL_APPS[i].name, NETRUNNER_EXTERNAL_APPS[i].icon,
            NETRUNNER_EXTERNAL_APPS[i].path, 0};
    }
    if((NETRUNNER_EXTSETTINGS_APPS_COUNT > 0) || (NETRUNNER_SETTINGS_APPS_COUNT > 0)) {
        entries[n++] = (LoaderMenuSortEntry){"Settings", &A_Settings_14, NULL, 1};
    }

    qsort(entries, n, sizeof(LoaderMenuSortEntry), loader_menu_sort_cmp);

    for(size_t i = 0; i < n; i++) {
        if(entries[i].kind == 1) {
            menu_add_item(
                app->primary_menu, "Settings", &A_Settings_14, 0,
                loader_menu_switch_to_settings, app);
        } else {
            loader_menu_add_app_entry(
                app, entries[i].name, entries[i].icon, entries[i].launch, false, false);
        }
    }

    free(entries);
}

static void loader_menu_build_menu(LoaderMenuApp* app, LoaderMenu* menu) {
    UNUSED(menu);
    const size_t pinned = loader_menu_pinned_index();
    bool used_default = false;

    menu_add_item(
        app->primary_menu,
        LOADER_APPLICATIONS_NAME,
        &A_Plugins_14,
        0,
        loader_menu_applications_callback,
        app);

    /* Marauder folder removed from the menu. */

    MenuAppList_init(app->apps_list);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();
    unsigned long version;

    furi_string_reset(app->layout);

    bool* seen_internal = calloc(NETRUNNER_APPS_COUNT, sizeof(bool));
    bool* seen_external = calloc(NETRUNNER_EXTERNAL_APPS_COUNT, sizeof(bool));

    if(file_stream_open(stream, MAINMENU_APPS_PATH, FSAM_READ, FSOM_OPEN_EXISTING) &&
       stream_read_line(stream, line) &&
       sscanf(furi_string_get_cstr(line), MAINMENU_APPS_HEADER_FMT, &version) == 1 &&
       version >= MAINMENU_APPS_VERSION_MIN && version <= MAINMENU_APPS_VERSION) {
        while(stream_read_line(stream, line)) {
            furi_string_trim(line);
            if(!furi_string_size(line)) continue;
            /* Recorded before the calls below, which reuse line as their label
             * output for path entries. */
            furi_string_cat(app->layout, line);
            furi_string_cat_str(app->layout, "\n");

            if(furi_string_get_char(line, 0) == MAINMENU_REMOVED_PREFIX) {
                /* Known and deliberately removed: account for it so it is not
                 * mistaken for a new app, but do not show it. */
                furi_string_right(line, 1);
                loader_menu_mark_seen(line, seen_internal, seen_external);
                continue;
            }

            loader_menu_mark_seen(line, seen_internal, seen_external);
            loader_menu_find_add_app(app, storage, line, pinned);
        }

        /* Anything the file never mentioned was added to the firmware after it
         * was saved. Append rather than drop: a layout records the user's order,
         * not permission to exist, and silently hiding a new app looks exactly
         * like the app failing to build. */
        for(size_t i = 0; i < NETRUNNER_APPS_COUNT; i++) {
            if(seen_internal[i]) continue;
            loader_menu_add_app_entry(
                app,
                NETRUNNER_APPS[i].name,
                NETRUNNER_APPS[i].icon,
                NETRUNNER_APPS[i].name,
                false,
                false);
        }
        for(size_t i = 0; i < NETRUNNER_EXTERNAL_APPS_COUNT; i++) {
            if(seen_external[i] || i == pinned) continue;
            if(loader_menu_entry_hidden(NETRUNNER_EXTERNAL_APPS[i].name)) continue;
            loader_menu_add_app_entry(
                app,
                NETRUNNER_EXTERNAL_APPS[i].name,
                NETRUNNER_EXTERNAL_APPS[i].icon,
                NETRUNNER_EXTERNAL_APPS[i].path,
                false,
                false);
        }
    } else {
        loader_menu_build_default(app, pinned);
        used_default = true;
    }

    free(seen_internal);
    free(seen_external);

    furi_string_free(line);
    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);

    if(pinned < NETRUNNER_EXTERNAL_APPS_COUNT) {
        menu_add_item(
            app->primary_menu,
            NETRUNNER_EXTERNAL_APPS[pinned].name,
            NETRUNNER_EXTERNAL_APPS[pinned].icon,
            pinned,
            loader_menu_pinned_callback,
            app);
    }

    /* The default build already inserts Settings in its sorted position; only
     * the layout path needs it appended at the end. */
    if(!used_default &&
       ((NETRUNNER_EXTSETTINGS_APPS_COUNT > 0) || (NETRUNNER_SETTINGS_APPS_COUNT > 0))) {
        menu_add_item(
            app->primary_menu, "Settings", &A_Settings_14, 0, loader_menu_switch_to_settings, app);
    }
}

static void loader_menu_free_entries(LoaderMenuApp* app) {
    /* A built-in entry points at flash and must not be freed. */
    for
        M_EACH(menu_app, app->apps_list, MenuAppList_t) {
            if(menu_app->owns_icon) loader_menu_free_fap_icon(menu_app->icon);
            if(menu_app->owns_strings) {
                free((void*)menu_app->name);
                free((void*)menu_app->launch);
            }
        }
    MenuAppList_clear(app->apps_list);
}

/* Reads the layout file the way loader_menu_build_menu() records it, so the two
 * can be compared. Leaves out empty when the file is missing or its header does
 * not match, which is also what selects the default order. */
static void loader_menu_read_layout(Storage* storage, FuriString* out) {
    furi_string_reset(out);

    Stream* stream = file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();
    unsigned long version;

    if(file_stream_open(stream, MAINMENU_APPS_PATH, FSAM_READ, FSOM_OPEN_EXISTING) &&
       stream_read_line(stream, line) &&
       sscanf(furi_string_get_cstr(line), MAINMENU_APPS_HEADER_FMT, &version) == 1 &&
       version >= MAINMENU_APPS_VERSION_MIN && version <= MAINMENU_APPS_VERSION) {
        while(stream_read_line(stream, line)) {
            furi_string_trim(line);
            if(!furi_string_size(line)) continue;
            furi_string_cat(out, line);
            furi_string_cat_str(out, "\n");
        }
    }

    furi_string_free(line);
    file_stream_close(stream);
    stream_free(stream);
}

static void loader_menu_reload_now(LoaderMenuApp* app, LoaderMenu* loader_menu) {
    /* Exiting an app is the common case and almost never changes the layout;
     * rebuilding regardless would flicker the menu through an empty state
     * every time. */
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* layout = furi_string_alloc();
    loader_menu_read_layout(storage, layout);
    const bool unchanged = furi_string_equal(layout, app->layout);
    furi_string_free(layout);
    furi_record_close(RECORD_STORAGE);

    if(unchanged) return;

    const uint32_t selected = menu_get_selected_item(app->primary_menu);

    /* menu_reset() stops the icon animations built over the entry icons, so it
     * has to run before those icons are freed. */
    menu_reset(app->primary_menu);
    loader_menu_free_entries(app);
    loader_menu_build_menu(app, loader_menu);

    menu_set_selected_item(app->primary_menu, selected);
}

enum {
    LoaderMenuEventReload = 1,
};

static bool loader_menu_custom_event_callback(void* context, uint32_t event) {
    LoaderMenu* loader_menu = context;
    if(event != LoaderMenuEventReload || !loader_menu || !loader_menu->app) return false;

    /* All view/model ownership lives on this thread. Rebuilding directly from
     * LoaderSrv during application teardown raced input drawing and was the
     * common crash seen when leaving otherwise unrelated apps. */
    loader_menu_reload_now(loader_menu->app, loader_menu);
    return true;
}

void loader_menu_reload(LoaderMenu* loader_menu) {
    if(!loader_menu || !loader_menu->app) return;
    view_dispatcher_send_custom_event(
        loader_menu->app->view_dispatcher, LoaderMenuEventReload);
}

static void loader_menu_build_submenu(LoaderMenuApp* app, LoaderMenu* loader_menu) {
    for(size_t i = 0; i < NETRUNNER_EXTSETTINGS_APPS_COUNT; i++) {
        submenu_add_item_ex(
            app->settings_menu,
            NETRUNNER_EXTSETTINGS_APPS[i].name,
            (uint32_t)NETRUNNER_EXTSETTINGS_APPS[i].path,
            loader_menu_settings_menu_callback,
            loader_menu);
    }
    for(size_t i = 0; i < NETRUNNER_SETTINGS_APPS_COUNT; i++) {
        submenu_add_item_ex(
            app->settings_menu,
            NETRUNNER_SETTINGS_APPS[i].name,
            (uint32_t)NETRUNNER_SETTINGS_APPS[i].name,
            loader_menu_settings_menu_callback,
            loader_menu);
    }
}

static void loader_menu_build_marauder(LoaderMenuApp* app) {
    submenu_set_header(app->marauder_menu, "Marauder");
    for(size_t i = 0; i < COUNT_OF(marauder_folder_entries); i++) {
        submenu_add_item(
            app->marauder_menu,
            marauder_folder_entries[i].label,
            (uint32_t)i,
            loader_menu_marauder_callback,
            app);
    }
}

static LoaderMenuApp* loader_menu_app_alloc(LoaderMenu* loader_menu) {
    LoaderMenuApp* app = malloc(sizeof(LoaderMenuApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->primary_menu = menu_alloc();
    app->settings_menu = submenu_alloc();
    app->marauder_menu = submenu_alloc();
    app->layout = furi_string_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, loader_menu);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, loader_menu_custom_event_callback);

    loader_menu_trace_settings_registry();
    loader_menu_build_menu(app, loader_menu);
    loader_menu_build_submenu(app, loader_menu);
    loader_menu_build_marauder(app);

    // Primary menu
    View* primary_view = menu_get_view(app->primary_menu);
    view_set_context(primary_view, app->primary_menu);
    view_set_previous_callback(primary_view, loader_menu_exit);
    view_dispatcher_add_view(
        app->view_dispatcher, LoaderMenuViewPrimary, primary_view);

    // Settings menu
    View* settings_view = submenu_get_view(app->settings_menu);
    view_set_context(settings_view, app->settings_menu);
    view_set_previous_callback(settings_view, loader_menu_switch_to_primary);
    view_dispatcher_add_view(
        app->view_dispatcher, LoaderMenuViewSettings, settings_view);

    // Marauder folder
    View* marauder_view = submenu_get_view(app->marauder_menu);
    view_set_context(marauder_view, app->marauder_menu);
    view_set_previous_callback(marauder_view, loader_menu_switch_to_primary);
    view_dispatcher_add_view(
        app->view_dispatcher, LoaderMenuViewMarauder, marauder_view);

    view_dispatcher_switch_to_view(
        app->view_dispatcher,
        loader_menu->start_in_settings ? LoaderMenuViewSettings : LoaderMenuViewPrimary);

    return app;
}

static void loader_menu_app_free(LoaderMenuApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewPrimary);
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewMarauder);
    view_dispatcher_free(app->view_dispatcher);

    loader_menu_free_entries(app);
    furi_string_free(app->layout);

    menu_free(app->primary_menu);
    submenu_free(app->settings_menu);
    submenu_free(app->marauder_menu);
    furi_record_close(RECORD_GUI);
    free(app);
}

static int32_t loader_menu_thread(void* p) {
    LoaderMenu* loader_menu = p;
    furi_assert(loader_menu);
    loader_menu_trace("thread_start");

    LoaderMenuApp* app = loader_menu_app_alloc(loader_menu);
    loader_menu->app = app;
    loader_menu_trace("app_alloc");

    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    loader_menu_trace("attached");
    view_dispatcher_run(app->view_dispatcher);
    loader_menu_trace("dispatcher_return");

    loader_menu->app = NULL;

    if(loader_menu->closed_cb) {
        loader_menu->closed_cb(loader_menu->context);
    }

    loader_menu_app_free(app);
    loader_menu_trace("thread_end");

    return 0;
}
