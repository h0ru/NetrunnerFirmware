#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASSET_PACKS_NAME_LEN 32

#define NETRUNNER_ANIM_SPEED_MIN     25U
#define NETRUNNER_ANIM_SPEED_MAX     300U
#define NETRUNNER_ANIM_SPEED_DEFAULT 100U

#define NETRUNNER_CYCLE_ANIMS_MIN     (-1)
#define NETRUNNER_CYCLE_ANIMS_MAX     86400
#define NETRUNNER_CYCLE_ANIMS_DEFAULT 0

#define NETRUNNER_BUTTHURT_TIMER_MIN     0U
#define NETRUNNER_BUTTHURT_TIMER_MAX     172800U
#define NETRUNNER_BUTTHURT_TIMER_DEFAULT 0U

typedef enum {
    MenuStyleList = 0,
    MenuStyleWii,
    MenuStyleDsi,
    MenuStylePs4,
    MenuStyleVertical,
    MenuStyleC64,
    MenuStyleCompact,
    MenuStyleMNTM,
    MenuStyleCoverFlow,
    MenuStyleCount,
} MenuStyle;

typedef enum {
    BrowserPathOff = 0,
    BrowserPathCurrent,
    BrowserPathBrief,
    BrowserPathFull,
    BrowserPathModeCount,
} BrowserPathMode;

typedef struct {
    char asset_pack[ASSET_PACKS_NAME_LEN];
    /* Chosen desktop wallpaper: an animation name from the asset pack's
     * manifest. Empty means Random (weighted pick). Pinned only while
     * Unlock Anims is off; see animation_manager_select_idle_animation. */
    char wallpaper[ASSET_PACKS_NAME_LEN];
    uint32_t anim_speed;
    int32_t cycle_anims;
    /* Hide the Dual Boot entry from the main menu. Off by default: the app
     * is how you reach the other firmware, so hiding it is a deliberate
     * choice, not something to inherit silently. */
    bool hide_dualboot;
    bool unlock_anims;
    MenuStyle menu_style;
    bool lock_on_boot;
    bool allow_locked_rpc_usb;
    bool allow_locked_rpc_ble;
    uint32_t butthurt_timer;
    bool lockscreen_poweroff;
    bool lockscreen_time;
    bool lockscreen_seconds;
    bool lockscreen_date;
    bool lockscreen_statusbar;
    bool lockscreen_prompt;
    bool lockscreen_transparent;
    bool lockscreen_skip_animation;
    bool scroll_marquee;
    bool midnight_format_00;
    bool popup_overlay;
    bool status_icons;
    bool bar_borders;
    bool bar_background;
    bool sort_dirs_first;
    bool show_hidden_files;
    bool show_internal_tab;
    BrowserPathMode browser_path_mode;
    bool dark_mode;
    bool file_naming_prefix_after;
} NetrunnerSettings;

/* Every persisted setting is described once, in netrunner_settings_entries.
 * Defaults, clamping, change detection, loading and saving all iterate that
 * table, so adding a Netrunner setting is one struct field plus one table row.
 * Fields are addressed by offset rather than by pointer so the same table can
 * be applied to any NetrunnerSettings instance, not just the global one. */
typedef enum {
    NetrunnerSettingsTypeStr,
    NetrunnerSettingsTypeInt,
    NetrunnerSettingsTypeUint,
    NetrunnerSettingsTypeBool,
    /* Serialised like Uint, but sanitized by falling back to its default
     * instead of clamping, since no enum case sits outside [min, max]. */
    NetrunnerSettingsTypeEnum,
} NetrunnerSettingsType;

/* Values are carried as int64_t so that the full uint32_t range and negative
 * int32_t values are both representable without one aliasing the other. */
typedef struct {
    NetrunnerSettingsType type;
    const char* key;
    uint16_t offset;
    uint16_t size;
    int64_t min;
    int64_t max;
    int64_t def;
} NetrunnerSettingsEntry;

/* Tripwire: adding a field to NetrunnerSettings without adding its table row
 * would leave it unsaved and invisible to change detection. Bump this only
 * together with the corresponding netrunner_settings_entries row. */
#define NETRUNNER_SETTINGS_ENTRY_COUNT 31

extern const NetrunnerSettingsEntry netrunner_settings_entries[];
extern const size_t netrunner_settings_entry_count;

/* Read/write one table entry on an arbitrary settings instance. Strings are
 * not addressable this way; use the asset_pack field directly. */
int64_t netrunner_settings_entry_get(const NetrunnerSettingsEntry* entry, const NetrunnerSettings* settings);
void netrunner_settings_entry_set(
    const NetrunnerSettingsEntry* entry,
    NetrunnerSettings* settings,
    int64_t value);

void netrunner_settings_set_defaults(NetrunnerSettings* settings);
void netrunner_settings_sanitize(NetrunnerSettings* settings);
bool netrunner_settings_equal(const NetrunnerSettings* left, const NetrunnerSettings* right);
bool netrunner_settings_asset_pack_is_safe(const char* name);

uint32_t netrunner_animation_frame_rate(uint32_t base_frame_rate, uint32_t speed_percent);
uint32_t netrunner_animation_cycle_period_ms(
    int32_t cycle_seconds,
    uint32_t metadata_duration_seconds,
    bool* enabled);

#ifdef __cplusplus
}
#endif
