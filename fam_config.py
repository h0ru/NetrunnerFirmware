import os

MANIFEST_ROOTS = [
    "components",
    "applications",
    "applications_user",
]

APP_SOURCE_OVERRIDES = {
    "desktop": "applications",
    "storage": "applications",
}

APPS = [
    "input",
    "notification",
    "gui",
    "dialogs",
    "locale",
    "cli",
    "cli_vcp",
    "storage",
    "storage_start",
    "power",
    "power_start",
    "power_settings",
    "loader",
    "loader_start",
    "namechanger_srv",
    "notification_settings",
    "system_settings",
    "backup_settings",
    "netfetch",
    "interface_settings",
    "desktop",
    "archive",
    "about",
    "bt_settings",
    "clock",
    "bad_usb",
    "subghz",
    "cli_subghz",
    "subghz_load_dangerous_settings",
    "passport",
    "nfc",
    "infrared",
    # 125 kHz RFID is NOT built: the T-Embed has NFC only, no LF antenna. The
    # app would open, find no hardware and mislead. The archive keeps its
    # .rfid tab, exactly as it already does for the unbuilt iButton app.
    "wlan",
    "wifi",
    "wlan_prepare",
    "nrf24",
    "chameleon",
    "ble_spam",
    "findmy",
    "findmy_startup",
    # Applies the custom Device Name at boot. The Netrunner app has always
    # written the name to NAMECHANGER_PATH and read it back to seed its own
    # field, so the setting LOOKED like it stuck -- but this service, the only
    # thing that ever applies it to the running device, was never in the build.
    # The name therefore reverted on every boot and every deep-sleep wake.
    "namechanger_srv",
    "js_app",
    "js_event_loop",
    "js_gui",
    "js_gui__loading",
    "js_gui__empty_screen",
    "js_gui__submenu",
    "js_gui__text_input",
    "js_gui__number_input",
    "js_gui__button_panel",
    "js_gui__popup",
    "js_gui__button_menu",
    "js_gui__menu",
    "js_gui__vi_list",
    "js_gui__byte_input",
    "js_gui__text_box",
    "js_gui__dialog",
    "js_gui__file_picker",
    "js_gui__widget",
    "js_gui__icon",
    "js_notification",
    "js_math",
    "js_storage",
    "js_subghz",
    "js_infrared",
    "js_blebeacon",
    # js_serial, js_gpio, js_i2c, js_spi excluded - need HAL porting
]

_board = os.environ.get("NETRUNNER_BOARD", "")

if _board == "lilygo_t_embed_cc1101":
    APPS.append("settings")
    # Passive WiFi/BLE/Sub-GHz scanner. It needs the T-Embed's PSRAM and
    # onboard CC1101, so do not expose it on the smaller C6 targets.
    APPS.append("wardriving")

# NOTE: cli_vcp and dolphin look like easy RAM wins (~4KB stack each) but both
# are hard-wired into the desktop service: desktop.c calls cli_vcp_enable/disable
# directly (USB-Storage/qNetrunner toggles) and lists "dolphin" in its `requires`
# while opening RECORD_DOLPHIN. Dropping either breaks the link / hangs the UI,
# so they stay. The real RAM wins are BLE-off (~68KB) + smaller WiFi buffers.

# qNetrunner and USB-Storage are no longer standalone apps — they live in the
# desktop lock menu (applications/services/desktop). The menu gates them itself:
# qNetrunner / USB-Storage behind a compile-time USB-OTG check (ESP32-S3/S2 only).
# So there is nothing to exclude here per board anymore.
# Note: nothing installs the TinyUSB composite at boot — doing so would switch
# the internal USB PHY to OTG and kill the USB-Serial-JTAG bridge that esptool
# uses, breaking the next `./buildAndFlash_T-Embed.sh` cycle. The composite is
# installed lazily, only when the user enables qNetrunner / opens USB-Storage.

EXTRA_EXT_APPS = []
TARGET_HW = 32
AUTORUN_APP = ""
