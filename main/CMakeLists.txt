set( SRC_FILES
    "main.c"
    "alac_magic_cookie.c"
    "settings.c"
    "audio/audio_receiver.c"
    "audio/audio_stream.c"
    "audio/audio_stream_realtime.c"
    "audio/audio_stream_buffered.c"
    "audio/audio_decoder.c"
    "audio/audio_buffer.c"
    "audio/audio_timing.c"
    "audio/audio_crypto.c"
    "audio/audio_resample.c"
    "rtsp/rtsp_server.c"
    "rtsp/rtsp_conn.c"
    "rtsp/rtsp_message.c"
    "rtsp/rtsp_handlers.c"
    "rtsp/rtsp_fairplay.c"
    "rtsp/rtsp_crypto.c"
    "rtsp/rtsp_rsa.c"
    "rtsp/rtsp_events.c"
    "plist/plist_xml.c"
    "plist/bplist_parser.c"
    "plist/bplist_builder.c"
    "plist/base64.c"
    "airplay_pair/hap.c"
    "airplay_pair/hap_pair_verify.c"
    "airplay_pair/hap_pair_setup.c"
    "airplay_pair/hap_crypto.c"
    "airplay_pair/srp.c"
    "airplay_pair/tlv8.c"
    "network/wifi.c"
    "network/mdns_airplay.c"
    "network/ptp_clock.c"
    "network/ntp_clock.c"
    "network/socket_utils.c"
    "network/web_server.c"
    "network/ota.c"
    "network/dns_server.c"
    "network/log_stream.c"
    "led.c"
    "audio/eq_events.c"
    "audio/audio_eq.c"
    "audio/chime.c"
    "now_playing.c"
    "buttons.c"
    "playback_control.c"
    "dacp_client.c"
)
# Note: led.c and buttons.c are upstream's single-LED + Kconfig-button-matrix
# drivers. On Voice PE every related GPIO is `-1` so all init/runtime paths
# early-return — they compile in as no-op stubs (~5 KB flash, <1 µs runtime
# per audio frame). Architecture review #7/#8 suggested gating them out,
# but call sites in main.c / audio_output.c / a2dp_sink.c reference the
# symbols, so the no-op pattern is the cheaper choice here.

# Select audio output implementation at compile time
if(CONFIG_SPDIF_OUTPUT)
    list(APPEND SRC_FILES "audio/audio_output_spdif.c")
else()
    list(APPEND SRC_FILES "audio/audio_output.c")
endif()

# Bluetooth A2DP sink (only available on ESP32 with Classic BT)
if(CONFIG_BT_A2DP_ENABLE)
    list(APPEND SRC_FILES "audio/a2dp_sink.c")
endif()

# W5500 SPI Ethernet
if(CONFIG_ETH_W5500_ENABLED)
    list(APPEND SRC_FILES "network/ethernet.c")
endif()

set( INC_DIRS
    "." 
    "audio" 
    "rtsp" 
    "plist"
    "airplay_pair"
    "network"
)

set( DEPS
    driver
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_ringbuf
    esp_driver_i2s
    esp_timer
    esp_http_server
    esp_http_client
    app_update
    json
    mbedtls
    esp_driver_ledc
    boards
    dac
    ha_airplay_artwork
    ha_airplay_leds
    ha_airplay_ui
    display
    audio-resampler
    esp_eth
    spiffs_storage
)

# TODO: why these fall over in CI and fine locally...
# Bluetooth component only needed when A2DP is enabled
#if(CONFIG_BT_A2DP_ENABLE)
    list(APPEND DEPS bt)
#endif()

# Ethernet component only needed when W5500 is enabled
#if(CONFIG_ETH_W5500_ENABLED)
    list(APPEND DEPS esp_eth)
#endif()

# Upstream TAS58xx DAC driver — only needed on boards that actually have one.
# Voice PE uses TLV320AIC3204; the `eq_events.h` definitions used to leak this
# include, but were collapsed onto AUDIO_EQ_BANDS in v0.5.0 so this can stay
# behind the gate cleanly.
if(CONFIG_DAC_TAS58XX)
    list(APPEND DEPS dac_tas58xx)
endif()

idf_component_register(
    SRCS ${SRC_FILES}
    INCLUDE_DIRS ${INC_DIRS}
    PRIV_REQUIRES ${DEPS}
    EMBED_FILES "network/index.html" "network/logs.html" "audio/chime.pcm"
)

# Enable function wrapping for abort() to intercept panics and mute GPIO
if(CONFIG_BOARD_SQUEEZEAMP_COMMON)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=abort")
endif()
