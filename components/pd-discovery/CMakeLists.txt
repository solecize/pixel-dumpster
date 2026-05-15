idf_component_register(
    SRCS "pd-discovery.c"
    INCLUDE_DIRS "."
    REQUIRES lwip cjson pd-network esp_timer
)
