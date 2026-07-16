cmake_minimum_required(VERSION 3.22.1)
project(camera_recorder)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Paths ──────────────────────────────────────────────────────────────────────
set(ROOT_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
set(LIBS_DIR     ${ROOT_DIR}/libs)

# First-party libraries (minervarr repos)
set(FIRSTPARTY_DIR ${LIBS_DIR}/firstparty)
set(CANVAS_DIR     ${FIRSTPARTY_DIR}/Vk_Canvas_Lb_LAW)
set(AUDIO_DIR      ${FIRSTPARTY_DIR}/audio_engine/src/main/cpp)
set(ARCHIVE_DIR    ${FIRSTPARTY_DIR}/archive_engine/src/main/cpp)

# Third-party libraries
set(THIRDPARTY_DIR ${LIBS_DIR}/thirdparty)

set(NATIVE_GLUE  ${ANDROID_NDK}/sources/android/native_app_glue)

# ── Vk_Canvas_Lb_LAW (canvas + font engine) ──────────────────────────────────
add_subdirectory(${CANVAS_DIR}/core ${CMAKE_BINARY_DIR}/vk_canvas_core)

# --- KissFFT ---
add_library(kiss_fft STATIC
    ${THIRDPARTY_DIR}/kiss_fft/kiss_fft.c
    ${THIRDPARTY_DIR}/kiss_fft/kiss_fftr.c
)
target_include_directories(kiss_fft PUBLIC ${THIRDPARTY_DIR}/kiss_fft)
target_compile_definitions(kiss_fft PRIVATE FLOATING_POINT USE_KISS_FFT EXPORT=)
target_compile_options(kiss_fft PRIVATE -Wno-unused-parameter -Wno-sign-compare)

# --- SoxR ---
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_OPENMP OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY_DIR}/soxr ${CMAKE_BINARY_DIR}/soxr)
target_include_directories(soxr PUBLIC ${THIRDPARTY_DIR}/soxr/src)

# --- ONNX Runtime ---
set(ONNXRUNTIME_DIR ${THIRDPARTY_DIR}/onnxruntime)
add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES IMPORTED_LOCATION
    ${ONNXRUNTIME_DIR}/lib/${ANDROID_ABI}/libonnxruntime.so
)
target_include_directories(onnxruntime INTERFACE ${ONNXRUNTIME_DIR}/include)

# ── libusb (from audio_engine submodule) ──────────────────────────────────────
set(LIBUSB_DIR ${AUDIO_DIR}/libusb)
add_library(usb1 STATIC
    ${LIBUSB_DIR}/libusb/core.c
    ${LIBUSB_DIR}/libusb/descriptor.c
    ${LIBUSB_DIR}/libusb/hotplug.c
    ${LIBUSB_DIR}/libusb/io.c
    ${LIBUSB_DIR}/libusb/sync.c
    ${LIBUSB_DIR}/libusb/strerror.c
    ${LIBUSB_DIR}/libusb/os/linux_usbfs.c
    ${LIBUSB_DIR}/libusb/os/linux_netlink.c
    ${LIBUSB_DIR}/libusb/os/events_posix.c
    ${LIBUSB_DIR}/libusb/os/threads_posix.c
)
target_include_directories(usb1 PRIVATE
    ${AUDIO_DIR}
    ${LIBUSB_DIR}/android
    ${LIBUSB_DIR}/libusb
    ${LIBUSB_DIR}/libusb/os
)
target_compile_options(usb1 PRIVATE -Wno-unused-parameter -Wno-sign-compare)

# ── libFLAC ───────────────────────────────────────────────────────────────────
set(BUILD_CXXLIBS       OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS      OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING       OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS          OFF CACHE BOOL "" FORCE)
set(WITH_OGG            OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES    OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY_DIR}/flac ${CMAKE_BINARY_DIR}/flac)

# ── libebml ───────────────────────────────────────────────────────────────────
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY_DIR}/libebml    ${CMAKE_BINARY_DIR}/libebml)
add_library(EBML::ebml ALIAS ebml)

# ── libmatroska ───────────────────────────────────────────────────────────────
set(CMAKE_DISABLE_FIND_PACKAGE_EBML ON)
add_subdirectory(${THIRDPARTY_DIR}/libmatroska ${CMAKE_BINARY_DIR}/libmatroska)

#  NCNN
set(NCNN_VULKAN ON CACHE BOOL "" FORCE)
set(NCNN_DISABLE_EXCEPTION OFF CACHE BOOL "" FORCE)
set(NCNN_DISABLE_RTTI OFF CACHE BOOL "" FORCE)
set(NCNN_SHARED_LIB OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY_DIR}/ncnn ${CMAKE_BINARY_DIR}/ncnn)

# ── Shader compilation (slangc from Vulkan SDK) ───────────────────────────────
# Reusable Slang→SPIR-V helper provided by the canvas library. Override the
# compiler path with -DVCE_SLANGC=/path/to/slangc if the Vulkan SDK is elsewhere.
include(${CANVAS_DIR}/cmake/VceShaders.cmake)

set(SHADER_OUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../assets/shaders)

# The canvas/font engine's shaders.
# Font engine shaders (composite/tiling/coverage) from vulkan_font_engine.
vce_compile_slang(compile_font_shaders ${SHADER_OUT_DIR}
    ${CANVAS_DIR}/first_party/vulkan_font_engine/shaders_src
    composite_vert composite_frag tiling coverage)
# Canvas engine shaders (overlay/image) from Vk_Canvas_Lb_LAW.
vce_compile_slang(compile_canvas_shaders ${SHADER_OUT_DIR}
    ${CANVAS_DIR}/shaders_src
    overlay_vert overlay_frag image_vert image_frag shape_vert shape_frag)

# ── Main shared library ───────────────────────────────────────────────────────
add_library(camera_recorder SHARED
    # Entry point + lifecycle
    main.cc
    app.cc
    logger.cc

    # Camera2 NDK
    camera/camera.cc
    camera/dng_writer.cc
    camera/dng_meta_source.cc
    camera/still_writer.cc
    camera/hdr_merge.cc
    camera/mtb_align.cc

    # ISP (Video Pipeline) — compute_context is WIP in canvas lib, compiled here
    isp/ai_denoiser.cc
    isp/frame_store.cc
    isp/raw_video_pipeline.cc
    ${CANVAS_DIR}/core/compute_context.cc

    # JNI bridge to the Java HDR camera session
    jni/jni_camera.cc

    # Device orientation (accelerometer) — Android platform code from canvas lib
    ${CANVAS_DIR}/platform/android/orientation.cc

    # Android platform seam (SurfaceProvider, AssetReader, FrameWaker)
    ${CANVAS_DIR}/platform/android/android_platform.cc

    # Audio capture (DAC/USB)
    audio/audio_capture.cc

    # AI audio denoise (DeepFilterNet 3, ONNX Runtime, offline at finalize)
    audio/df_net.cc

    # MKV muxer wrapper
    muxer/muxer.cc
    muxer/hevc_bitstream.cc

    # Recorder: orchestrates camera + audio + muxer
    recorder/recorder.cc

    # UI (draws over Vulkan canvas)
    ui/ui.cc

    # audio_engine native sources (JNI wrappers excluded — we call C++ API directly)
    ${AUDIO_DIR}/usb_audio.cpp

    # archive_engine native sources
    archive.cc

    # NDK native app glue
    ${NATIVE_GLUE}/android_native_app_glue.c
)

add_dependencies(camera_recorder compile_font_shaders compile_canvas_shaders)

target_include_directories(camera_recorder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CANVAS_DIR}/core
    ${CANVAS_DIR}/platform/android
    ${CANVAS_DIR}/first_party/vulkan_font_engine/core
    ${AUDIO_DIR}
    ${LIBUSB_DIR}/libusb
    ${ARCHIVE_DIR}/archive/include
    ${ARCHIVE_DIR}/util/include
    ${NATIVE_GLUE}
    ${THIRDPARTY_DIR}/libebml
    ${THIRDPARTY_DIR}/libmatroska
    ${THIRDPARTY_DIR}/flac/include
    ${THIRDPARTY_DIR}/ncnn/src
)

target_compile_options(camera_recorder PRIVATE -O2)

target_link_libraries(camera_recorder
    # Canvas + Font engine (vk_canvas_core links vk_font_core which links freetype + msdfgen)
    vk_canvas_core
    # AI Noise Reduction
    ncnn
    # Audio and DSP
    FLAC
    kiss_fft
    soxr
    onnxruntime
    # Audio (USB capture + FLAC encoding)
    usb1
    # MKV
    matroska
    ebml
    # Camera2 + MediaCodec
    camera2ndk
    mediandk
    # Built-in microphone capture
    aaudio
    # Android + Vulkan
    vulkan
    android
    log
    EGL
    GLESv3
)
