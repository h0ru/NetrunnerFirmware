#include <furi.h>
#include <furi_hal.h>
#include <netrunner.h>
#include <applications.h>
#include <netrunner/netrunner.h>
#include <toolbox/name_generator.h>
#include <asset_installer.h>

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_rom_uart.h>

static const char* TAG = "Main";

static bool service_stack_is_psram_safe(const char* appid) {
    if(!appid) return false;

    /* These event/render services never initiate flash or NVS writes from
     * their own tasks. Their 16 KiB of stacks needlessly occupied the scarce
     * internal heap needed by BLE, WiFi SoftAP, and foreground apps. */
    return strcmp(appid, "input") == 0 || strcmp(appid, "gui") == 0 ||
           strcmp(appid, "dialogs") == 0;
}

static void log_internal_registry(
    const char* label,
    const NetrunnerInternalApplication* apps,
    size_t count) {
    if(count == 0) {
        ESP_LOGI(TAG, "%s: none", label);
        return;
    }

    for(size_t i = 0; i < count; ++i) {
        ESP_LOGI(
            TAG,
            "%s[%u]: appid=%s name=%s stack=%lu",
            label,
            (unsigned)i,
            apps[i].appid ? apps[i].appid : "(null)",
            apps[i].name ? apps[i].name : "(null)",
            (unsigned long)apps[i].stack_size);
    }
}

static void log_external_registry(
    const char* label,
    const NetrunnerExternalApplication* apps,
    size_t count) {
    if(count == 0) {
        ESP_LOGI(TAG, "%s: none", label);
        return;
    }

    for(size_t i = 0; i < count; ++i) {
        ESP_LOGI(
            TAG,
            "%s[%u]: name=%s launch=%s",
            label,
            (unsigned)i,
            apps[i].name ? apps[i].name : "(null)",
            apps[i].path ? apps[i].path : "(null)");
    }
}

static bool registry_contains_appid(
    const NetrunnerInternalApplication* apps,
    size_t count,
    const char* appid) {
    for(size_t i = 0; i < count; ++i) {
        if(apps[i].appid && strcmp(apps[i].appid, appid) == 0) {
            return true;
        }
    }

    return false;
}

static bool registry_contains_launch(
    const NetrunnerExternalApplication* apps,
    size_t count,
    const char* launch) {
    for(size_t i = 0; i < count; ++i) {
        if(apps[i].path && strcmp(apps[i].path, launch) == 0) {
            return true;
        }
    }

    return false;
}

static void log_registry_snapshot(void) {
    ESP_LOGI(
        TAG,
        "Registry counts: services=%u startup=%u apps=%u settings=%u extsettings=%u external=%u internal_external=%u system=%u debug=%u",
        (unsigned)NETRUNNER_SERVICES_COUNT,
        (unsigned)NETRUNNER_ON_SYSTEM_START_COUNT,
        (unsigned)NETRUNNER_APPS_COUNT,
        (unsigned)NETRUNNER_SETTINGS_APPS_COUNT,
        (unsigned)NETRUNNER_EXTSETTINGS_APPS_COUNT,
        (unsigned)NETRUNNER_EXTERNAL_APPS_COUNT,
        (unsigned)NETRUNNER_INTERNAL_EXTERNAL_APPS_COUNT,
        (unsigned)NETRUNNER_SYSTEM_APPS_COUNT,
        (unsigned)NETRUNNER_DEBUG_APPS_COUNT);

    log_internal_registry("service", NETRUNNER_SERVICES, NETRUNNER_SERVICES_COUNT);
    log_internal_registry("settings", NETRUNNER_SETTINGS_APPS, NETRUNNER_SETTINGS_APPS_COUNT);
    log_external_registry("extsettings", NETRUNNER_EXTSETTINGS_APPS, NETRUNNER_EXTSETTINGS_APPS_COUNT);

    if(!registry_contains_appid(NETRUNNER_SERVICES, NETRUNNER_SERVICES_COUNT, "bt")) {
        ESP_LOGW(TAG, "Bluetooth service appid 'bt' is not present in NETRUNNER_SERVICES");
    }

    if(!registry_contains_appid(NETRUNNER_SETTINGS_APPS, NETRUNNER_SETTINGS_APPS_COUNT, "bt_settings") &&
       !registry_contains_launch(
           NETRUNNER_EXTSETTINGS_APPS, NETRUNNER_EXTSETTINGS_APPS_COUNT, "bt_settings")) {
        ESP_LOGW(
            TAG,
            "Bluetooth settings app 'bt_settings' is not present in settings or extsettings registry");
    }
}

static void furi_log_esp_callback(const uint8_t* data, size_t size, void* context) {
    (void)context;
    for(size_t i = 0; i < size; ++i) {
        esp_rom_output_putc((char)data[i]);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting Furi Core on ESP32...");

    // ESP-IDF: Scheduler is already running!
    furi_init();

    // Register ESP32 log handler
    FuriLogHandler log_handler = {
        .callback = furi_log_esp_callback,
        .context = NULL,
    };
    furi_log_add_handler(log_handler);

    // Services can read netrunner_settings before storage is up and the file is
    // loaded below, so give it its defaults before any of them start.
    netrunner_settings_set_defaults(&netrunner_settings);

    furi_hal_init_early();
    furi_hal_init();
    netrunner_init();
    log_registry_snapshot();

    for(size_t i = 0; i < NETRUNNER_SERVICES_COUNT; i++) {
        FuriThread* thread;
        if(service_stack_is_psram_safe(NETRUNNER_SERVICES[i].appid)) {
            thread = furi_thread_alloc_service_psram(
                NETRUNNER_SERVICES[i].name,
                NETRUNNER_SERVICES[i].stack_size,
                NETRUNNER_SERVICES[i].app,
                NULL);
        } else {
            thread = furi_thread_alloc_service(
                NETRUNNER_SERVICES[i].name,
                NETRUNNER_SERVICES[i].stack_size,
                NETRUNNER_SERVICES[i].app,
                NULL);
        }
        furi_thread_set_appid(thread, NETRUNNER_SERVICES[i].appid);
        furi_thread_start(thread);

        if(NETRUNNER_SERVICES[i].appid &&
           strcmp(NETRUNNER_SERVICES[i].appid, RECORD_STORAGE) == 0) {
            // Release images carry the SD content this port adds in an `assets`
            // partition. Install it before anything reads the card, so a fresh
            // card is complete by the time settings and asset packs load.
            // No-op on personal builds, which have no such partition.
            asset_installer_run();
            // Storage publishes its record after GUI-backed SD status setup. Opening the
            // record here waits for that point and loads settings before desktop starts.
            netrunner_settings_load();
            name_generator_set_prefix_after(netrunner_settings.file_naming_prefix_after);
            /* Do not scan and decode a complete SD asset pack on the boot
             * thread. On ESP32 this can take seconds and consumes PSRAM while
             * the desktop is still black; the same pressure makes SD FAPs
             * launch with an unreliable heap. The selected pack remains on
             * the card and can be applied by a later settings action. */
        }

        furi_delay_ms(10);
    }

    for(size_t i = 0; i < NETRUNNER_ON_SYSTEM_START_COUNT; i++) {
        NETRUNNER_ON_SYSTEM_START[i]();
    }

    ESP_LOGI(
        TAG,
        "All services started: internal free=%u largest=%u; entering background",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    // This blocks forever (thread scrubber)
    furi_background();
}
