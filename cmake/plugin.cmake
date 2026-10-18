# OBS plugin target (obs-avsync.dll). Builds against OBS headers + import libraries
# generated from an installed OBS (tools/make-implibs.ps1) and the OBS Qt6 SDK.

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs.h")
  message(FATAL_ERROR "OBS headers not found in ${OBS_SOURCE_DIR}")
endif()

set(AVSYNC_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${AVSYNC_GENERATED}/obsconfig.h.tmp"
  "#pragma once\n"
  "#define OBS_DATA_PATH \"../../data\"\n"
  "#define OBS_PLUGIN_PATH \"../../obs-plugins/64bit\"\n"
  "#define OBS_PLUGIN_DESTINATION \"obs-plugins/64bit\"\n"
  "#define OBS_INSTALL_PREFIX \"\"\n"
  "#define OBS_RELEASE_CANDIDATE 0\n"
  "#define OBS_BETA 0\n")
configure_file("${AVSYNC_GENERATED}/obsconfig.h.tmp" "${AVSYNC_GENERATED}/obsconfig.h" COPYONLY)

add_library(obs-avsync MODULE
  src/obs/plugin.cpp
  src/obs/frame-grabber.cpp
  src/obs/program-probes.cpp
  src/obs/video-sensor.cpp
  src/obs/audio-sensor.cpp
  src/obs/test-source.cpp
  src/obs/dock.cpp
  third_party/qrcodegen/qrcodegen.cpp
)
target_include_directories(obs-avsync PRIVATE
  src
  src/obs
  third_party/qrcodegen
  "${AVSYNC_GENERATED}"
  "${OBS_SOURCE_DIR}/libobs"
  "${OBS_SOURCE_DIR}/frontend/api"
)
target_link_libraries(obs-avsync PRIVATE
  avsync-core
  Qt6::Core Qt6::Gui Qt6::Widgets
  "${OBS_IMPLIB_DIR}/obs.lib"
  "${OBS_IMPLIB_DIR}/obs-frontend-api.lib"
)
set_target_properties(obs-avsync PROPERTIES PREFIX "")
if(MSVC)
  target_compile_options(obs-avsync PRIVATE /Zc:__cplusplus /permissive-)
  # keep debug info for crash reports
  target_link_options(obs-avsync PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

# Package layout for %ProgramData%\obs-studio\plugins\obs-avsync
set(AVSYNC_DIST "${CMAKE_CURRENT_BINARY_DIR}/dist/obs-avsync")
add_custom_command(TARGET obs-avsync POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${AVSYNC_DIST}/bin/64bit" "${AVSYNC_DIST}/data/locale"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:obs-avsync>" "${AVSYNC_DIST}/bin/64bit/"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:obs-avsync>" "${AVSYNC_DIST}/bin/64bit/"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data" "${AVSYNC_DIST}/data"
  COMMENT "Packaging obs-avsync into ${AVSYNC_DIST}")
