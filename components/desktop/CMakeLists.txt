idf_component_register(
    SRCS
        "animations/assets_dolphin_internal.c"
        "animations/assets_dolphin_blocking.c"
    INCLUDE_DIRS "." "animations" ".." "../gui"
    REQUIRES furi furi_hal gui u8g2 assets toolbox storage input loader
)
