cmake_minimum_required(VERSION 3.20)
project(PrefixFabric VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(PREFIX_FABRIC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(PREFIX_FABRIC_BUILD_TESTS "Build tests" ON)
option(PREFIX_FABRIC_BUILD_EXAMPLES "Build examples" ON)
option(PREFIX_FABRIC_BUILD_BENCHMARKS "Build benchmarks" ON)
option(PREFIX_FABRIC_BUILD_CLI "Build the CLI" ON)

set(PREFIX_FABRIC_SOURCES
  src/hash.cpp
  src/compatibility.cpp
  src/state_reference.cpp
  src/canonical.cpp
  src/event.cpp
  src/index.cpp
  src/store.cpp
  src/serializer.cpp
  src/persist.cpp
  src/net.cpp
  src/protocol.cpp
  src/coordinator.cpp
  src/node.cpp
  src/client.cpp
  src/explain.cpp
)

add_library(prefix_fabric ${PREFIX_FABRIC_SOURCES})
add_library(prefix_fabric::prefix_fabric ALIAS prefix_fabric)
target_include_directories(prefix_fabric PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_include_directories(prefix_fabric PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(prefix_fabric PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

function(pf_set_warnings tgt)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W4 /EHsc /utf-8)
    if(PREFIX_FABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${tgt} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra)
    if(PREFIX_FABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${tgt} PRIVATE -Werror)
    endif()
  endif()
endfunction()

pf_set_warnings(prefix_fabric)
if(WIN32)
  target_link_libraries(prefix_fabric PUBLIC ws2_32)
endif()

include(GNUInstallDirs)

# ---- CLI ----
if(PREFIX_FABRIC_BUILD_CLI)
  add_executable(prefix-fabric cli/main.cpp)
  target_link_libraries(prefix-fabric PRIVATE prefix_fabric)
  pf_set_warnings(prefix-fabric)
endif()

# ---- tests ----
if(PREFIX_FABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---- examples ----
if(PREFIX_FABRIC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ---- benchmarks ----
if(PREFIX_FABRIC_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---- install ----
install(TARGETS prefix_fabric EXPORT prefix_fabric_targets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS prefix-fabric RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT prefix_fabric_targets FILE prefix_fabricTargets.cmake NAMESPACE prefix_fabric:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/prefix_fabric)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/prefix_fabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/prefix_fabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/prefix_fabricConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/prefix_fabric)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/prefix_fabricConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/prefix_fabricConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/prefix_fabric)
