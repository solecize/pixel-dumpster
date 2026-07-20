idf_component_register(
    SRCS "bench_main.c"
    INCLUDE_DIRS "."
    REQUIRES pd-config pd-display pd-storage lodepng esp_timer nvs_flash
)
