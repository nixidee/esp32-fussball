# Pure bounded JSON protocol; no document allocator or transport dependency.
idf_component_register(SRCS "json_stream.cpp"
  INCLUDE_DIRS "include" "../../include")
