idf_component_register(
    SRCS "pd-network.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_http_server lwip
)
