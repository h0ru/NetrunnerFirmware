#include <furi.h>
#include <furi_hal.h>
#include <applications.h>
#include <assets_icons.h>
#include <desktop/desktop.h>
#include <dialogs/dialogs.h>
#include <dolphin/dolphin.h>
#include <dolphin/dolphin_i.h>
#include <dolphin/helpers/dolphin_state.h>
#include <gui/gui.h>
#include <gui/modules/submenu.h>
#include <gui/modules/number_input.h>
#include <gui/modules/text_input.h>
#include <gui/view.h>
#include <datetime.h>
#include <gui/modules/variable_item_list.h>
#include <netrunner_format/netrunner_format.h>
#include <gui/view_dispatcher.h>
#include <locale/locale.h>
/* Resolves to components/loader, which is the loader that this build compiles;
 * applications/services/loader is a second, uncompiled copy. */
#include <loader/loader_menu.h>
#include <loader/loader.h>
#include <netrunner/netrunner.h>
#include <namechanger/namechanger.h>
#include <power/power_service/power.h>
#include <storage/storage.h>
#include <subghz/subghz_extended_range.h>
#include <toolbox/stream/file_stream.h>
#include <toolbox/value_index.h>
#include <toolbox/name_generator.h>
#include <wifi/wlan_hal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TAG "SettingsApp"

/* Upper bound on wallpaper names listed from an asset pack manifest. */
#define NETRUNNER_MAX_WALLPAPERS 64
#define NETRUNNER_MAX_SELECTABLE_PACKS 254U
#define NETRUNNER_DIRECTORY_NAME_SIZE  256U
#define NETRUNNER_MAX_USER_FREQS       24U
#define NETRUNNER_MAX_MAINMENU_ITEMS   64U
/* One past the top threshold, so the editor can actually reach the max level.
 * Derived rather than hardcoded: this used to be a literal 9999 that silently
 * disagreed with a dolphin capped at 1800. */
#define NETRUNNER_DOLPHIN_XP_MAX       (DOLPHIN_LEVELS[DOLPHIN_LEVEL_COUNT - 1] + 1U)
#define NETRUNNER_DOLPHIN_BUTTHURT_MAX 14
#define NETRUNNER_SUBGHZ_USER_SETTINGS EXT_PATH("subghz/assets/setting_user")
/* Mirrors the private constants in lib/subghz/subghz_setting.c, which are not
 * exported by its header. They must stay in step with it. */
#define NETRUNNER_SUBGHZ_SETTING_TYPE    "Netrunner SubGhz Setting File"
#define NETRUNNER_SUBGHZ_SETTING_VERSION 1

typedef enum {
    NetrunnerSettingsViewList,
    NetrunnerSettingsViewAssetPacks,
    NetrunnerSettingsViewWallpaper,
    NetrunnerSettingsViewTextInput,
    NetrunnerSettingsViewFreqList,
    NetrunnerSettingsViewNumberInput,
    NetrunnerSettingsViewSetTime,
    NetrunnerSettingsViewMainmenu,
} NetrunnerSettingsView;

typedef enum {
    NetrunnerSettingsPageRoot,
    NetrunnerSettingsPageInterface,
    NetrunnerSettingsPageGraphics,
    NetrunnerSettingsPageMainmenu,
    NetrunnerSettingsPageLockscreen,
    NetrunnerSettingsPageStatusbar,
    NetrunnerSettingsPageFileBrowser,
    NetrunnerSettingsPageGeneral,
    NetrunnerSettingsPageProtocols,
    NetrunnerSettingsPageMisc,
    NetrunnerSettingsPageScreen,
    NetrunnerSettingsPageFrequencies,
    NetrunnerSettingsPageDolphin,
    NetrunnerSettingsPageCount,
} NetrunnerSettingsPage;

/* The Mainmenu page borrows one Submenu for three screens, the way the
 * frequency list does, rather than allocating a view per screen. */
typedef enum {
    NetrunnerMainmenuScreenAddSource,
    NetrunnerMainmenuScreenAddBuiltin,
    NetrunnerMainmenuScreenResetConfirm,
} NetrunnerMainmenuScreen;

/* Rows of the Mainmenu page, which the enter handler and the post-edit refresh
 * both index into. */
typedef enum {
    /* These are POSITIONS in the Mainmenu page's variable_item_list, so the
     * order here must match the order the rows are added in, exactly. When
     * "Hide Dual Boot" was added it was inserted into the list between Add and
     * Move but appended to the END of this enum, which shifted every row from
     * index 4 on: the enter callback tested Remove(5) against the Move row --
     * which has values, so OK entered edit mode and the callback never ran --
     * and refresh() applied the "need two items" lock to Hide Dual Boot. Net
     * effect: items could not be removed. Keep this list and the
     * variable_item_list_add() sequence in step. */
    NetrunnerMainmenuRowMenuStyle,
    NetrunnerMainmenuRowReset,
    NetrunnerMainmenuRowItem,
    NetrunnerMainmenuRowAdd,
    NetrunnerMainmenuRowHideDualBoot,
    NetrunnerMainmenuRowMove,
    NetrunnerMainmenuRowRemove,
} NetrunnerMainmenuRow;

typedef struct {
    Gui* gui;
    Storage* storage;
    Desktop* desktop;
    Power* power;
    Dolphin* dolphin;
    DialogsApp* dialogs;
    ViewDispatcher* view_dispatcher;
    VariableItemList* variable_item_list;
    Submenu* asset_pack_submenu;
    Submenu* freq_submenu;
    Submenu* mainmenu_submenu;
    TextInput* text_input;
    NumberInput* number_input;
    View* set_time_view;
    VariableItem* asset_pack_item;
    char** asset_pack_names;
    uint8_t asset_pack_count;
    Submenu* wallpaper_submenu;
    VariableItem* wallpaper_item;
    char** wallpaper_names;
    uint8_t wallpaper_count;
    NetrunnerSettings settings;
    DesktopSettings desktop_settings;
    NetrunnerSettingsPage current_page;
    bool sd_ready;
    bool dirty;
    bool desktop_dirty;
    bool name_dirty;
    bool subghz_extended_range;
    bool subghz_use_defaults;
    bool subghz_freqs_dirty;
    bool subghz_editing_hopper;
    bool dolphin_dirty;
    bool number_input_for_xp;
    uint32_t dolphin_xp;
    int32_t dolphin_butthurt;
    uint8_t subghz_static_count;
    uint8_t subghz_hopper_count;
    uint32_t subghz_static_freqs[NETRUNNER_MAX_USER_FREQS];
    uint32_t subghz_hopper_freqs[NETRUNNER_MAX_USER_FREQS];
    char device_name[FURI_HAL_VERSION_ARRAY_NAME_LENGTH];
    /* labels are what the page shows, lines are what gets written back. They
     * differ only for file entries, where the label is the FAP manifest name
     * and the line is the path. */
    char* mainmenu_labels[NETRUNNER_MAX_MAINMENU_ITEMS];
    char* mainmenu_lines[NETRUNNER_MAX_MAINMENU_ITEMS];
    uint8_t mainmenu_count;
    uint8_t mainmenu_index;
    bool mainmenu_dirty;
    NetrunnerMainmenuScreen mainmenu_screen;
    /* variable_item_set_item_label() keeps the pointer it is given rather than
     * copying, so the "Item n/m" text has to outlive the call. */
    char mainmenu_item_label[24];
    /* Settings app waiting to run on top of this one; see launch_app. */
    const NetrunnerInternalApplication* pending_child;
} NetrunnerSettingsApp;

/* Model for the "Set Time" picker. Holds the date/time being edited (date part
 * preserved from the RTC, only H/M/S change) and which of the three fields is
 * focused: 0 = hours, 1 = minutes, 2 = seconds. */
typedef struct {
    DateTime datetime;
    uint8_t field;
} SetTimeModel;

/* Custom event that runs pending_child. Above every page value, which share the
 * same event space. */
#define NETRUNNER_SETTINGS_EVENT_RUN_CHILD 0x100U

static const uint32_t netrunner_anim_speed_values[] = {
    25,
    50,
    75,
    100,
    125,
    150,
    175,
    200,
    225,
    250,
    275,
    300,
};

static const char* const netrunner_anim_speed_text[] = {
    "25%",
    "50%",
    "75%",
    "100%",
    "125%",
    "150%",
    "175%",
    "200%",
    "225%",
    "250%",
    "275%",
    "300%",
};

static const int32_t netrunner_cycle_anim_values[] = {
    -1,
    0,
    15,
    30,
    60,
    120,
    300,
    600,
    900,
    1800,
    3600,
    7200,
    21600,
    43200,
    86400,
};

static const char* const netrunner_cycle_anim_text[] = {
    "OFF",
    "Meta.txt",
    "15 S",
    "30 S",
    "1 M",
    "2 M",
    "5 M",
    "10 M",
    "15 M",
    "30 M",
    "1 H",
    "2 H",
    "6 H",
    "12 H",
    "24 H",
};

static const bool netrunner_unlock_anim_values[] = {
    false,
    true,
};

static const char* const netrunner_unlock_anim_text[] = {
    "OFF",
    "ON",
};

static const char* const netrunner_menu_style_text[MenuStyleCount] = {
    "List",
    "Wii",
    "DSi",
    "PS4",
    "Vertical",
    "C64",
    "Compact",
    "ARSK",
    "CoverFlow",
};

static const char* const netrunner_browser_path_text[] = {
    "OFF",
    "Current",
    "Brief",
    "Full",
};

static const uint32_t netrunner_battery_icon_values[] = {
    DISPLAY_BATTERY_OFF,
    DISPLAY_BATTERY_BAR,
    DISPLAY_BATTERY_PERCENT,
    DISPLAY_BATTERY_INVERTED_PERCENT,
    DISPLAY_BATTERY_RETRO_3,
    DISPLAY_BATTERY_RETRO_5,
    DISPLAY_BATTERY_BAR_PERCENT,
};

static const char* const netrunner_battery_icon_text[] = {
    "OFF",
    "Bar",
    "%",
    "Inv. %",
    "Retro 3",
    "Retro 5",
    "Bar %",
};

/* Shell Color (device case colour, spoofed in device info / BLE). Moved here
 * from Spoofing. Persisted as the "Color" field in /ext/dolphin/name.settings
 * (written on exit below, loaded by the namechanger service at boot). */
static const char* const netrunner_shell_color_text[] = {
    "Real Default",
    "Black",
    "White",
    "Transparent",
};

static const FuriHalVersionColor netrunner_shell_color_values[] = {
    FuriHalVersionColorUnknown,
    FuriHalVersionColorBlack,
    FuriHalVersionColorWhite,
    FuriHalVersionColorTransparent,
};

static const uint32_t netrunner_clock_values[] = {
    0,
    1,
};

static const uint32_t netrunner_time_format_values[] = {
    LocaleTimeFormat24h,
    LocaleTimeFormat12h,
};

static const char* const netrunner_time_format_text[] = {
    "24H",
    "12H",
};

/* Auto uses a certificate-verified IP lookup only while WiFi is connected.
 * Every currently inhabited civil UTC offset is also available manually, so
 * the clock remains usable offline and if the public service is unavailable. */
static const int32_t netrunner_timezone_values[] = {
    0,
    -720,
    -660,
    -600,
    -570,
    -540,
    -480,
    -420,
    -360,
    -300,
    -240,
    -210,
    -180,
    -120,
    -60,
    0,
    60,
    120,
    180,
    210,
    240,
    270,
    300,
    330,
    345,
    360,
    390,
    420,
    480,
    525,
    540,
    570,
    600,
    630,
    660,
    720,
    765,
    780,
    825,
    840,
};

static const char* const netrunner_timezone_text[] = {
    "Auto (IP)",
    "UTC-12:00",
    "UTC-11:00",
    "UTC-10:00",
    "UTC-09:30",
    "UTC-09:00",
    "UTC-08:00",
    "UTC-07:00",
    "UTC-06:00",
    "UTC-05:00",
    "UTC-04:00",
    "UTC-03:30",
    "UTC-03:00",
    "UTC-02:00",
    "UTC-01:00",
    "UTC+00:00",
    "UTC+01:00",
    "UTC+02:00",
    "UTC+03:00",
    "UTC+03:30",
    "UTC+04:00",
    "UTC+04:30",
    "UTC+05:00",
    "UTC+05:30",
    "UTC+05:45",
    "UTC+06:00",
    "UTC+06:30",
    "UTC+07:00",
    "UTC+08:00",
    "UTC+08:45",
    "UTC+09:00",
    "UTC+09:30",
    "UTC+10:00",
    "UTC+10:30",
    "UTC+11:00",
    "UTC+12:00",
    "UTC+12:45",
    "UTC+13:00",
    "UTC+13:45",
    "UTC+14:00",
};

static void netrunner_settings_show_page(
    NetrunnerSettingsApp* app,
    NetrunnerSettingsPage page,
    uint8_t selected_item);
static void netrunner_settings_set_time_draw(Canvas* canvas, void* model);
static bool netrunner_settings_set_time_input(InputEvent* event, void* context);
static void netrunner_settings_show_freq_list(NetrunnerSettingsApp* app);
static void netrunner_settings_use_defaults_changed(VariableItem* item);
static void netrunner_settings_butthurt_changed(VariableItem* item);
static void netrunner_settings_number_done(void* context, int32_t number);
static void netrunner_settings_mainmenu_show_reset(NetrunnerSettingsApp* app);
static void netrunner_settings_mainmenu_show_add_source(NetrunnerSettingsApp* app);
static void netrunner_settings_mainmenu_remove(NetrunnerSettingsApp* app, uint8_t index);
static void netrunner_settings_mainmenu_refresh(NetrunnerSettingsApp* app);
static bool netrunner_settings_flush(NetrunnerSettingsApp* app);
static void netrunner_settings_show_wallpaper_list(NetrunnerSettingsApp* app);
static void netrunner_settings_update_wallpaper_item(NetrunnerSettingsApp* app);
static void netrunner_settings_free_wallpaper_names(NetrunnerSettingsApp* app);

static bool netrunner_settings_add_asset_pack(NetrunnerSettingsApp* app, const char* name) {
    furi_assert(app);
    furi_assert(name);

    if(app->asset_pack_count >= NETRUNNER_MAX_SELECTABLE_PACKS) return false;

    const size_t name_length = strlen(name);
    char* name_copy = malloc(name_length + 1U);
    if(!name_copy) return false;
    memcpy(name_copy, name, name_length + 1U);

    char** resized_names =
        realloc(app->asset_pack_names, (app->asset_pack_count + 1U) * sizeof(char*));
    if(!resized_names) {
        free(name_copy);
        return false;
    }
    app->asset_pack_names = resized_names;

    uint8_t insert_at = 0;
    while(insert_at < app->asset_pack_count) {
        int comparison = strcasecmp(app->asset_pack_names[insert_at], name);
        if(comparison == 0) comparison = strcmp(app->asset_pack_names[insert_at], name);
        if(comparison > 0) break;
        insert_at++;
    }

    memmove(
        &app->asset_pack_names[insert_at + 1U],
        &app->asset_pack_names[insert_at],
        (app->asset_pack_count - insert_at) * sizeof(char*));
    app->asset_pack_names[insert_at] = name_copy;
    app->asset_pack_count++;
    return true;
}

static void netrunner_settings_scan_asset_packs(NetrunnerSettingsApp* app) {
    furi_assert(app);
    if(!app->sd_ready) return;

    File* directory = storage_file_alloc(app->storage);
    bool opened = storage_dir_open(directory, ASSET_PACKS_PATH);
    if(opened) {
        FileInfo file_info;
        char name[NETRUNNER_DIRECTORY_NAME_SIZE];

        while(storage_dir_read(directory, &file_info, name, sizeof(name))) {
            const size_t name_length = strlen(name);
            if(!file_info_is_dir(&file_info) || name_length == 0 || name[0] == '.' ||
               name_length >= ASSET_PACKS_NAME_LEN ||
               !netrunner_settings_asset_pack_is_safe(name)) {
                continue;
            }

            char manifest_path[NETRUNNER_DIRECTORY_NAME_SIZE];
            int written = snprintf(
                manifest_path, sizeof(manifest_path), "%s/%s/Anims/manifest.txt", ASSET_PACKS_PATH, name);
            if(written <= 0 || (size_t)written >= sizeof(manifest_path) ||
               !storage_file_exists(app->storage, manifest_path)) {
                continue;
            }

            if(!netrunner_settings_add_asset_pack(app, name)) {
                FURI_LOG_W(TAG, "Asset pack limit reached while scanning %s", ASSET_PACKS_PATH);
                break;
            }
        }
    }

    storage_dir_close(directory);
    storage_file_free(directory);
}

static void netrunner_settings_free_wallpaper_names(NetrunnerSettingsApp* app) {
    furi_assert(app);
    if(!app->wallpaper_names) return;
    for(uint8_t i = 0; i < app->wallpaper_count; i++) free(app->wallpaper_names[i]);
    free(app->wallpaper_names);
    app->wallpaper_names = NULL;
    app->wallpaper_count = 0;
}

static bool netrunner_settings_add_wallpaper_name(NetrunnerSettingsApp* app, const char* name) {
    furi_assert(app);
    furi_assert(name);
    if(app->wallpaper_count >= NETRUNNER_MAX_WALLPAPERS) return false;
    const size_t name_length = strlen(name);
    if(name_length == 0 || name_length >= ASSET_PACKS_NAME_LEN) return false;

    char* name_copy = malloc(name_length + 1U);
    if(!name_copy) return false;
    memcpy(name_copy, name, name_length + 1U);

    char** resized =
        realloc(app->wallpaper_names, (app->wallpaper_count + 1U) * sizeof(char*));
    if(!resized) {
        free(name_copy);
        return false;
    }
    app->wallpaper_names = resized;
    app->wallpaper_names[app->wallpaper_count++] = name_copy;
    return true;
}

/* List the idle-animation names from the selected pack's manifest, in file
 * order. Mirrors animation_storage's reader: open, header, then every "Name"
 * key. Any failure leaves the list empty, so the picker still offers Random. */
static void netrunner_settings_scan_wallpapers(NetrunnerSettingsApp* app) {
    furi_assert(app);
    netrunner_settings_free_wallpaper_names(app);
    if(!app->sd_ready) return;

    const char* pack = app->settings.asset_pack;
    if(!pack[0] || !netrunner_settings_asset_pack_is_safe(pack)) return;

    char manifest_path[NETRUNNER_DIRECTORY_NAME_SIZE];
    int written = snprintf(
        manifest_path,
        sizeof(manifest_path),
        "%s/%s/Anims/manifest.txt",
        ASSET_PACKS_PATH,
        pack);
    if(written <= 0 || (size_t)written >= sizeof(manifest_path)) return;

    NetrunnerFormat* file = netrunner_format_file_alloc(app->storage);
    FuriString* str = furi_string_alloc();
    do {
        uint32_t version;
        if(!netrunner_format_file_open_existing(file, manifest_path)) break;
        if(!netrunner_format_read_header(file, str, &version)) break;
        /* Names are interleaved with each animation's other keys, so let the
         * reader skip past everything that is not the next "Name". */
        netrunner_format_set_strict_mode(file, false);
        while(app->wallpaper_count < NETRUNNER_MAX_WALLPAPERS &&
              netrunner_format_read_string(file, "Name", str)) {
            netrunner_settings_add_wallpaper_name(app, furi_string_get_cstr(str));
        }
    } while(false);
    furi_string_free(str);
    netrunner_format_free(file);
}

static uint8_t netrunner_settings_wallpaper_index(const NetrunnerSettingsApp* app) {
    furi_assert(app);
    if(!app->settings.wallpaper[0]) return 0;
    for(uint8_t i = 0; i < app->wallpaper_count; i++) {
        if(!strcmp(app->wallpaper_names[i], app->settings.wallpaper)) return i + 1U;
    }
    return 0;
}

static void netrunner_settings_update_wallpaper_item(NetrunnerSettingsApp* app) {
    furi_assert(app);
    if(!app->wallpaper_item) return;
    variable_item_set_current_value_text(
        app->wallpaper_item, app->settings.wallpaper[0] ? app->settings.wallpaper : "Random");
}

static void netrunner_settings_wallpaper_selected(void* context, uint32_t index) {
    NetrunnerSettingsApp* app = context;
    furi_assert(app);
    furi_assert(index <= app->wallpaper_count);

    const char* selected_name = index ? app->wallpaper_names[index - 1U] : "";
    if(strcmp(app->settings.wallpaper, selected_name)) {
        snprintf(
            app->settings.wallpaper, sizeof(app->settings.wallpaper), "%s", selected_name);
        app->dirty = true;
        netrunner_settings_update_wallpaper_item(app);
    }

    submenu_set_selected_item(app->wallpaper_submenu, index);
    view_dispatcher_switch_to_view(app->view_dispatcher, NetrunnerSettingsViewList);
}

static void netrunner_settings_show_wallpaper_list(NetrunnerSettingsApp* app) {
    furi_assert(app);
    netrunner_settings_scan_wallpapers(app);

    submenu_reset(app->wallpaper_submenu);
    submenu_set_header(app->wallpaper_submenu, "Wallpaper");
    /* Random is index 0 (empty setting): the weighted pick the pack already
     * uses. Specific names follow, 1-based. */
    submenu_add_item(
        app->wallpaper_submenu, "Random", 0, netrunner_settings_wallpaper_selected, app);
    for(uint8_t i = 0; i < app->wallpaper_count; i++) {
        submenu_add_item(
            app->wallpaper_submenu,
            app->wallpaper_names[i],
            i + 1U,
            netrunner_settings_wallpaper_selected,
            app);
    }
    submenu_set_selected_item(app->wallpaper_submenu, netrunner_settings_wallpaper_index(app));
    view_dispatcher_switch_to_view(app->view_dispatcher, NetrunnerSettingsViewWallpaper);
}

static uint8_t netrunner_settings_asset_pack_index(const NetrunnerSettingsApp* app) {
    furi_assert(app);
    if(app->settings.asset_pack[0] == '\0') return 0;

    for(uint8_t i = 0; i < app->asset_pack_count; i++) {
        if(!strcmp(app->asset_pack_names[i], app->settings.asset_pack)) return i + 1U;
    }

    return 0;
}

static void netrunner_settings_update_asset_pack_item(NetrunnerSettingsApp* app) {
    furi_assert(app);
    variable_item_set_current_value_text(
        app->asset_pack_item,
        app->settings.asset_pack[0] ? app->settings.asset_pack : "Default");
}

static void netrunner_settings_asset_pack_selected(void* context, uint32_t index) {
    NetrunnerSettingsApp* app = context;
    furi_assert(app);
    furi_assert(index <= app->asset_pack_count);

    const char* selected_name = index ? app->asset_pack_names[index - 1U] : "";
    if(strcmp(app->settings.asset_pack, selected_name)) {
        snprintf(
            app->settings.asset_pack,
            sizeof(app->settings.asset_pack),
            "%s",
            selected_name);
        app->dirty = true;
        netrunner_settings_update_asset_pack_item(app);
    }

    submenu_set_selected_item(app->asset_pack_submenu, index);
    view_dispatcher_switch_to_view(app->view_dispatcher, NetrunnerSettingsViewList);
}

static void netrunner_settings_device_name_done(void* context) {
    NetrunnerSettingsApp* app = context;
    app->name_dirty = true;
    netrunner_settings_show_page(app, NetrunnerSettingsPageMisc, 1);
}

/* Launch args that reopen this app on a given page and row: "<page>,<row>". */
#define NETRUNNER_SETTINGS_RETURN_ARGS_FMT "%u,%u"

static const NetrunnerInternalApplication* netrunner_settings_find_app(const char* appid) {
    const struct {
        const NetrunnerInternalApplication* list;
        size_t count;
    } lists[] = {
        {NETRUNNER_SETTINGS_APPS, NETRUNNER_SETTINGS_APPS_COUNT},
        {NETRUNNER_SYSTEM_APPS, NETRUNNER_SYSTEM_APPS_COUNT},
        {NETRUNNER_APPS, NETRUNNER_APPS_COUNT},
    };
    for(size_t i = 0; i < COUNT_OF(lists); i++) {
        for(size_t j = 0; j < lists[i].count; j++) {
            if(!strcmp(lists[i].list[j].appid, appid)) return &lists[i].list[j];
        }
    }
    return NULL;
}

/* Re-reads what a settings app run from here may have changed underneath this
 * one, so the page is rebuilt from current values and a later save on exit does
 * not write stale copies back over them. */
static void netrunner_settings_reload_after_child(NetrunnerSettingsApp* app) {
    app->settings = netrunner_settings;
    desktop_api_get_settings(app->desktop, &app->desktop_settings);
    DolphinStats stats = dolphin_stats(app->dolphin);
    app->dolphin_xp = stats.icounter;
    app->dolphin_butthurt = (int32_t)stats.butthurt;
}

/* Settings that live in their own app (Power, Storage, Bluetooth, ...) run on
 * top of this one, on their own thread, exactly as the loader would start
 * them. This app stays resident underneath, so the settings app opens at once
 * and Back lands on the row it was opened from, with no teardown, relaunch or
 * SD rescan in between.
 *
 * Pending changes are saved first: several of these apps can reboot, and the
 * Desktop app writes the same desktop settings this one edits. A pending asset
 * pack change needs a reboot of its own, so that one case still goes through
 * the loader: exit (which saves and reboots), then the queued app. */
static void netrunner_settings_launch_app(NetrunnerSettingsApp* app, const char* appid) {
    const NetrunnerInternalApplication* child = netrunner_settings_find_app(appid);
    const bool pack_pending =
        app->dirty && strcmp(netrunner_settings.asset_pack, app->settings.asset_pack) != 0;

    if(!child || pack_pending) {
        char return_args[16];
        snprintf(
            return_args,
            sizeof(return_args),
            NETRUNNER_SETTINGS_RETURN_ARGS_FMT,
            (unsigned)app->current_page,
            (unsigned)variable_item_list_get_selected_item_index(app->variable_item_list));

        Loader* loader = furi_record_open(RECORD_LOADER);
        loader_enqueue_launch(loader, appid, NULL, LoaderDeferredLaunchFlagGui);
        loader_enqueue_launch(loader, "settings", return_args, LoaderDeferredLaunchFlagGui);
        furi_record_close(RECORD_LOADER);
        view_dispatcher_stop(app->view_dispatcher);
        return;
    }

    /* Run from the custom event rather than here: this is called from inside
     * the list's OK handler, which holds the list model lock, and the list's
     * scroll timer takes that lock from the shared timer thread. Holding it for
     * the child's whole lifetime would stall every timer, the child's too. */
    app->pending_child = child;
    view_dispatcher_send_custom_event(app->view_dispatcher, NETRUNNER_SETTINGS_EVENT_RUN_CHILD);
}

static void netrunner_settings_run_child(NetrunnerSettingsApp* app) {
    const NetrunnerInternalApplication* child = app->pending_child;
    app->pending_child = NULL;
    if(!child) return;

    netrunner_settings_flush(app);

    const uint8_t row = variable_item_list_get_selected_item_index(app->variable_item_list);

    FuriThread* thread =
        furi_thread_alloc_ex_foreground(child->name, child->stack_size, child->app, NULL);
    /* Apps resolve their data paths from the thread's appid. */
    furi_thread_set_appid(thread, child->appid);
    furi_thread_start(thread);
    furi_thread_join(thread);
    furi_thread_free(thread);

    netrunner_settings_reload_after_child(app);
    netrunner_settings_show_page(app, app->current_page, row);
}

static void netrunner_settings_list_enter(void* context, uint32_t index) {
    NetrunnerSettingsApp* app = context;
    furi_assert(app);

    if(app->current_page == NetrunnerSettingsPageRoot) {
        /* Flat top level, in display order: Misc, General, Wallpaper, Mainmenu,
         * Statusbar, Sound & Display, Protocols, Lock Screen, File Browser.
         * Sound & Display opens its own app. */
        if(index == 0U) {
            view_dispatcher_send_custom_event(app->view_dispatcher, NetrunnerSettingsPageMisc);
        } else if(index == 1U) {
            view_dispatcher_send_custom_event(app->view_dispatcher, NetrunnerSettingsPageGeneral);
        } else if(index == 2U) {
            view_dispatcher_send_custom_event(app->view_dispatcher, NetrunnerSettingsPageGraphics);
        } else if(index == 3U) {
            view_dispatcher_send_custom_event(app->view_dispatcher, NetrunnerSettingsPageMainmenu);
        } else if(index == 4U) {
            view_dispatcher_send_custom_event(app->view_dispatcher, NetrunnerSettingsPageStatusbar);
        } else if(index == 5U) {
            netrunner_settings_launch_app(app, "notification_settings");
        } else if(index == 6U) {
            view_dispatcher_send_custom_event(app->view_dispatcher, NetrunnerSettingsPageProtocols);
        } else if(index == 7U) {
            view_dispatcher_send_custom_event(app->view_dispatcher, NetrunnerSettingsPageLockscreen);
        } else if(index == 8U) {
            view_dispatcher_send_custom_event(app->view_dispatcher, NetrunnerSettingsPageFileBrowser);
        }
    } else if(app->current_page == NetrunnerSettingsPageMainmenu) {
        if(index == NetrunnerMainmenuRowReset) {
            netrunner_settings_mainmenu_show_reset(app);
        } else if(index == NetrunnerMainmenuRowAdd) {
            netrunner_settings_mainmenu_show_add_source(app);
        } else if(index == NetrunnerMainmenuRowRemove && app->mainmenu_count) {
            netrunner_settings_mainmenu_remove(app, app->mainmenu_index);
            app->mainmenu_dirty = true;
            netrunner_settings_mainmenu_refresh(app);
        }
    } else if(app->current_page == NetrunnerSettingsPageGraphics && index == 0U) {
        submenu_set_selected_item(
            app->asset_pack_submenu, netrunner_settings_asset_pack_index(app));
        view_dispatcher_switch_to_view(
            app->view_dispatcher, NetrunnerSettingsViewAssetPacks);
    } else if(app->current_page == NetrunnerSettingsPageGraphics && index == 1U) {
        netrunner_settings_show_wallpaper_list(app);
    } else if(app->current_page == NetrunnerSettingsPageGeneral && index == 3U) {
        with_view_model(
            app->set_time_view,
            SetTimeModel * m,
            {
                furi_hal_rtc_get_datetime(&m->datetime);
                m->field = 0;
            },
            true);
        view_dispatcher_switch_to_view(app->view_dispatcher, NetrunnerSettingsViewSetTime);
    } else if(app->current_page == NetrunnerSettingsPageProtocols && index == 0U) {
        netrunner_settings_launch_app(app, "bt_settings");
    } else if(app->current_page == NetrunnerSettingsPageProtocols && index == 3U) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, NetrunnerSettingsPageFrequencies);
    } else if(app->current_page == NetrunnerSettingsPageDolphin && index == 1U) {
        app->number_input_for_xp = true;
        number_input_set_header_text(app->number_input, "Dolphin XP");
        number_input_set_result_callback(
            app->number_input,
            netrunner_settings_number_done,
            app,
            (int32_t)app->dolphin_xp,
            0,
            (int32_t)NETRUNNER_DOLPHIN_XP_MAX);
        view_dispatcher_switch_to_view(app->view_dispatcher, NetrunnerSettingsViewNumberInput);
    } else if(app->current_page == NetrunnerSettingsPageFrequencies && index > 0U) {
        app->subghz_editing_hopper = (index == 2U);
        netrunner_settings_show_freq_list(app);
    } else if(app->current_page == NetrunnerSettingsPageMisc) {
        /* Alphabetical rows: Backup Settings, Device Name, Levels, Passport,
         * Power, Screen, Shell Color, Storage. Backup Settings, Passport, Power
         * and Storage are their own apps; Shell Color is an inline enum handled
         * by its change callback, so it needs no case here. */
        if(index == 0U) {
            netrunner_settings_launch_app(app, "backup_settings");
        } else if(index == 1U) {
            text_input_set_header_text(app->text_input, "Device Name");
            text_input_set_result_callback(
                app->text_input,
                netrunner_settings_device_name_done,
                app,
                app->device_name,
                sizeof(app->device_name),
                true);
            view_dispatcher_switch_to_view(
                app->view_dispatcher, NetrunnerSettingsViewTextInput);
        } else if(index == 2U) {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, NetrunnerSettingsPageDolphin);
        } else if(index == 3U) {
            netrunner_settings_launch_app(app, "passport");
        } else if(index == 4U) {
            netrunner_settings_launch_app(app, "power_settings");
        } else if(index == 5U) {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, NetrunnerSettingsPageScreen);
        } else if(index == 7U) {
            netrunner_settings_launch_app(app, "storage_settings");
        }
    }
}

static void netrunner_settings_anim_speed_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_anim_speed_text[index]);
    app->settings.anim_speed = netrunner_anim_speed_values[index];
    app->dirty = true;
}

static void netrunner_settings_cycle_anims_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_cycle_anim_text[index]);
    app->settings.cycle_anims = netrunner_cycle_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_hide_dualboot_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    /* Stored inverted from the label: the row reads "Dual Boot: Show/Hide",
     * the setting is hide_dualboot. */
    app->settings.hide_dualboot = (index != 0);
    app->dirty = true;
}

static void netrunner_settings_unlock_anims_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.unlock_anims = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_lockscreen_poweroff_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.lockscreen_poweroff = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_lockscreen_time_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.lockscreen_time = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_lockscreen_seconds_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.lockscreen_seconds = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_lockscreen_date_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.lockscreen_date = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_lockscreen_statusbar_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.lockscreen_statusbar = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_lockscreen_prompt_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.lockscreen_prompt = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_lockscreen_transparent_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.lockscreen_transparent = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_lockscreen_skip_animation_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.lockscreen_skip_animation = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_battery_icon_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_battery_icon_text[index]);
    app->desktop_settings.displayBatteryPercentage = netrunner_battery_icon_values[index];
    app->desktop_dirty = true;
}

static void netrunner_settings_clock_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->desktop_settings.display_clock = netrunner_clock_values[index];
    app->desktop_dirty = true;
}

static void netrunner_settings_status_icons_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.status_icons = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_bar_borders_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.bar_borders = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_bar_background_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.bar_background = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_sort_dirs_first_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.sort_dirs_first = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_show_hidden_files_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.show_hidden_files = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_show_internal_tab_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.show_internal_tab = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_menu_style_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_menu_style_text[index]);
    app->settings.menu_style = (MenuStyle)index;
    // Apply live so the main menu already uses the new style on Back.
    netrunner_settings.menu_style = app->settings.menu_style;
    app->dirty = true;
}

static void netrunner_settings_lock_on_boot_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.lock_on_boot = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_locked_rpc_usb_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.allow_locked_rpc_usb = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_locked_rpc_ble_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.allow_locked_rpc_ble = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_shell_color_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_shell_color_text[index]);
    /* Live RAM+BLE update; persisted to name.settings on app exit (name_dirty
     * drives the write, which now also stores the Color field). */
    furi_hal_version_set_hw_color(netrunner_shell_color_values[index]);
    app->name_dirty = true;
}

static void netrunner_settings_subghz_extended_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->subghz_extended_range = netrunner_unlock_anim_values[index];
    /* Written and applied straight away rather than on exit: this changes what
     * the radio is allowed to transmit, so it should not linger unsaved. */
    subghz_extended_range_save(app->subghz_extended_range);
}

static void netrunner_settings_browser_path_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_browser_path_text[index]);
    app->settings.browser_path_mode = (BrowserPathMode)index;
    app->dirty = true;
}

static void netrunner_settings_dark_mode_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.dark_mode = netrunner_unlock_anim_values[index];
    netrunner_settings.dark_mode = app->settings.dark_mode;
    app->dirty = true;
}

static void netrunner_settings_left_handed_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    if(netrunner_unlock_anim_values[index]) {
        furi_hal_rtc_set_flag(FuriHalRtcFlagHandOrient);
    } else {
        furi_hal_rtc_reset_flag(FuriHalRtcFlagHandOrient);
    }
}

static void netrunner_settings_file_naming_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, index ? "After" : "Before");
    app->settings.file_naming_prefix_after = index != 0U;
    app->dirty = true;
}

static void netrunner_settings_scroll_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, index ? "Marquee" : "Standard");
    app->settings.scroll_marquee = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_time_format_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_time_format_text[index]);
    locale_set_time_format((LocaleTimeFormat)netrunner_time_format_values[index]);
}

static void netrunner_settings_timezone_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    if(index >= COUNT_OF(netrunner_timezone_values)) return;

    variable_item_set_current_value_text(item, netrunner_timezone_text[index]);
    if(index == 0U) {
        /* Keep showing the last valid offset until the asynchronous refresh
         * succeeds; a network failure must never throw the clock back to UTC. */
        furi_hal_rtc_set_timezone(true, furi_hal_rtc_get_timezone_offset_minutes());
        wlan_hal_request_timezone_refresh();
    } else {
        furi_hal_rtc_set_timezone(false, (int16_t)netrunner_timezone_values[index]);
    }
}

/* Horizontal HH:MM:SS picker, centred on screen. One field is highlighted at a
 * time; arrows above/below it show it is the adjustable one. */
#define SET_TIME_BLOCK_W 28
#define SET_TIME_BLOCK_H 20
#define SET_TIME_BLOCK_STEP 36 /* block width + colon gap */
#define SET_TIME_ORIGIN_X 14 /* centres three blocks + two colons on 128px */
#define SET_TIME_BLOCK_Y ((64 - SET_TIME_BLOCK_H) / 2)

static void netrunner_settings_set_time_draw_block(
    Canvas* canvas,
    int32_t x,
    const char* text,
    bool focused) {
    if(focused) {
        canvas_draw_icon(
            canvas, x + SET_TIME_BLOCK_W / 2 - 1, SET_TIME_BLOCK_Y - 7, &I_SmallArrowUp_3x5);
        canvas_draw_icon(
            canvas,
            x + SET_TIME_BLOCK_W / 2 - 1,
            SET_TIME_BLOCK_Y + SET_TIME_BLOCK_H + 2,
            &I_SmallArrowDown_3x5);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rbox(canvas, x, SET_TIME_BLOCK_Y, SET_TIME_BLOCK_W, SET_TIME_BLOCK_H, 2);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_set_color(canvas, ColorBlack);
    }
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(
        canvas, x + SET_TIME_BLOCK_W / 2, 32, AlignCenter, AlignCenter, text);
    canvas_set_color(canvas, ColorBlack);
}

static void netrunner_settings_set_time_draw(Canvas* canvas, void* model) {
    SetTimeModel* m = model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 1, AlignCenter, AlignTop, "Set Time");

    char field[3][4];
    snprintf(field[0], sizeof(field[0]), "%02u", m->datetime.hour);
    snprintf(field[1], sizeof(field[1]), "%02u", m->datetime.minute);
    snprintf(field[2], sizeof(field[2]), "%02u", m->datetime.second);

    for(uint8_t i = 0; i < 3; i++) {
        netrunner_settings_set_time_draw_block(
            canvas, SET_TIME_ORIGIN_X + i * SET_TIME_BLOCK_STEP, field[i], m->field == i);
    }

    /* Colons as stacked dots (FontBigNumbers has no good ':' glyph), centred in
     * each gap between the blocks. */
    canvas_set_color(canvas, ColorBlack);
    for(uint8_t i = 1; i <= 2; i++) {
        const int32_t cx = SET_TIME_ORIGIN_X + i * SET_TIME_BLOCK_STEP - 5;
        canvas_draw_box(canvas, cx, SET_TIME_BLOCK_Y + 6, 2, 2);
        canvas_draw_box(canvas, cx, SET_TIME_BLOCK_Y + 12, 2, 2);
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: next   Back: save");
}

/* Encoder-friendly flow: rotate (Up/Down) sets the focused field, OK cycles the
 * focus Hours -> Minutes -> Seconds -> Hours, and Back applies the time and
 * exits. Left/Right (encoder held + rotated) also jump between fields. */
static bool netrunner_settings_set_time_input(InputEvent* event, void* context) {
    NetrunnerSettingsApp* app = context;

    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    bool consumed = true;
    bool apply = false;
    DateTime applied = {0};

    with_view_model(
        app->set_time_view,
        SetTimeModel * m,
        {
            uint8_t* value = (m->field == 0) ? &m->datetime.hour :
                             (m->field == 1) ? &m->datetime.minute :
                                               &m->datetime.second;
            const uint8_t wrap = (m->field == 0) ? 24U : 60U;

            switch(event->key) {
            case InputKeyUp:
                *value = (uint8_t)((*value + 1U) % wrap);
                break;
            case InputKeyDown:
                *value = (uint8_t)((*value + wrap - 1U) % wrap);
                break;
            case InputKeyLeft:
                if(m->field > 0) m->field--;
                break;
            case InputKeyRight:
                if(m->field < 2) m->field++;
                break;
            case InputKeyOk:
                m->field = (m->field + 1U) % 3U;
                break;
            case InputKeyBack:
                apply = true;
                applied = m->datetime;
                break;
            default:
                consumed = false;
                break;
            }
        },
        true);

    if(apply) {
        /* The date is preserved from the RTC, but an unset clock reads back as
         * 1970, which fails validation (year < 2000) and would silently drop
         * the whole set. Substitute a valid placeholder date so the time the
         * user entered is actually applied. */
        if(!datetime_validate_datetime(&applied)) {
            applied.year = 2024;
            applied.month = 1;
            applied.day = 1;
            applied.weekday = 1;
        }
        furi_hal_rtc_set_datetime(&applied);
        netrunner_settings_show_page(app, NetrunnerSettingsPageGeneral, 3);
    }

    return consumed;
}

/* System settings relocated into the General page. Each row mirrors the old
 * standalone System app: a value list whose handler writes straight through to
 * furi_hal_rtc / locale, with no Netrunner-settings persistence of its own. */
static const char* const sys_measurement_units_text[] = {"Metric", "Imperial"};
static const uint32_t sys_measurement_units_value[] = {
    LocaleMeasurementUnitsMetric,
    LocaleMeasurementUnitsImperial,
};
static const char* const sys_date_format_text[] = {"D/M/Y", "M/D/Y", "Y/M/D"};
static const uint32_t sys_date_format_value[] = {
    LocaleDateFormatDMY,
    LocaleDateFormatMDY,
    LocaleDateFormatYMD,
};
static const char* const sys_log_level_text[] =
    {"Default", "None", "Error", "Warning", "Info", "Debug", "Trace"};
static const uint32_t sys_log_level_value[] = {
    FuriLogLevelDefault,
    FuriLogLevelNone,
    FuriLogLevelError,
    FuriLogLevelWarn,
    FuriLogLevelInfo,
    FuriLogLevelDebug,
    FuriLogLevelTrace,
};
static const char* const sys_log_device_text[] = {"USART", "LPUART", "None"};
static const uint32_t sys_log_device_value[] = {
    FuriHalRtcLogDeviceUsart,
    FuriHalRtcLogDeviceLpuart,
    FuriHalRtcLogDeviceNone,
};
static const char* const sys_log_baud_rate_text[] =
    {"9600", "38400", "57600", "115200", "230400", "460800", "921600", "1843200"};
static const uint32_t sys_log_baud_rate_value[] = {
    FuriHalRtcLogBaudRate9600,
    FuriHalRtcLogBaudRate38400,
    FuriHalRtcLogBaudRate57600,
    FuriHalRtcLogBaudRate115200,
    FuriHalRtcLogBaudRate230400,
    FuriHalRtcLogBaudRate460800,
    FuriHalRtcLogBaudRate921600,
    FuriHalRtcLogBaudRate1843200,
};
static const char* const sys_debug_text[] = {"OFF", "ON"};
static const char* const sys_heap_trace_text[] = {
    "None",
    "Main",
#ifdef FURI_DEBUG
    "Tree",
    "All",
#endif
};
static const uint32_t sys_heap_trace_value[] = {
    FuriHalRtcHeapTrackModeNone,
    FuriHalRtcHeapTrackModeMain,
#ifdef FURI_DEBUG
    FuriHalRtcHeapTrackModeTree,
    FuriHalRtcHeapTrackModeAll,
#endif
};
static const char* const sys_sleep_method_text[] = {"Default", "Legacy"};

static void netrunner_settings_sys_units_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, sys_measurement_units_text[index]);
    locale_set_measurement_unit(sys_measurement_units_value[index]);
}

static void netrunner_settings_sys_date_format_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, sys_date_format_text[index]);
    locale_set_date_format(sys_date_format_value[index]);
}

static void netrunner_settings_sys_log_level_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, sys_log_level_text[index]);
    furi_hal_rtc_set_log_level(sys_log_level_value[index]);
}

static void netrunner_settings_sys_log_device_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, sys_log_device_text[index]);
    furi_hal_rtc_set_log_device(sys_log_device_value[index]);
}

static void netrunner_settings_sys_log_baud_rate_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, sys_log_baud_rate_text[index]);
    furi_hal_rtc_set_log_baud_rate(sys_log_baud_rate_value[index]);
}

static void netrunner_settings_sys_debug_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, sys_debug_text[index]);
    if(index) {
        furi_hal_rtc_set_flag(FuriHalRtcFlagDebug);
    } else {
        furi_hal_rtc_reset_flag(FuriHalRtcFlagDebug);
    }
}

static void netrunner_settings_sys_heap_trace_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, sys_heap_trace_text[index]);
    furi_hal_rtc_set_heap_track_mode(sys_heap_trace_value[index]);
}

static void netrunner_settings_sys_sleep_method_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, sys_sleep_method_text[index]);
    if(index) {
        furi_hal_rtc_set_flag(FuriHalRtcFlagLegacySleep);
    } else {
        furi_hal_rtc_reset_flag(FuriHalRtcFlagLegacySleep);
    }
}

static void netrunner_settings_midnight_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, index ? "00:XX" : "12:XX");
    app->settings.midnight_format_00 = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

static void netrunner_settings_popup_overlay_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->settings.popup_overlay = netrunner_unlock_anim_values[index];
    app->dirty = true;
}

/* ---- Main menu layout ---------------------------------------------------- */

static bool
    netrunner_settings_mainmenu_push(NetrunnerSettingsApp* app, const char* label, const char* line) {
    if(app->mainmenu_count >= NETRUNNER_MAX_MAINMENU_ITEMS) return false;

    char* label_copy = strdup(label);
    char* line_copy = strdup(line);
    if(!label_copy || !line_copy) {
        free(label_copy);
        free(line_copy);
        return false;
    }

    app->mainmenu_labels[app->mainmenu_count] = label_copy;
    app->mainmenu_lines[app->mainmenu_count] = line_copy;
    app->mainmenu_count++;
    return true;
}

/* The pinned app is always in the menu and is not part of the editable list,
 * so it is skipped wherever the built-in apps are enumerated. */
static bool netrunner_settings_mainmenu_is_pinned(size_t index) {
    return !strcmp(NETRUNNER_EXTERNAL_APPS[index].path, MAINMENU_PINNED_APPID);
}

static void netrunner_settings_mainmenu_load_defaults(NetrunnerSettingsApp* app) {
    for(size_t i = 0; i < NETRUNNER_APPS_COUNT; i++) {
        netrunner_settings_mainmenu_push(app, NETRUNNER_APPS[i].name, NETRUNNER_APPS[i].name);
    }
    for(size_t i = 0; i < NETRUNNER_EXTERNAL_APPS_COUNT; i++) {
        if(netrunner_settings_mainmenu_is_pinned(i)) continue;
        netrunner_settings_mainmenu_push(
            app, NETRUNNER_EXTERNAL_APPS[i].name, NETRUNNER_EXTERNAL_APPS[i].name);
    }
}

/* Mirrors the parse in components/loader/loader_menu.c, so what this page shows
 * is what the menu will build. */
static void netrunner_settings_mainmenu_add_line(NetrunnerSettingsApp* app, FuriString* line) {
    if(furi_string_start_with(line, "/")) {
        FuriString* label = furi_string_alloc();
        const Icon* icon;
        if(loader_menu_load_fap_meta(app->storage, line, label, &icon)) {
            loader_menu_free_fap_icon(icon);
        } else {
            furi_string_set(label, line);
        }
        netrunner_settings_mainmenu_push(
            app, furi_string_get_cstr(label), furi_string_get_cstr(line));
        furi_string_free(label);
        return;
    }

    for(size_t i = 0; i < NETRUNNER_APPS_COUNT; i++) {
        if(furi_string_equal(line, NETRUNNER_APPS[i].name)) {
            netrunner_settings_mainmenu_push(app, NETRUNNER_APPS[i].name, NETRUNNER_APPS[i].name);
            return;
        }
    }

    for(size_t i = 0; i < NETRUNNER_EXTERNAL_APPS_COUNT; i++) {
        if(netrunner_settings_mainmenu_is_pinned(i)) continue;
        if(furi_string_equal(line, NETRUNNER_EXTERNAL_APPS[i].name)) {
            netrunner_settings_mainmenu_push(
                app, NETRUNNER_EXTERNAL_APPS[i].name, NETRUNNER_EXTERNAL_APPS[i].name);
            return;
        }
    }
}

static void netrunner_settings_mainmenu_clear(NetrunnerSettingsApp* app) {
    for(uint8_t i = 0; i < app->mainmenu_count; i++) {
        free(app->mainmenu_labels[i]);
        free(app->mainmenu_lines[i]);
    }
    app->mainmenu_count = 0;
}

/* Mirrors loader_menu_mark_seen(): shown and removed entries both mean the file
 * knew about the app, so it is not new. */
static void netrunner_settings_mainmenu_mark_seen(
    FuriString* name,
    bool* seen_internal,
    bool* seen_external) {
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

static void netrunner_settings_mainmenu_load(NetrunnerSettingsApp* app) {
    Stream* stream = file_stream_alloc(app->storage);
    FuriString* line = furi_string_alloc();
    unsigned long version;

    bool* seen_internal = calloc(NETRUNNER_APPS_COUNT, sizeof(bool));
    bool* seen_external = calloc(NETRUNNER_EXTERNAL_APPS_COUNT, sizeof(bool));

    if(file_stream_open(stream, MAINMENU_APPS_PATH, FSAM_READ, FSOM_OPEN_EXISTING) &&
       stream_read_line(stream, line) &&
       sscanf(furi_string_get_cstr(line), MAINMENU_APPS_HEADER_FMT, &version) == 1 &&
       version >= MAINMENU_APPS_VERSION_MIN && version <= MAINMENU_APPS_VERSION) {
        while(stream_read_line(stream, line)) {
            furi_string_trim(line);
            if(!furi_string_size(line)) continue;

            if(furi_string_get_char(line, 0) == MAINMENU_REMOVED_PREFIX) {
                furi_string_right(line, 1);
                netrunner_settings_mainmenu_mark_seen(line, seen_internal, seen_external);
                continue;
            }

            netrunner_settings_mainmenu_mark_seen(line, seen_internal, seen_external);
            netrunner_settings_mainmenu_add_line(app, line);
        }

        /* Same rule as the menu itself: an app the file never mentioned is newer
         * than the file, so show it rather than hide it. The editor has to agree
         * with the menu, or saving would write the new app straight back out as
         * removed. Path entries are never built-ins, so they need no accounting. */
        FuriString* name = furi_string_alloc();
        for(size_t i = 0; i < NETRUNNER_APPS_COUNT; i++) {
            if(seen_internal[i]) continue;
            furi_string_set_str(name, NETRUNNER_APPS[i].name);
            netrunner_settings_mainmenu_add_line(app, name);
        }
        for(size_t i = 0; i < NETRUNNER_EXTERNAL_APPS_COUNT; i++) {
            if(seen_external[i] || netrunner_settings_mainmenu_is_pinned(i)) continue;
            furi_string_set_str(name, NETRUNNER_EXTERNAL_APPS[i].name);
            netrunner_settings_mainmenu_add_line(app, name);
        }
        furi_string_free(name);
    } else {
        netrunner_settings_mainmenu_load_defaults(app);
    }

    free(seen_internal);
    free(seen_external);

    furi_string_free(line);
    file_stream_close(stream);
    stream_free(stream);
}

static bool netrunner_settings_mainmenu_has_line(NetrunnerSettingsApp* app, const char* line) {
    for(uint8_t i = 0; i < app->mainmenu_count; i++) {
        if(!strcmp(app->mainmenu_lines[i], line)) return true;
    }
    return false;
}

static void netrunner_settings_mainmenu_save(NetrunnerSettingsApp* app) {
    Stream* stream = file_stream_alloc(app->storage);

    if(file_stream_open(stream, MAINMENU_APPS_PATH, FSAM_READ_WRITE, FSOM_CREATE_ALWAYS)) {
        stream_write_format(stream, MAINMENU_APPS_HEADER_FMT "\n", MAINMENU_APPS_VERSION);
        for(uint8_t i = 0; i < app->mainmenu_count; i++) {
            stream_write_format(stream, "%s\n", app->mainmenu_lines[i]);
        }

        /* Record the built-ins that are NOT in the menu, so a later build can tell a
         * deliberate removal from an app that did not exist yet, and append only the
         * latter. Without these lines every removal would come back on next boot. */
        for(size_t i = 0; i < NETRUNNER_APPS_COUNT; i++) {
            if(netrunner_settings_mainmenu_has_line(app, NETRUNNER_APPS[i].name)) continue;
            stream_write_format(
                stream, "%c%s\n", MAINMENU_REMOVED_PREFIX, NETRUNNER_APPS[i].name);
        }
        for(size_t i = 0; i < NETRUNNER_EXTERNAL_APPS_COUNT; i++) {
            if(netrunner_settings_mainmenu_is_pinned(i)) continue;
            if(netrunner_settings_mainmenu_has_line(app, NETRUNNER_EXTERNAL_APPS[i].name)) continue;
            stream_write_format(
                stream, "%c%s\n", MAINMENU_REMOVED_PREFIX, NETRUNNER_EXTERNAL_APPS[i].name);
        }
    } else {
        FURI_LOG_E(TAG, "Main menu layout could not be saved");
    }

    file_stream_close(stream);
    stream_free(stream);
}

static void netrunner_settings_mainmenu_remove(NetrunnerSettingsApp* app, uint8_t index) {
    if(index >= app->mainmenu_count) return;

    free(app->mainmenu_labels[index]);
    free(app->mainmenu_lines[index]);

    const size_t tail = (app->mainmenu_count - index - 1U) * sizeof(char*);
    memmove(&app->mainmenu_labels[index], &app->mainmenu_labels[index + 1U], tail);
    memmove(&app->mainmenu_lines[index], &app->mainmenu_lines[index + 1U], tail);
    app->mainmenu_count--;
}

static void netrunner_settings_mainmenu_swap(NetrunnerSettingsApp* app, uint8_t a, uint8_t b) {
    char* label = app->mainmenu_labels[a];
    char* line = app->mainmenu_lines[a];
    app->mainmenu_labels[a] = app->mainmenu_labels[b];
    app->mainmenu_lines[a] = app->mainmenu_lines[b];
    app->mainmenu_labels[b] = label;
    app->mainmenu_lines[b] = line;
}

/* Rewrites the rows that depend on the list after it changes, so add, remove
 * and move do not each have to rebuild the page. */
static void netrunner_settings_mainmenu_refresh(NetrunnerSettingsApp* app) {
    VariableItem* item = variable_item_list_get(app->variable_item_list, NetrunnerMainmenuRowItem);
    if(!item) return;

    if(app->mainmenu_count) {
        if(app->mainmenu_index >= app->mainmenu_count) {
            app->mainmenu_index = app->mainmenu_count - 1U;
        }
        snprintf(
            app->mainmenu_item_label,
            sizeof(app->mainmenu_item_label),
            "Item %u/%u",
            (unsigned)(app->mainmenu_index + 1U),
            (unsigned)app->mainmenu_count);
        variable_item_set_current_value_text(item, app->mainmenu_labels[app->mainmenu_index]);
    } else {
        app->mainmenu_index = 0;
        snprintf(app->mainmenu_item_label, sizeof(app->mainmenu_item_label), "Item");
        variable_item_set_current_value_text(item, "None");
    }
    variable_item_set_item_label(item, app->mainmenu_item_label);
    variable_item_set_values_count(item, app->mainmenu_count);
    variable_item_set_current_value_index(item, app->mainmenu_index);

    VariableItem* move = variable_item_list_get(app->variable_item_list, NetrunnerMainmenuRowMove);
    if(move) {
        variable_item_set_locked(
            move, app->mainmenu_count < 2U, "Need at least\ntwo items\nto move");
    }
}

static void netrunner_settings_mainmenu_item_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    app->mainmenu_index = variable_item_get_current_value_index(item);
    netrunner_settings_mainmenu_refresh(app);
}

static const char* const netrunner_mainmenu_move_text[] = {
    "Up",
    "Move",
    "Down",
};

static void netrunner_settings_mainmenu_move_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    const uint8_t direction = variable_item_get_current_value_index(item);

    if(direction == 0U && app->mainmenu_index > 0U) {
        netrunner_settings_mainmenu_swap(app, app->mainmenu_index, app->mainmenu_index - 1U);
        app->mainmenu_index--;
        app->mainmenu_dirty = true;
    } else if(direction == 2U && app->mainmenu_index + 1U < app->mainmenu_count) {
        netrunner_settings_mainmenu_swap(app, app->mainmenu_index, app->mainmenu_index + 1U);
        app->mainmenu_index++;
        app->mainmenu_dirty = true;
    }

    /* Snaps back to the neutral value so the next turn in either direction is
     * another single step. */
    variable_item_set_current_value_index(item, 1U);
    variable_item_set_current_value_text(item, netrunner_mainmenu_move_text[1]);
    netrunner_settings_mainmenu_refresh(app);
}

static void netrunner_settings_mainmenu_submenu_callback(void* context, uint32_t index);

/* Built-in apps are offered in one flat list, so the submenu index has to map
 * back through the same enumeration order the picker was built with. */
static const char* netrunner_settings_mainmenu_builtin_name(uint32_t index) {
    if(index < NETRUNNER_APPS_COUNT) return NETRUNNER_APPS[index].name;

    uint32_t remaining = index - NETRUNNER_APPS_COUNT;
    for(size_t i = 0; i < NETRUNNER_EXTERNAL_APPS_COUNT; i++) {
        if(netrunner_settings_mainmenu_is_pinned(i)) continue;
        if(remaining == 0U) return NETRUNNER_EXTERNAL_APPS[i].name;
        remaining--;
    }

    return NULL;
}

static void netrunner_settings_mainmenu_show_add_source(NetrunnerSettingsApp* app) {
    app->mainmenu_screen = NetrunnerMainmenuScreenAddSource;
    submenu_reset(app->mainmenu_submenu);
    submenu_set_header(app->mainmenu_submenu, "Add Menu Item");
    submenu_add_item(
        app->mainmenu_submenu, "Built-in App", 0, netrunner_settings_mainmenu_submenu_callback, app);
    submenu_add_item(
        app->mainmenu_submenu,
        "App or Script",
        1,
        netrunner_settings_mainmenu_submenu_callback,
        app);
    view_dispatcher_switch_to_view(app->view_dispatcher, NetrunnerSettingsViewMainmenu);
}

static void netrunner_settings_mainmenu_show_add_builtin(NetrunnerSettingsApp* app) {
    app->mainmenu_screen = NetrunnerMainmenuScreenAddBuiltin;
    submenu_reset(app->mainmenu_submenu);
    submenu_set_header(app->mainmenu_submenu, "Built-in App");

    uint32_t index = 0;
    for(size_t i = 0; i < NETRUNNER_APPS_COUNT; i++) {
        submenu_add_item(
            app->mainmenu_submenu,
            NETRUNNER_APPS[i].name,
            index++,
            netrunner_settings_mainmenu_submenu_callback,
            app);
    }
    for(size_t i = 0; i < NETRUNNER_EXTERNAL_APPS_COUNT; i++) {
        if(netrunner_settings_mainmenu_is_pinned(i)) continue;
        submenu_add_item(
            app->mainmenu_submenu,
            NETRUNNER_EXTERNAL_APPS[i].name,
            index++,
            netrunner_settings_mainmenu_submenu_callback,
            app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, NetrunnerSettingsViewMainmenu);
}

/* A submenu rather than a DialogEx: DialogEx puts its buttons on Left and
 * Right, which on this board means holding the encoder while turning, with no
 * on-screen hint that this is what a confirmation needs. */
static void netrunner_settings_mainmenu_show_reset(NetrunnerSettingsApp* app) {
    app->mainmenu_screen = NetrunnerMainmenuScreenResetConfirm;
    submenu_reset(app->mainmenu_submenu);
    submenu_set_header(app->mainmenu_submenu, "Reset menu items?");
    submenu_add_item(
        app->mainmenu_submenu, "Cancel", 0, netrunner_settings_mainmenu_submenu_callback, app);
    submenu_add_item(
        app->mainmenu_submenu, "Reset", 1, netrunner_settings_mainmenu_submenu_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, NetrunnerSettingsViewMainmenu);
}

static void netrunner_settings_mainmenu_add_file(NetrunnerSettingsApp* app) {
    /* Keep the picker responsive even with hundreds of apps. The chosen FAP's
     * metadata is read once by netrunner_settings_mainmenu_add_line(); opening
     * every ELF merely to populate this list is unnecessary SD I/O. */
    DialogsFileBrowserOptions options = {
        .extension = ".fap|.js",
        .base_path = EXT_PATH("apps"),
        .skip_assets = true,
        .hide_dot_files = true,
        .icon = &I_file_10px,
        .hide_ext = true,
        .item_loader_callback = NULL,
        .item_loader_context = NULL,
    };

    FuriString* path = furi_string_alloc_set_str(options.base_path);
    if(dialog_file_browser_show(app->dialogs, path, path, &options)) {
        const uint8_t before = app->mainmenu_count;
        netrunner_settings_mainmenu_add_line(app, path);
        if(app->mainmenu_count > before) {
            app->mainmenu_index = app->mainmenu_count - 1U;
            app->mainmenu_dirty = true;
        }
    }
    furi_string_free(path);

    netrunner_settings_show_page(app, NetrunnerSettingsPageMainmenu, NetrunnerMainmenuRowAdd);
}

static void netrunner_settings_mainmenu_submenu_callback(void* context, uint32_t index) {
    NetrunnerSettingsApp* app = context;
    uint8_t return_row = NetrunnerMainmenuRowAdd;

    switch(app->mainmenu_screen) {
    case NetrunnerMainmenuScreenAddSource:
        /* Both branches move to another screen of their own. */
        if(index == 0U) {
            netrunner_settings_mainmenu_show_add_builtin(app);
        } else {
            netrunner_settings_mainmenu_add_file(app);
        }
        return;

    case NetrunnerMainmenuScreenAddBuiltin: {
        const char* name = netrunner_settings_mainmenu_builtin_name(index);
        if(name && netrunner_settings_mainmenu_push(app, name, name)) {
            app->mainmenu_index = app->mainmenu_count - 1U;
            app->mainmenu_dirty = true;
        }
        break;
    }

    case NetrunnerMainmenuScreenResetConfirm:
        return_row = NetrunnerMainmenuRowReset;
        if(index == 1U) {
            storage_simply_remove(app->storage, MAINMENU_APPS_PATH);
            netrunner_settings_mainmenu_clear(app);
            netrunner_settings_mainmenu_load_defaults(app);
            app->mainmenu_index = 0;
            /* The file is gone and the list is back to the default order, so
             * there is nothing left for the exit path to write. */
            app->mainmenu_dirty = false;
        }
        break;
    }

    netrunner_settings_show_page(app, NetrunnerSettingsPageMainmenu, return_row);
}

static uint32_t netrunner_settings_back_to_list(void* context) {
    UNUSED(context);
    return NetrunnerSettingsViewList;
}

static void netrunner_settings_lock_if_storage_unavailable(
    const NetrunnerSettingsApp* app,
    VariableItem* item) {
    if(!app->sd_ready) variable_item_set_locked(item, true, "SD card required");
}

static void netrunner_settings_add_bool_item(
    NetrunnerSettingsApp* app,
    const char* label,
    bool value,
    VariableItemChangeCallback callback) {
    VariableItem* item = variable_item_list_add(
        app->variable_item_list,
        label,
        COUNT_OF(netrunner_unlock_anim_values),
        callback,
        app);
    uint8_t value_index = value ? 1U : 0U;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);
}

static void netrunner_settings_show_page(
    NetrunnerSettingsApp* app,
    NetrunnerSettingsPage page,
    uint8_t selected_item) {
    furi_assert(app);

    variable_item_list_reset(app->variable_item_list);
    app->asset_pack_item = NULL;
    app->current_page = page;

    VariableItem* item;
    uint8_t value_index;

    if(page == NetrunnerSettingsPageRoot) {
        variable_item_list_set_header(app->variable_item_list, "Settings");
        /* Flat top level: every page is one tap from here, in the chosen order.
         * The enter handler maps each index. Sound & Display opens its own app;
         * the rest are pages in this one. The former System page is gone -- its
         * items live under General now. */
        variable_item_list_add(app->variable_item_list, "Misc", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "General", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Wallpaper", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Mainmenu", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Statusbar", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Sound & Display", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Protocols", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Lock Screen", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "File Browser", 1, NULL, app);
    } else if(page == NetrunnerSettingsPageMainmenu) {
        variable_item_list_set_header(app->variable_item_list, "Mainmenu");

        item = variable_item_list_add(
            app->variable_item_list,
            "Menu Style",
            COUNT_OF(netrunner_menu_style_text),
            netrunner_settings_menu_style_changed,
            app);
        value_index = (uint8_t)app->settings.menu_style;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_menu_style_text[value_index]);

        variable_item_list_add(app->variable_item_list, "Reset Menu", 1, NULL, app);

        variable_item_list_add(
            app->variable_item_list,
            "Item",
            app->mainmenu_count,
            netrunner_settings_mainmenu_item_changed,
            app);

        variable_item_list_add(app->variable_item_list, "Add Item", 1, NULL, app);

        /* Placed on the Mainmenu page because that is what it changes: the
         * Dual Boot entry reboots into another firmware, which is not
         * something to leave one OK press away on a shared device. */
        item = variable_item_list_add(
            app->variable_item_list,
            "Hide Dual Boot",
            COUNT_OF(netrunner_unlock_anim_text),
            netrunner_settings_hide_dualboot_changed,
            app);
        value_index = app->settings.hide_dualboot ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Move Item",
            COUNT_OF(netrunner_mainmenu_move_text),
            netrunner_settings_mainmenu_move_changed,
            app);
        variable_item_set_current_value_index(item, 1U);
        variable_item_set_current_value_text(item, netrunner_mainmenu_move_text[1]);

        variable_item_list_add(app->variable_item_list, "Remove Item", 1, NULL, app);

        /* Fills in the Item row's label and value and the Move lock from the
         * list, so those three places are written in one function only. */
        netrunner_settings_mainmenu_refresh(app);
    } else if(page == NetrunnerSettingsPageGraphics) {
        variable_item_list_set_header(app->variable_item_list, "Wallpaper");

        app->asset_pack_item = variable_item_list_add(
            app->variable_item_list, "Asset Pack", 1, NULL, app);
        netrunner_settings_update_asset_pack_item(app);
        netrunner_settings_lock_if_storage_unavailable(app, app->asset_pack_item);

        app->wallpaper_item = variable_item_list_add(
            app->variable_item_list, "Wallpaper", 1, NULL, app);
        netrunner_settings_update_wallpaper_item(app);
        netrunner_settings_lock_if_storage_unavailable(app, app->wallpaper_item);

        item = variable_item_list_add(
            app->variable_item_list,
            "Anim Speed",
            COUNT_OF(netrunner_anim_speed_values),
            netrunner_settings_anim_speed_changed,
            app);
        value_index = (uint8_t)value_index_uint32(
            app->settings.anim_speed,
            netrunner_anim_speed_values,
            COUNT_OF(netrunner_anim_speed_values));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_anim_speed_text[value_index]);
        netrunner_settings_lock_if_storage_unavailable(app, item);

        item = variable_item_list_add(
            app->variable_item_list,
            "Cycle Anims",
            COUNT_OF(netrunner_cycle_anim_values),
            netrunner_settings_cycle_anims_changed,
            app);
        value_index = (uint8_t)value_index_int32(
            app->settings.cycle_anims,
            netrunner_cycle_anim_values,
            COUNT_OF(netrunner_cycle_anim_values));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_cycle_anim_text[value_index]);
        netrunner_settings_lock_if_storage_unavailable(app, item);

        item = variable_item_list_add(
            app->variable_item_list,
            "Unlock Anims",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_unlock_anims_changed,
            app);
        value_index = app->settings.unlock_anims ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);
        netrunner_settings_lock_if_storage_unavailable(app, item);
    } else if(page == NetrunnerSettingsPageLockscreen) {
        variable_item_list_set_header(app->variable_item_list, "Lock Screen");

        netrunner_settings_add_bool_item(
            app,
            "Lock On Boot",
            app->settings.lock_on_boot,
            netrunner_settings_lock_on_boot_changed);
        netrunner_settings_add_bool_item(
            app,
            "Locked USB RPC",
            app->settings.allow_locked_rpc_usb,
            netrunner_settings_locked_rpc_usb_changed);
        netrunner_settings_add_bool_item(
            app,
            "Locked BLE RPC",
            app->settings.allow_locked_rpc_ble,
            netrunner_settings_locked_rpc_ble_changed);
        netrunner_settings_add_bool_item(
            app,
            "Allow Poweroff",
            app->settings.lockscreen_poweroff,
            netrunner_settings_lockscreen_poweroff_changed);
        netrunner_settings_add_bool_item(
            app,
            "Show Time",
            app->settings.lockscreen_time,
            netrunner_settings_lockscreen_time_changed);
        netrunner_settings_add_bool_item(
            app,
            "Show Seconds",
            app->settings.lockscreen_seconds,
            netrunner_settings_lockscreen_seconds_changed);
        netrunner_settings_add_bool_item(
            app,
            "Show Date",
            app->settings.lockscreen_date,
            netrunner_settings_lockscreen_date_changed);
        netrunner_settings_add_bool_item(
            app,
            "Show Statusbar",
            app->settings.lockscreen_statusbar,
            netrunner_settings_lockscreen_statusbar_changed);
        netrunner_settings_add_bool_item(
            app,
            "Unlock Prompt",
            app->settings.lockscreen_prompt,
            netrunner_settings_lockscreen_prompt_changed);
        netrunner_settings_add_bool_item(
            app,
            "Transparent (see animation)",
            app->settings.lockscreen_transparent,
            netrunner_settings_lockscreen_transparent_changed);
        netrunner_settings_add_bool_item(
            app,
            "Skip Sliding Animation",
            app->settings.lockscreen_skip_animation,
            netrunner_settings_lockscreen_skip_animation_changed);
    } else if(page == NetrunnerSettingsPageStatusbar) {
        variable_item_list_set_header(app->variable_item_list, "Statusbar");

        item = variable_item_list_add(
            app->variable_item_list,
            "Battery Icon",
            COUNT_OF(netrunner_battery_icon_values),
            netrunner_settings_battery_icon_changed,
            app);
        value_index = (uint8_t)value_index_uint32(
            app->desktop_settings.displayBatteryPercentage,
            netrunner_battery_icon_values,
            COUNT_OF(netrunner_battery_icon_values));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_battery_icon_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Show Clock",
            COUNT_OF(netrunner_clock_values),
            netrunner_settings_clock_changed,
            app);
        value_index = (uint8_t)value_index_uint32(
            app->desktop_settings.display_clock,
            netrunner_clock_values,
            COUNT_OF(netrunner_clock_values));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Status Icons",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_status_icons_changed,
            app);
        value_index = app->settings.status_icons ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Bar Borders",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_bar_borders_changed,
            app);
        value_index = app->settings.bar_borders ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Bar Background",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_bar_background_changed,
            app);
        value_index = app->settings.bar_background ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);
    } else if(page == NetrunnerSettingsPageFileBrowser) {
        variable_item_list_set_header(app->variable_item_list, "File Browser");

        item = variable_item_list_add(
            app->variable_item_list,
            "Folders Above Files",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_sort_dirs_first_changed,
            app);
        value_index = app->settings.sort_dirs_first ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Show Hidden Files",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_show_hidden_files_changed,
            app);
        value_index = app->settings.show_hidden_files ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Show Internal Tab",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_show_internal_tab_changed,
            app);
        value_index = app->settings.show_internal_tab ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Show Path",
            COUNT_OF(netrunner_browser_path_text),
            netrunner_settings_browser_path_changed,
            app);
        value_index = (uint8_t)app->settings.browser_path_mode;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_browser_path_text[value_index]);

    } else if(page == NetrunnerSettingsPageGeneral) {
        variable_item_list_set_header(app->variable_item_list, "General");

        item = variable_item_list_add(
            app->variable_item_list,
            "Text Scroll",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_scroll_changed,
            app);
        value_index = app->settings.scroll_marquee ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, value_index ? "Marquee" : "Standard");

        item = variable_item_list_add(
            app->variable_item_list,
            "Time Format",
            COUNT_OF(netrunner_time_format_values),
            netrunner_settings_time_format_changed,
            app);
        value_index = (uint8_t)value_index_uint32(
            locale_get_time_format(),
            netrunner_time_format_values,
            COUNT_OF(netrunner_time_format_values));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_time_format_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Time Zone",
            COUNT_OF(netrunner_timezone_values),
            netrunner_settings_timezone_changed,
            app);
        if(furi_hal_rtc_get_timezone_auto()) {
            value_index = 0U;
        } else {
            value_index = (uint8_t)(1U + value_index_int32(
                                             furi_hal_rtc_get_timezone_offset_minutes(),
                                             &netrunner_timezone_values[1],
                                             COUNT_OF(netrunner_timezone_values) - 1U));
        }
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_timezone_text[value_index]);

        item = variable_item_list_add(app->variable_item_list, "Set Time", 1, NULL, app);
        variable_item_set_current_value_text(item, "Press OK");

        item = variable_item_list_add(
            app->variable_item_list,
            "Midnight Format",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_midnight_changed,
            app);
        value_index = app->settings.midnight_format_00 ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, value_index ? "00:XX" : "12:XX");

        item = variable_item_list_add(
            app->variable_item_list,
            "Popup Overlay",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_popup_overlay_changed,
            app);
        value_index = app->settings.popup_overlay ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);

        /* Relocated from the former System page. These write through to
         * furi_hal_rtc / locale and are initialised from their live values, so
         * they carry no Netrunner-settings state. Time Format lives above, Hand
         * Orient is "Left Handed" under Screen, and File Naming is under
         * Protocols, so none of those are repeated here. */
        item = variable_item_list_add(
            app->variable_item_list,
            "Units",
            COUNT_OF(sys_measurement_units_text),
            netrunner_settings_sys_units_changed,
            app);
        value_index = (uint8_t)value_index_uint32(
            locale_get_measurement_unit(),
            sys_measurement_units_value,
            COUNT_OF(sys_measurement_units_value));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, sys_measurement_units_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Date Format",
            COUNT_OF(sys_date_format_text),
            netrunner_settings_sys_date_format_changed,
            app);
        value_index = (uint8_t)value_index_uint32(
            locale_get_date_format(), sys_date_format_value, COUNT_OF(sys_date_format_value));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, sys_date_format_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Log Level",
            COUNT_OF(sys_log_level_text),
            netrunner_settings_sys_log_level_changed,
            app);
        value_index = (uint8_t)value_index_uint32(
            furi_hal_rtc_get_log_level(), sys_log_level_value, COUNT_OF(sys_log_level_value));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, sys_log_level_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Log Device",
            COUNT_OF(sys_log_device_text),
            netrunner_settings_sys_log_device_changed,
            app);
        value_index = (uint8_t)value_index_uint32(
            furi_hal_rtc_get_log_device(), sys_log_device_value, COUNT_OF(sys_log_device_value));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, sys_log_device_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Log Baud Rate",
            COUNT_OF(sys_log_baud_rate_text),
            netrunner_settings_sys_log_baud_rate_changed,
            app);
        value_index = (uint8_t)value_index_uint32(
            furi_hal_rtc_get_log_baud_rate(),
            sys_log_baud_rate_value,
            COUNT_OF(sys_log_baud_rate_value));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, sys_log_baud_rate_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Debug",
            COUNT_OF(sys_debug_text),
            netrunner_settings_sys_debug_changed,
            app);
        value_index = furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug) ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, sys_debug_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Heap Trace",
            COUNT_OF(sys_heap_trace_text),
            netrunner_settings_sys_heap_trace_changed,
            app);
        value_index = (uint8_t)value_index_uint32(
            furi_hal_rtc_get_heap_track_mode(),
            sys_heap_trace_value,
            COUNT_OF(sys_heap_trace_value));
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, sys_heap_trace_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Sleep Method",
            COUNT_OF(sys_sleep_method_text),
            netrunner_settings_sys_sleep_method_changed,
            app);
        value_index = furi_hal_rtc_is_flag_set(FuriHalRtcFlagLegacySleep) ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, sys_sleep_method_text[value_index]);
    } else if(page == NetrunnerSettingsPageProtocols) {
        /* Rows stay alphabetical; the enter handler maps each index. Bluetooth
         * opens its own app; Extended Range and File Naming are inline enums. */
        variable_item_list_set_header(app->variable_item_list, "Protocols");

        variable_item_list_add(app->variable_item_list, "Bluetooth", 1, NULL, app);

        item = variable_item_list_add(
            app->variable_item_list,
            "Extended Range",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_subghz_extended_changed,
            app);
        value_index = app->subghz_extended_range ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "File Naming",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_file_naming_changed,
            app);
        value_index = app->settings.file_naming_prefix_after ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, value_index ? "After" : "Before");

        variable_item_list_add(app->variable_item_list, "Frequencies", 1, NULL, app);
    } else if(page == NetrunnerSettingsPageDolphin) {
        variable_item_list_set_header(app->variable_item_list, "Levels");

        char label[16];
        item = variable_item_list_add(app->variable_item_list, "Level", 1, NULL, app);
        snprintf(label, sizeof(label), "%u", (unsigned)dolphin_get_level(app->dolphin_xp));
        variable_item_set_current_value_text(item, label);

        item = variable_item_list_add(app->variable_item_list, "XP", 1, NULL, app);
        snprintf(label, sizeof(label), "%lu", (unsigned long)app->dolphin_xp);
        variable_item_set_current_value_text(item, label);

        item = variable_item_list_add(
            app->variable_item_list,
            "Mood",
            NETRUNNER_DOLPHIN_BUTTHURT_MAX + 1,
            netrunner_settings_butthurt_changed,
            app);
        value_index = (uint8_t)CLAMP(app->dolphin_butthurt, NETRUNNER_DOLPHIN_BUTTHURT_MAX, 0);
        variable_item_set_current_value_index(item, value_index);
        snprintf(label, sizeof(label), "%u", (unsigned)value_index);
        variable_item_set_current_value_text(item, label);
    } else if(page == NetrunnerSettingsPageFrequencies) {
        variable_item_list_set_header(app->variable_item_list, "Frequencies");

        item = variable_item_list_add(
            app->variable_item_list,
            "Default Freqs",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_use_defaults_changed,
            app);
        value_index = app->subghz_use_defaults ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);

        variable_item_list_add(app->variable_item_list, "Static Freqs", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Hopper Freqs", 1, NULL, app);
    } else if(page == NetrunnerSettingsPageMisc) {
        /* Rows stay alphabetical; the enter handler maps each index. Backup
         * Settings, Passport, Power and Storage open their own apps; Shell Color
         * is an inline enum handled by its change callback. */
        variable_item_list_set_header(app->variable_item_list, "Misc");

        variable_item_list_add(app->variable_item_list, "Backup Settings", 1, NULL, app);

        item = variable_item_list_add(app->variable_item_list, "Device Name", 1, NULL, app);
        variable_item_set_current_value_text(
            item, app->device_name[0] ? app->device_name : "Default");

        variable_item_list_add(app->variable_item_list, "Levels", 1, NULL, app);

        variable_item_list_add(app->variable_item_list, "Passport", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Power", 1, NULL, app);
        variable_item_list_add(app->variable_item_list, "Screen", 1, NULL, app);

        item = variable_item_list_add(
            app->variable_item_list,
            "Shell Color",
            COUNT_OF(netrunner_shell_color_values),
            netrunner_settings_shell_color_changed,
            app);
        {
            FuriHalVersionColor cur = furi_hal_version_get_hw_color();
            uint8_t ci = 0;
            for(uint8_t i = 0; i < COUNT_OF(netrunner_shell_color_values); i++) {
                if(netrunner_shell_color_values[i] == cur) {
                    ci = i;
                    break;
                }
            }
            variable_item_set_current_value_index(item, ci);
            variable_item_set_current_value_text(item, netrunner_shell_color_text[ci]);
        }

        variable_item_list_add(app->variable_item_list, "Storage", 1, NULL, app);
    } else if(page == NetrunnerSettingsPageScreen) {
        variable_item_list_set_header(app->variable_item_list, "Screen");

        item = variable_item_list_add(
            app->variable_item_list,
            "Dark Mode",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_dark_mode_changed,
            app);
        value_index = app->settings.dark_mode ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);

        item = variable_item_list_add(
            app->variable_item_list,
            "Left Handed",
            COUNT_OF(netrunner_unlock_anim_values),
            netrunner_settings_left_handed_changed,
            app);
        value_index = furi_hal_rtc_is_flag_set(FuriHalRtcFlagHandOrient) ? 1U : 0U;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, netrunner_unlock_anim_text[value_index]);
    }

    variable_item_list_set_selected_item(app->variable_item_list, selected_item);
    view_dispatcher_switch_to_view(app->view_dispatcher, NetrunnerSettingsViewList);
}

static bool netrunner_settings_custom_event(void* context, uint32_t event) {
    NetrunnerSettingsApp* app = context;
    furi_assert(app);

    if(event == NETRUNNER_SETTINGS_EVENT_RUN_CHILD) {
        netrunner_settings_run_child(app);
        return true;
    }

    /* Bounded by the enum's sentinel rather than by whichever page happened to
     * be last, so adding a page cannot silently drop its navigation event. */
    if(event < NetrunnerSettingsPageInterface || event >= NetrunnerSettingsPageCount) {
        return false;
    }

    netrunner_settings_show_page(app, (NetrunnerSettingsPage)event, 0);
    return true;
}

static bool netrunner_settings_back_event(void* context) {
    NetrunnerSettingsApp* app = context;
    furi_assert(app);

    if(app->current_page == NetrunnerSettingsPageRoot) {
        return false;
        /* Flat top level: every page's parent is Root. The row is that page's
         * position in the Root list (Misc, General, Wallpaper, Mainmenu,
         * Statusbar, Sound & Display, Protocols, Lock Screen, File Browser,
         * System), so Back lands on the row the page was opened from. */
    } else if(app->current_page == NetrunnerSettingsPageMisc) {
        netrunner_settings_show_page(app, NetrunnerSettingsPageRoot, 0);
    } else if(app->current_page == NetrunnerSettingsPageGeneral) {
        netrunner_settings_show_page(app, NetrunnerSettingsPageRoot, 1);
    } else if(app->current_page == NetrunnerSettingsPageGraphics) {
        netrunner_settings_show_page(app, NetrunnerSettingsPageRoot, 2);
    } else if(app->current_page == NetrunnerSettingsPageMainmenu) {
        netrunner_settings_show_page(app, NetrunnerSettingsPageRoot, 3);
    } else if(app->current_page == NetrunnerSettingsPageStatusbar) {
        netrunner_settings_show_page(app, NetrunnerSettingsPageRoot, 4);
    } else if(app->current_page == NetrunnerSettingsPageProtocols) {
        netrunner_settings_show_page(app, NetrunnerSettingsPageRoot, 6);
    } else if(app->current_page == NetrunnerSettingsPageLockscreen) {
        netrunner_settings_show_page(app, NetrunnerSettingsPageRoot, 7);
    } else if(app->current_page == NetrunnerSettingsPageFileBrowser) {
        netrunner_settings_show_page(app, NetrunnerSettingsPageRoot, 8);
    } else if(app->current_page == NetrunnerSettingsPageFrequencies) {
        netrunner_settings_show_page(app, NetrunnerSettingsPageProtocols, 3);
    } else if(app->current_page == NetrunnerSettingsPageDolphin) {
        netrunner_settings_show_page(app, NetrunnerSettingsPageMisc, 2);
    } else if(app->current_page == NetrunnerSettingsPageScreen) {
        netrunner_settings_show_page(app, NetrunnerSettingsPageMisc, 5);
    }

    return true;
}

/* Reads the name the namechanger service applies at boot. Seeding from the
 * effective name instead would show the eFuse-derived default as though it
 * were a custom one. */
static void netrunner_settings_load_device_name(NetrunnerSettingsApp* app) {
    NetrunnerFormat* file = netrunner_format_file_alloc(app->storage);
    FuriString* str = furi_string_alloc();

    do {
        uint32_t version;
        if(!netrunner_format_file_open_existing(file, NAMECHANGER_PATH)) break;
        if(!netrunner_format_read_header(file, str, &version)) break;
        if(furi_string_cmp_str(str, NAMECHANGER_HEADER)) break;
        if(version != NAMECHANGER_VERSION) break;
        if(!netrunner_format_read_string(file, "Name", str)) break;
        strncpy(app->device_name, furi_string_get_cstr(str), sizeof(app->device_name) - 1);
    } while(false);

    furi_string_free(str);
    netrunner_format_free(file);
}

/* Frequency lists live in the same file subghz_setting_load() reads, so edits
 * here are picked up by SubGHz and SubGHz Remote without a reboot of anything
 * but the app that reads them. */
static void netrunner_settings_load_freqs(NetrunnerSettingsApp* app) {
    app->subghz_use_defaults = true;
    app->subghz_static_count = 0;
    app->subghz_hopper_count = 0;

    NetrunnerFormat* file = netrunner_format_file_alloc(app->storage);
    FuriString* str = furi_string_alloc();

    do {
        uint32_t version;
        if(!netrunner_format_file_open_existing(file, NETRUNNER_SUBGHZ_USER_SETTINGS)) break;
        if(!netrunner_format_read_header(file, str, &version)) break;
        if(furi_string_cmp_str(str, NETRUNNER_SUBGHZ_SETTING_TYPE) ||
           version != NETRUNNER_SUBGHZ_SETTING_VERSION)
            break;

        bool use_defaults = true;
        netrunner_format_read_bool(file, "Add_standard_frequencies", &use_defaults, 1);
        app->subghz_use_defaults = use_defaults;

        uint32_t value;
        if(netrunner_format_rewind(file)) {
            while(app->subghz_static_count < NETRUNNER_MAX_USER_FREQS &&
                  netrunner_format_read_uint32(file, "Frequency", &value, 1)) {
                app->subghz_static_freqs[app->subghz_static_count++] = value;
            }
        }

        if(netrunner_format_rewind(file)) {
            while(app->subghz_hopper_count < NETRUNNER_MAX_USER_FREQS &&
                  netrunner_format_read_uint32(file, "Hopper_frequency", &value, 1)) {
                app->subghz_hopper_freqs[app->subghz_hopper_count++] = value;
            }
        }
    } while(false);

    furi_string_free(str);
    netrunner_format_free(file);
}

static void netrunner_settings_save_freqs(NetrunnerSettingsApp* app) {
    NetrunnerFormat* file = netrunner_format_file_alloc(app->storage);

    do {
        if(!netrunner_format_file_open_always(file, NETRUNNER_SUBGHZ_USER_SETTINGS)) break;
        if(!netrunner_format_write_header_cstr(
               file, NETRUNNER_SUBGHZ_SETTING_TYPE, NETRUNNER_SUBGHZ_SETTING_VERSION))
            break;

        /* Keys are deleted before rewriting: netrunner_format appends, so without
         * this every save would leave the previous entries in place. */
        while(netrunner_format_delete_key(file, "Add_standard_frequencies"))
            ;
        netrunner_format_write_bool(
            file, "Add_standard_frequencies", &app->subghz_use_defaults, 1);

        if(!netrunner_format_rewind(file)) break;
        while(netrunner_format_delete_key(file, "Frequency"))
            ;
        for(uint8_t i = 0; i < app->subghz_static_count; i++) {
            netrunner_format_write_uint32(file, "Frequency", &app->subghz_static_freqs[i], 1);
        }

        if(!netrunner_format_rewind(file)) break;
        while(netrunner_format_delete_key(file, "Hopper_frequency"))
            ;
        for(uint8_t i = 0; i < app->subghz_hopper_count; i++) {
            netrunner_format_write_uint32(
                file, "Hopper_frequency", &app->subghz_hopper_freqs[i], 1);
        }
    } while(false);

    netrunner_format_free(file);
}

static uint32_t* netrunner_settings_freq_list(NetrunnerSettingsApp* app, uint8_t** count) {
    if(app->subghz_editing_hopper) {
        *count = &app->subghz_hopper_count;
        return app->subghz_hopper_freqs;
    }
    *count = &app->subghz_static_count;
    return app->subghz_static_freqs;
}

static void netrunner_settings_freq_submenu_callback(void* context, uint32_t index);

static void netrunner_settings_show_freq_list(NetrunnerSettingsApp* app) {
    uint8_t* count;
    uint32_t* freqs = netrunner_settings_freq_list(app, &count);

    submenu_reset(app->freq_submenu);
    submenu_set_header(
        app->freq_submenu, app->subghz_editing_hopper ? "Hopper Freqs" : "Static Freqs");

    static char labels[NETRUNNER_MAX_USER_FREQS][24];
    for(uint8_t i = 0; i < *count; i++) {
        snprintf(
            labels[i],
            sizeof(labels[i]),
            "%lu.%02lu MHz",
            (unsigned long)(freqs[i] / 1000000UL),
            (unsigned long)((freqs[i] % 1000000UL) / 10000UL));
        submenu_add_item(
            app->freq_submenu, labels[i], i, netrunner_settings_freq_submenu_callback, app);
    }

    if(*count < NETRUNNER_MAX_USER_FREQS) {
        submenu_add_item(
            app->freq_submenu,
            "Add frequency...",
            NETRUNNER_MAX_USER_FREQS,
            netrunner_settings_freq_submenu_callback,
            app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, NetrunnerSettingsViewFreqList);
}

static void netrunner_settings_number_done(void* context, int32_t number) {
    NetrunnerSettingsApp* app = context;

    if(app->number_input_for_xp) {
        app->dolphin_xp = (uint32_t)number;
        app->dolphin_dirty = true;
        netrunner_settings_show_page(app, NetrunnerSettingsPageDolphin, 1);
        return;
    }

    uint8_t* count;
    uint32_t* freqs = netrunner_settings_freq_list(app, &count);

    const uint32_t value = (uint32_t)number * 1000UL; // entered in kHz
    if(*count < NETRUNNER_MAX_USER_FREQS && furi_hal_subghz_is_frequency_valid(value)) {
        freqs[(*count)++] = value;
        app->subghz_freqs_dirty = true;
    }
    netrunner_settings_show_freq_list(app);
}

static void netrunner_settings_freq_submenu_callback(void* context, uint32_t index) {
    NetrunnerSettingsApp* app = context;
    uint8_t* count;
    uint32_t* freqs = netrunner_settings_freq_list(app, &count);

    if(index == NETRUNNER_MAX_USER_FREQS) {
        app->number_input_for_xp = false;
        number_input_set_header_text(app->number_input, "Frequency in kHz");
        number_input_set_result_callback(
            app->number_input, netrunner_settings_number_done, app, 433920, 281000, 962000);
        view_dispatcher_switch_to_view(app->view_dispatcher, NetrunnerSettingsViewNumberInput);
        return;
    }

    /* Selecting an entry removes it; there is nothing else to do to one. */
    if(index < *count) {
        memmove(&freqs[index], &freqs[index + 1], (*count - index - 1) * sizeof(uint32_t));
        (*count)--;
        app->subghz_freqs_dirty = true;
    }
    netrunner_settings_show_freq_list(app);
}

static void netrunner_settings_butthurt_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    char label[8];
    snprintf(label, sizeof(label), "%u", (unsigned)index);
    variable_item_set_current_value_text(item, label);
    app->dolphin_butthurt = (int32_t)index;
    app->dolphin_dirty = true;
}

static void netrunner_settings_use_defaults_changed(VariableItem* item) {
    NetrunnerSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, netrunner_unlock_anim_text[index]);
    app->subghz_use_defaults = netrunner_unlock_anim_values[index];
    app->subghz_freqs_dirty = true;
}

static NetrunnerSettingsApp* netrunner_settings_app_alloc(void) {
    NetrunnerSettingsApp* app = malloc(sizeof(NetrunnerSettingsApp));
    memset(app, 0, sizeof(NetrunnerSettingsApp));
    app->settings = netrunner_settings;
    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->desktop = furi_record_open(RECORD_DESKTOP);
    app->power = furi_record_open(RECORD_POWER);
    app->dolphin = furi_record_open(RECORD_DOLPHIN);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    DolphinStats stats = dolphin_stats(app->dolphin);
    app->dolphin_xp = stats.icounter;
    app->dolphin_butthurt = (int32_t)stats.butthurt;
    desktop_api_get_settings(app->desktop, &app->desktop_settings);
    app->sd_ready = storage_sd_status(app->storage) == FSE_OK;

    netrunner_settings_load_device_name(app);
    app->subghz_extended_range = subghz_extended_range_load();
    netrunner_settings_load_freqs(app);
    netrunner_settings_mainmenu_load(app);

    netrunner_settings_scan_asset_packs(app);

    app->view_dispatcher = view_dispatcher_alloc();
    app->variable_item_list = variable_item_list_alloc();
    app->asset_pack_submenu = submenu_alloc();
    app->text_input = text_input_alloc();
    app->freq_submenu = submenu_alloc();
    app->wallpaper_submenu = submenu_alloc();
    app->mainmenu_submenu = submenu_alloc();
    app->number_input = number_input_alloc();

    app->set_time_view = view_alloc();
    view_allocate_model(app->set_time_view, ViewModelTypeLocking, sizeof(SetTimeModel));
    view_set_context(app->set_time_view, app);
    view_set_draw_callback(app->set_time_view, netrunner_settings_set_time_draw);
    view_set_input_callback(app->set_time_view, netrunner_settings_set_time_input);
    /* Encoder rotation must reach the picker as Up/Down (value changes) rather
     * than being remapped to Left/Right by the viewport. */
    view_set_input_mode(app->set_time_view, ViewInputModeUpDown);

    View* list_view = variable_item_list_get_view(app->variable_item_list);
    variable_item_list_set_enter_callback(
        app->variable_item_list, netrunner_settings_list_enter, app);
    view_dispatcher_add_view(
        app->view_dispatcher, NetrunnerSettingsViewList, list_view);

    View* asset_pack_view = submenu_get_view(app->asset_pack_submenu);
    view_set_previous_callback(asset_pack_view, netrunner_settings_back_to_list);
    submenu_set_header(app->asset_pack_submenu, "Asset Pack");
    submenu_add_item(
        app->asset_pack_submenu, "Default", 0, netrunner_settings_asset_pack_selected, app);
    for(uint8_t i = 0; i < app->asset_pack_count; i++) {
        submenu_add_item(
            app->asset_pack_submenu,
            app->asset_pack_names[i],
            i + 1U,
            netrunner_settings_asset_pack_selected,
            app);
    }
    submenu_set_selected_item(
        app->asset_pack_submenu, netrunner_settings_asset_pack_index(app));
    view_dispatcher_add_view(
        app->view_dispatcher, NetrunnerSettingsViewAssetPacks, asset_pack_view);

    View* wallpaper_view = submenu_get_view(app->wallpaper_submenu);
    view_set_previous_callback(wallpaper_view, netrunner_settings_back_to_list);
    view_dispatcher_add_view(
        app->view_dispatcher, NetrunnerSettingsViewWallpaper, wallpaper_view);

    View* text_input_view = text_input_get_view(app->text_input);
    view_set_previous_callback(text_input_view, netrunner_settings_back_to_list);
    view_dispatcher_add_view(
        app->view_dispatcher, NetrunnerSettingsViewTextInput, text_input_view);

    View* freq_view = submenu_get_view(app->freq_submenu);
    view_set_previous_callback(freq_view, netrunner_settings_back_to_list);
    view_dispatcher_add_view(app->view_dispatcher, NetrunnerSettingsViewFreqList, freq_view);

    View* mainmenu_view = submenu_get_view(app->mainmenu_submenu);
    view_set_previous_callback(mainmenu_view, netrunner_settings_back_to_list);
    view_dispatcher_add_view(app->view_dispatcher, NetrunnerSettingsViewMainmenu, mainmenu_view);

    View* number_view = number_input_get_view(app->number_input);
    view_dispatcher_add_view(
        app->view_dispatcher, NetrunnerSettingsViewNumberInput, number_view);

    view_dispatcher_add_view(
        app->view_dispatcher, NetrunnerSettingsViewSetTime, app->set_time_view);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, netrunner_settings_custom_event);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, netrunner_settings_back_event);
    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    netrunner_settings_show_page(app, NetrunnerSettingsPageRoot, 0);
    return app;
}

static void netrunner_settings_app_free(NetrunnerSettingsApp* app) {
    furi_assert(app);

    view_dispatcher_remove_view(app->view_dispatcher, NetrunnerSettingsViewSetTime);
    view_dispatcher_remove_view(app->view_dispatcher, NetrunnerSettingsViewNumberInput);
    view_dispatcher_remove_view(app->view_dispatcher, NetrunnerSettingsViewMainmenu);
    view_dispatcher_remove_view(app->view_dispatcher, NetrunnerSettingsViewFreqList);
    view_dispatcher_remove_view(app->view_dispatcher, NetrunnerSettingsViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, NetrunnerSettingsViewWallpaper);
    view_dispatcher_remove_view(app->view_dispatcher, NetrunnerSettingsViewAssetPacks);
    view_dispatcher_remove_view(app->view_dispatcher, NetrunnerSettingsViewList);
    view_free(app->set_time_view);
    number_input_free(app->number_input);
    submenu_free(app->mainmenu_submenu);
    submenu_free(app->freq_submenu);
    text_input_free(app->text_input);
    submenu_free(app->wallpaper_submenu);
    submenu_free(app->asset_pack_submenu);
    variable_item_list_free(app->variable_item_list);
    view_dispatcher_free(app->view_dispatcher);

    for(uint8_t i = 0; i < app->asset_pack_count; i++) {
        free(app->asset_pack_names[i]);
    }
    free(app->asset_pack_names);
    netrunner_settings_free_wallpaper_names(app);
    netrunner_settings_mainmenu_clear(app);

    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_DOLPHIN);
    furi_record_close(RECORD_POWER);
    furi_record_close(RECORD_DESKTOP);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
}

/* Writes every pending change and clears the dirty flags, so it can run both
 * before a settings app is started from here and on exit. Returns true when
 * the asset pack changed and was saved, which needs a reboot to take effect. */
static bool netrunner_settings_flush(NetrunnerSettingsApp* app) {
    bool reboot_for_asset_pack = false;

    if(app->dirty) {
        const bool pack_changed =
            strcmp(netrunner_settings.asset_pack, app->settings.asset_pack) != 0;

        netrunner_settings = app->settings;
        name_generator_set_prefix_after(netrunner_settings.file_naming_prefix_after);
        const bool settings_saved = netrunner_settings_save();
        if(!settings_saved) {
            FURI_LOG_E(TAG, "Settings are active for this session but were not saved");
        }

        /* Existing IconAnimation objects retain pointers into the active pack,
         * while the desktop animation manager caches the old animation tree.
         * Freeing/reloading the pack live made those pointers dangle and still
         * did not refresh the desktop. A clean restart after the saved change
         * atomically applies icons, fonts, and animations. */
        reboot_for_asset_pack = pack_changed && settings_saved;
    }

    if(app->name_dirty) {
        // Apply the name first so get_name_ptr() reflects the final effective
        // name (eFuse default when the user cleared it).
        furi_hal_version_set_name(app->device_name[0] ? app->device_name : NULL);

        // name.settings stores BOTH Name and Color (loaded by the namechanger
        // service at boot). Shell Color lives in Netrunner > Misc now, so the
        // write must preserve it instead of dropping the field.
        uint32_t color = (uint32_t)furi_hal_version_get_hw_color();
        if(app->device_name[0] == '\0' && color == (uint32_t)FuriHalVersionColorUnknown) {
            // Both at factory defaults → drop the file so the next boot derives them.
            storage_simply_remove(app->storage, NAMECHANGER_PATH);
        } else {
            NetrunnerFormat* file = netrunner_format_file_alloc(app->storage);
            do {
                if(!netrunner_format_file_open_always(file, NAMECHANGER_PATH)) break;
                if(!netrunner_format_write_header_cstr(
                       file, NAMECHANGER_HEADER, NAMECHANGER_VERSION))
                    break;
                if(!netrunner_format_write_string_cstr(
                       file, "Name", furi_hal_version_get_name_ptr()))
                    break;
                if(!netrunner_format_write_uint32(file, "Color", &color, 1)) break;
            } while(0);
            netrunner_format_free(file);
        }
    }

    if(app->subghz_freqs_dirty) {
        netrunner_settings_save_freqs(app);
    }

    if(app->mainmenu_dirty) {
        netrunner_settings_mainmenu_save(app);
    }

    if(app->dolphin_dirty) {
        /* Written straight into the dolphin's state: there is no public setter
         * for XP or mood, and this is what upstream does too. */
        app->dolphin->state->data.icounter = app->dolphin_xp;
        app->dolphin->state->data.butthurt = app->dolphin_butthurt;
        /* dolphin_state_save() returns early unless the state is marked dirty,
         * and app->dolphin_dirty is this app's own flag, not the state's -- so
         * without this the save was a silent no-op and an edited XP/mood lived
         * only in RAM, reverting on the next reboot or deep-sleep wake. */
        app->dolphin->state->dirty = true;
        dolphin_state_save(app->dolphin->state);
    }

    if(app->desktop_dirty) {
        desktop_settings_save(&app->desktop_settings);
        desktop_api_set_settings(app->desktop, &app->desktop_settings);
        power_trigger_ui_update(app->power);
    }

    app->dirty = false;
    app->name_dirty = false;
    app->subghz_freqs_dirty = false;
    app->mainmenu_dirty = false;
    app->dolphin_dirty = false;
    app->desktop_dirty = false;

    return reboot_for_asset_pack;
}

int32_t settings_app(void* p) {
    NetrunnerSettingsApp* app = netrunner_settings_app_alloc();

    unsigned page;
    unsigned row;
    if(p && sscanf((const char*)p, NETRUNNER_SETTINGS_RETURN_ARGS_FMT, &page, &row) == 2 &&
       page < NetrunnerSettingsPageCount && row <= UINT8_MAX) {
        netrunner_settings_show_page(app, (NetrunnerSettingsPage)page, (uint8_t)row);
    }

    view_dispatcher_run(app->view_dispatcher);
    const bool reboot_for_asset_pack = netrunner_settings_flush(app);

    /* Everything above has been written, so it is safe to go down here. This
     * does not return. */
    if(reboot_for_asset_pack) {
        power_reboot(app->power, PowerBootModeNormal);
    }

    netrunner_settings_app_free(app);
    return 0;
}
