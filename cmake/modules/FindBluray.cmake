#.rst:
# FindBluray
# ----------
# Finds the libbluray library
#
# This will define the following variables::
#
# BLURAY_FOUND - system has libbluray
# BLURAY_INCLUDE_DIRS - the libbluray include directory
# BLURAY_LIBRARIES - the libbluray libraries
# BLURAY_DEFINITIONS - the libbluray compile definitions
#
# and the following imported targets::
#
#   Bluray::Bluray   - The libbluray library

set(Bluray_FIND_VERSION 0.9.3)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_BLURAY libbluray>=${Bluray_FIND_VERSION} QUIET)
  set(BLURAY_VERSION ${PC_BLURAY_VERSION})
endif()

find_path(BLURAY_INCLUDE_DIR libbluray/bluray.h
                             PATHS ${PC_BLURAY_INCLUDEDIR})

if(NOT BLURAY_VERSION AND EXISTS ${BLURAY_INCLUDE_DIR}/libbluray/bluray-version.h)
  file(STRINGS ${BLURAY_INCLUDE_DIR}/libbluray/bluray-version.h _bluray_version_str
       REGEX "#define[ \t]BLURAY_VERSION_STRING[ \t][\"]?[0-9.]+[\"]?")
  string(REGEX REPLACE "^.*BLURAY_VERSION_STRING[ \t][\"]?([0-9.]+).*$" "\\1" BLURAY_VERSION ${_bluray_version_str})
  unset(_bluray_version_str)
endif()

find_library(BLURAY_LIBRARY NAMES bluray libbluray
                            PATHS ${PC_BLURAY_LIBDIR})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Bluray
                                  REQUIRED_VARS BLURAY_LIBRARY BLURAY_INCLUDE_DIR BLURAY_VERSION
                                  VERSION_VAR BLURAY_VERSION)

if(BLURAY_FOUND)
  set(BLURAY_LIBRARIES ${BLURAY_LIBRARY})
  set(BLURAY_INCLUDE_DIRS ${BLURAY_INCLUDE_DIR})
  set(BLURAY_DEFINITIONS -DHAVE_LIBBLURAY=1)

  include(CheckSymbolExists)
  set(CMAKE_REQUIRED_INCLUDES ${BLURAY_INCLUDE_DIR})
  set(CMAKE_REQUIRED_LIBRARIES ${BLURAY_LIBRARY})
  foreach(symbol bd_presented
                 bd_set_presentation_hold
                 bd_set_presentation_clock
                 BD_PRESENTED_ALL
                 BD_SEEK_REARMS_PLAYLIST_END
                 BD_PRESENTED_TITLE_END)
    check_symbol_exists(${symbol} libbluray/bluray.h LIBBLURAY_HAS_${symbol})
    if(NOT LIBBLURAY_HAS_${symbol})
      message(FATAL_ERROR "libbluray lacks ${symbol} of the CoreELEC presentation hold API")
    endif()
  endforeach()
  check_symbol_exists(bd_bdj_video_active libbluray/bluray.h LIBBLURAY_HAS_BDJ_VIDEO_ACTIVE)
  if(LIBBLURAY_HAS_BDJ_VIDEO_ACTIVE)
    list(APPEND BLURAY_DEFINITIONS -DHAVE_LIBBLURAY_BDJ_VIDEO_ACTIVE=1)
  endif()
  unset(CMAKE_REQUIRED_INCLUDES)
  unset(CMAKE_REQUIRED_LIBRARIES)

  # todo: improve syntax
  if (NOT CORE_PLATFORM_NAME_LC STREQUAL windowsstore)
    list(APPEND BLURAY_DEFINITIONS -DHAVE_LIBBLURAY_BDJ=1)
  endif()

  if(${BLURAY_LIBRARY} MATCHES ".+\.a$" AND PC_BLURAY_STATIC_LIBRARIES)
    list(APPEND BLURAY_LIBRARIES ${PC_BLURAY_STATIC_LIBRARIES})
  endif()

  if(NOT TARGET Bluray::Bluray)
    add_library(Bluray::Bluray UNKNOWN IMPORTED)
    if(BLURAY_LIBRARY)
      set_target_properties(Bluray::Bluray PROPERTIES
                                           IMPORTED_LOCATION "${BLURAY_LIBRARY}")
    endif()
    set_target_properties(Bluray::Bluray PROPERTIES
                                         INTERFACE_INCLUDE_DIRECTORIES "${BLURAY_INCLUDE_DIR}"
                                         INTERFACE_COMPILE_DEFINITIONS HAVE_LIBBLURAY=1)
  endif()
endif()

mark_as_advanced(BLURAY_INCLUDE_DIR BLURAY_LIBRARY)
