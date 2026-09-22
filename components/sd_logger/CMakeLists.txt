idf_component_register(
    SRCS "sd_logger.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_sdmmc sdmmc fatfs vfs esp_timer
)
