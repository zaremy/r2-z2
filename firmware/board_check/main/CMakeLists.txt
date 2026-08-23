idf_component_register(
    SRCS "main.c" "pmu_read.c"
    REQUIRES board_revision esp_driver_i2c
    PRIV_REQUIRES spi_flash esp_psram)
