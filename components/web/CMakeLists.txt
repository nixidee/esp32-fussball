idf_component_register(SRCS "web_service.cpp" "command_json.cpp" "request_json.cpp" "config_json.cpp" "settings_json.cpp" "screen_json.cpp"
  INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "../../include" "../.."
  REQUIRES json football images network settings system files timekeeping events
  PRIV_REQUIRES application names esp_http_server esp_app_format esp_hw_support heap lwip freertos ui display)
# The embed_web.py pre-script regenerates web_asset.h on every PlatformIO run
# (a changed web/index.html alone does not trigger a CMake reconfigure, W129);
# CMake only consumes the result.
set(web_asset_dir "${CMAKE_BINARY_DIR}/web_asset")
if(NOT EXISTS "${web_asset_dir}/web_asset.h")
  message(FATAL_ERROR
    "Missing Web UI header; run the embed_web.py pre-script")
endif()
target_include_directories(${COMPONENT_LIB} PRIVATE "${web_asset_dir}")
