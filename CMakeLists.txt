cmake_minimum_required(VERSION 3.22)

project(OrderSystem VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "OrderSystem benchmark currently targets Linux only")
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

find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET libpq)

set(NEBULARPC_ROOT "$ENV{HOME}/.vs/NebulaRPC" CACHE PATH "Path to the existing NebulaRPC source repository")
if(NOT EXISTS "${NEBULARPC_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "NebulaRPC source was not found. Configure with -DNEBULARPC_ROOT=/path/to/NebulaRPC")
endif()

# 复用已经完成的 NebulaRPC 框架，订单压测只新增数据库业务，不再复制一套网络层。
add_subdirectory("${NEBULARPC_ROOT}" "${CMAKE_BINARY_DIR}/nebularpc" EXCLUDE_FROM_ALL)

# 子目录配置了自己的输出路径；这里重新指定订单系统程序的输出目录。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)

set(ORDER_DB_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/proto/order_db_benchmark.proto)
protobuf_generate_cpp(ORDER_DB_PROTO_SRCS ORDER_DB_PROTO_HDRS ${ORDER_DB_PROTO})

# 数据库压测请求和响应的 Protobuf 消息。
add_library(order_db_benchmark_proto STATIC
    ${ORDER_DB_PROTO_SRCS}
    ${ORDER_DB_PROTO_HDRS}
)
target_include_directories(order_db_benchmark_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(order_db_benchmark_proto PUBLIC protobuf::libprotobuf)

# 服务端把数据库操作交给固定数量的数据库线程，避免阻塞 RPC 的 EventLoop。
add_executable(order_db_benchmark_server
    benchmarks/order_db/order_db_benchmark_server.cpp
    src/database/db_pool.cpp
)
target_include_directories(order_db_benchmark_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(order_db_benchmark_server PRIVATE nebula_rpc order_db_benchmark_proto PkgConfig::LIBPQ Threads::Threads)

# 客户端沿用 NebulaRPC 的固定并发模型，window 表示每条连接同时保持的在途请求数。
add_executable(order_db_benchmark_client
    benchmarks/order_db/order_db_benchmark_client.cpp
)
target_include_directories(order_db_benchmark_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(order_db_benchmark_client PRIVATE nebula_rpc order_db_benchmark_proto Threads::Threads)

if(ORDERSYSTEM_ENABLE_ASAN)
    target_compile_options(order_db_benchmark_server PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(order_db_benchmark_server PRIVATE -fsanitize=address)
    target_compile_options(order_db_benchmark_client PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(order_db_benchmark_client PRIVATE -fsanitize=address)
endif()

if(ORDERSYSTEM_ENABLE_UBSAN)
    target_compile_options(order_db_benchmark_server PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(order_db_benchmark_server PRIVATE -fsanitize=undefined)
    target_compile_options(order_db_benchmark_client PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(order_db_benchmark_client PRIVATE -fsanitize=undefined)
endif()

enable_testing()
