# The observer wraps installed callbacks; it neither copies nor patches LVGL.
# Refuse changed private layout, source acknowledgement, callback/EOF or FS
# contracts until reviewed.
if(NOT DEFINED JPEG_OBSERVER_LVGL_ROOT)
  get_filename_component(JPEG_OBSERVER_LVGL_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../managed_components/lvgl__lvgl" ABSOLUTE)
endif()
set(_jpeg_observer_contracts
  "include/lvgl/lv_version.h=d7bfefbc2f9a3cc79236ba61efb128c8c3b382a5f2710888e0bf8156a901c801"
  "include/lvgl/image/lv_image_decoder.h=a68b59364e39800370b2537b336c66968d84d5f3ed7b4d5ce9406347f72117ed"
  "include/lvgl/fs/lv_fs.h=846e6aef9e053d1641b86d249db2b9f3d16de0a4155e3e1b4875216a3edc94ed"
  "src/image/lv_image_decoder_private.h=2500fbe7e09e7603390b84bb6b016e56d9fe2d6385d0ea543ee6e0fde722dc06"
  "src/image/lv_image_decoder.c=d28dd17354e2732470c8c062b5b99f2f1517c39d3bccab56e648ae35bc6246d6"
  "src/image/lv_tjpgd.c=3b346915201651112782c966e8bf63aff847bb31b3d7f22e076b24005ee4a7a6"
  "src/draw/lv_draw_image.c=eda7a142395efcb4abe16a32e1613dbc823bff5373ccabd393321594b0b5bdfc"
  "src/widgets/image/lv_image.c=53df2b6128f69a7c53b3741594b953623bfcd648332dd95026b6987e27167608"
  "src/fs/lv_fs.c=0f9dd2fbeb023c58ecb91fd070a3110c7cdddf6d8ca2bf120136317e4617c12c"
  "src/fs/lv_fs_posix.c=6c7a4c4594f299726483281bf3eb8ca0e64e7bfd60cb17afbd53e01823bf3b1b")
foreach(_jpeg_observer_contract IN LISTS _jpeg_observer_contracts)
  string(REPLACE "=" ";" _jpeg_observer_pair "${_jpeg_observer_contract}")
  list(GET _jpeg_observer_pair 0 _jpeg_observer_relative)
  list(GET _jpeg_observer_pair 1 _jpeg_observer_expected)
  set(_jpeg_observer_file
    "${JPEG_OBSERVER_LVGL_ROOT}/${_jpeg_observer_relative}")
  if(NOT EXISTS "${_jpeg_observer_file}")
    message(FATAL_ERROR "JPEG observer: missing ${_jpeg_observer_relative}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_jpeg_observer_file}")
  file(SHA256 "${_jpeg_observer_file}" _jpeg_observer_actual)
  if(NOT _jpeg_observer_actual STREQUAL _jpeg_observer_expected)
    message(FATAL_ERROR
      "JPEG observer: ${_jpeg_observer_relative} changed; review callbacks, EOF and lifecycle before updating the guard")
  endif()
endforeach()
