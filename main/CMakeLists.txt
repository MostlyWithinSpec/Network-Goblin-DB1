idf_component_register(
    SRCS "main.c" "lcd.c" "gfx.c" "ui.c" "state.c" "pet.c" "net.c" "scan.c" "web.c"
    INCLUDE_DIRS "."
    EMBED_FILES "web/index.html.gz"
    PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_driver_ledc esp_wifi esp_netif
                  esp_event nvs_flash esp_http_server app_update json lwip esp_timer
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation -Wno-missing-field-initializers)
