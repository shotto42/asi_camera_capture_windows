# CMake build for the ASI camera app — x64 Windows only (MSVC 2022, nmake).
#
# The build is driven by build_win.bat (vcvars64 + moc + CMake configure,
# then nmake); the bare CMake route also works on a normal desktop:
#
#   cmake -S . -B build_win -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
#   cmake --build build_win
#
# All third-party toolchains are resolved from the vendored third_party/
# tree (Qt 6.7.3 msvc2019_64, OpenCV 4.12.0 vc16, GStreamer 1.24.12 MSYS2
# binaries); the ZWO SDK is vendored in ASI_SDK/ (x64 .lib + .dll).
# A camera is only required at RUN time, not to compile.
cmake_minimum_required(VERSION 3.16)
project(camera_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Q_OBJECT headers (include/*.h) get moc'd into the build tree — but on the
# original build machine CMake's AUTOMOC cannot spawn moc (the sandbox denies
# the autogen libuv child process), so AUTOMOC stays off there and
# build_win.bat pre-generates moc_*.cpp; plain cmake on a normal desktop can
# flip AUTOMOC on if it prefers.
set(CMAKE_AUTOMOC OFF)

# Default to Release when not specified.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# MSVC: emit UTF-8 source/execution charset (sources contain µ, °, § …).
add_compile_options(/utf-8)
# We link the ZWO SDK + mingw GStreamer by full path to the import libs.
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)

set(SRC_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INC_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(TEST_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set(ASI_SDK   ${CMAKE_CURRENT_SOURCE_DIR}/ASI_SDK)
set(THIRD     ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ---------------------------------------------------------------------------
# 1. Qt6 (Widgets only) — vendored 6.7.3 msvc2019_64 prefix
# ---------------------------------------------------------------------------
set(QT_PREFIX ${THIRD}/qt/6.7.3/msvc2019_64)
if(NOT EXISTS ${QT_PREFIX}/lib/cmake/Qt6/Qt6Config.cmake)
  message(FATAL_ERROR
    "Qt6 not found at ${QT_PREFIX}.\n"
    "Expected a Qt 6 msvc2019_64 prefix with bin/ lib/ include/ plugins/.")
endif()
list(PREPEND CMAKE_PREFIX_PATH ${QT_PREFIX})
find_package(Qt6 6.3 REQUIRED COMPONENTS Widgets)
message(STATUS "Qt6: ${Qt6_DIR}")

# ---------------------------------------------------------------------------
# 2. OpenCV — vendored 4.12.0 vc16 (opencv_world4120.dll + .lib + headers)
# ---------------------------------------------------------------------------
set(OPENCV_PREFIX ${THIRD}/opencv/opencv/build)
if(EXISTS ${OPENCV_PREFIX}/x64/vc16/lib/OpenCVConfig.cmake)
  set(OpenCV_DIR ${OPENCV_PREFIX}/x64/vc16/lib)
endif()
# Headers live at build/include; the OpenCV config may not advertise them
# for a bare world-DLL layout, so add them explicitly.
set(OPENCV_INCLUDE_HINT ${OPENCV_PREFIX}/include)
find_package(OpenCV 4.5 REQUIRED)
message(STATUS "OpenCV: version=${OpenCV_VERSION} dirs=${OpenCV_INCLUDE_DIRS}")

# ---------------------------------------------------------------------------
# 3. GStreamer — vendored mingw-w64 1.24.12 (MSYS2 packages) + glib + x264
# ---------------------------------------------------------------------------
# The app links the core + app libs and uses the C API only; the plugin
# DLLs (appsrc/videoconvert/x264enc/mp4mux/filesink) are bundled at runtime.
set(GST_ROOT ${THIRD}/gst)
if(NOT EXISTS ${GST_ROOT}/include/gstreamer-1.0/gst/gst.h)
  message(FATAL_ERROR "GStreamer headers not found under ${GST_ROOT}/include")
endif()
# MSYS2 layout: <gst/gst.h> lives under include/gstreamer-1.0/, <glib.h> +
# <gio/gio.h> under include/glib-2.0/, and the generated <glibconfig.h>
# under lib/glib-2.0/include/.
set(GST_INCLUDE_DIRS
    ${GST_ROOT}/include/gstreamer-1.0
    ${GST_ROOT}/include/glib-2.0
    ${GST_ROOT}/lib/glib-2.0/include
    ${GST_ROOT}/include)
# Import libraries (MSYS2 *.dll.a are COFF import libs; renamed to .lib).
set(GST_LIBS
    ${GST_ROOT}/lib/libgstreamer-1.0.dll.lib
    ${GST_ROOT}/lib/libgstapp-1.0.dll.lib
    ${GST_ROOT}/lib/libglib-2.0.dll.lib
    ${GST_ROOT}/lib/libgobject-2.0.dll.lib)
foreach(_l IN LISTS GST_LIBS)
  if(NOT EXISTS ${_l})
    message(FATAL_ERROR "GStreamer import lib missing: ${_l}")
  endif()
endforeach()

# ---------------------------------------------------------------------------
# 4. ZWO ASICamera2 SDK — vendored x64 (.lib + .dll)
# ---------------------------------------------------------------------------
set(ASI_SDK_LIB ${ASI_SDK}/x64/ASICamera2.lib)
set(ASI_SDK_DLL ${ASI_SDK}/x64/ASICamera2.dll)
if(NOT EXISTS ${ASI_SDK_LIB})
  message(FATAL_ERROR "ZWO SDK import lib missing: ${ASI_SDK_LIB}")
endif()
add_library(ASICamera2 SHARED IMPORTED GLOBAL)
set_target_properties(ASICamera2 PROPERTIES
  IMPORTED_LOCATION ${ASI_SDK_DLL}
  IMPORTED_IMPLIB   ${ASI_SDK_LIB})

# ---------------------------------------------------------------------------
# 5. Application target
# ---------------------------------------------------------------------------
file(GLOB APP_SRC  ${SRC_DIR}/*.cpp)
file(GLOB TEST_SRC ${TEST_DIR}/*.cpp)
# Standalone probes (own main(), built separately, see docs/build.md §5):
# the raw SDK probes plus the synthetic GStreamer encode probe.
list(FILTER TEST_SRC EXCLUDE REGEX ".*/(usb_bench|camera_probe|wb_probe|gst_encode_test)\\.cpp$")
# moc is run by build_win.bat (the sandbox on the build machine denies
# CMake's autogen child-process spawn), so the pre-generated moc_*.cpp are
# regular sources.
file(GLOB MOC_SRC ${CMAKE_BINARY_DIR}/moc_*.cpp)
if(NOT MOC_SRC)
  message(FATAL_ERROR
    "No moc_*.cpp in ${CMAKE_BINARY_DIR} — run build_win.bat configure first "
    "(it runs moc on the six Q_OBJECT headers before calling CMake).")
endif()
list(APPEND APP_SRC ${MOC_SRC})
add_executable(camera_app ${APP_SRC} ${TEST_SRC})
# GUI-subsystem twin of the console target: the SAME sources built as a
# windows-GUI executable (no cmd window) - this is the binary that gets
# distributed (package_win\camera_app.exe) and installed via the .msi. The
# console target stays for the headless self-tests (--smoke, --sertest, ...),
# which need stdout. Entry point: the code uses plain main(), so override the
# MSVC default GUI entry (WinMainCRTStartup) with mainCRTStartup, which
# fills argc/argv from the command line exactly like the console CRT.
add_executable(camera_app_gui ${APP_SRC} ${TEST_SRC})
set_target_properties(camera_app_gui PROPERTIES
  WIN32_EXECUTABLE ON
  OUTPUT_NAME camera_app
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/gui)
target_link_options(camera_app_gui PRIVATE "LINKER:/ENTRY:mainCRTStartup")
set_target_properties(camera_app PROPERTIES WIN32_EXECUTABLE OFF)  # console app
# The GUI twin gets the very same include/link settings:
set(APP_TARGETS camera_app camera_app_gui)
foreach(_t IN LISTS APP_TARGETS)
  target_include_directories(${_t} PRIVATE
    ${INC_DIR}
    ${TEST_DIR}
    ${ASI_SDK}
    ${OpenCV_INCLUDE_DIRS}
    ${GST_INCLUDE_DIRS})
  target_link_libraries(${_t} PRIVATE
    ASICamera2
    Qt6::Widgets
    ${OpenCV_LIBS}
    # CaptureStackBackTrace links against the SDK's DbgHelp (dbghlp.dll) — the
    # old dbghlp.lib import lib is no longer installed.
    ${GST_LIBS} DbgHelp)
  if(OPENCV_INCLUDE_HINT AND NOT OpenCV_INCLUDE_DIRS MATCHES "include$")
    target_include_directories(${_t} PRIVATE ${OPENCV_INCLUDE_HINT})
  endif()
endforeach()

# ---------------------------------------------------------------------------
# 6. Optional standalone hardware probes (camera_probe / usb_bench / wb_probe)
# ---------------------------------------------------------------------------
option(BUILD_PROBES "Build the standalone raw-SDK probes" OFF)
if(BUILD_PROBES)
  foreach(_p camera_probe usb_bench wb_probe)
    add_executable(${_p} ${TEST_DIR}/${_p}.cpp)
    set_target_properties(${_p} PROPERTIES WIN32_EXECUTABLE OFF)
    target_include_directories(${_p} PRIVATE ${INC_DIR} ${TEST_DIR} ${ASI_SDK} ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(${_p} PRIVATE ASICamera2 ${OpenCV_LIBS})
  endforeach()
endif()

# ---------------------------------------------------------------------------
# 7. Post-build: stage runtime DLLs next to each exe (self-contained tree)
# ---------------------------------------------------------------------------
# OpenCV world DLL (+ optional ffmpeg videoio backend) and the ZWO SDK DLL.
add_custom_command(TARGET camera_app POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:camera_app>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${THIRD}/opencv/opencv/build/x64/vc16/bin/opencv_world4120.dll
          ${THIRD}/opencv/opencv/build/x64/vc16/bin/opencv_videoio_ffmpeg4120_64.dll
          ${ASI_SDK_DLL}
          "$<TARGET_FILE_DIR:camera_app>"
  COMMENT "Staging OpenCV + ZWO SDK DLLs next to camera_app")

# Qt runtime: the three Widgets-chain DLLs next to the exe, and the
# windows + offscreen platform plugins in platforms/ (the GUI needs
# qwindows.dll; the headless self-tests run with -platform offscreen).
add_custom_command(TARGET camera_app POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:camera_app>/platforms"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${QT_PREFIX}/bin/Qt6Core.dll
          ${QT_PREFIX}/bin/Qt6Gui.dll
          ${QT_PREFIX}/bin/Qt6Widgets.dll
          "$<TARGET_FILE_DIR:camera_app>/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${QT_PREFIX}/plugins/platforms/qwindows.dll
          ${QT_PREFIX}/plugins/platforms/qoffscreen.dll
          "$<TARGET_FILE_DIR:camera_app>/platforms/"
  COMMENT "Staging Qt runtime + platform plugins next to camera_app")

# GStreamer: all core/glib/x264 runtime DLLs next to the exe, and the
# encoder plugins into gstreamer-1.0/ next to the exe (main.cpp points
# GST_PLUGIN_PATH there when the directory exists).
set(GST_RUNTIME_DLLS "")
file(GLOB _gstdlls ${GST_ROOT}/runtime/*.dll)
set(GST_RUNTIME_DLLS ${_gstdlls})
add_custom_command(TARGET camera_app POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:camera_app>/gstreamer-1.0"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GST_RUNTIME_DLLS} "$<TARGET_FILE_DIR:camera_app>/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${GST_ROOT}/plugins/libgstapp.dll
          ${GST_ROOT}/plugins/libgstvideoconvertscale.dll
          ${GST_ROOT}/plugins/libgstcoreelements.dll
          ${GST_ROOT}/plugins/libgstisomp4.dll
          ${GST_ROOT}/plugins/libgstx264.dll
          "$<TARGET_FILE_DIR:camera_app>/gstreamer-1.0/"
  COMMENT "Staging GStreamer runtime + encoder plugins")

# Stage the identical runtime tree next to the GUI exe (self-contained,
# straight from the source tree - no dependency on the console target's
# output dir), so build_win\gui\camera_app.exe runs standalone too.
add_custom_command(TARGET camera_app_gui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:camera_app_gui>/gstreamer-1.0"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:camera_app_gui>/platforms"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${THIRD}/opencv/opencv/build/x64/vc16/bin/opencv_world4120.dll
          ${THIRD}/opencv/opencv/build/x64/vc16/bin/opencv_videoio_ffmpeg4120_64.dll
          ${ASI_SDK_DLL}
          ${QT_PREFIX}/bin/Qt6Core.dll
          ${QT_PREFIX}/bin/Qt6Gui.dll
          ${QT_PREFIX}/bin/Qt6Widgets.dll
          ${GST_RUNTIME_DLLS}
          "$<TARGET_FILE_DIR:camera_app_gui>/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${QT_PREFIX}/plugins/platforms/qwindows.dll
          ${QT_PREFIX}/plugins/platforms/qoffscreen.dll
          "$<TARGET_FILE_DIR:camera_app_gui>/platforms/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${GST_ROOT}/plugins/libgstapp.dll
          ${GST_ROOT}/plugins/libgstvideoconvertscale.dll
          ${GST_ROOT}/plugins/libgstcoreelements.dll
          ${GST_ROOT}/plugins/libgstisomp4.dll
          ${GST_ROOT}/plugins/libgstx264.dll
          "$<TARGET_FILE_DIR:camera_app_gui>/gstreamer-1.0/"
  COMMENT "Staging the same runtime tree next to the GUI exe")

message(STATUS "=== camera_app configured (x64 Windows, type=${CMAKE_BUILD_TYPE}) ===")
