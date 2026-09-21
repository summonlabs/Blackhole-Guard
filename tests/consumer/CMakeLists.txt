cmake_minimum_required(VERSION 3.20)

# Independent downstream consumer project. It is deliberately built OUT OF TREE
# against an installed prefix and never references the source tree.
project(BlackholeGuardConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(BlackholeGuard CONFIG REQUIRED)

add_executable(consumer_main main.cpp)
target_link_libraries(consumer_main PRIVATE BlackholeGuard::blackhole_guard)

if(MSVC)
  target_compile_options(consumer_main PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /WX)
else()
  target_compile_options(consumer_main PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
