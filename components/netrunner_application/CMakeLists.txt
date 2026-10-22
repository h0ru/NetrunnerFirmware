idf_component_register(
    SRCS
        "netrunner_application/application_manifest.c"
        "arm_fap/arm_fap_vm.c"
        "arm_fap/arm_fap_runtime.c"
        "netrunner_application/elf/elf_file.c"
        "netrunner_application/netrunner_application.c"
        "netrunner_application/plugins/composite_resolver.c"
        "netrunner_application/plugins/plugin_manager.c"
        "netrunner_application/firmware_api.c"
        "netrunner_application/nfc_app_api.c"
        "netrunner_application/api_hashtable/api_hashtable.c"
    INCLUDE_DIRS "."
    REQUIRES furi furi_hal storage toolbox gui notification dialogs netrunner_format subghz assets mjs nfc bit_lib datetime mbedtls locale infrared
)

# firmware_api.c ist die FAP-ABI-Symboltabelle und exportiert bewusst auch deprecated APIs
# (z.B. view_dispatcher_enable_queue) -> deprecated-Warnung hier gezielt unterdruecken.
# Ein #pragma scheidet aus: die Tabelle ist ein Array-Initializer, dort sind keine Statements erlaubt.
set_source_files_properties(
    "netrunner_application/firmware_api.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations"
)
