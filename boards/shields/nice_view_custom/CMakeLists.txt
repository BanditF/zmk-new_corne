if(CONFIG_ZMK_DISPLAY AND CONFIG_NICE_VIEW_WIDGET_STATUS)
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_sources(custom_status_screen.c)
  zephyr_library_sources(widgets/bolt.c)
  zephyr_library_sources(widgets/util.c)
  zephyr_library_sources(widgets/screen_snap.c)

  if(NOT CONFIG_ZMK_SPLIT OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zephyr_library_sources(widgets/status.c)
    if(CONFIG_NICE_VIEW_HOST_LINK)
      zephyr_library_sources(widgets/host_link.c)
      if(CONFIG_FOXGLOVE_TEST_HOOKS)
        zephyr_library_sources(widgets/test_hooks.c)
      endif()
      if(CONFIG_NICE_VIEW_ART_RELAY)
        zephyr_library_sources(widgets/art_relay_central.c)
      endif()
    endif()
  else()
    zephyr_library_sources(widgets/custom_art.c)
    zephyr_library_sources(widgets/peripheral_status.c)
    if(CONFIG_NICE_VIEW_ART_RELAY)
      zephyr_library_sources(widgets/art_relay_peripheral.c)
      zephyr_library_sources(widgets/art_slots.c)
      if(CONFIG_FOXGLOVE_TEST_HOOKS)
        zephyr_library_sources(widgets/test_hooks.c)
      endif()
    endif()
  endif()
endif()

# Not a display feature: the underglow's idle handling is built whenever it's on.
if(CONFIG_NICE_VIEW_RGB_IDLE)
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_sources(widgets/rgb_idle.c)
  if(CONFIG_FOXGLOVE_LAMP_STRIP)
    zephyr_library_sources(widgets/lamp_strip.c)
  endif()
  if(CONFIG_FOXGLOVE_LAMPARRAY)
    zephyr_library_sources(widgets/lamparray.c)
  endif()
endif()

# Half identification is present with or without a display and survives section GC.
execute_process(COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  OUTPUT_VARIABLE FOXGLOVE_BUILD_SHA OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET RESULT_VARIABLE FOXGLOVE_GIT_RESULT)
if(NOT FOXGLOVE_GIT_RESULT EQUAL 0 OR NOT FOXGLOVE_BUILD_SHA)
  # CI's container can't run git on the checkout (ownership), but Actions sets GITHUB_SHA.
  if(DEFINED ENV{GITHUB_SHA})
    string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 FOXGLOVE_BUILD_SHA)
  else()
    set(FOXGLOVE_BUILD_SHA unknown)
  endif()
endif()
# The firmware version (SemVer) lives in FIRMWARE_VERSION; a fw-vX.Y.Z tag must match it (CI).
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/FIRMWARE_VERSION FOXGLOVE_FW_VERSION LIMIT_COUNT 1)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" FOXGLOVE_FW_VERSION_OK "${FOXGLOVE_FW_VERSION}")
if(NOT FOXGLOVE_FW_VERSION_OK)
  message(FATAL_ERROR "FIRMWARE_VERSION must be X.Y.Z, not '${FOXGLOVE_FW_VERSION}'")
endif()
zephyr_library_sources(widgets/half_marker.c)
# Library-wide on purpose: a source property set here wouldn't reach the app library's target
# (it lives in another CMake directory), which is why the SHA used to read 'unknown'.
zephyr_library_compile_definitions(
  FOXGLOVE_BUILD_SHA="${FOXGLOVE_BUILD_SHA}"
  FOXGLOVE_FW_VERSION="${FOXGLOVE_FW_VERSION}"
  FOXGLOVE_FW_MAJOR=${CMAKE_MATCH_1}
  FOXGLOVE_FW_MINOR=${CMAKE_MATCH_2}
  FOXGLOVE_FW_PATCH=${CMAKE_MATCH_3})
zephyr_linker_sources(RODATA half_marker.ld)
