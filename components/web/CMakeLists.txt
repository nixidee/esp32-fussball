idf_component_register(SRCS "web_service.cpp" "settings_json.cpp"
  INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "../../include" "../.."
  REQUIRES core football images network settings system
  PRIV_REQUIRES esp_http_server esp_app_format esp_hw_support heap lwip freertos)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(web_header "${CMAKE_CURRENT_BINARY_DIR}/web_asset.h")
# PlatformIO compiles component sources through SCons rather than building
# arbitrary CMake custom targets. Generate during configuration in both flows.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PROJECT_DIR}/web/index.html" "${PROJECT_DIR}/scripts/embed_web.py")
execute_process(COMMAND ${Python3_EXECUTABLE} "${PROJECT_DIR}/scripts/embed_web.py"
  "${PROJECT_DIR}/web/index.html" "${web_header}"
  RESULT_VARIABLE web_result)
if(NOT web_result EQUAL 0)
  message(FATAL_ERROR "Offline Web UI generation failed")
endif()
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
