idf_component_register(
    SRCS
        "../../lib/netrunner/settings.c"
        "../../lib/netrunner/settings_core.c"
        "../../lib/netrunner/png_icon.c"
    INCLUDE_DIRS "../../lib"
    REQUIRES furi furi_hal storage netrunner_format esp_rom
)
