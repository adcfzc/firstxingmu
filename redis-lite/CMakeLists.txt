cmake_minimum_required(VERSION 3.10)
project(redis_lite CXX)

# ---------------------------------------------------------------- 全局设置
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------- 平台依赖
if(WIN32)
  # Windows 下需要 winsock 库；本项目在 Windows 上走 WSAPoll 后端。
  set(RL_PLATFORM_LIBS ws2_32)
else()
  find_package(Threads REQUIRED)
  set(RL_PLATFORM_LIBS Threads::Threads)
endif()

# ---------------------------------------------------------------- 核心库
# 把除 main 之外的所有实现打成静态库，供服务端、压测工具、单元测试复用。
file(GLOB RL_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM RL_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cc)

add_library(rlcore STATIC ${RL_CORE_SOURCES})
target_include_directories(rlcore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(rlcore PUBLIC ${RL_PLATFORM_LIBS})

if(MSVC)
  target_compile_options(rlcore PRIVATE /W4 /utf-8)
else()
  target_compile_options(rlcore PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
  )
endif()

# ---------------------------------------------------------------- 服务端
add_executable(redis-lite ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cc)
target_link_libraries(redis-lite PRIVATE rlcore)

# ---------------------------------------------------------------- 压测工具
add_executable(rl-bench ${CMAKE_CURRENT_SOURCE_DIR}/tools/bench.cc)
target_link_libraries(rl-bench PRIVATE rlcore)

# ---------------------------------------------------------------- 单元测试
file(GLOB RL_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cc)
add_executable(rl-tests ${RL_TEST_SOURCES})
target_include_directories(rl-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(rl-tests PRIVATE rlcore)

enable_testing()
add_test(NAME unit COMMAND rl-tests)

# ---------------------------------------------------------------- 说明
message(STATUS "redis-lite build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "redis-lite core files : ${RL_CORE_SOURCES}")
if(WIN32)
  message(STATUS "redis-lite io backend  : WSAPoll (level-triggered)")
else()
  message(STATUS "redis-lite io backend  : epoll (edge-triggered)")
endif()
