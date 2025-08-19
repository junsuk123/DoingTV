idf_component_register(
    SRCS "charging_indicator.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc st7789
)
