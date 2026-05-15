idf_component_register(
    SRCS "pd-content.c"
    INCLUDE_DIRS "include"
    REQUIRES pd-display pd-storage pd-transition pd-config
    PRIV_REQUIRES esp_timer esp_http_server cjson lodepng pd-network pd-discovery
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
