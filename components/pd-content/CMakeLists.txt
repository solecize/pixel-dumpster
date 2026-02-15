idf_component_register(
    SRCS "pd-content.c"
    INCLUDE_DIRS "include"
    REQUIRES pd-display pd-storage
    PRIV_REQUIRES esp_timer esp_http_server cjson lodepng
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
