idf_component_register(
    SRCS "button_ctrl.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_gpio esp_timer freertos
)
