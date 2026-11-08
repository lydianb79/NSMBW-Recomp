cmake_minimum_required(VERSION 3.20)
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
endif()
project(NSMBWRecomp C)

# ---------------------------------------------------------------------------
# Generated code location (output of tools/recomp.py). Run `make -C . gen`
# or scripts/build.sh first; this file only compiles what is there.
# ---------------------------------------------------------------------------
set(BUILD_DATA_DIR "${CMAKE_SOURCE_DIR}/build" CACHE PATH "Directory with image.bin/image.json and gen/")
set(GEN_DIR "${BUILD_DATA_DIR}/gen")

if(NOT EXISTS "${GEN_DIR}/sources.cmake")
  message(FATAL_ERROR "No generated code in ${GEN_DIR}. Run scripts/build.sh (or tools/recomp.py) first.")
endif()
include("${GEN_DIR}/sources.cmake")

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(WARNING "Clang is strongly recommended: generated code relies on [[musttail]] for guaranteed tail calls.")
endif()

# --- recompiled game code: big, warning-heavy, compiled once -----------------
add_library(nsmbw_gen STATIC ${PPC_GEN_SOURCES})
target_include_directories(nsmbw_gen PUBLIC runtime/include ${GEN_DIR})
target_compile_options(nsmbw_gen PRIVATE -w -fno-strict-aliasing
  $<$<CONFIG:Release>:-O2> $<$<CONFIG:RelWithDebInfo>:-O2 -g1> $<$<CONFIG:Debug>:-O1 -g1>)

# --- runtime ----------------------------------------------------------------
add_library(nsmbw_runtime OBJECT
  runtime/src/ppc_runtime.c
  runtime/src/memory.c
  runtime/src/mmio.c
  runtime/src/ios.c
  runtime/src/gx_fifo.c
  runtime/src/gx_soft.c
  runtime/src/video.c
  runtime/src/audio.c
  runtime/src/settings.c
  runtime/src/rt_main.c
  runtime/src/hle/hle_os.c
  runtime/src/hle/hle_wpad.c
  runtime/src/hle/hle_dsp.c
  runtime/src/hle/hle_ax.c)
target_include_directories(nsmbw_runtime PUBLIC runtime/include PRIVATE runtime/src ${GEN_DIR})
target_compile_options(nsmbw_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
find_package(Threads REQUIRED)
target_link_libraries(nsmbw_runtime PUBLIC Threads::Threads m)

# HLE overrides are strong definitions of weak generated symbols. The runtime
# is an OBJECT library, so its objects are linked directly (no archive lookup,
# no -force_load, which breaks on paths containing spaces).
set(RT_LINK nsmbw_runtime nsmbw_gen Threads::Threads m)

# --- headless runner (any platform) ------------------------------------------
add_executable(nsmbw-headless runtime/src/headless.c)
target_link_libraries(nsmbw-headless PRIVATE ${RT_LINK})

# --- macOS application -------------------------------------------------------
if(APPLE)
  enable_language(OBJC)
  set(APP_SOURCES
    platform/macos/main.m
    platform/macos/AppDelegate.m
    platform/macos/GameView.m
    platform/macos/Renderer.m
    platform/macos/InputManager.m
    platform/macos/PreferencesWindowController.m
    platform/macos/MetalGX.m
    platform/macos/AudioOutput.m)
  set(APP_SHADERS platform/macos/Shaders.metal)
  set_source_files_properties(${APP_SHADERS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

  # All icon variants are bundled (Preferences > Graphics > App icon switches the Dock icon at run
  # time); NSMBW_ICON picks the one Finder shows (see platform/macos/icons/).
  set(NSMBW_ICON "pixel_leap" CACHE STRING "Default app icon (day, sunset, night, winter, autumn, arcade_chip, pixel_leap, neon, paper, minimal)")
  set(APP_ICONS
    platform/macos/icons/AppIcon-day.icns platform/macos/icons/AppIcon-sunset.icns
    platform/macos/icons/AppIcon-night.icns platform/macos/icons/AppIcon-winter.icns
    platform/macos/icons/AppIcon-autumn.icns
    platform/macos/icons/AppIcon-arcade_chip.icns platform/macos/icons/AppIcon-pixel_leap.icns
    platform/macos/icons/AppIcon-neon.icns platform/macos/icons/AppIcon-paper.icns
    platform/macos/icons/AppIcon-minimal.icns)
  set_source_files_properties(${APP_ICONS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  add_executable(NSMBWRecomp MACOSX_BUNDLE ${APP_SOURCES} ${APP_SHADERS} ${APP_ICONS})
  set_target_properties(NSMBWRecomp PROPERTIES
    OUTPUT_NAME "NSMBW Recomp"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/platform/macos/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.local.nsmbw-recomp"
    MACOSX_BUNDLE_BUNDLE_NAME "NSMBW Recomp"
    MACOSX_BUNDLE_ICON_FILE "AppIcon-${NSMBW_ICON}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)
  target_compile_options(NSMBWRecomp PRIVATE -fobjc-arc -Wall -Wno-unused-parameter -Wno-deprecated-declarations)
  target_include_directories(NSMBWRecomp PRIVATE runtime/include runtime/src)
  target_link_libraries(NSMBWRecomp PRIVATE ${RT_LINK}
    "-framework Cocoa" "-framework Metal" "-framework MetalKit" "-framework QuartzCore"
    "-framework GameController" "-framework AudioToolbox" "-framework CoreVideo" "-framework CoreHaptics" "-framework UniformTypeIdentifiers")

  # Shaders.metal ships as a resource and is compiled at launch, so only the
  # Command Line Tools are required. The recompiled image is copied in too.
  add_custom_command(TARGET NSMBWRecomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DATA_DIR}/image.bin "$<TARGET_BUNDLE_CONTENT_DIR:NSMBWRecomp>/Resources/image.bin"
    COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DATA_DIR}/image.json "$<TARGET_BUNDLE_CONTENT_DIR:NSMBWRecomp>/Resources/image.json"
    COMMENT "Copying recompiled image into the app bundle")
endif()
