idf_component_register(
    SRCS "pd-ble.c"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash pd-wizard
)
