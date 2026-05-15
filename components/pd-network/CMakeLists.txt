idf_component_register(
    SRCS "pd-network.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_http_server esp_wifi esp_event nvs_flash lwip cjson mdns pd-storage pd-config
)
