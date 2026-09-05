# OBS's own dependency bundle on macOS/Windows; distro shared libraries on
# Linux. Do not bundle a competing FFmpeg or Qt runtime into the plugin.
if(APPLE AND OBS_LOCAL_APP_SDK)
  list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-2026-07-15-universal")
endif()
find_path(DELAY_AV_INCLUDE_DIR libavformat/avformat.h REQUIRED)
find_path(DELAY_TLS_INCLUDE_DIR mbedtls/ssl.h REQUIRED)
foreach(
  component
  IN
  ITEMS avformat avcodec avutil mbedtls mbedx509 mbedcrypto
)
  find_library(DELAY_${component}_LIBRARY NAMES ${component} REQUIRED)
endforeach()
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_library(
  dynamic-delay-rtmp
  STATIC
  vendor/librtmp/amf.c
  vendor/librtmp/cencode.c
  vendor/librtmp/hashswf.c
  vendor/librtmp/log.c
  vendor/librtmp/md5.c
  vendor/librtmp/parseurl.c
  vendor/librtmp/rtmp.c
  vendor/happy-eyeballs/happy-eyeballs.c
)
set_target_properties(dynamic-delay-rtmp PROPERTIES POSITION_INDEPENDENT_CODE TRUE C_VISIBILITY_PRESET hidden)
target_include_directories(
  dynamic-delay-rtmp
  PUBLIC vendor/librtmp "${DELAY_TLS_INCLUDE_DIR}"
  PRIVATE vendor/happy-eyeballs
)
target_compile_definitions(dynamic-delay-rtmp PUBLIC USE_MBEDTLS CRYPTO)
target_link_libraries(
  dynamic-delay-rtmp
  PUBLIC
    OBS::libobs
    Threads::Threads
    "${DELAY_mbedtls_LIBRARY}"
    "${DELAY_mbedx509_LIBRARY}"
    "${DELAY_mbedcrypto_LIBRARY}"
    ZLIB::ZLIB
)
if(APPLE)
  target_link_libraries(dynamic-delay-rtmp PRIVATE "-framework Security" "-framework Foundation")
elseif(WIN32)
  target_link_libraries(dynamic-delay-rtmp PRIVATE crypt32 ws2_32 iphlpapi winmm)
  if(TARGET OBS::w32-pthreads)
    target_link_libraries(dynamic-delay-rtmp PRIVATE OBS::w32-pthreads)
  endif()
endif()

add_library(dynamic-delay-network STATIC src/multistream-transport.cpp)
set_target_properties(dynamic-delay-network PROPERTIES POSITION_INDEPENDENT_CODE TRUE CXX_VISIBILITY_PRESET hidden)
target_compile_features(dynamic-delay-network PUBLIC cxx_std_20)
target_include_directories(dynamic-delay-network PUBLIC src "${DELAY_AV_INCLUDE_DIR}" PRIVATE vendor)
target_link_libraries(
  dynamic-delay-network
  PUBLIC
    dynamic-delay-rtmp
    Qt6::Core
    Qt6::Network
    "${DELAY_avformat_LIBRARY}"
    "${DELAY_avcodec_LIBRARY}"
    "${DELAY_avutil_LIBRARY}"
)
