idf_component_register(
    SRCS "pd-wizard.c"
    INCLUDE_DIRS "include"
    REQUIRES pd-config pd-display
    PRIV_REQUIRES esp_timer esp_driver_uart esp_tinyusb esp_driver_usb_serial_jtag esp_wifi cjson pd-network
)
