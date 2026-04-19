idf_component_register(
    SRCS "leds.c" "audio_tap.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__led_strip
    PRIV_REQUIRES driver esp_timer
)
