cmake_minimum_required(VERSION 3.13)
project(vinylclean VERSION 1.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Optimisations: -O3; on the Raspberry Pi 3B+ we target the Cortex-A53 (NEON).
# The "math" options below allow vectorized sums without dropping NaN
# handling (no full -ffast-math).
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
add_compile_options(-Wall -Wextra -fno-math-errno -fno-trapping-math -fno-signed-zeros -fassociative-math)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  add_compile_options(-mcpu=cortex-a53)
endif()

# ---- DSP core (same code everywhere) ----
add_library(vccore STATIC
  core/ar.cpp
  core/lsar.cpp
  core/declick.cpp
  core/decrackle.cpp
  core/antidist.cpp)
target_include_directories(vccore PUBLIC core)

# ---- Tools ----
add_executable(vc-file tools/vc_file.cpp)
target_include_directories(vc-file PRIVATE tools)
target_link_libraries(vc-file vccore)

add_executable(vc-bench tools/vc_bench.cpp)
target_include_directories(vc-bench PRIVATE tools tests)
target_link_libraries(vc-bench vccore)

add_executable(vc-tests tests/test_declick.cpp)
target_include_directories(vc-tests PRIVATE tools tests)
target_link_libraries(vc-tests vccore)

add_executable(vc-tests-antidist tests/test_antidist.cpp)
target_link_libraries(vc-tests-antidist vccore)
add_test(NAME antidist COMMAND vc-tests-antidist)

add_executable(vc-tests-riaa tests/test_riaa.cpp)
target_include_directories(vc-tests-riaa PRIVATE tools core)
add_test(NAME riaa COMMAND vc-tests-riaa)

add_executable(vc-tests-loudness tests/test_loudness.cpp)
target_include_directories(vc-tests-loudness PRIVATE tools)

add_executable(vc-tests-crackle tests/test_decrackle.cpp)
target_include_directories(vc-tests-crackle PRIVATE tools tests)
target_link_libraries(vc-tests-crackle vccore)

# ---- Real-time playback (ALSA) ----
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(ALSA QUIET alsa)
  pkg_check_modules(FLAC QUIET flac)
endif()
if(ALSA_FOUND AND NOT FLAC_FOUND)
  message(WARNING "libFLAC not found (package libflac-dev): vc-live is built without recording")
endif()
if(FLAC_FOUND)
  find_package(Threads REQUIRED)
  add_executable(vc-tests-rec tests/test_recorder.cpp)
  target_include_directories(vc-tests-rec PRIVATE tools ${FLAC_INCLUDE_DIRS})
  target_link_libraries(vc-tests-rec ${FLAC_LIBRARIES} Threads::Threads)
  add_test(NAME recorder COMMAND vc-tests-rec)
  install(TARGETS vc-tests-rec RUNTIME DESTINATION bin)
endif()
if(ALSA_FOUND)
  find_package(Threads REQUIRED)
  # The web page is embedded in the program (and can also be read from disk).
  file(READ ${CMAKE_SOURCE_DIR}/web/index.html INDEX_HTML)
  configure_file(tools/index_html.h.in ${CMAKE_BINARY_DIR}/index_html.h @ONLY)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/web/index.html)
  add_executable(vc-live tools/vc_live.cpp tools/web_server.cpp)
  target_include_directories(vc-live PRIVATE tools ${CMAKE_BINARY_DIR} ${ALSA_INCLUDE_DIRS})
  target_link_libraries(vc-live vccore ${ALSA_LIBRARIES} Threads::Threads)
  if(FLAC_FOUND)
    target_compile_definitions(vc-live PRIVATE VC_RECORD=1)
    target_include_directories(vc-live PRIVATE ${FLAC_INCLUDE_DIRS})
    target_link_libraries(vc-live ${FLAC_LIBRARIES})
  endif()
  install(FILES web/index.html DESTINATION share/vinylclean)
endif()

enable_testing()
add_test(NAME declick COMMAND vc-tests)
add_test(NAME decrackle COMMAND vc-tests-crackle)
add_test(NAME loudness COMMAND vc-tests-loudness)

install(TARGETS vc-file vc-bench vc-tests vc-tests-crackle vc-tests-loudness vc-tests-riaa vc-tests-antidist RUNTIME DESTINATION bin)
if(ALSA_FOUND)
  install(TARGETS vc-live RUNTIME DESTINATION bin)
endif()

