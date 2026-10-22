#include "furi_hal.h"
#include "boards/board.h"
#include <furi_hal_gpio.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/rtc_io.h>

static const char* TAG = "FuriHal";

void furi_hal_init_early(void) {
    furi_hal_cortex_init_early();
    /* Consume a completed deep-sleep marker before services read inactivity.
     * This uses RTC slow memory only and must run before Dolphin starts. */
    furi_hal_rtc_init_early();

#ifdef BOARD_PIN_PWR_EN
    /* Power-enable must be set early — powers CC1101, BQ27220 fuel gauge, WS2812 */
    /* Release any RTC hold left on PWR_EN by a previous power-off: the shutdown
     * path holds this pin LOW across deep sleep (so the peripheral rail does not
     * leak the battery while "off"). The hold persists through the deep-sleep
     * wake/reset, and while it is held the digital driver below cannot pull the
     * pin high — the radios, LED and fuel gauge would stay dead. Drop the hold
     * and route the pad back to the digital GPIO matrix first. */
    if(rtc_gpio_is_valid_gpio((gpio_num_t)BOARD_PIN_PWR_EN)) {
        rtc_gpio_hold_dis((gpio_num_t)BOARD_PIN_PWR_EN);
        rtc_gpio_deinit((gpio_num_t)BOARD_PIN_PWR_EN);
    }
    static const GpioPin pwr_en = {.port = NULL, .pin = BOARD_PIN_PWR_EN};
    furi_hal_gpio_init_simple(&pwr_en, GpioModeOutputPushPull);
    furi_hal_gpio_write(&pwr_en, true);
    ESP_LOGI(TAG, "PWR_EN GPIO%d set HIGH", BOARD_PIN_PWR_EN);
#endif

#ifdef BOARD_PIN_NRF24_CSN
    /* T-Embed Plus shares SPI2 between CC1101 and NRF24. Drive NRF24 CSN HIGH
     * (deselected) and CE LOW (standby) at boot, before any CC1101 SPI traffic.
     * Without this, NRF24 sees CC1101 traffic and corrupts the bus, manifesting
     * as a stuck ~312 MHz reading in the Frequency Analyzer and total RX failure. */
    static const GpioPin nrf24_csn = {.port = NULL, .pin = BOARD_PIN_NRF24_CSN};
    furi_hal_gpio_init_simple(&nrf24_csn, GpioModeOutputPushPull);
    furi_hal_gpio_write(&nrf24_csn, true);
    ESP_LOGI(TAG, "NRF24_CSN GPIO%d set HIGH (deselect)", BOARD_PIN_NRF24_CSN);
#endif

#ifdef BOARD_PIN_NRF24_CE
    static const GpioPin nrf24_ce = {.port = NULL, .pin = BOARD_PIN_NRF24_CE};
    furi_hal_gpio_init_simple(&nrf24_ce, GpioModeOutputPushPull);
    furi_hal_gpio_write(&nrf24_ce, false);
    ESP_LOGI(TAG, "NRF24_CE GPIO%d set LOW (standby)", BOARD_PIN_NRF24_CE);
#endif

    /* Bring up the panel and backlight before the slower radio, USB, and
     * storage HAL work so the screen is visible during the rest of boot.
     *
     * Order matters: the panel is initialised FIRST, which clears its GRAM to
     * black before turning the display on, and only THEN is the backlight lit.
     * furi_hal_light_init() drives the backlight to full brightness immediately,
     * so lighting it before the panel showed the leftover previous frame (the
     * red UI retained in GRAM across a reboot) for a few ms with the boot
     * default colour order — the quick blue flash. Backlight last = the first
     * thing ever lit is the black clear. */
    furi_hal_display_init();
    furi_hal_light_init();

    ESP_LOGI(TAG, "Early init complete");
}

void furi_hal_deinit_early(void) {
}

void furi_hal_init(void) {
    /* NVS is required by WiFi and BLE — init once at boot */
    esp_err_t nvs_err = nvs_flash_init();
    if(nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    furi_hal_rtc_init();
    furi_hal_version_init();
    furi_hal_info_init();
    furi_hal_power_init();
    furi_hal_crypto_init();
    furi_hal_subghz_init();
    furi_hal_usb_init();
    furi_hal_speaker_init();
    furi_hal_nfc_init();
    ESP_LOGI(TAG, "Init complete");
}
