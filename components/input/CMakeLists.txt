# Target-specific input driver selection
if(NOT DEFINED NETRUNNER_BOARD)
    set(NETRUNNER_BOARD "lilygo_t_embed_cc1101" CACHE STRING "Target board")
endif()

set(TARGET_INPUT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../targets/${NETRUNNER_BOARD}")

if(NOT EXISTS "${TARGET_INPUT_DIR}/target_input.c")
    message(FATAL_ERROR "No target_input.c found for board '${NETRUNNER_BOARD}' at ${TARGET_INPUT_DIR}")
endif()

idf_component_register(
    SRCS
        "input.c"
        "${TARGET_INPUT_DIR}/target_input.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../targets"
    REQUIRES furi
    PRIV_REQUIRES furi_hal
)
