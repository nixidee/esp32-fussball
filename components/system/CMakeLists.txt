idf_component_register(SRCS "ota_service.cpp" INCLUDE_DIRS "include"
  PRIV_INCLUDE_DIRS "../.." "../../include"
  REQUIRES core PRIV_REQUIRES app_update esp_app_format esp_system esp_timer)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u fussball_image_identity")
