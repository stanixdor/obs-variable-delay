cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version} LANGUAGES C CXX)

if(OBS_LOCAL_APP_SDK)
  configure_file(cmake/macos/obsconfig.h.in "${CMAKE_CURRENT_BINARY_DIR}/obsconfig.h" @ONLY)
endif()

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
option(ENABLE_TESTS "Build Dynamic Delay unit tests" OFF)
option(ENABLE_OBS_INTEGRATION_TESTS "Build opt-in tests against real libobs and a graphics backend" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

if(LINUX)
  # OBS rejects modules built against a newer major/minor module ABI. Keep Linux
  # CI on the 32.2 ABI even when the OBS PPA advances to a newer release line.
  find_package(libobs 32.2...<32.3 REQUIRED)
else()
  find_package(libobs REQUIRED)
endif()
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      $<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-Wno-quoted-include-in-framework-header>
      $<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-Wno-comma>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wno-quoted-include-in-framework-header>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

include(multistream-dependencies)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE dynamic-delay-network)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/audience-preview.cpp
    src/audience-preview.hpp
    src/audio-preflight.cpp
    src/audio-preflight.hpp
    src/delay-controller.cpp
    src/delay-controller.hpp
    src/delay-dock.cpp
    src/delay-dock.hpp
    src/delay-types.cpp
    src/delay-types.hpp
    src/hold-audio-tap.cpp
    src/hold-audio-tap.hpp
    src/hold-media-hub.cpp
    src/hold-media-hub.hpp
    src/hold-pipeline.cpp
    src/hold-pipeline.hpp
    src/obs-packet.hpp
    src/multistream-controller.cpp
    src/multistream-controller.hpp
    src/multistream-dock.cpp
    src/multistream-dock.hpp
    src/multistream-encoders.cpp
    src/multistream-encoders.hpp
    src/output-session.cpp
    src/output-session.hpp
    src/plugin-main.cpp
    src/preview-capture.cpp
    src/preview-capture.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(APPLE AND OBS_LOCAL_APP_SDK)
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/Info.plist.in"
      MACOSX_BUNDLE_GUI_IDENTIFIER "${MACOS_BUNDLEID}"
      MACOSX_BUNDLE_BUNDLE_NAME "OBS Dynamic Delay"
      MACOSX_BUNDLE_BUNDLE_VERSION "${PLUGIN_VERSION}"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${PLUGIN_VERSION}"
  )
  target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "LINKER:-rpath,@executable_path/../Frameworks")
endif()

if(ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ENABLE_OBS_INTEGRATION_TESTS)
  find_package(Qt6 REQUIRED COMPONENTS Gui)
  add_executable(preview_gpu_integration tests/preview_gpu_integration.cpp src/preview-capture.cpp)
  target_compile_features(preview_gpu_integration PRIVATE cxx_std_20)
  target_include_directories(preview_gpu_integration PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(preview_gpu_integration PRIVATE OBS::libobs Qt6::Gui)
  target_compile_options(
    preview_gpu_integration
    PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>:-Wno-deprecated-declarations>
  )
  if(APPLE AND OBS_LOCAL_APP_SDK)
    target_link_options(preview_gpu_integration PRIVATE "LINKER:-rpath,${OBS_APP}/Contents/Frameworks")
  endif()
  add_executable(
    output_session_integration
    tests/output_session_integration.cpp
    src/output-session.cpp
    src/hold-pipeline.cpp
    src/hold-media-hub.cpp
    src/hold-audio-tap.cpp
  )
  target_compile_features(output_session_integration PRIVATE cxx_std_20)
  target_include_directories(output_session_integration PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(output_session_integration PRIVATE OBS::libobs Qt6::Gui plugin-support)
  target_compile_options(
    output_session_integration
    PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>:-Wno-deprecated-declarations>
  )
  if(APPLE AND OBS_LOCAL_APP_SDK)
    target_link_options(output_session_integration PRIVATE "LINKER:-rpath,${OBS_APP}/Contents/Frameworks")
  endif()
  add_executable(
    multistream_integration
    tests/multistream_integration.cpp
    src/multistream-controller.cpp
    src/multistream-controller.hpp
    src/delay-controller.cpp
    src/delay-controller.hpp
    src/audio-preflight.cpp
    src/delay-types.cpp
    src/multistream-encoders.cpp
    src/output-session.cpp
    src/hold-pipeline.cpp
    src/hold-media-hub.cpp
    src/hold-audio-tap.cpp
  )
  target_compile_features(multistream_integration PRIVATE cxx_std_20)
  target_include_directories(
    multistream_integration
    PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}" $<TARGET_PROPERTY:OBS::obs-frontend-api,INTERFACE_INCLUDE_DIRECTORIES>
  )
  set_target_properties(multistream_integration PROPERTIES AUTOMOC ON)
  target_link_libraries(multistream_integration PRIVATE OBS::libobs Qt6::Widgets plugin-support dynamic-delay-network)
  target_compile_options(
    multistream_integration
    PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>:-Wno-deprecated-declarations>
  )
  if(APPLE AND OBS_LOCAL_APP_SDK)
    target_link_options(multistream_integration PRIVATE "LINKER:-rpath,${OBS_APP}/Contents/Frameworks")
  endif()
endif()
