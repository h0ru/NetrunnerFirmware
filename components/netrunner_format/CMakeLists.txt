idf_component_register(
    SRCS
        "netrunner_format.c"
        "netrunner_format_stream.c"
    INCLUDE_DIRS "."
    REQUIRES furi toolbox storage
)
