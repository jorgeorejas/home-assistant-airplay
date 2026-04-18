#!/usr/bin/env bash
#
# denair-build.sh — one-liner for DenAir IDF builds on HA Voice PE.
#
# Usage:
#   tools/denair-build.sh [reconfigure|build|flash|monitor|clean|menuconfig]
#
# First run should be `reconfigure` after any sdkconfig.defaults* change;
# otherwise `build` is fine. The build is pinned to the ha_voice_pe target
# by default. Override with DENAIR_TARGET=esp32s3 to build the upstream
# airplay-esp32 esp32s3-generic baseline (useful as a smoke test).

set -euo pipefail

IDF_PATH_DEFAULT="${HOME}/esp/esp-idf"
: "${IDF_PATH:=${IDF_PATH_DEFAULT}}"

if [ ! -f "${IDF_PATH}/export.sh" ]; then
    echo "error: ${IDF_PATH}/export.sh not found." >&2
    echo "Install ESP-IDF v5.4+ to ~/esp/esp-idf or set IDF_PATH before running." >&2
    exit 1
fi

# shellcheck disable=SC1091
source "${IDF_PATH}/export.sh" >/dev/null

TARGET="${DENAIR_TARGET:-ha_voice_pe}"
case "$TARGET" in
    ha_voice_pe)
        SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.ha_voice_pe"
        ;;
    esp32s3)
        SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3"
        ;;
    *)
        echo "error: unknown DENAIR_TARGET=${TARGET}. Valid: ha_voice_pe | esp32s3." >&2
        exit 1
        ;;
esac

idf.py set-target esp32s3 >/dev/null

cmd="${1:-build}"
case "$cmd" in
    reconfigure)
        idf.py -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS}" reconfigure
        ;;
    build)
        idf.py -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS}" build
        ;;
    flash)
        idf.py -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS}" flash
        ;;
    monitor)
        idf.py monitor
        ;;
    menuconfig)
        idf.py -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS}" menuconfig
        ;;
    clean)
        idf.py fullclean
        ;;
    *)
        echo "unknown subcommand: $cmd" >&2
        echo "usage: tools/denair-build.sh [reconfigure|build|flash|monitor|clean|menuconfig]" >&2
        exit 1
        ;;
esac
