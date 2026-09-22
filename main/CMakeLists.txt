idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES button_ctrl display_ui usb_manager net_bridge esp_timer nvs_flash
)
