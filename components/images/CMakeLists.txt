idf_component_register(SRCS "image_service.cpp" INCLUDE_DIRS "include"
  PRIV_INCLUDE_DIRS "../../include" "../.." REQUIRES settings files display system timekeeping
  PRIV_REQUIRES esp_timer)
