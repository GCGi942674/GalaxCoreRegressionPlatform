cmake_minimum_required(VERSION 3.15.2)

project(GalaxCoreRegressionPlatform LANGUAGES CXX)

# =====================
# C++ 标准
# =====================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =====================
# 构建类型（默认 Release）
# =====================
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# =====================
# 输出路径
# =====================
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Parallelism is controlled by make PARALLEL_JOBS or cmake --build --parallel.
# CMAKE_BUILD_PARALLEL_LEVEL is an environment variable, not a cache variable.

# =====================
# Debug 编译选项
# =====================
# Use CMake's standard per-configuration compiler flags.

# =====================
# 子模块
# =====================
add_subdirectory(common)
add_subdirectory(server)
add_subdirectory(client)
set(GCRP_BUILD_QT_GUI "AUTO" CACHE STRING "Qt5 GUI: AUTO, ON (required), OFF")
set_property(CACHE GCRP_BUILD_QT_GUI PROPERTY STRINGS AUTO ON OFF)
if(NOT GCRP_BUILD_QT_GUI MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "GCRP_BUILD_QT_GUI must be AUTO, ON or OFF")
endif()
if(GCRP_BUILD_QT_GUI STREQUAL "AUTO")
    find_package(Qt5 5.8 QUIET COMPONENTS Core Widgets Network)
endif()
if(GCRP_BUILD_QT_GUI STREQUAL "ON" OR
    (GCRP_BUILD_QT_GUI STREQUAL "AUTO" AND Qt5_FOUND))
    message(STATUS "Qt5 GUI enabled: build/bin/gcrp_gui")
    add_subdirectory(gui/qt)
elseif(GCRP_BUILD_QT_GUI STREQUAL "AUTO")
    message(STATUS "Qt5 (5.8+) Widgets/Network not found: skipping GUI; use GCRP_BUILD_QT_GUI=ON to require it")
else()
    message(STATUS "Qt5 GUI disabled explicitly")
endif()

option(GCRP_BUILD_TIMEOUT_TESTS "Build isolated adaptive timeout tests" OFF)
if(GCRP_BUILD_TIMEOUT_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
