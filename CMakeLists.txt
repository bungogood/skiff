cmake_minimum_required(VERSION 4.0)

project(skiff LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(skiff_core
  src/raft_node.cpp
)

target_include_directories(skiff_core PUBLIC include)
target_compile_options(skiff_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(skiff_node src/main.cpp)
target_link_libraries(skiff_node PRIVATE skiff_core)
target_compile_options(skiff_node PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(skiff_bench src/benchmark_client.cpp)
target_include_directories(skiff_bench PRIVATE include)
target_compile_options(skiff_bench PRIVATE -Wall -Wextra -Wpedantic -Werror)

enable_testing()

add_executable(skiff_smoke_test tests/smoke_test.cpp)
target_link_libraries(skiff_smoke_test PRIVATE skiff_core)
add_test(NAME skiff_smoke_test COMMAND skiff_smoke_test)
