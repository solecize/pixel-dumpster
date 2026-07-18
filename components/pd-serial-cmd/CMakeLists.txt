idf_component_register(
    SRCS "pd-serial-cmd.c"
    INCLUDE_DIRS "include"
    REQUIRES pd-wizard pd-content
    PRIV_REQUIRES cjson mbedtls freertos esp_driver_usb_serial_jtag esp_http_server
)
