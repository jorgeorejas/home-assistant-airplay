# During component requirements scanning, CONFIG_ vars are not yet available.
# We must declare all possible dependencies upfront.
set(PRIV_REQUIRES driver nvs_flash esp_driver_i2c esp_driver_spi dac dac_tas57xx dac_tas58xx dac_tlv320aic3204 esp_eth bt main board_utils)

if(CONFIG_BOARD_TARGET_PATH)
    set(boards_dir ${CONFIG_BOARD_TARGET_PATH})
else()
    # Requirements scan phase - register with all dirs so headers are found
    set(boards_dir "squeezeamp")
endif()

idf_component_register(
    SRC_DIRS "${boards_dir}" "."
    INCLUDE_DIRS "${boards_dir}" "."
    REQUIRES esp_driver_i2c board_utils
    PRIV_REQUIRES ${PRIV_REQUIRES}
)

# DenAir: the ha_voice_pe board includes the project-root wifi_config.h
# (gitignored, user-provided). Add the project root to this component's
# private include search path so __has_include("wifi_config.h") finds it.
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_SOURCE_DIR})
