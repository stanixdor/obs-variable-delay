if(NOT OBS_LOCAL_APP_SDK)
  find_package(obs-frontend-api CONFIG REQUIRED)
  return()
endif()

set(_obs_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-32.2.2")
set(OBS_APP "/Applications/OBS.app" CACHE PATH "OBS application used by the local SDK build")
set(_frontend_binary "${OBS_APP}/Contents/Frameworks/obs-frontend-api.dylib")

if(NOT EXISTS "${_frontend_binary}")
  message(FATAL_ERROR "obs-frontend-api was not found in ${OBS_APP}")
endif()

add_library(OBS::obs-frontend-api SHARED IMPORTED GLOBAL)
set_target_properties(
  OBS::obs-frontend-api
  PROPERTIES IMPORTED_LOCATION "${_frontend_binary}" INTERFACE_INCLUDE_DIRECTORIES "${_obs_source_dir}/frontend/api"
)
target_link_libraries(OBS::obs-frontend-api INTERFACE OBS::libobs)

set(obs-frontend-api_FOUND TRUE)
