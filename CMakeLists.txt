cmake_minimum_required(VERSION 3.22)

project(OrderSystem VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "OrderSystem server baseline currently targets Linux only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)

option(ORDERSYSTEM_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ORDERSYSTEM_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

add_library(ordersystem_build_options INTERFACE)
target_compile_options(ordersystem_build_options INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
)

if(ORDERSYSTEM_ENABLE_ASAN)
    target_compile_options(ordersystem_build_options INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(ordersystem_build_options INTERFACE -fsanitize=address)
endif()

if(ORDERSYSTEM_ENABLE_UBSAN)
    target_compile_options(ordersystem_build_options INTERFACE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(ordersystem_build_options INTERFACE -fsanitize=undefined)
endif()

# 目前只创建项目骨架，源码目录暂时为空；添加实际代码后再接入对应子目录和测试目标。
enable_testing()
