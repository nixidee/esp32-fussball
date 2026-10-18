# Portable checked operations plus the LittleFS mount/format adapter.
idf_component_register(
  SRCS
    "file_names.cpp"
    "file_io.cpp"
    "file_service.cpp"
    "file_test.cpp"
  INCLUDE_DIRS
    "include"
    "../../include"
  PRIV_REQUIRES joltwallet__littlefs esp_partition esp_timer freertos heap log vfs
)
