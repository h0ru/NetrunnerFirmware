#include "settings_core.h"

#include <string.h>

#define FIELD_OFF(n) ((uint16_t)offsetof(NetrunnerSettings, n))
#define FIELD_SZ(n)  ((uint16_t)sizeof(((NetrunnerSettings*)0)->n))

#define ENTRY_STR(n)              {NetrunnerSettingsTypeStr, #n, FIELD_OFF(n), FIELD_SZ(n), 0, 0, 0}
#define ENTRY_BOOL(n, d)          {NetrunnerSettingsTypeBool, #n, FIELD_OFF(n), FIELD_SZ(n), 0, 1, (d)}
#define ENTRY_INT(n, lo, hi, d)   {NetrunnerSettingsTypeInt, #n, FIELD_OFF(n), FIELD_SZ(n), (lo), (hi), (d)}
#define ENTRY_UINT(n, lo, hi, d)  {NetrunnerSettingsTypeUint, #n, FIELD_OFF(n), FIELD_SZ(n), (lo), (hi), (d)}
#define ENTRY_ENUM(n, count, d)   {NetrunnerSettingsTypeEnum, #n, FIELD_OFF(n), FIELD_SZ(n), 0, (count) - 1, (d)}

const NetrunnerSettingsEntry netrunner_settings_entries[] = {
    ENTRY_STR(asset_pack),
    ENTRY_STR(wallpaper),
    ENTRY_UINT(
        anim_speed,
        NETRUNNER_ANIM_SPEED_MIN,
        NETRUNNER_ANIM_SPEED_MAX,
        NETRUNNER_ANIM_SPEED_DEFAULT),
    ENTRY_INT(
        cycle_anims,
        NETRUNNER_CYCLE_ANIMS_MIN,
        NETRUNNER_CYCLE_ANIMS_MAX,
        NETRUNNER_CYCLE_ANIMS_DEFAULT),
    ENTRY_BOOL(hide_dualboot, false),
    ENTRY_BOOL(unlock_anims, false),
    /* Factory default is the DSi menu. /int lives on
     * the SD card, so without a card this default is what the device shows. */
    ENTRY_ENUM(menu_style, MenuStyleCount, MenuStyleDsi),
    ENTRY_BOOL(lock_on_boot, true),
    ENTRY_BOOL(allow_locked_rpc_usb, false),
    ENTRY_BOOL(allow_locked_rpc_ble, false),
    ENTRY_UINT(
        butthurt_timer,
        NETRUNNER_BUTTHURT_TIMER_MIN,
        NETRUNNER_BUTTHURT_TIMER_MAX,
        NETRUNNER_BUTTHURT_TIMER_DEFAULT),
    ENTRY_BOOL(lockscreen_poweroff, true),
    ENTRY_BOOL(lockscreen_time, true),
    ENTRY_BOOL(lockscreen_seconds, false),
    ENTRY_BOOL(lockscreen_date, true),
    ENTRY_BOOL(lockscreen_statusbar, true),
    ENTRY_BOOL(lockscreen_prompt, true),
    ENTRY_BOOL(lockscreen_transparent, false),
    ENTRY_BOOL(lockscreen_skip_animation, false),
    ENTRY_BOOL(scroll_marquee, false),
    ENTRY_BOOL(midnight_format_00, true),
    ENTRY_BOOL(popup_overlay, true),
    ENTRY_BOOL(status_icons, true),
    ENTRY_BOOL(bar_borders, true),
    ENTRY_BOOL(bar_background, false),
    ENTRY_BOOL(sort_dirs_first, true),
    ENTRY_BOOL(show_hidden_files, false),
    ENTRY_BOOL(show_internal_tab, false),
    ENTRY_ENUM(browser_path_mode, BrowserPathModeCount, BrowserPathOff),
    ENTRY_BOOL(dark_mode, false),
    ENTRY_BOOL(file_naming_prefix_after, false),
};

const size_t netrunner_settings_entry_count =
    sizeof(netrunner_settings_entries) / sizeof(netrunner_settings_entries[0]);

/* Only NetrunnerSettingsTypeInt is signed. Widening every other type through
 * int32_t would turn a large uint32_t into a negative number, which would then
 * clamp to the entry's minimum instead of its maximum. */
static bool entry_is_signed(const NetrunnerSettingsEntry* entry) {
    return entry->type == NetrunnerSettingsTypeInt;
}

static void field_write(void* field, uint16_t size, bool is_signed, int64_t value) {
    if(size == 1) {
        uint8_t narrowed = (uint8_t)value;
        memcpy(field, &narrowed, sizeof(narrowed));
    } else if(size == 2) {
        uint16_t narrowed = (uint16_t)value;
        memcpy(field, &narrowed, sizeof(narrowed));
    } else if(is_signed) {
        int32_t narrowed = (int32_t)value;
        memcpy(field, &narrowed, sizeof(narrowed));
    } else {
        uint32_t narrowed = (uint32_t)value;
        memcpy(field, &narrowed, sizeof(narrowed));
    }
}

static int64_t field_read(const void* field, uint16_t size, bool is_signed) {
    if(size == 1) {
        uint8_t widened;
        memcpy(&widened, field, sizeof(widened));
        return (int64_t)widened;
    }
    if(size == 2) {
        uint16_t widened;
        memcpy(&widened, field, sizeof(widened));
        return (int64_t)widened;
    }
    if(is_signed) {
        int32_t widened;
        memcpy(&widened, field, sizeof(widened));
        return (int64_t)widened;
    }
    uint32_t widened;
    memcpy(&widened, field, sizeof(widened));
    return (int64_t)widened;
}

int64_t
    netrunner_settings_entry_get(const NetrunnerSettingsEntry* entry, const NetrunnerSettings* settings) {
    if(!entry || !settings || entry->type == NetrunnerSettingsTypeStr) return 0;
    return field_read(
        (const uint8_t*)settings + entry->offset, entry->size, entry_is_signed(entry));
}

void netrunner_settings_entry_set(
    const NetrunnerSettingsEntry* entry,
    NetrunnerSettings* settings,
    int64_t value) {
    if(!entry || !settings || entry->type == NetrunnerSettingsTypeStr) return;
    field_write((uint8_t*)settings + entry->offset, entry->size, entry_is_signed(entry), value);
}

void netrunner_settings_set_defaults(NetrunnerSettings* settings) {
    if(!settings) return;

    memset(settings, 0, sizeof(*settings));
    for(size_t i = 0; i < netrunner_settings_entry_count; ++i) {
        const NetrunnerSettingsEntry* entry = &netrunner_settings_entries[i];
        if(entry->type == NetrunnerSettingsTypeStr) continue;
        netrunner_settings_entry_set(entry, settings, entry->def);
    }
}

bool netrunner_settings_asset_pack_is_safe(const char* name) {
    if(!name) return false;
    if(!strcmp(name, ".") || !strcmp(name, "..")) return false;

    for(const unsigned char* cursor = (const unsigned char*)name; *cursor; ++cursor) {
        if((*cursor < 0x20U) || (*cursor == 0x7FU) || (*cursor == '/') || (*cursor == '\\')) {
            return false;
        }
    }

    return true;
}

void netrunner_settings_sanitize(NetrunnerSettings* settings) {
    if(!settings) return;

    settings->asset_pack[ASSET_PACKS_NAME_LEN - 1] = '\0';
    if(!netrunner_settings_asset_pack_is_safe(settings->asset_pack)) {
        settings->asset_pack[0] = '\0';
    }

    settings->wallpaper[ASSET_PACKS_NAME_LEN - 1] = '\0';
    if(!netrunner_settings_asset_pack_is_safe(settings->wallpaper)) {
        settings->wallpaper[0] = '\0';
    }

    for(size_t i = 0; i < netrunner_settings_entry_count; ++i) {
        const NetrunnerSettingsEntry* entry = &netrunner_settings_entries[i];
        if(entry->type == NetrunnerSettingsTypeStr) continue;

        const int64_t value = netrunner_settings_entry_get(entry, settings);
        if(value >= entry->min && value <= entry->max) continue;

        /* An out-of-range numeric setting is still meaningful at the nearest
         * limit, but an out-of-range enum names no case at all, so it falls
         * back to its default rather than to a neighbouring style. */
        if(entry->type == NetrunnerSettingsTypeEnum) {
            netrunner_settings_entry_set(entry, settings, entry->def);
        } else {
            netrunner_settings_entry_set(entry, settings, value < entry->min ? entry->min : entry->max);
        }
    }
}

bool netrunner_settings_equal(const NetrunnerSettings* left, const NetrunnerSettings* right) {
    if(!left || !right) return left == right;

    for(size_t i = 0; i < netrunner_settings_entry_count; ++i) {
        const NetrunnerSettingsEntry* entry = &netrunner_settings_entries[i];
        if(entry->type == NetrunnerSettingsTypeStr) {
            if(strncmp(
                   (const char*)left + entry->offset,
                   (const char*)right + entry->offset,
                   entry->size)) {
                return false;
            }
        } else if(
            netrunner_settings_entry_get(entry, left) != netrunner_settings_entry_get(entry, right)) {
            return false;
        }
    }

    return true;
}

uint32_t netrunner_animation_frame_rate(uint32_t base_frame_rate, uint32_t speed_percent) {
    if(speed_percent < NETRUNNER_ANIM_SPEED_MIN) {
        speed_percent = NETRUNNER_ANIM_SPEED_MIN;
    } else if(speed_percent > NETRUNNER_ANIM_SPEED_MAX) {
        speed_percent = NETRUNNER_ANIM_SPEED_MAX;
    }

    uint64_t scaled = ((uint64_t)base_frame_rate * speed_percent) / 100U;
    if(scaled < 1U) return 1U;
    if(scaled > UINT32_MAX) return UINT32_MAX;
    return (uint32_t)scaled;
}

uint32_t netrunner_animation_cycle_period_ms(
    int32_t cycle_seconds,
    uint32_t metadata_duration_seconds,
    bool* enabled) {
    if(enabled) *enabled = false;
    if(cycle_seconds < 0) return 0;

    const uint32_t seconds = cycle_seconds == 0 ? metadata_duration_seconds : (uint32_t)cycle_seconds;
    if(seconds == 0) return 0;

    if(enabled) *enabled = true;
    const uint64_t milliseconds = (uint64_t)seconds * 1000U;
    return milliseconds > UINT32_MAX ? UINT32_MAX : (uint32_t)milliseconds;
}
