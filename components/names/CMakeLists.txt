# Durable adopted names and their immutable source catalogue. Football owns
# volatile provider data; only pure football value contracts cross this boundary.
idf_component_register(
  SRCS
    "team_name_catalogue.cpp"
    "team_name_store.cpp"
    "team_name_files.cpp"
    "team_name_service.cpp"
    "team_name_json.cpp"
    "effective_names.cpp"
  INCLUDE_DIRS "include"
  PRIV_INCLUDE_DIRS "../../include"
  REQUIRES football settings json
  PRIV_REQUIRES files network system)

# Generated before every PlatformIO build, including cached CMake runs.
set(team_name_asset_dir "${CMAKE_BINARY_DIR}/team_name_asset")
if(NOT EXISTS "${team_name_asset_dir}/team_name_asset.h")
  message(FATAL_ERROR
    "Missing team-name preset header; run the embed_team_names.py pre-script")
endif()
target_include_directories(${COMPONENT_LIB} PRIVATE "${team_name_asset_dir}")
