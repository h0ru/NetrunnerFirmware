# ESP-IDF already selects the interpreter used for its build. Reuse it here so
# FAM generation does not accidentally pick a stale Windows `py` registration.
if(DEFINED PYTHON AND EXISTS "${PYTHON}")
    set(Python3_EXECUTABLE "${PYTHON}")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_filename_component(ESP32_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(ESP32_FAM_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ESP32_FAM_REGISTRY_C "${ESP32_FAM_GENERATED_DIR}/applications.c")
set(ESP32_FAM_PORTED_CMAKE "${ESP32_FAM_GENERATED_DIR}/ported_apps.cmake")

if(NOT DEFINED NETRUNNER_BOARD)
    # This port supports only the T-Embed CC1101 (see the guard below), so make
    # it the default. A bare `idf.py set-target`/`build` (no -DNETRUNNER_BOARD)
    # then configures correctly instead of falling back to an unsupported board.
    set(NETRUNNER_BOARD "lilygo_t_embed_cc1101")
endif()

if(NOT CMAKE_BUILD_EARLY_EXPANSION AND
   NOT NETRUNNER_BOARD STREQUAL "lilygo_t_embed_cc1101")
    message(FATAL_ERROR
        "This Netrunner port supports only NETRUNNER_BOARD=lilygo_t_embed_cc1101")
endif()

set(ENV{NETRUNNER_BOARD} "${NETRUNNER_BOARD}")

# Re-run CMake (and therefore the generator below) when the app allowlist or the
# generator itself changes. Without this the registry is produced once at configure
# time and a plain rebuild silently keeps the OLD app list, so an app added to
# fam_config.py appears to build fine yet never enters the firmware -- which is
# exactly how the wlan_prepare startup hook went missing.
set_property(
    DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ESP32_DIR}/fam_config.py"
    "${ESP32_DIR}/tools/fam/generate.py"
)

execute_process(
    COMMAND
        ${Python3_EXECUTABLE}
        "${ESP32_DIR}/tools/fam/generate.py"
        --project-dir "${ESP32_DIR}"
        --config "${ESP32_DIR}/fam_config.py"
        --registry-out "${ESP32_FAM_REGISTRY_C}"
        --cmake-out "${ESP32_FAM_PORTED_CMAKE}"
    RESULT_VARIABLE ESP32_FAM_GENERATE_RESULT
    OUTPUT_VARIABLE ESP32_FAM_GENERATE_STDOUT
    ERROR_VARIABLE ESP32_FAM_GENERATE_STDERR
)

if(NOT ESP32_FAM_GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR "ESP32 FAM generation failed:\n${ESP32_FAM_GENERATE_STDOUT}\n${ESP32_FAM_GENERATE_STDERR}")
endif()

# No LDFRAGMENTS: nothing needs a custom section placement. Doom once forced
# its .bss into PSRAM (extram_bss) here so ~31 KB of internal BSS would not
# starve the WiFi/BLE stacks at boot; it is now an external FAP whose
# .bss/.data the ELF loader allocates from the PSRAM heap at runtime. If a
# large module is ever linked statically again, add a .lf back here.
idf_component_register(
    SRCS "app_main.c" "${ESP32_FAM_REGISTRY_C}"
    INCLUDE_DIRS "."
    REQUIRES furi furi_hal input notification gui locale storage loader desktop btshim wifi app_update libsmb2 netrunner toolbox
)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    include("${ESP32_FAM_PORTED_CMAKE}")

    add_dependencies(${COMPONENT_LIB} esp32_fam_stage_assets)

    set(ESP32_FAM_PORTED_COMPONENT_LIBS
        idf::app_update
        idf::archive
        idf::assets
        idf::ble_hid
        idf::ble_profile
        idf::btshim
        idf::bt
        idf::wifi
        idf::datetime
        idf::desktop
        idf::dialogs
        idf::netrunner_application
        idf::furi
        idf::furi_hal
        idf::gui
        idf::input
        idf::loader
        idf::locale
        idf::notification
        idf::nimble_glue
        idf::storage
        idf::nfc
        idf::bit_lib
        idf::infrared
        idf::subghz
        idf::toolbox
        idf::esp_http_client
        idf::esp_http_server
        idf::lfrfid
        idf::mjs
        idf::netrunner
        idf::libsmb2
        idf::mbedtls
    )

    foreach(esp32_fam_target IN LISTS ESP32_FAM_PORTED_OBJECT_TARGETS)
        target_link_libraries(${esp32_fam_target} PRIVATE ${ESP32_FAM_PORTED_COMPONENT_LIBS})
        target_include_directories(${esp32_fam_target} PRIVATE
            "${ESP32_DIR}/components"
            "${ESP32_DIR}/lib"
            "${ESP32_DIR}/lib/subghz"
            "${ESP32_DIR}/lib/infrared"
            "${ESP32_DIR}/lib/infrared/encoder_decoder"
            "${ESP32_DIR}/lib/infrared/signal"
            "${ESP32_DIR}/lib/infrared/worker"
            "${ESP32_DIR}/lib/lfrfid"
            "${ESP32_DIR}/lib/lfrfid/protocols"
            "${ESP32_DIR}/lib/lfrfid/tools"
            "${ESP32_DIR}/lib/mjs"
            "${ESP32_DIR}/lib/mjs/common"
            "${ESP32_DIR}"
            "${ESP32_DIR}/applications"
            "${ESP32_DIR}/applications/main"
            "${ESP32_DIR}/applications/services"
            "${ESP32_DIR}/applications/settings"
            "${ESP32_DIR}/applications/system"
            "${ESP32_DIR}/applications/drivers"
            # Lets the compiled-in NDEF parser (applications/main/nfc/helpers/
            # nfc_ndef_builtin_*.c, which #includes the plugin source ndef.c)
            # resolve ndef.c's bare <netrunner_application.h> include.
            "${ESP32_DIR}/components/netrunner_application/netrunner_application"
        )
        target_sources(${COMPONENT_LIB} PRIVATE $<TARGET_OBJECTS:${esp32_fam_target}>)
    endforeach()

    # The Streaming app bundles former-FAP media modules (airplay_raop,
    # dlna_render, cast_client, ...) that were built with relaxed warnings as
    # FAPs. Their harmless snprintf format-truncation diagnostics stay
    # non-fatal here instead of tripping -Werror.
    if(TARGET esp32_fam_app_streaming)
        target_compile_options(esp32_fam_app_streaming PRIVATE
            -Wno-error=format-truncation -Wno-error=format-overflow)
    endif()
endif()
