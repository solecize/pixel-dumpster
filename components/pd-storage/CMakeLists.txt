idf_component_register(
    SRCS "pd-storage.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_littlefs json
)
