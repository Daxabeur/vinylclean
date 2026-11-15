# vinylclean desktop (Windows .exe, Linux AppImage).
# Separate project: the Raspberry Pi build (../CMakeLists.txt) is not affected.
#
#   Linux:    cmake -S desktop -B build-desktop -DFLAC_ROOT=/path/to/static/flac && cmake --build build-desktop
#   Windows:  same with -DCMAKE_TOOLCHAIN_FILE=desktop/packaging/mingw-w64.cmake (cross-compilation from Linux)
#   See desktop/build.sh, which does everything (libFLAC included).
cmake_minimum_required(VERSION 3.16)
project(vinylclean_desktop VERSION 1.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Same DSP options as the Raspberry Pi build (identical results).
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(DSP_FLAGS -Wall -Wextra -fno-math-errno -fno-trapping-math -fno-signed-zeros -fassociative-math)

set(FLAC_ROOT "" CACHE PATH "Folder with include/FLAC and lib/libFLAC.a (static)")
if(NOT FLAC_ROOT)
  message(FATAL_ERROR "Set FLAC_ROOT (static libFLAC), see desktop/build.sh")
endif()

# Page: the Pi page + the desktop card.
file(READ ${ROOT}/web/index.html INDEX_HTML)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/extra.html EXTRA_HTML)
configure_file(${ROOT}/tools/index_html.h.in ${CMAKE_BINARY_DIR}/index_html.h @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/extra_html.h.in ${CMAKE_BINARY_DIR}/extra_html.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ROOT}/web/index.html ${CMAKE_CURRENT_SOURCE_DIR}/extra.html)

add_library(vcdsp STATIC ${ROOT}/core/ar.cpp ${ROOT}/core/lsar.cpp ${ROOT}/core/declick.cpp ${ROOT}/core/decrackle.cpp ${ROOT}/core/antidist.cpp)
target_include_directories(vcdsp PUBLIC ${ROOT}/core)
target_compile_options(vcdsp PRIVATE ${DSP_FLAGS})
target_compile_definitions(vcdsp PUBLIC _USE_MATH_DEFINES)

add_library(miniaudio STATIC third_party/miniaudio.c)
target_include_directories(miniaudio PUBLIC third_party)
# Only what is needed: devices + WAV/FLAC decoding.
target_compile_definitions(miniaudio PUBLIC MA_NO_MP3 MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION MA_NO_ENCODING)
if(NOT WIN32)
  target_compile_options(miniaudio PRIVATE -fPIC)
endif()

set(SOURCES vc_desktop.cpp http_server.cpp)
if(WIN32)
  list(APPEND SOURCES packaging/vinylclean.rc)
endif()
add_executable(vinylclean ${SOURCES})
target_include_directories(vinylclean PRIVATE ${ROOT}/tools ${CMAKE_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${FLAC_ROOT}/include)
target_compile_options(vinylclean PRIVATE ${DSP_FLAGS})
target_compile_definitions(vinylclean PRIVATE VC_VERSION="${PROJECT_VERSION}" FLAC__NO_DLL)
target_link_libraries(vinylclean vcdsp miniaudio ${FLAC_ROOT}/lib/libFLAC.a)
if(WIN32)
  target_link_libraries(vinylclean ws2_32 ole32 shell32 uuid)
  target_link_options(vinylclean PRIVATE -static -mwindows)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(vinylclean Threads::Threads ${CMAKE_DL_LIBS} m)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(vinylclean PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endif()
