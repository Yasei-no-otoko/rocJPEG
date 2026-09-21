# Copyright (c) 2026 rocJPEG Windows contributors.
# SPDX-License-Identifier: MIT

# Windows builds use the driver-provided AMF decoder and the selected ROCm SDK.
# Keep this path separate from the upstream Linux VA-API build.
if(NOT ROCM_PATH)
  if(DEFINED ENV{ROCM_PATH} AND NOT "$ENV{ROCM_PATH}" STREQUAL "")
    set(ROCM_PATH "$ENV{ROCM_PATH}" CACHE PATH "Windows ROCm SDK root")
  elseif(DEFINED ENV{HIP_PATH} AND NOT "$ENV{HIP_PATH}" STREQUAL "")
    set(ROCM_PATH "$ENV{HIP_PATH}" CACHE PATH "Windows ROCm SDK root")
  else()
    message(FATAL_ERROR "Set ROCM_PATH to the Windows ROCm SDK root (for wheels, _rocm_sdk_core).")
  endif()
endif()
cmake_path(ABSOLUTE_PATH ROCM_PATH NORMALIZE OUTPUT_VARIABLE _rocjpeg_rocm_root)
if(NOT EXISTS "${_rocjpeg_rocm_root}/include/hip/hip_runtime.h")
  message(FATAL_ERROR "ROCM_PATH does not contain include/hip/hip_runtime.h: ${_rocjpeg_rocm_root}")
endif()

if(NOT CMAKE_CXX_COMPILER_LOADED AND NOT DEFINED CMAKE_CXX_COMPILER)
  find_program(_rocjpeg_clang NAMES clang++ amdclang++
    PATHS "${_rocjpeg_rocm_root}/lib/llvm/bin" "${_rocjpeg_rocm_root}/bin"
    NO_DEFAULT_PATH REQUIRED)
  set(CMAKE_CXX_COMPILER "${_rocjpeg_clang}" CACHE FILEPATH "ROCm C++ compiler")
endif()
project(rocjpeg VERSION 1.10.0 LANGUAGES CXX)
include(GNUInstallDirs)
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC" OR
   NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
  message(FATAL_ERROR "Windows rocJPEG requires ROCm clang++.exe (GNU frontend, MSVC ABI), not cl.exe or clang-cl.exe.")
endif()
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

get_filename_component(_rocjpeg_sdk_parent "${_rocjpeg_rocm_root}" DIRECTORY)
set(ROCJPEG_ROCM_DEVEL_PATH "" CACHE PATH "Optional split ROCm development SDK root")
if(NOT ROCJPEG_ROCM_DEVEL_PATH AND EXISTS "${_rocjpeg_sdk_parent}/_rocm_sdk_devel/include")
  set(ROCJPEG_ROCM_DEVEL_PATH "${_rocjpeg_sdk_parent}/_rocm_sdk_devel")
endif()
set(_rocjpeg_hip_include_dirs "${_rocjpeg_rocm_root}/include")
if(ROCJPEG_ROCM_DEVEL_PATH)
  list(APPEND _rocjpeg_hip_include_dirs "${ROCJPEG_ROCM_DEVEL_PATH}/include")
endif()

set(ROCJPEG_HIP_DEVICE_LIB_PATH "${_rocjpeg_rocm_root}/lib/llvm/amdgcn/bitcode"
    CACHE PATH "ROCm HIP device bitcode directory")
if(NOT EXISTS "${ROCJPEG_HIP_DEVICE_LIB_PATH}/hip.bc")
  message(FATAL_ERROR "HIP device bitcode not found; set ROCJPEG_HIP_DEVICE_LIB_PATH.")
endif()
if(NOT GPU_TARGETS)
  if(HIP_ARCHITECTURES)
    set(_rocjpeg_gpu_targets "${HIP_ARCHITECTURES}")
  elseif(DEFINED ENV{GPU_ARCHS} AND NOT "$ENV{GPU_ARCHS}" STREQUAL "")
    set(_rocjpeg_gpu_targets "$ENV{GPU_ARCHS}")
  else()
    set(_rocjpeg_gpu_targets native)
  endif()
  set(GPU_TARGETS "${_rocjpeg_gpu_targets}" CACHE STRING "AMDGPU architectures for HIP conversion kernels")
endif()

set(AMF_ROOT "" CACHE PATH "AMF source checkout; empty fetches the pinned official headers")
if(NOT AMF_ROOT)
  include(FetchContent)
  FetchContent_Declare(rocjpeg_amf
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git
    GIT_TAG d0b3e6dd544a5f207bb6a12a1ecb98532491176a
    GIT_PROGRESS TRUE)
  FetchContent_MakeAvailable(rocjpeg_amf)
  set(AMF_ROOT "${rocjpeg_amf_SOURCE_DIR}")
endif()
if(EXISTS "${AMF_ROOT}/amf/public/include/core/Factory.h")
  set(_rocjpeg_amf_include "${AMF_ROOT}/amf/public/include")
elseif(EXISTS "${AMF_ROOT}/public/include/core/Factory.h")
  set(_rocjpeg_amf_include "${AMF_ROOT}/public/include")
else()
  message(FATAL_ERROR "AMF_ROOT must contain amf/public/include/core/Factory.h (or public/include/core/Factory.h).")
endif()

unset(ROCJPEG_HIP_RUNTIME_LIBRARY CACHE)
find_library(ROCJPEG_HIP_RUNTIME_LIBRARY NAMES amdhip64
  PATHS "${_rocjpeg_rocm_root}/lib" NO_DEFAULT_PATH REQUIRED)
mark_as_advanced(ROCJPEG_HIP_RUNTIME_LIBRARY)

set(_rocjpeg_kernel "${CMAKE_CURRENT_SOURCE_DIR}/src/windows/rocjpeg_windows_kernels.hip.cpp")
set_property(SOURCE "${_rocjpeg_kernel}" APPEND PROPERTY COMPILE_OPTIONS
  -x hip
  "--rocm-path=${_rocjpeg_rocm_root}"
  "--rocm-device-lib-path=${ROCJPEG_HIP_DEVICE_LIB_PATH}")
foreach(_rocjpeg_arch IN LISTS GPU_TARGETS)
  set_property(SOURCE "${_rocjpeg_kernel}" APPEND PROPERTY COMPILE_OPTIONS
    "--offload-arch=${_rocjpeg_arch}")
endforeach()

add_library(rocjpeg SHARED
  src/windows/rocjpeg_windows.cpp
  "${_rocjpeg_kernel}")
add_library(rocjpeg::rocjpeg ALIAS rocjpeg)
target_compile_features(rocjpeg PRIVATE cxx_std_20)
set_target_properties(rocjpeg PROPERTIES
  CXX_SCAN_FOR_MODULES OFF
  VERSION "${PROJECT_VERSION}"
  SOVERSION "${PROJECT_VERSION_MAJOR}")
target_compile_definitions(rocjpeg
  PUBLIC __HIP_PLATFORM_AMD__
  PRIVATE ROCJPEG_EXPORTS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_include_directories(rocjpeg
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/api>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${AMF_ROOT}"
    "${_rocjpeg_amf_include}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/windows")
foreach(_rocjpeg_include IN LISTS _rocjpeg_hip_include_dirs)
  target_include_directories(rocjpeg PUBLIC "$<BUILD_INTERFACE:${_rocjpeg_include}>")
endforeach()
target_link_libraries(rocjpeg PRIVATE "${ROCJPEG_HIP_RUNTIME_LIBRARY}" d3d11 d3d12 dxgi dxguid)

# The AMD driver can also place an older amdhip64_7.dll in System32. PATH alone
# does not override it: keep the selected SDK runtime beside build-tree binaries.
find_file(_rocjpeg_hip_dll NAMES amdhip64_7.dll amdhip64.dll
  PATHS "${_rocjpeg_rocm_root}/bin" NO_DEFAULT_PATH REQUIRED NO_CACHE)
set(_rocjpeg_runtime_files "${_rocjpeg_hip_dll}")
file(GLOB _rocjpeg_runtime_companions
  "${_rocjpeg_rocm_root}/bin/amd_comgr*.dll"
  "${_rocjpeg_rocm_root}/bin/hiprtc*.dll"
  "${_rocjpeg_rocm_root}/bin/rocm_kpack.dll")
list(APPEND _rocjpeg_runtime_files ${_rocjpeg_runtime_companions})
set_property(TARGET rocjpeg PROPERTY ROCJPEG_HIP_RUNTIME_FILES "${_rocjpeg_runtime_files}")
add_custom_command(TARGET rocjpeg POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_rocjpeg_runtime_files}
    "$<TARGET_FILE_DIR:rocjpeg>"
  COMMENT "Staging the selected Windows HIP runtime"
  VERBATIM)

file(GLOB _rocjpeg_builtins
  "${_rocjpeg_rocm_root}/lib/llvm/lib/clang/*/lib/windows/clang_rt.builtins-x86_64.lib")
if(_rocjpeg_builtins)
  list(SORT _rocjpeg_builtins COMPARE NATURAL ORDER DESCENDING)
  list(GET _rocjpeg_builtins 0 _rocjpeg_builtins_library)
  target_link_libraries(rocjpeg PRIVATE "${_rocjpeg_builtins_library}")
endif()

option(ROCJPEG_BUILD_WINDOWS_TESTS "Build Windows API and HIP kernel tests" OFF)
if(ROCJPEG_BUILD_WINDOWS_TESTS)
  set(_rocjpeg_kernel_test "${CMAKE_CURRENT_SOURCE_DIR}/test/windows/rocjpeg_windows_kernels_test.hip.cpp")
  get_source_file_property(_rocjpeg_hip_options "${_rocjpeg_kernel}" COMPILE_OPTIONS)
  set_property(SOURCE "${_rocjpeg_kernel_test}" PROPERTY COMPILE_OPTIONS "${_rocjpeg_hip_options}")
  add_executable(rocjpeg_windows_kernels_test "${_rocjpeg_kernel_test}" "${_rocjpeg_kernel}")
  target_compile_features(rocjpeg_windows_kernels_test PRIVATE cxx_std_20)
  set_target_properties(rocjpeg_windows_kernels_test PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  target_compile_definitions(rocjpeg_windows_kernels_test PRIVATE __HIP_PLATFORM_AMD__ NOMINMAX)
  target_include_directories(rocjpeg_windows_kernels_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/api" "${CMAKE_CURRENT_SOURCE_DIR}/src/windows"
    ${_rocjpeg_hip_include_dirs})
  target_link_libraries(rocjpeg_windows_kernels_test PRIVATE "${ROCJPEG_HIP_RUNTIME_LIBRARY}")
  if(_rocjpeg_builtins_library)
    target_link_libraries(rocjpeg_windows_kernels_test PRIVATE "${_rocjpeg_builtins_library}")
  endif()
  add_custom_command(TARGET rocjpeg_windows_kernels_test POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_rocjpeg_runtime_files}
      "$<TARGET_FILE_DIR:rocjpeg_windows_kernels_test>"
    VERBATIM)
  enable_testing()
  add_test(NAME rocjpeg_windows_kernels_test COMMAND rocjpeg_windows_kernels_test)
  set_tests_properties(rocjpeg_windows_kernels_test PROPERTIES TIMEOUT 60)

  add_executable(rocjpeg_windows_api_test
    "${CMAKE_CURRENT_SOURCE_DIR}/test/windows/rocjpeg_windows_api_test.cpp")
  target_compile_features(rocjpeg_windows_api_test PRIVATE cxx_std_20)
  set_target_properties(rocjpeg_windows_api_test PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  target_link_libraries(rocjpeg_windows_api_test PRIVATE rocjpeg::rocjpeg "${ROCJPEG_HIP_RUNTIME_LIBRARY}")
  add_custom_command(TARGET rocjpeg_windows_api_test POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_rocjpeg_runtime_files}
      "$<TARGET_FILE_DIR:rocjpeg_windows_api_test>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:rocjpeg>"
      "$<TARGET_FILE_DIR:rocjpeg_windows_api_test>"
    VERBATIM)
  add_test(NAME rocjpeg_windows_api_test COMMAND rocjpeg_windows_api_test)
  add_test(NAME rocjpeg_windows_api_decode_test COMMAND rocjpeg_windows_api_test
    "${CMAKE_CURRENT_SOURCE_DIR}/data/images/mug_420.jpg")
  add_test(NAME rocjpeg_windows_api_gray_test COMMAND rocjpeg_windows_api_test
    "${CMAKE_CURRENT_SOURCE_DIR}/data/images/mug_400.jpg")
  add_test(NAME rocjpeg_windows_api_422_test COMMAND rocjpeg_windows_api_test
    "${CMAKE_CURRENT_SOURCE_DIR}/data/images/mug_422.jpg")
  add_test(NAME rocjpeg_windows_api_mixed_test COMMAND rocjpeg_windows_api_test
    "${CMAKE_CURRENT_SOURCE_DIR}/data/images/mug_420.jpg"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/images/mug_422.jpg"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/images/mug_400.jpg"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/images/mug_420.jpg")
  add_test(NAME rocjpeg_windows_api_unsupported_test COMMAND rocjpeg_windows_api_test
    --unsupported "${CMAKE_CURRENT_SOURCE_DIR}/test/windows/baseline_444.jpg"
    "${CMAKE_CURRENT_SOURCE_DIR}/test/windows/baseline_440.jpg"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/images/mug_420.jpg")
  set_tests_properties(rocjpeg_windows_api_test rocjpeg_windows_api_decode_test
    rocjpeg_windows_api_gray_test rocjpeg_windows_api_422_test rocjpeg_windows_api_mixed_test
    rocjpeg_windows_api_unsupported_test
    PROPERTIES TIMEOUT 60)
endif()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/api/rocjpeg_version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/include/rocjpeg/rocjpeg_version.h" @ONLY)
include(CMakePackageConfigHelpers)
install(TARGETS rocjpeg EXPORT rocjpeg-targets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/api/rocjpeg/rocjpeg.h"
  "${CMAKE_CURRENT_BINARY_DIR}/include/rocjpeg/rocjpeg_version.h"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/rocjpeg")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/rocjpeg")
if(EXISTS "${AMF_ROOT}/LICENSE.txt")
  install(FILES "${AMF_ROOT}/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/rocjpeg" RENAME AMF-LICENSE.txt)
endif()

# Only rocjpeg's DLL/import library and headers are installed. The application
# supplies its selected HIP runtime; AMF is loaded from the installed AMD driver.
set(_rocjpeg_config [=[
find_path(rocjpeg_HIP_INCLUDE_DIR hip/hip_runtime.h
  HINTS "${ROCM_PATH}" "${HIP_PATH}" "$ENV{ROCM_PATH}" "$ENV{HIP_PATH}"
  PATH_SUFFIXES include)
if(NOT rocjpeg_HIP_INCLUDE_DIR)
  set(rocjpeg_FOUND FALSE)
  set(rocjpeg_NOT_FOUND_MESSAGE "Set ROCM_PATH to the Windows ROCm SDK so rocjpeg can find HIP headers.")
  return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/rocjpeg-targets.cmake")
set_property(TARGET rocjpeg::rocjpeg APPEND PROPERTY
  INTERFACE_INCLUDE_DIRECTORIES "${rocjpeg_HIP_INCLUDE_DIR}")
]=])
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rocjpeg-config.cmake"
  CONTENT "${_rocjpeg_config}" @ONLY)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/rocjpeg-config-version.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/rocjpeg-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/rocjpeg-config-version.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rocjpeg")
install(EXPORT rocjpeg-targets FILE rocjpeg-targets.cmake
  NAMESPACE rocjpeg:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rocjpeg")

message(STATUS "rocJPEG Windows backend: AMF (driver runtime), HIP ${GPU_TARGETS}")
message(STATUS "rocJPEG ROCm SDK: ${_rocjpeg_rocm_root}")
message(STATUS "rocJPEG AMF headers: ${_rocjpeg_amf_include}")
