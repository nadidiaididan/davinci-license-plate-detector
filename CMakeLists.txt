cmake_minimum_required(VERSION 3.16)
project(PlateMask LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  # Self-contained DLL: static C runtime, no dependency on a redistributable.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W3 /EHsc /wd4996 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)
endif()
if(WIN32)
  # ofxCore.h keys its dllexport macro on WIN32/WIN64 (as the Visual Studio templates define them).
  add_compile_definitions(WIN32 WIN64)
endif()
if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
  if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
  endif()
endif()

set(OFX_INC ${CMAKE_SOURCE_DIR}/third_party/openfx/include ${CMAKE_SOURCE_DIR}/third_party/openfx/Support/include)

# ---- core
file(GLOB CORE_SRC ${CMAKE_SOURCE_DIR}/src/core/*.cpp)
add_library(pmcore STATIC ${CORE_SRC})
target_include_directories(pmcore PUBLIC ${CMAKE_SOURCE_DIR}/src)
find_package(Threads REQUIRED)
target_link_libraries(pmcore PUBLIC Threads::Threads)

# ---- tests and tools
enable_testing()
add_executable(test_core tests/test_core.cpp)
target_include_directories(test_core PRIVATE tests)
target_link_libraries(test_core pmcore)
add_test(NAME core COMMAND test_core)

add_executable(platemask_cli tools/platemask_cli.cpp)
target_include_directories(platemask_cli PRIVATE tests)
target_link_libraries(platemask_cli pmcore)

# ---- plugin
set(SUPPORT_DIR ${CMAKE_SOURCE_DIR}/third_party/openfx/Support/Library)
set(SUPPORT_SRC ${SUPPORT_DIR}/ofxsCore.cpp ${SUPPORT_DIR}/ofxsImageEffect.cpp ${SUPPORT_DIR}/ofxsInteract.cpp ${SUPPORT_DIR}/ofxsLog.cpp
                ${SUPPORT_DIR}/ofxsMultiThread.cpp ${SUPPORT_DIR}/ofxsParams.cpp ${SUPPORT_DIR}/ofxsProperty.cpp ${SUPPORT_DIR}/ofxsPropertyValidation.cpp)
set(PLUGIN_SRC src/ofx/PlateMaskPlugin.cpp src/ofx/Monitor_stub.cpp src/ofx/Monitor_win.cpp)
if(APPLE)
  list(APPEND PLUGIN_SRC src/ofx/Monitor.mm)
endif()
add_library(PlateMask MODULE ${PLUGIN_SRC} ${SUPPORT_SRC})
target_include_directories(PlateMask PRIVATE ${OFX_INC} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(PlateMask PRIVATE pmcore)
if(NOT MSVC)
  set_source_files_properties(${SUPPORT_SRC} PROPERTIES COMPILE_OPTIONS "-w")
endif()

set(BUNDLE_DIR ${CMAKE_BINARY_DIR}/PlateMask.ofx.bundle)
if(APPLE)
  set(ARCH_DIR MacOS)
  target_link_libraries(PlateMask PRIVATE "-framework Cocoa")
  set_source_files_properties(src/ofx/Monitor.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
elseif(WIN32)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(ARCH_DIR Win-arm64)
  else()
    set(ARCH_DIR Win64)
  endif()
  target_link_libraries(PlateMask PRIVATE comctl32 user32 gdi32)
else()
  set(ARCH_DIR Linux-x86-64)
endif()
set_target_properties(PlateMask PROPERTIES
  PREFIX "" SUFFIX ".ofx" OUTPUT_NAME PlateMask
  LIBRARY_OUTPUT_DIRECTORY "$<1:${BUNDLE_DIR}/Contents/${ARCH_DIR}>"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${BUNDLE_DIR}/Contents/${ARCH_DIR}>")
if(APPLE)
  add_custom_command(TARGET PlateMask POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/packaging/Info.plist ${BUNDLE_DIR}/Contents/Info.plist)
endif()

# ---- mini host: loads the built plugin binary and drives it like a host would
add_executable(minihost tests/minihost.cpp)
target_include_directories(minihost PRIVATE ${OFX_INC} tests)
target_link_libraries(minihost pmcore ${CMAKE_DL_LIBS})
add_test(NAME plugin_minihost COMMAND minihost $<TARGET_FILE:PlateMask> ${CMAKE_BINARY_DIR}/minihost_out)
