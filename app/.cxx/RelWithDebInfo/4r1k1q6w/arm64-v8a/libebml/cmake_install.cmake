# Install script for directory: C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml

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

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/libebml/libebml.a")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/ebml" TYPE FILE FILES
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlBinary.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlConfig.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlContexts.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlCrc32.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlDate.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlDummy.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlElement.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlEndian.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlFloat.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlHead.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlId.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlMaster.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlSInteger.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlStream.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlString.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlTypes.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlUInteger.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlUnicodeString.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlVersion.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/EbmlVoid.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/IOCallback.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/MemIOCallback.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/MemReadIOCallback.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/SafeReadIOCallback.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libebml/ebml/StdIOCallback.h"
    )
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/ebml" TYPE FILE FILES "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/libebml/ebml_export.h")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/libebml/libebml.pc")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/EBML/EBMLTargets.cmake")
    file(DIFFERENT EXPORT_FILE_CHANGED FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/EBML/EBMLTargets.cmake"
         "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/libebml/CMakeFiles/Export/lib/cmake/EBML/EBMLTargets.cmake")
    if(EXPORT_FILE_CHANGED)
      file(GLOB OLD_CONFIG_FILES "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/EBML/EBMLTargets-*.cmake")
      if(OLD_CONFIG_FILES)
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/EBML/EBMLTargets.cmake\" will be replaced.  Removing files [${OLD_CONFIG_FILES}].")
        file(REMOVE ${OLD_CONFIG_FILES})
      endif()
    endif()
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/EBML" TYPE FILE FILES "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/libebml/CMakeFiles/Export/lib/cmake/EBML/EBMLTargets.cmake")
  if("${CMAKE_INSTALL_CONFIG_NAME}" MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/EBML" TYPE FILE FILES "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/libebml/CMakeFiles/Export/lib/cmake/EBML/EBMLTargets-relwithdebinfo.cmake")
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/EBML" TYPE FILE FILES
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/libebml/EBMLConfig.cmake"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/libebml/EBMLConfigVersion.cmake"
    )
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/RelWithDebInfo/4r1k1q6w/arm64-v8a/_deps/utf8cpp-build/cmake_install.cmake")

endif()

