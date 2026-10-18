idf_component_register(SRCS "ota_service.cpp" "health_service.cpp" "health_meter.cpp" "health_test.cpp" INCLUDE_DIRS "include"
  PRIV_INCLUDE_DIRS "../.." "../../include"
  REQUIRES events files timekeeping
  PRIV_REQUIRES display app_update esp_app_format esp_system esp_timer freertos heap log)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u fussball_image_identity")
