cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
  elseif(EXISTS "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
  endif()
endif()

project(Ferryman LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FERRYMAN_USING_VCPKG_TOOLCHAIN OFF)
if(DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg\\.cmake$")
  set(FERRYMAN_USING_VCPKG_TOOLCHAIN ON)
endif()

if(WIN32 AND MSVC AND DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(WIN32 AND FERRYMAN_USING_VCPKG_TOOLCHAIN)
  if(DEFINED VCPKG_TARGET_TRIPLET)
    if(VCPKG_TARGET_TRIPLET MATCHES "-static")
      message(STATUS "Windows static vcpkg triplet detected: ${VCPKG_TARGET_TRIPLET}")
    else()
      message(WARNING
        "VCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET} uses dynamic third-party libraries. "
        "Use x64-windows-static to avoid runtime DLL dependencies.")
    endif()
  else()
    message(WARNING
      "VCPKG_TARGET_TRIPLET is not set for Windows build. "
      "Use -DVCPKG_TARGET_TRIPLET=x64-windows-static to avoid runtime DLL dependencies.")
  endif()
endif()

if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 20)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

option(FERRYMAN_BUILD_FRONTEND "Build frontend with npm before embedding assets" OFF)

set(FERRYMAN_FRONTEND_DIR "${CMAKE_SOURCE_DIR}/frontend")
set(FERRYMAN_EMBED_CPP "${CMAKE_BINARY_DIR}/generated/EmbeddedAssets.cpp")
set(FERRYMAN_EMBED_STAMP "${CMAKE_BINARY_DIR}/generated/EmbeddedAssets.stamp")
file(GLOB_RECURSE FERRYMAN_FRONTEND_ASSETS CONFIGURE_DEPENDS "${FERRYMAN_FRONTEND_DIR}/dist/*")

if(FERRYMAN_BUILD_FRONTEND)
  find_program(NPM_EXECUTABLE npm)
  if(NPM_EXECUTABLE)
    add_custom_target(frontend_build
      COMMAND ${NPM_EXECUTABLE} install
      COMMAND ${NPM_EXECUTABLE} run build
      WORKING_DIRECTORY ${FERRYMAN_FRONTEND_DIR}
      COMMENT "Building frontend with npm"
      VERBATIM
    )
  else()
    message(WARNING "FERRYMAN_BUILD_FRONTEND is ON but npm was not found; skipping frontend build")
  endif()
endif()

add_custom_command(
  OUTPUT ${FERRYMAN_EMBED_STAMP}
  BYPRODUCTS ${FERRYMAN_EMBED_CPP}
  COMMAND ${CMAKE_COMMAND}
          -DASSET_DIR=${FERRYMAN_FRONTEND_DIR}/dist
          -DOUT_CPP=${FERRYMAN_EMBED_CPP}
          -P ${CMAKE_SOURCE_DIR}/cmake/EmbedAssets.cmake
  COMMAND ${CMAKE_COMMAND} -E touch ${FERRYMAN_EMBED_STAMP}
  DEPENDS ${CMAKE_SOURCE_DIR}/cmake/EmbedAssets.cmake ${FERRYMAN_FRONTEND_ASSETS}
  COMMENT "Embedding frontend assets"
  VERBATIM
)

add_custom_target(embed_assets DEPENDS ${FERRYMAN_EMBED_STAMP})
if(TARGET frontend_build)
  add_dependencies(embed_assets frontend_build)
endif()

file(GLOB_RECURSE FERRYMAN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
if(APPLE)
  file(GLOB_RECURSE FERRYMAN_OBJC_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.mm")
  list(APPEND FERRYMAN_SOURCES ${FERRYMAN_OBJC_SOURCES})
endif()
list(APPEND FERRYMAN_SOURCES ${FERRYMAN_EMBED_CPP})

add_executable(Ferryman ${FERRYMAN_SOURCES})
add_dependencies(Ferryman embed_assets)

target_include_directories(Ferryman
  PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(Ferryman PRIVATE Threads::Threads)

set(FERRYMAN_WITH_FFMPEG 0)
find_package(FFMPEG QUIET)
if(FFMPEG_FOUND)
  target_include_directories(Ferryman PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_link_libraries(Ferryman PRIVATE ${FFMPEG_LIBRARIES})
  set(FERRYMAN_WITH_FFMPEG 1)
  message(STATUS "Using ffmpeg libraries for native screen encoding")
else()
  message(WARNING "FFMPEG not found; native h264 screen encoding will be disabled")
endif()

find_package(nlohmann_json CONFIG QUIET)
if(nlohmann_json_FOUND)
  target_link_libraries(Ferryman PRIVATE nlohmann_json::nlohmann_json)
  message(STATUS "Using nlohmann_json via CMake package")
else()
  find_path(NLOHMANN_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp)
  if(NLOHMANN_JSON_INCLUDE_DIR)
    target_include_directories(Ferryman PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
    message(STATUS "Using nlohmann/json header from ${NLOHMANN_JSON_INCLUDE_DIR}")
  else()
    message(FATAL_ERROR "nlohmann/json.hpp not found. Install nlohmann-json via vcpkg or system package manager.")
  endif()
endif()

set(FERRYMAN_WITH_LIBHV 0)
set(LIBHV_TARGET "")

set(FERRYMAN_LOCAL_LIBHV_CONFIG "${CMAKE_SOURCE_DIR}/vcpkg_installed/arm64-osx/share/libhv")
find_package(libhv CONFIG QUIET PATHS "${FERRYMAN_LOCAL_LIBHV_CONFIG}" NO_DEFAULT_PATH)
if(NOT libhv_FOUND)
  find_package(libhv CONFIG QUIET)
endif()

foreach(candidate IN ITEMS hv::hv libhv::libhv libhv::hv hv libhv hv_static)
  if(TARGET ${candidate})
    set(LIBHV_TARGET ${candidate})
    break()
  endif()
endforeach()

if(LIBHV_TARGET)
  set(FERRYMAN_WITH_LIBHV 1)
  target_link_libraries(Ferryman PRIVATE ${LIBHV_TARGET})
  message(STATUS "Using libhv CMake package target: ${LIBHV_TARGET}")
endif()

if(NOT FERRYMAN_WITH_LIBHV)
  find_path(LIBHV_INCLUDE_DIR NAMES hv/HttpServer.h)
  find_library(LIBHV_LIBRARY NAMES hv libhv)

  if(LIBHV_INCLUDE_DIR AND LIBHV_LIBRARY)
    set(FERRYMAN_WITH_LIBHV 1)
    target_include_directories(Ferryman PRIVATE ${LIBHV_INCLUDE_DIR})
    target_link_libraries(Ferryman PRIVATE ${LIBHV_LIBRARY})
    message(STATUS "Using system libhv: ${LIBHV_LIBRARY}")
  elseif(EXISTS "${CMAKE_SOURCE_DIR}/third_party/libhv/CMakeLists.txt")
    add_subdirectory(third_party/libhv)
    if(TARGET hv)
      set(FERRYMAN_WITH_LIBHV 1)
      target_link_libraries(Ferryman PRIVATE hv)
      message(STATUS "Using vendored libhv target: hv")
    elseif(TARGET libhv)
      set(FERRYMAN_WITH_LIBHV 1)
      target_link_libraries(Ferryman PRIVATE libhv)
      message(STATUS "Using vendored libhv target: libhv")
    endif()
  endif()
endif()

if(APPLE)
  if(FERRYMAN_OBJC_SOURCES)
    set_source_files_properties(${FERRYMAN_OBJC_SOURCES} PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  endif()
  target_link_libraries(Ferryman PRIVATE
    "-framework ApplicationServices"
    "-framework CoreFoundation"
    "-framework CoreImage"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework Foundation"
    "-framework ImageIO"
    "-framework IOKit"
    "-framework ScreenCaptureKit"
  )
endif()

if(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)
  target_include_directories(Ferryman PRIVATE ${X11_INCLUDE_DIR})
  target_link_libraries(Ferryman PRIVATE ${X11_LIBRARIES} ${X11_Xtst_LIB})
endif()

if(WIN32)
  target_link_libraries(Ferryman PRIVATE user32 gdi32)
endif()

if(FERRYMAN_WITH_LIBHV)
  target_compile_definitions(Ferryman PRIVATE FERRYMAN_WITH_LIBHV=1 FERRYMAN_WITH_FFMPEG=${FERRYMAN_WITH_FFMPEG})
else()
  target_compile_definitions(Ferryman PRIVATE FERRYMAN_WITH_LIBHV=0 FERRYMAN_WITH_FFMPEG=${FERRYMAN_WITH_FFMPEG})
  message(WARNING "libhv not found. Binary will compile without network runtime. Install libhv or add third_party/libhv.")
endif()
