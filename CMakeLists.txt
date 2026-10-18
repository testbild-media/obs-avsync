cmake_minimum_required(VERSION 3.24)

project(obs-avsync VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(AVSYNC_BUILD_PLUGIN "Build the OBS plugin (needs OBS headers, import libs and Qt6)" ON)
option(AVSYNC_BUILD_TESTS "Build the offline unit tests and the dev server" ON)

set(AVSYNC_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_deps" CACHE PATH "Folder with obs-studio sources, implib and qt6")
set(OBS_SOURCE_DIR "${AVSYNC_DEPS_DIR}/obs-studio" CACHE PATH "obs-studio source tree (headers only)")
set(OBS_IMPLIB_DIR "${AVSYNC_DEPS_DIR}/implib" CACHE PATH "Folder with obs.lib and obs-frontend-api.lib")

if(MSVC)
  add_compile_options(/W3 /utf-8 /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

# ---------------------------------------------------------------- embedded phone page
file(GLOB AVSYNC_WEB_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/*")
set(AVSYNC_WEB_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/web-assets.cpp")
add_custom_command(
  OUTPUT "${AVSYNC_WEB_CPP}"
  COMMAND "${CMAKE_COMMAND}" -DINPUT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/web -DOUTPUT=${AVSYNC_WEB_CPP}
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-web.cmake"
  DEPENDS ${AVSYNC_WEB_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-web.cmake"
  COMMENT "Embedding web/ into web-assets.cpp"
  VERBATIM)

# ---------------------------------------------------------------- OBS independent core
add_library(avsync-core STATIC
  src/signal-spec.cpp
  src/fft.cpp
  src/audio-detector.cpp
  src/video-detector.cpp
  src/engine.cpp
  src/mini-json.cpp
  src/ntp-clock.cpp
  src/web-server.cpp
  "${AVSYNC_WEB_CPP}"
)
target_include_directories(avsync-core PUBLIC src)
set_target_properties(avsync-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
  target_link_libraries(avsync-core PUBLIC ws2_32 iphlpapi)
endif()

if(AVSYNC_BUILD_TESTS)
  add_executable(avsync-tests tests/test-main.cpp)
  target_link_libraries(avsync-tests PRIVATE avsync-core)

  add_executable(avsync-devserver tests/devserver.cpp)
  target_link_libraries(avsync-devserver PRIVATE avsync-core)

  add_executable(avsync-replay tests/replay.cpp)
  target_link_libraries(avsync-replay PRIVATE avsync-core)
endif()

if(AVSYNC_BUILD_PLUGIN)
  include(cmake/plugin.cmake)
endif()
