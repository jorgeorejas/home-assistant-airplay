idf_component_register(
    SRCS "ui.c" "encoder.c" "button.c" "led_switch.c" "jack_detect.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_timer main ha_airplay_leds dac
)

# fabsf comes from <math.h>; ESP-IDF's libm is linked by default.

