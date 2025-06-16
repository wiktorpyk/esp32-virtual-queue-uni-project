idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event nvs_flash esp_driver_gpio
    PRIV_REQUIRES esp_http_server
)
