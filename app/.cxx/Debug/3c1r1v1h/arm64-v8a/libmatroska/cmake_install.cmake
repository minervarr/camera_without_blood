# Install script for directory: C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska

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
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/Debug/3c1r1v1h/arm64-v8a/libmatroska/libmatroska.a")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/matroska" TYPE FILE FILES
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska/matroska/KaxBlockData.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska/matroska/KaxBlock.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska/matroska/KaxCluster.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska/matroska/KaxConfig.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska/matroska/KaxContexts.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska/matroska/KaxCuesData.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska/matroska/KaxCues.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska/matroska/KaxDefines.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska/matroska/KaxSeekHead.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska/matroska/KaxSegment.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska/matroska/KaxSemantic.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska/matroska/KaxTracks.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska/matroska/KaxTypes.h"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/libs/libmatroska/matroska/KaxVersion.h"
    )
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/matroska" TYPE FILE FILES "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/Debug/3c1r1v1h/arm64-v8a/libmatroska/matroska_export.h")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/Debug/3c1r1v1h/arm64-v8a/libmatroska/libmatroska.pc")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/Matroska/MatroskaTargets.cmake")
    file(DIFFERENT EXPORT_FILE_CHANGED FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/Matroska/MatroskaTargets.cmake"
         "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/Debug/3c1r1v1h/arm64-v8a/libmatroska/CMakeFiles/Export/lib/cmake/Matroska/MatroskaTargets.cmake")
    if(EXPORT_FILE_CHANGED)
      file(GLOB OLD_CONFIG_FILES "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/Matroska/MatroskaTargets-*.cmake")
      if(OLD_CONFIG_FILES)
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/Matroska/MatroskaTargets.cmake\" will be replaced.  Removing files [${OLD_CONFIG_FILES}].")
        file(REMOVE ${OLD_CONFIG_FILES})
      endif()
    endif()
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/Matroska" TYPE FILE FILES "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/Debug/3c1r1v1h/arm64-v8a/libmatroska/CMakeFiles/Export/lib/cmake/Matroska/MatroskaTargets.cmake")
  if("${CMAKE_INSTALL_CONFIG_NAME}" MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/Matroska" TYPE FILE FILES "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/Debug/3c1r1v1h/arm64-v8a/libmatroska/CMakeFiles/Export/lib/cmake/Matroska/MatroskaTargets-debug.cmake")
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/Matroska" TYPE FILE FILES
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/Debug/3c1r1v1h/arm64-v8a/libmatroska/MatroskaConfig.cmake"
    "C:/Users/incxiuefb/Documents/Files/clone/camera_without_blood/app/.cxx/Debug/3c1r1v1h/arm64-v8a/libmatroska/MatroskaConfigVersion.cmake"
    )
endif()

