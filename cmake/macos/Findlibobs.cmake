if(NOT OBS_LOCAL_APP_SDK)
  find_package(libobs CONFIG REQUIRED)
  return()
endif()

set(_obs_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-32.2.2")
set(_obs_deps_dir "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-2026-07-15-universal")
set(OBS_APP "/Applications/OBS.app" CACHE PATH "OBS application used by the local SDK build")
set(_libobs_binary "${OBS_APP}/Contents/Frameworks/libobs.framework/libobs")

if(NOT EXISTS "${_libobs_binary}")
  message(FATAL_ERROR "OBS 32.2.2 was not found at ${OBS_APP}")
endif()

add_library(OBS::libobs SHARED IMPORTED GLOBAL)
set_target_properties(
  OBS::libobs
  PROPERTIES
    IMPORTED_LOCATION "${_libobs_binary}"
    INTERFACE_INCLUDE_DIRECTORIES "${_obs_source_dir}/libobs;${_obs_source_dir}/libobs/util;${_obs_deps_dir}/include"
)

set(libobs_FOUND TRUE)
