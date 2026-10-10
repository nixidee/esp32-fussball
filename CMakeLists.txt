cmake_minimum_required(VERSION 3.22)

# One version source for boot, Web status and the ESP-IDF app descriptor.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" PROJECT_VER LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/VERSION")
option(FUSSBALL_RELEASE_BUILD "Require release version and disabled test selectors" OFF)
if(FUSSBALL_RELEASE_BUILD)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  execute_process(COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/scripts/release_guard.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}" RESULT_VARIABLE release_result)
  if(NOT release_result EQUAL 0)
    message(FATAL_ERROR "Release guard rejected the source configuration")
  endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# One component lockfile per chip target (resolved versions differ per target
# and must not overwrite each other); all lockfiles are tracked in Git.
idf_build_set_property(DEPENDENCIES_LOCK
  "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")
project(esp32_fussball)

include("${CMAKE_CURRENT_LIST_DIR}/scripts/heap_monitor_patch.cmake")
