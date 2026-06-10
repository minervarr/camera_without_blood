#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Matroska::matroska" for configuration "Debug"
set_property(TARGET Matroska::matroska APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(Matroska::matroska PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libmatroska.a"
  )

list(APPEND _IMPORT_CHECK_TARGETS Matroska::matroska )
list(APPEND _IMPORT_CHECK_FILES_FOR_Matroska::matroska "${_IMPORT_PREFIX}/lib/libmatroska.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
