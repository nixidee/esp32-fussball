# The PlatformIO pre-script validates and generates the local source every run.
# Keep the original heap target and its SDK flags, dependencies and link script.
if(NOT BOOTLOADER_BUILD)
    if(CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS)
        message(FATAL_ERROR
            "Heap-monitor error recovery requires allocation aborts to be disabled")
    endif()

    set(_heap_monitor_copy
        "${CMAKE_BINARY_DIR}/heap_monitor_patch/heap_caps.c")
    if(NOT EXISTS "${_heap_monitor_copy}")
        message(FATAL_ERROR
            "Missing local heap source; run the heap_monitor_patch.py pre-script")
    endif()

    idf_component_get_property(_heap_lib heap COMPONENT_LIB)
    idf_component_get_property(_heap_root heap COMPONENT_DIR)
    get_filename_component(_heap_original
        "${_heap_root}/heap_caps.c" REALPATH)
    get_target_property(_heap_sources "${_heap_lib}" SOURCES)
    set(_heap_replacements 0)
    set(_heap_patched_sources "")
    foreach(_heap_source IN LISTS _heap_sources)
        get_filename_component(_heap_absolute "${_heap_source}" REALPATH
            BASE_DIR "${_heap_root}")
        if("${_heap_absolute}" STREQUAL "${_heap_original}")
            list(APPEND _heap_patched_sources "${_heap_monitor_copy}")
            math(EXPR _heap_replacements "${_heap_replacements} + 1")
        else()
            list(APPEND _heap_patched_sources "${_heap_source}")
        endif()
    endforeach()
    if(NOT _heap_replacements EQUAL 1)
        message(FATAL_ERROR
            "Expected exactly one original heap_caps.c; found ${_heap_replacements}")
    endif()
    set_property(TARGET "${_heap_lib}" PROPERTY SOURCES
        "${_heap_patched_sources}")
    # Quoted private includes previously resolved next to the SDK source.
    target_include_directories("${_heap_lib}" PRIVATE "${_heap_root}")
endif()
