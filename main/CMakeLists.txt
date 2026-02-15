idf_component_register(
    SRCS "app-main.c"
    INCLUDE_DIRS "."
    REQUIRES pd-config pd-content pd-display pd-network pd-storage pd-wizard
)
