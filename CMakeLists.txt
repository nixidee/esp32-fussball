cmake_minimum_required(VERSION 3.22)

# Firmware version reported in the boot log and the OTA image header.
# Development version; the release versioning scheme is not defined yet.
set(PROJECT_VER "0.0.0-dev")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_fussball)
