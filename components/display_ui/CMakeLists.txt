idf_component_register(
    SRCS "display_ui.c" "font8x16.c"
    INCLUDE_DIRS "include" "."
    REQUIRES driver esp_driver_gpio esp_driver_spi esp_driver_ledc esp_lcd esp_timer freertos heap
)
