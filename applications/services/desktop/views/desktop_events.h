#pragma once

typedef enum {
    DesktopMainEventLock,
    DesktopMainEventOpenLockMenu,
    DesktopMainEventOpenArchive,
    DesktopMainEventOpenFavoriteLeftShort,
    DesktopMainEventOpenFavoriteLeftLong,
    DesktopMainEventOpenFavoriteRightShort,
    DesktopMainEventOpenFavoriteRightLong,
    DesktopMainEventOpenFavoriteOkLong,
    DesktopMainEventOpenMenu,
    DesktopMainEventOpenDebug,
    DesktopMainEventOpenPowerOff,
    DesktopMainEventOpenPassport,

    DesktopDummyEventOpenLeft,
    DesktopDummyEventOpenDown,
    DesktopDummyEventOpenOk,
    DesktopDummyEventOpenUpLong,
    DesktopDummyEventOpenDownLong,
    DesktopDummyEventOpenLeftLong,
    DesktopDummyEventOpenRightLong,
    DesktopDummyEventOpenOkLong,

    DesktopLockedEventUnlocked,
    DesktopLockedEventUpdate,
    DesktopLockedEventShowPinInput,
    DesktopLockedEventCoversClosed,
    DesktopLockedEventOpenPowerOff,

    DesktopPinInputEventResetWrongPinLabel,
    DesktopPinInputEventUnlocked,
    DesktopPinInputEventUnlockFailed,
    DesktopPinInputEventBack,

    DesktopPinTimeoutExit,

    DesktopDebugEventToggleDebugMode,
    DesktopDebugEventExit,

    DesktopLockMenuEventBluetoothToggle,
    DesktopLockMenuEventWifiToggle,
    DesktopLockMenuEventWifiScan,
    DesktopLockMenuEventWakeToggle,
    DesktopLockMenuEventDarkModeToggle,
    DesktopLockMenuEventStealthToggle,
    DesktopLockMenuEventLock,
    DesktopLockMenuEventOpenSettings,
    DesktopLockMenuEventOpenApps,
    DesktopLockMenuEventOpenDisplay,
    DesktopLockMenuEventOpenPower,
    DesktopLockMenuEventOpenPassport,
    DesktopLockMenuEventWebFs,
    DesktopLockMenuEventQnetrunnerToggle,

    DesktopUsbStorageEventExit,

    /* qNetrunner-Bridge auf dem Desktop-Thread starten (wie der Lock-Menue-
     * Toggle): verzoegert nach einem OTA-Reboot (RTC-Flag) oder auf das
     * "qnetrunner"-Kommando von qT-Embed ueber die USB-Serial-JTAG-Konsole. */
    DesktopGlobalQnetrunnerStart,
    /* Host hat die qNetrunner-Verbindung beendet (Auto-off der Bridge): Bridge
     * stoppen + Composite abbauen → zurueck zu USB-Serial-JTAG. */
    DesktopGlobalQnetrunnerStop,

    DesktopAnimationEventCheckAnimation,
    DesktopAnimationEventNewIdleAnimation,
    DesktopAnimationEventInteractAnimation,

    DesktopSlideshowCompleted,
    DesktopSlideshowPoweroff,

    DesktopHwMismatchExit,

    DesktopEnclaveExit,

    // Global events
    DesktopGlobalBeforeAppStarted,
    DesktopGlobalAfterAppFinished,
    DesktopGlobalAutoLock,
    DesktopGlobalApiUnlock,
    DesktopGlobalSaveSettings,
    DesktopGlobalReloadSettings,
    DesktopGlobalApplicationsBrowserOpened,
    DesktopGlobalApplicationsBrowserClosed,
    DesktopGlobalLauncherMenuOpened,
    DesktopGlobalLauncherMenuClosed,
} DesktopEvent;
