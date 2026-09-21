cmake_minimum_required(VERSION 3.20)

project(BlackholeGuard
  VERSION 1.0.1
  DESCRIPTION "Delivery blackhole detection, localization and bounded fencing intent"
  HOMEPAGE_URL "https://github.com/summonlabs/Blackhole-Guard"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(BHG_BUILD_TESTS "Build the Blackhole Guard test suites" ON)
option(BHG_BUILD_TOOLS "Build the Blackhole Guard command line tools" ON)
option(BHG_BUILD_EXAMPLES "Build the Blackhole Guard examples" ON)
option(BHG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(BHG_ENABLE_ASAN "Enable AddressSanitizer when the toolchain supports it" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  if(BHG_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  if(BHG_ENABLE_ASAN)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  if(BHG_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
  if(BHG_ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

find_package(Threads REQUIRED)

set(BHG_SOURCES
  src/core/hash.cpp
  src/core/canonical.cpp
  src/core/time.cpp
  src/core/ids.cpp
  src/core/nonce.cpp
  src/core/path.cpp
  src/domain/policy.cpp
  src/domain/evidence.cpp
  src/evidence/ledger.cpp
  src/diagnosis/classifier.cpp
  src/authority/authority.cpp
  src/localize/localizer.cpp
  src/persist/record.cpp
  src/persist/journal.cpp
  src/persist/snapshot.cpp
  src/persist/store.cpp
  src/protocol/frame.cpp
  src/protocol/messages.cpp
  src/protocol/session.cpp
  src/service/engine.cpp
  src/service/net.cpp
  src/service/server.cpp
  src/service/client.cpp
)

add_library(blackhole_guard ${BHG_SOURCES})
add_library(BlackholeGuard::blackhole_guard ALIAS blackhole_guard)

target_include_directories(blackhole_guard
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(blackhole_guard PUBLIC Threads::Threads)

if(WIN32)
  # Winsock2 is a system library; it is part of the public link interface because
  # the service layer is built into the same static library.
  target_link_libraries(blackhole_guard PUBLIC ws2_32)
endif()

set_target_properties(blackhole_guard PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME blackhole_guard)

if(BHG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(BHG_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(BHG_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

install(TARGETS blackhole_guard
  EXPORT BlackholeGuardTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT BlackholeGuardTargets
  FILE BlackholeGuardTargets.cmake
  NAMESPACE BlackholeGuard::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BlackholeGuard)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BlackholeGuardConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/BlackholeGuardConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BlackholeGuard)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/BlackholeGuardConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/BlackholeGuardConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/BlackholeGuardConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BlackholeGuard)
