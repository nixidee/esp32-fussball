# File name rules, path building and the erased-flash check of the file
# service (docs/ARCHITECTURE.md); no ESP-IDF includes, host-tested.
# app_config.h is a public dependency.
idf_component_register(
  SRCS
    "file_names.cpp"
  INCLUDE_DIRS
    "include"
    "../../include"
)
