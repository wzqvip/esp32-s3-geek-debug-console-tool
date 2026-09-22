idf_component_register(
    SRCS "usb_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_tinyusb freertos esp_hw_support espressif__tinyusb
)
