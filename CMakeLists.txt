# AirPlay 2 Receiver for ESP32
cmake_minimum_required(VERSION 3.16)

# Read firmware version from version.txt
file(READ "${CMAKE_SOURCE_DIR}/version.txt" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)

# Add components directory
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(airplay2-receiver VERSION ${PROJECT_VER})

# airplay-esp32 was last cut against ESP-IDF 5.3, where several log-format
# macros fed `int` through size_t-returning helpers. ESP-IDF 5.4 widened the
# return types, which now trips -Werror=format=. Relax this for the whole
# project until we either (a) downgrade to a working 5.3 toolchain or (b)
# patch the upstream call sites. See docs/phase0-report.md.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format;-Wno-format" APPEND)
