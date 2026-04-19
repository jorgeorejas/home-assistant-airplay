idf_component_register(
    SRCS "leds.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__led_strip
    PRIV_REQUIRES driver esp_timer
)
