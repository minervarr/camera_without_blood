# Install script for directory: C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/camera_recorder")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "RelWithDebInfo")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "0")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "C:/PPProgam/android_sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objdump.exe")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/flac/src/cmake_install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/FLAC/targets.cmake")
    file(DIFFERENT EXPORT_FILE_CHANGED FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/FLAC/targets.cmake"
         "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/flac/CMakeFiles/Export/lib/cmake/FLAC/targets.cmake")
    if(EXPORT_FILE_CHANGED)
      file(GLOB OLD_CONFIG_FILES "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/FLAC/targets-*.cmake")
      if(OLD_CONFIG_FILES)
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/FLAC/targets.cmake\" will be replaced.  Removing files [${OLD_CONFIG_FILES}].")
        file(REMOVE ${OLD_CONFIG_FILES})
      endif()
    endif()
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/FLAC" TYPE FILE FILES "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/flac/CMakeFiles/Export/lib/cmake/FLAC/targets.cmake")
  if("${CMAKE_INSTALL_CONFIG_NAME}" MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/FLAC" TYPE FILE FILES "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/flac/CMakeFiles/Export/lib/cmake/FLAC/targets-relwithdebinfo.cmake")
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/FLAC" TYPE FILE FILES
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/flac/flac-config.cmake"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/flac/flac-config-version.cmake"
    )
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/FLAC" TYPE FILE FILES
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/flac/flac-config.cmake"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/flac/flac-config-version.cmake"
    )
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/FLAC" TYPE FILE FILES
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac/include/FLAC/all.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac/include/FLAC/assert.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac/include/FLAC/callback.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac/include/FLAC/export.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac/include/FLAC/format.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac/include/FLAC/metadata.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac/include/FLAC/ordinals.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac/include/FLAC/stream_decoder.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac/include/FLAC/stream_encoder.h"
    )
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/FLAC++" TYPE FILE FILES
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac/include/FLAC++/all.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac/include/FLAC++/decoder.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac/include/FLAC++/encoder.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac/include/FLAC++/export.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/flac/include/FLAC++/metadata.h"
    )
endif()

