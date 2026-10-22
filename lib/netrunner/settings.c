#include "settings_i.h"

#include <netrunner_format/netrunner_format.h>
#include <furi.h>

#include <string.h>

#define TAG "NetrunnerSettings"

/* Zero-initialised here and given its table defaults by
 * netrunner_settings_set_defaults() at the top of app_main, before any service
 * that reads it can start. netrunner_settings_load() then overlays the file. */
NetrunnerSettings netrunner_settings;
uint32_t netrunner_settings_revision = 0;

static char* netrunner_settings_field(NetrunnerSettings* settings, const NetrunnerSettingsEntry* entry) {
    return (char*)settings + entry->offset;
}

static void netrunner_settings_copy_str(
    NetrunnerSettings* settings,
    const NetrunnerSettingsEntry* entry,
    FuriString* source) {
    char* field = netrunner_settings_field(settings, entry);
    size_t length = furi_string_size(source);
    if(length >= entry->size) length = entry->size - 1;

    memcpy(field, furi_string_get_cstr(source), length);
    field[length] = '\0';
}

void netrunner_settings_load(void) {
    const NetrunnerSettings previous_settings = netrunner_settings;
    netrunner_settings_set_defaults(&netrunner_settings);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    NetrunnerFormat* file = netrunner_format_file_alloc(storage);
    FuriString* string_value = furi_string_alloc();

    const bool opened = netrunner_format_file_open_existing(file, NETRUNNER_SETTINGS_PATH);
    if(opened) {
        for(size_t i = 0; i < netrunner_settings_entry_count; ++i) {
            const NetrunnerSettingsEntry* entry = &netrunner_settings_entries[i];
            int32_t int_value;
            uint32_t uint_value;
            bool bool_value;

            /* Rewind per entry so a key missing from an older or hand-edited
             * file cannot consume the position of the keys after it. */
            if(!netrunner_format_rewind(file)) break;

            switch(entry->type) {
            case NetrunnerSettingsTypeStr:
                if(netrunner_format_read_string(file, entry->key, string_value)) {
                    netrunner_settings_copy_str(&netrunner_settings, entry, string_value);
                }
                break;
            case NetrunnerSettingsTypeInt:
                if(netrunner_format_read_int32(file, entry->key, &int_value, 1)) {
                    netrunner_settings_entry_set(entry, &netrunner_settings, int_value);
                }
                break;
            case NetrunnerSettingsTypeUint:
            case NetrunnerSettingsTypeEnum:
                if(netrunner_format_read_uint32(file, entry->key, &uint_value, 1)) {
                    netrunner_settings_entry_set(entry, &netrunner_settings, (int32_t)uint_value);
                }
                break;
            case NetrunnerSettingsTypeBool:
                if(netrunner_format_read_bool(file, entry->key, &bool_value, 1)) {
                    netrunner_settings_entry_set(entry, &netrunner_settings, bool_value ? 1 : 0);
                }
                break;
            }
        }
    }

    netrunner_settings_sanitize(&netrunner_settings);
    if(!netrunner_settings_equal(&previous_settings, &netrunner_settings)) {
        netrunner_settings_revision++;
    }

    FURI_LOG_I(
        TAG,
        "%s: pack='%s', speed=%lu, cycle=%ld, unlock=%s, menu=%u",
        opened ? "Loaded" : "Defaults",
        netrunner_settings.asset_pack,
        (unsigned long)netrunner_settings.anim_speed,
        (long)netrunner_settings.cycle_anims,
        netrunner_settings.unlock_anims ? "on" : "off",
        (unsigned)netrunner_settings.menu_style);

    furi_string_free(string_value);
    netrunner_format_free(file);
    furi_record_close(RECORD_STORAGE);
}

bool netrunner_settings_save(void) {
    netrunner_settings_sanitize(&netrunner_settings);
    netrunner_settings_revision++;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    NetrunnerFormat* file = netrunner_format_file_alloc(storage);
    bool success = netrunner_format_file_open_always(file, NETRUNNER_SETTINGS_PATH);

    for(size_t i = 0; success && i < netrunner_settings_entry_count; ++i) {
        const NetrunnerSettingsEntry* entry = &netrunner_settings_entries[i];
        const int64_t value = netrunner_settings_entry_get(entry, &netrunner_settings);
        int32_t int_value = (int32_t)value;
        uint32_t uint_value = (uint32_t)value;
        bool bool_value = value != 0;

        switch(entry->type) {
        case NetrunnerSettingsTypeStr:
            success = netrunner_format_write_string_cstr(
                file, entry->key, netrunner_settings_field(&netrunner_settings, entry));
            break;
        case NetrunnerSettingsTypeInt:
            success = netrunner_format_write_int32(file, entry->key, &int_value, 1);
            break;
        case NetrunnerSettingsTypeUint:
        case NetrunnerSettingsTypeEnum:
            success = netrunner_format_write_uint32(file, entry->key, &uint_value, 1);
            break;
        case NetrunnerSettingsTypeBool:
            success = netrunner_format_write_bool(file, entry->key, &bool_value, 1);
            break;
        }
    }

    if(!success) {
        FURI_LOG_E(TAG, "Failed to save %s", NETRUNNER_SETTINGS_PATH);
    }

    netrunner_format_free(file);
    furi_record_close(RECORD_STORAGE);
    return success;
}
