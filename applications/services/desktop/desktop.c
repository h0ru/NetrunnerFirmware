#include "desktop_i.h"

#include <string.h>
#include <esp_heap_caps.h>

#include <cli/cli_vcp.h>

#include <gui/gui_i.h>

#include <locale/locale.h>
#include <netrunner/settings.h>
#include <storage/storage.h>
#include <btshim.h>
#include <wifi/wlan_hal.h>

#include <assets_icons.h>

#include "scenes/desktop_scene.h"
#include "scenes/desktop_scene_locked.h"

#include "helpers/qnetrunner_bridge.h"
#include <furi_hal_usb_tinyusb_composite.h>
#include "helpers/qnetrunner_usj_cmd.h"

#include "furi_hal_power.h"

#define TAG "Desktop"
/* Verzoegerung fuer den qNetrunner-Bridge-Resume nach einem OTA-Reboot. */
#define QNETRUNNER_RESUME_DELAY_MS 5000
/* Poll-Intervall fuer das "qnetrunner"-Kommando auf der USJ-Konsole. */
#define QNETRUNNER_USJ_POLL_MS 100


static void desktop_auto_lock_arm(Desktop*);
static void desktop_auto_lock_inhibit(Desktop*);
static void desktop_start_auto_lock_timer(Desktop*);
static void desktop_qnetrunner_resume_timer_callback(void* context);
static void desktop_qnetrunner_usj_timer_callback(void* context);
static void desktop_apply_settings(Desktop*);

static void desktop_loader_callback(const void* message, void* context) {
    furi_assert(context);
    Desktop* desktop = context;
    const LoaderEvent* event = message;

    if(event->type == LoaderEventTypeApplicationBeforeLoad ||
       event->type == LoaderEventTypeApplicationsBrowserOpened ||
       event->type == LoaderEventTypeMenuOpened) {
        view_dispatcher_send_custom_event(
            desktop->view_dispatcher,
            event->type == LoaderEventTypeMenuOpened ? DesktopGlobalLauncherMenuOpened :
            event->type == LoaderEventTypeApplicationsBrowserOpened ?
                DesktopGlobalApplicationsBrowserOpened : DesktopGlobalBeforeAppStarted);
        /* SD animation loading on T-Embed can exceed the STM32's old 3 s
         * deadline. Keep the acknowledgement paired with this request rather
         * than rebooting, or continuing with an unconsumed acknowledgement. */
        if(furi_semaphore_acquire(desktop->animation_semaphore, 3000) != FuriStatusOk) {
            FURI_LOG_W("Desktop", "Waiting for desktop animation to become idle");
            furi_check(furi_semaphore_acquire(desktop->animation_semaphore, FuriWaitForever) == FuriStatusOk);
        }
    } else if(event->type == LoaderEventTypeNoMoreAppsInQueue) {
        view_dispatcher_send_custom_event(desktop->view_dispatcher, DesktopGlobalAfterAppFinished);
    } else if(event->type == LoaderEventTypeApplicationsBrowserClosed) {
        view_dispatcher_send_custom_event(
            desktop->view_dispatcher, DesktopGlobalApplicationsBrowserClosed);
    } else if(event->type == LoaderEventTypeMenuClosed) {
        view_dispatcher_send_custom_event(
            desktop->view_dispatcher, DesktopGlobalLauncherMenuClosed);
    }
}

static void desktop_storage_callback(const void* message, void* context) {
    furi_assert(context);
    Desktop* desktop = context;
    const StorageEvent* event = message;

    if(event->type == StorageEventTypeCardMount) {
        view_dispatcher_send_custom_event(desktop->view_dispatcher, DesktopGlobalReloadSettings);
    }
}

static void desktop_lock_icon_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    furi_assert(canvas);
    canvas_draw_icon(canvas, 0, 0, &I_Lock_7x8);
}

/* Compact 9x8 WiFi glyph drawn natively so it follows the same monochrome
 * status-bar treatment as Bluetooth without adding a generated asset. */
static void desktop_wifi_icon_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    furi_assert(canvas);

    canvas_draw_line(canvas, 0, 2, 4, 0);
    canvas_draw_line(canvas, 4, 0, 8, 2);
    canvas_draw_line(canvas, 2, 4, 4, 3);
    canvas_draw_line(canvas, 4, 3, 6, 4);
    canvas_draw_line(canvas, 3, 6, 4, 5);
    canvas_draw_line(canvas, 4, 5, 5, 6);
    canvas_draw_dot(canvas, 4, 7);
}

// Dummy-/Game-Mode-Icon: im Port noch nicht verdrahtet (vgl. stm32/-Referenz) -> bewusst ungenutzt
__attribute__((unused)) static void
    desktop_dummy_mode_icon_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    furi_assert(canvas);
    canvas_draw_icon(canvas, 0, 0, &I_GameMode_11x8);
}

static void desktop_clock_update(Desktop* desktop) {
    furi_assert(desktop);

    DateTime curr_dt;
    furi_hal_rtc_get_datetime(&curr_dt);
    bool time_format_12 = locale_get_time_format() == LocaleTimeFormat12h;

    if(desktop->clock.hour != curr_dt.hour || desktop->clock.minute != curr_dt.minute ||
       desktop->clock.format_12 != time_format_12) {
        desktop->clock.format_12 = time_format_12;
        desktop->clock.hour = curr_dt.hour;
        desktop->clock.minute = curr_dt.minute;
        view_port_update(desktop->clock_viewport);
    }
}

static void desktop_clock_reconfigure(Desktop* desktop) {
    furi_assert(desktop);

    desktop_clock_update(desktop);

    if(desktop->settings.display_clock) {
        furi_timer_start(desktop->update_clock_timer, furi_ms_to_ticks(1000));
    } else {
        furi_timer_stop(desktop->update_clock_timer);
    }

    view_port_enabled_set(desktop->clock_viewport, desktop->settings.display_clock);
}

static void desktop_clock_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    furi_assert(canvas);

    Desktop* desktop = context;

    canvas_set_font(canvas, FontPrimary);

    uint8_t hour = desktop->clock.hour;
    if(desktop->clock.format_12) {
        if(hour > 12) {
            hour -= 12;
        }
        if(hour == 0) {
            hour = netrunner_settings.midnight_format_00 ? 0 : 12;
        }
    }

    char buffer[20];
    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug)) {
        snprintf(buffer, sizeof(buffer), "D %02u:%02u", hour, desktop->clock.minute);
    } else {
        snprintf(buffer, sizeof(buffer), "%02u:%02u", hour, desktop->clock.minute);
    }

    view_port_set_width(
        desktop->clock_viewport,
        canvas_string_width(canvas, buffer) - 1 + (desktop->clock.minute % 10 == 1));

    canvas_draw_str_aligned(canvas, 0, 8, AlignLeft, AlignBottom, buffer);
}

static void desktop_stealth_mode_icon_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    furi_assert(canvas);
    canvas_draw_icon(canvas, 0, 0, &I_Muted_8x8);
}

/* Wake mode: 8x8 sun — hollow ring plus eight rays. Drawn natively like the
 * WiFi glyph above rather than as an asset, so an asset pack cannot restyle it. */
static void desktop_wake_icon_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    furi_assert(canvas);

    canvas_draw_frame(canvas, 2, 2, 4, 4);
    canvas_draw_line(canvas, 3, 0, 4, 0);
    canvas_draw_line(canvas, 3, 7, 4, 7);
    canvas_draw_line(canvas, 0, 3, 0, 4);
    canvas_draw_line(canvas, 7, 3, 7, 4);
    canvas_draw_dot(canvas, 1, 1);
    canvas_draw_dot(canvas, 6, 1);
    canvas_draw_dot(canvas, 1, 6);
    canvas_draw_dot(canvas, 6, 6);
}

/* Web-Filesystem background indicator: a small broadcast tower (mast + base +
 * radiating waves). Shown on the desktop whenever the Web-FS server is still
 * holding the radio in the background (wlan_hal_is_background_hold()). */
static void desktop_webfs_icon_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    furi_assert(canvas);

    canvas_draw_line(canvas, 4, 3, 4, 7); // mast
    canvas_draw_line(canvas, 2, 7, 6, 7); // base
    canvas_draw_dot(canvas, 4, 1); // top
    canvas_draw_dot(canvas, 2, 2); // left waves
    canvas_draw_dot(canvas, 1, 4);
    canvas_draw_dot(canvas, 6, 2); // right waves
    canvas_draw_dot(canvas, 7, 4);
}

static bool desktop_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    Desktop* desktop = (Desktop*)context;

    if(event == DesktopGlobalBeforeAppStarted ||
       event == DesktopGlobalApplicationsBrowserOpened ||
       event == DesktopGlobalLauncherMenuOpened) {
        if(animation_manager_is_animation_loaded(desktop->animation_manager)) {
            animation_manager_unload_and_stall_animation(desktop->animation_manager);
        }

        desktop_auto_lock_inhibit(desktop);

        desktop->app_running = true;

        if(event == DesktopGlobalApplicationsBrowserOpened || event == DesktopGlobalLauncherMenuOpened) {
            if(event == DesktopGlobalApplicationsBrowserOpened) desktop->applications_browser_open = true;
            else desktop->launcher_menu_open = true;
            furi_semaphore_release(desktop->animation_semaphore);
            return true;
        }

        /* WiFi/ESP-NOW abschalten, damit Apps wie wlan_app/esp_now/nrf24 ihren
         * eigenen WiFi-Stack initialisieren können. */
        wlan_hal_suspend_user_radio();

        furi_semaphore_release(desktop->animation_semaphore);

    } else if(event == DesktopGlobalAfterAppFinished ||
              event == DesktopGlobalApplicationsBrowserClosed ||
              event == DesktopGlobalLauncherMenuClosed) {
        if(event == DesktopGlobalApplicationsBrowserClosed) {
            desktop->applications_browser_open = false;
        } else if(event == DesktopGlobalLauncherMenuClosed) {
            desktop->launcher_menu_open = false;
        } else {
            wlan_hal_resume_user_radio();
        }
        desktop_set_wifi_icon_state(
            desktop, wlan_hal_is_user_enabled() && !wlan_hal_is_held_after_update());
        /* Web-FS background indicator: on while the server holds the radio. */
        view_port_enabled_set(
            desktop->webfs_icon_viewport, wlan_hal_is_background_hold());
        /* Locale and Netrunner midnight format can change while Settings owns
         * the screen. Refresh both the cached format and the clock immediately
         * instead of waiting for the next changed minute. */
        desktop_clock_update(desktop);
        view_port_update(desktop->clock_viewport);
        /* Both launcher layers can outlive an app. Reload only after both
         * close, so reopening Apps from the menu never races an SD reload. */
        if(!desktop->applications_browser_open && !desktop->launcher_menu_open) {
            if(!animation_manager_is_animation_loaded(desktop->animation_manager)) {
                animation_manager_load_and_continue_animation(desktop->animation_manager);
            }
            desktop_auto_lock_arm(desktop);
        }
        desktop->app_running = desktop->applications_browser_open || desktop->launcher_menu_open;

    } else if(event == DesktopGlobalAutoLock) {
        if(!desktop->app_running && !desktop->locked) {
            // Disable AutoLock if usb_inhibit_autolock option enabled and device have active USB session.
            if((desktop->settings.usb_inhibit_auto_lock) && (furi_hal_usb_is_locked())) {
                return (0);
            }

            desktop_lock(desktop);
        }
    } else if(event == DesktopGlobalSaveSettings) {
        desktop_settings_save(&desktop->settings);
        desktop_apply_settings(desktop);

    } else if(event == DesktopGlobalReloadSettings) {
        desktop_settings_load(&desktop->settings);
        desktop_apply_settings(desktop);

        /* The chosen wallpaper is an idle animation. At boot the idle screen is
         * picked before the SD card finishes mounting, so an SD-resident
         * wallpaper is not found yet and the default is shown; this event also
         * fires on card mount (see desktop_storage_callback). Re-roll the idle
         * selection here so the wallpaper is applied as soon as it becomes
         * resolvable, instead of only after the user leaves and re-enters the
         * main screen. No-op unless the animation is currently idle, so apps
         * and blocking animations are never interrupted. */
        if(!netrunner_settings.unlock_anims && netrunner_settings.wallpaper[0]) {
            animation_manager_new_idle_process(desktop->animation_manager);
        }

    } else if(event == DesktopGlobalQnetrunnerStop) {
        /* Auto-off der Bridge (Host weg): derselbe Pfad wie "Disable qNetrunner"
         * im Lock-Menue — Bridge stoppen, Composite abbauen, PHY zurueck zu USJ. */
        if(qnetrunner_bridge_is_active()) {
            FURI_LOG_I(TAG, "Host disconnected — leaving qNetrunner mode");
            qnetrunner_bridge_stop();
            furi_hal_usb_composite_uninstall();
            desktop_scene_lock_menu_refresh(desktop);
        }

    } else if(event == DesktopGlobalQnetrunnerStart) {
        /* Laeuft auf dem Desktop-Thread mit fertig hochgefahrenem System —
         * exakt der Pfad des Lock-Menue-Toggles "Enable qNetrunner". Quelle:
         * OTA-Resume-Timer oder "qnetrunner"-Kommando von qT-Embed (USJ). */
        if(qnetrunner_bridge_is_active()) {
            FURI_LOG_D(TAG, "qNetrunner bridge already active");
        } else {
            FURI_LOG_I(TAG, "Starting qNetrunner bridge (host request / OTA resume)");
            if(!qnetrunner_bridge_start()) {
                FURI_LOG_W(TAG, "qNetrunner bridge start failed (no USB-OTG on this board?)");
            }
            /* Falls das Lock-Menue gerade offen ist: Label "Enable" → "Disable qNetrunner"
             * nachziehen (die View liest den Zustand sonst nur beim Betreten). */
            desktop_scene_lock_menu_refresh(desktop);
        }

    } else {
        return scene_manager_handle_custom_event(desktop->scene_manager, event);
    }

    return true;
}

static bool desktop_back_event_callback(void* context) {
    furi_assert(context);
    Desktop* desktop = (Desktop*)context;
    return scene_manager_handle_back_event(desktop->scene_manager);
}

static void desktop_tick_event_callback(void* context) {
    furi_assert(context);
    Desktop* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static void desktop_input_event_callback(const void* value, void* context) {
    furi_assert(value);
    furi_assert(context);
    const InputEvent* event = value;
    Desktop* desktop = context;
    if(event->type == InputTypePress) {
        desktop_start_auto_lock_timer(desktop);
    }
}

static void desktop_auto_lock_timer_callback(void* context) {
    furi_assert(context);
    Desktop* desktop = context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, DesktopGlobalAutoLock);
}

static void desktop_qnetrunner_resume_timer_callback(void* context) {
    furi_assert(context);
    Desktop* desktop = context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, DesktopGlobalQnetrunnerStart);
}

/* Alle 100 ms den USJ-RX-FIFO auf das "qnetrunner"-Kommando von qT-Embed pruefen
 * (nur solange das Composite nicht installiert ist — sonst liefert der HAL 0). */
/* Aus dem Bridge-Thread: nur Event posten, der Stop laeuft auf dem Desktop-Thread. */
static void desktop_qnetrunner_auto_off_callback(void* context) {
    furi_assert(context);
    Desktop* desktop = context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, DesktopGlobalQnetrunnerStop);
}

static void desktop_qnetrunner_usj_timer_callback(void* context) {
    furi_assert(context);
    Desktop* desktop = context;
    if(qnetrunner_usj_cmd_poll()) {
        view_dispatcher_send_custom_event(desktop->view_dispatcher, DesktopGlobalQnetrunnerStart);
    }
}

static void desktop_start_auto_lock_timer(Desktop* desktop) {
    furi_timer_start(
        desktop->auto_lock_timer, furi_ms_to_ticks(desktop->settings.auto_lock_delay_ms));
}

static void desktop_stop_auto_lock_timer(Desktop* desktop) {
    furi_timer_stop(desktop->auto_lock_timer);
}

static void desktop_auto_lock_arm(Desktop* desktop) {
    if(desktop->settings.auto_lock_delay_ms) {
        if(!desktop->input_events_subscription) {
            desktop->input_events_subscription = furi_pubsub_subscribe(
                desktop->input_events_pubsub, desktop_input_event_callback, desktop);
        }
        desktop_start_auto_lock_timer(desktop);
    }
}

static void desktop_auto_lock_inhibit(Desktop* desktop) {
    desktop_stop_auto_lock_timer(desktop);
    if(desktop->input_events_subscription) {
        furi_pubsub_unsubscribe(desktop->input_events_pubsub, desktop->input_events_subscription);
        desktop->input_events_subscription = NULL;
    }
}

static void desktop_clock_timer_callback(void* context) {
    furi_assert(context);
    Desktop* desktop = context;

    const bool clock_enabled = gui_active_view_port_count(desktop->gui, GuiLayerStatusBarLeft) < 6;

    if(clock_enabled) {
        desktop_clock_update(desktop);
    }

    view_port_enabled_set(desktop->clock_viewport, clock_enabled);
}

static void desktop_apply_settings(Desktop* desktop) {
    desktop->in_transition = true;

    desktop_clock_reconfigure(desktop);

    // Dummy mode disabled on ESP32 port — always use normal mode
    desktop_main_set_dummy_mode_state(desktop->main_view, false);

    if(!desktop->app_running && !desktop->locked) {
        desktop_auto_lock_arm(desktop);
    }

    desktop->in_transition = false;
}

static void desktop_init_settings(Desktop* desktop) {
    furi_pubsub_subscribe(storage_get_pubsub(desktop->storage), desktop_storage_callback, desktop);

    if(storage_sd_status(desktop->storage) != FSE_OK) {
        FURI_LOG_D(TAG, "SD Card not ready, skipping settings");
        return;
    }

    desktop_settings_load(&desktop->settings);
    desktop_apply_settings(desktop);
}

static Desktop* desktop_alloc(void) {
    Desktop* desktop = malloc(sizeof(Desktop));

    desktop->animation_semaphore = furi_semaphore_alloc(1, 0);
    desktop->animation_manager = animation_manager_alloc();
    desktop->gui = furi_record_open(RECORD_GUI);
    desktop->scene_thread = furi_thread_alloc();
    desktop->view_dispatcher = view_dispatcher_alloc();
    desktop->scene_manager = scene_manager_alloc(&desktop_scene_handlers, desktop);

    view_dispatcher_attach_to_gui(
        desktop->view_dispatcher, desktop->gui, ViewDispatcherTypeDesktop);
    view_dispatcher_set_tick_event_callback(
        desktop->view_dispatcher, desktop_tick_event_callback, 500);

    view_dispatcher_set_event_callback_context(desktop->view_dispatcher, desktop);
    view_dispatcher_set_custom_event_callback(
        desktop->view_dispatcher, desktop_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        desktop->view_dispatcher, desktop_back_event_callback);

    desktop->lock_menu = desktop_lock_menu_alloc();
    desktop->usb_storage_view = desktop_usb_storage_alloc();
    desktop->debug_view = desktop_debug_alloc();
    desktop->popup = popup_alloc();
    desktop->locked_view = desktop_view_locked_alloc();
    desktop->pin_input_view = desktop_view_pin_input_alloc();
    desktop->pin_timeout_view = desktop_view_pin_timeout_alloc();
    desktop->slideshow_view = desktop_view_slideshow_alloc();

    desktop->main_view_stack = view_stack_alloc();
    desktop->main_view = desktop_main_alloc();
    View* dolphin_view = animation_manager_get_animation_view(desktop->animation_manager);
    view_stack_add_view(desktop->main_view_stack, desktop_main_get_view(desktop->main_view));
    view_stack_add_view(desktop->main_view_stack, dolphin_view);
    view_stack_add_view(
        desktop->main_view_stack, desktop_view_locked_get_view(desktop->locked_view));

    /* locked view (as animation view) attends in 2 scenes: main & locked,
     * because it has to draw "Unlocked" label on main scene */
    desktop->locked_view_stack = view_stack_alloc();
    view_stack_add_view(desktop->locked_view_stack, dolphin_view);
    view_stack_add_view(
        desktop->locked_view_stack, desktop_view_locked_get_view(desktop->locked_view));

    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdMain,
        view_stack_get_view(desktop->main_view_stack));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdLocked,
        view_stack_get_view(desktop->locked_view_stack));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdLockMenu,
        desktop_lock_menu_get_view(desktop->lock_menu));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdUsbStorage,
        desktop_usb_storage_get_view(desktop->usb_storage_view));
    view_dispatcher_add_view(
        desktop->view_dispatcher, DesktopViewIdDebug, desktop_debug_get_view(desktop->debug_view));
    view_dispatcher_add_view(
        desktop->view_dispatcher, DesktopViewIdPopup, popup_get_view(desktop->popup));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdPinTimeout,
        desktop_view_pin_timeout_get_view(desktop->pin_timeout_view));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdPinInput,
        desktop_view_pin_input_get_view(desktop->pin_input_view));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdSlideshow,
        desktop_view_slideshow_get_view(desktop->slideshow_view));

    // Lock icon
    desktop->lock_icon_viewport = view_port_alloc();
    view_port_set_width(desktop->lock_icon_viewport, icon_get_width(&I_Lock_7x8));
    view_port_draw_callback_set(
        desktop->lock_icon_viewport, desktop_lock_icon_draw_callback, desktop);
    view_port_enabled_set(desktop->lock_icon_viewport, false);
    gui_add_view_port(desktop->gui, desktop->lock_icon_viewport, GuiLayerStatusBarLeft);

    // WiFi radio icon (enabled by the persisted lock-menu switch)
    desktop->wifi_icon_viewport = view_port_alloc();
    view_port_set_width(desktop->wifi_icon_viewport, 9);
    view_port_draw_callback_set(
        desktop->wifi_icon_viewport, desktop_wifi_icon_draw_callback, desktop);
    view_port_enabled_set(desktop->wifi_icon_viewport, false);
    gui_add_view_port(desktop->gui, desktop->wifi_icon_viewport, GuiLayerStatusBarLeft);

    // Dummy mode icon (disabled — no dummy mode on ESP32 port)
    desktop->dummy_mode_icon_viewport = NULL;

    // Clock
    desktop->clock_viewport = view_port_alloc();
    view_port_set_width(desktop->clock_viewport, 25);
    view_port_draw_callback_set(desktop->clock_viewport, desktop_clock_draw_callback, desktop);
    view_port_enabled_set(desktop->clock_viewport, false);
    gui_add_view_port(desktop->gui, desktop->clock_viewport, GuiLayerStatusBarRight);

    // Stealth mode icon
    desktop->stealth_mode_icon_viewport = view_port_alloc();
    view_port_set_width(desktop->stealth_mode_icon_viewport, icon_get_width(&I_Muted_8x8));
    view_port_draw_callback_set(
        desktop->stealth_mode_icon_viewport, desktop_stealth_mode_icon_draw_callback, desktop);
    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode)) {
        view_port_enabled_set(desktop->stealth_mode_icon_viewport, true);
    } else {
        view_port_enabled_set(desktop->stealth_mode_icon_viewport, false);
    }
    gui_add_view_port(desktop->gui, desktop->stealth_mode_icon_viewport, GuiLayerStatusBarLeft);

    // Wake mode icon (Control Centre tile; off at boot, like s_wake_mode)
    desktop->wake_icon_viewport = view_port_alloc();
    view_port_set_width(desktop->wake_icon_viewport, 8);
    view_port_draw_callback_set(
        desktop->wake_icon_viewport, desktop_wake_icon_draw_callback, desktop);
    /* view_port_alloc() defaults to enabled — clearing it before the add is
     * load-bearing, or the icon shows and counts against the clock from boot. */
    view_port_enabled_set(desktop->wake_icon_viewport, false);
    gui_add_view_port(desktop->gui, desktop->wake_icon_viewport, GuiLayerStatusBarLeft);

    desktop->webfs_icon_viewport = view_port_alloc();
    view_port_set_width(desktop->webfs_icon_viewport, 9);
    view_port_draw_callback_set(
        desktop->webfs_icon_viewport, desktop_webfs_icon_draw_callback, desktop);
    view_port_enabled_set(desktop->webfs_icon_viewport, false);
    gui_add_view_port(desktop->gui, desktop->webfs_icon_viewport, GuiLayerStatusBarLeft);

    // Unload animations before starting an application
    desktop->loader = furi_record_open(RECORD_LOADER);
    furi_pubsub_subscribe(loader_get_pubsub(desktop->loader), desktop_loader_callback, desktop);

    desktop->storage = furi_record_open(RECORD_STORAGE);
    desktop->notification = furi_record_open(RECORD_NOTIFICATION);
    desktop->input_events_pubsub = furi_record_open(RECORD_INPUT_EVENTS);

    desktop->auto_lock_timer =
        furi_timer_alloc(desktop_auto_lock_timer_callback, FuriTimerTypeOnce, desktop);
    desktop->qnetrunner_resume_timer =
        furi_timer_alloc(desktop_qnetrunner_resume_timer_callback, FuriTimerTypeOnce, desktop);
    desktop->qnetrunner_usj_timer =
        furi_timer_alloc(desktop_qnetrunner_usj_timer_callback, FuriTimerTypePeriodic, desktop);
    qnetrunner_bridge_set_auto_off_callback(desktop_qnetrunner_auto_off_callback, desktop);

    desktop->status_pubsub = furi_pubsub_alloc();

    desktop->update_clock_timer =
        furi_timer_alloc(desktop_clock_timer_callback, FuriTimerTypePeriodic, desktop);

    desktop->app_running = loader_is_locked(desktop->loader);

    furi_record_create(RECORD_DESKTOP, desktop);

    return desktop;
}

/*
 * Private API
 */

void desktop_lock(Desktop* desktop) {
    furi_assert(!desktop->locked);

    furi_hal_rtc_set_flag(FuriHalRtcFlagLock);

    if(desktop_pin_code_is_set()) {
        CliVcp* cli_vcp = furi_record_open(RECORD_CLI_VCP);
        cli_vcp_disable(cli_vcp);
        furi_record_close(RECORD_CLI_VCP);
    }

    desktop_auto_lock_inhibit(desktop);
    scene_manager_set_scene_state(
        desktop->scene_manager, DesktopSceneLocked, DesktopSceneLockedStateFirstEnter);
    scene_manager_next_scene(desktop->scene_manager, DesktopSceneLocked);

    DesktopStatus status = {.locked = true};
    furi_pubsub_publish(desktop->status_pubsub, &status);

    desktop->locked = true;
}

void desktop_unlock(Desktop* desktop) {
    furi_assert(desktop->locked);

    view_port_enabled_set(desktop->lock_icon_viewport, false);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_set_lockdown(gui, false);
    furi_record_close(RECORD_GUI);
    desktop_view_locked_unlock(desktop->locked_view);
    scene_manager_search_and_switch_to_previous_scene(desktop->scene_manager, DesktopSceneMain);
    desktop_auto_lock_arm(desktop);
    furi_hal_rtc_reset_flag(FuriHalRtcFlagLock);
    furi_hal_rtc_set_pin_fails(0);

    if(desktop_pin_code_is_set()) {
        CliVcp* cli_vcp = furi_record_open(RECORD_CLI_VCP);
        cli_vcp_enable(cli_vcp);
        furi_record_close(RECORD_CLI_VCP);
    }

    DesktopStatus status = {.locked = false};
    furi_pubsub_publish(desktop->status_pubsub, &status);

    desktop->locked = false;
}

void desktop_set_dummy_mode_state(Desktop* desktop, bool enabled) {
    UNUSED(desktop);
    UNUSED(enabled);
    // Dummy mode disabled on ESP32 port
}

void desktop_set_stealth_mode_state(Desktop* desktop, bool enabled) {
    desktop->in_transition = true;

    if(enabled) {
        furi_hal_rtc_set_flag(FuriHalRtcFlagStealthMode);
    } else {
        furi_hal_rtc_reset_flag(FuriHalRtcFlagStealthMode);
    }

    view_port_enabled_set(desktop->stealth_mode_icon_viewport, enabled);

    desktop->in_transition = false;
}

void desktop_set_wifi_icon_state(Desktop* desktop, bool enabled) {
    furi_assert(desktop);
    UNUSED(enabled);
    view_port_enabled_set(desktop->wifi_icon_viewport, false);
}

void desktop_set_wake_icon_state(Desktop* desktop, bool enabled) {
    furi_assert(desktop);
    view_port_enabled_set(desktop->wake_icon_viewport, enabled);
}

/*
 *  Public API
 */

bool desktop_api_is_locked(Desktop* instance) {
    furi_assert(instance);
    return furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock);
}

void desktop_api_unlock(Desktop* instance) {
    furi_assert(instance);
    view_dispatcher_send_custom_event(instance->view_dispatcher, DesktopGlobalApiUnlock);
}

FuriPubSub* desktop_api_get_status_pubsub(Desktop* instance) {
    furi_assert(instance);
    return instance->status_pubsub;
}

void desktop_api_reload_settings(Desktop* instance) {
    furi_assert(instance);
    view_dispatcher_send_custom_event(instance->view_dispatcher, DesktopGlobalReloadSettings);
}

void desktop_api_get_settings(Desktop* instance, DesktopSettings* settings) {
    furi_assert(instance);
    furi_assert(settings);

    *settings = instance->settings;
}

void desktop_api_set_settings(Desktop* instance, const DesktopSettings* settings) {
    furi_assert(instance);
    furi_assert(settings);

    instance->settings = *settings;
    view_dispatcher_send_custom_event(instance->view_dispatcher, DesktopGlobalSaveSettings);
}

/*
 * Application thread
 */

int32_t desktop_srv(void* p) {
    UNUSED(p);

    if(furi_hal_rtc_get_boot_mode() != FuriHalRtcBootModeNormal) {
        FURI_LOG_W(TAG, "Skipping start in special boot mode");

        furi_thread_suspend(furi_thread_get_current_id());
        return 0;
    }

    Desktop* desktop = desktop_alloc();

    desktop_init_settings(desktop);

    scene_manager_next_scene(desktop->scene_manager, DesktopSceneMain);

    // desktop_lock() turns the USB CLI off when a PIN is set, so re-enable it
    // afterwards if RPC is allowed while locked.
    bool enable_cli_vcp = true;
    if(desktop_pin_code_is_set() &&
       (netrunner_settings.lock_on_boot || furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock))) {
        enable_cli_vcp = netrunner_settings.allow_locked_rpc_usb;
        desktop_lock(desktop);
    }
    if(enable_cli_vcp) {
        CliVcp* cli_vcp = furi_record_open(RECORD_CLI_VCP);
        cli_vcp_enable(cli_vcp);
        furi_record_close(RECORD_CLI_VCP);
    }

    if(storage_file_exists(desktop->storage, SLIDESHOW_FS_PATH)) {
        scene_manager_next_scene(desktop->scene_manager, DesktopSceneSlideshow);
    }

    if(!furi_hal_version_do_i_belong_here()) {
        scene_manager_next_scene(desktop->scene_manager, DesktopSceneHwMismatch);
    }

    if(furi_hal_rtc_get_fault_data()) {
        scene_manager_next_scene(desktop->scene_manager, DesktopSceneFault);
    }

    uint8_t keys_total, keys_valid;
    if(!furi_hal_crypto_enclave_verify(&keys_total, &keys_valid)) {
        FURI_LOG_E(
            TAG,
            "Secure Enclave verification failed: total %hhu, valid %hhu",
            keys_total,
            keys_valid);

        scene_manager_next_scene(desktop->scene_manager, DesktopSceneSecureEnclave);
    }

    // Special case: autostart application is already running
    if(desktop->app_running && animation_manager_is_animation_loaded(desktop->animation_manager)) {
        animation_manager_unload_and_stall_animation(desktop->animation_manager);
    }

    /* qT-Embed kann die Bridge ueber die USB-Serial-JTAG-Konsole anfordern
     * ("qnetrunner\n"), siehe helpers/qnetrunner_usj_cmd.h. */
    furi_timer_start(desktop->qnetrunner_usj_timer, furi_ms_to_ticks(QNETRUNNER_USJ_POLL_MS));

    view_dispatcher_run(desktop->view_dispatcher);

    // Should never get here (a service thread will crash automatically if it returns)
    return 0;
}
