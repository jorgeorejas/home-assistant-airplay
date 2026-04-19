idf_component_register(
    SRCS "ui.c" "encoder.c" "button.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_timer main denair_leds
)
