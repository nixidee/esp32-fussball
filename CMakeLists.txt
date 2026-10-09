cmake_minimum_required(VERSION 3.22)

# Firmware version reported in the boot log and the OTA image header.
# Development version; the release versioning scheme is not defined yet.
set(PROJECT_VER "0.0.0-dev")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# One component lockfile per chip target (resolved versions differ per target
# and must not overwrite each other); all lockfiles are tracked in Git.
idf_build_set_property(DEPENDENCIES_LOCK
  "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")
project(esp32_fussball)

include("${CMAKE_CURRENT_LIST_DIR}/scripts/heap_monitor_patch.cmake")
