if(CONFIG_DAC_TLV320AIC3204)
    set(srcs "dac_tlv320aic3204.c")
else()
    set(srcs "")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES dac
    PRIV_REQUIRES driver esp_driver_i2c board_utils
)
