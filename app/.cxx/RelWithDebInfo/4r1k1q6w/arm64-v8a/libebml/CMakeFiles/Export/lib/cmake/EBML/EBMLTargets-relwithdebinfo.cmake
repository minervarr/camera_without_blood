#----------------------------------------------------------------
# Generated CMake target import file for configuration "RelWithDebInfo".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "EBML::ebml" for configuration "RelWithDebInfo"
set_property(TARGET EBML::ebml APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(EBML::ebml PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELWITHDEBINFO "CXX"
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/libebml.a"
  )

list(APPEND _IMPORT_CHECK_TARGETS EBML::ebml )
list(APPEND _IMPORT_CHECK_FILES_FOR_EBML::ebml "${_IMPORT_PREFIX}/lib/libebml.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
