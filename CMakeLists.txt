cmake_minimum_required(VERSION 3.22)
project(CrowdyCPP VERSION 0.8.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(CROWDY_BUILD_TESTS "Build unit tests" ON)
option(CROWDY_BUILD_E2E "Build env-gated e2e tests" ON)
option(CROWDY_BUILD_BENCHMARKS "Build benchmarks" ON)
option(CROWDY_BUILD_EXAMPLES "Build example programs" ON)
option(CROWDY_WITH_CURL "Build the default libcurl HTTP transport" ON)
option(CROWDY_WITH_OPENSSL "Build the default OpenSSL crypto provider" ON)

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wshadow -Wconversion)
endif()

find_package(Threads REQUIRED)

# ---- vendored yyjson (JSON for the GraphQL layer only; never on the UDP path)
add_library(crowdy_yyjson STATIC third_party/yyjson/yyjson.c)
target_include_directories(crowdy_yyjson PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/yyjson>)
set_target_properties(crowdy_yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- core + wire (no external deps beyond the ICrypto interface)
add_library(crowdy_core STATIC
  src/core/base64.cpp
  src/core/clock.cpp
  src/core/logger.cpp
  src/core/uuid.cpp
)
target_include_directories(crowdy_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(crowdy_core PUBLIC Threads::Threads)

# wire is header-only; provide an interface target for consumers
add_library(crowdy_wire INTERFACE)
target_include_directories(crowdy_wire INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(crowdy_wire INTERFACE crowdy_core)

# ---- default crypto provider (OpenSSL)
if(CROWDY_WITH_OPENSSL)
  find_package(OpenSSL REQUIRED)
  add_library(crowdy_crypto_openssl STATIC src/core/crypto_openssl.cpp)
  target_include_directories(crowdy_crypto_openssl PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
  target_link_libraries(crowdy_crypto_openssl PUBLIC OpenSSL::Crypto crowdy_core)
endif()

# ---- GraphQL core + generated surface + domains
add_library(crowdy_graphql STATIC
  src/graphql/json.cpp
  src/graphql/graphql_client.cpp
  src/graphql/auth_state.cpp
  src/graphql/errors.cpp
)
target_include_directories(crowdy_graphql PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(crowdy_graphql PUBLIC crowdy_core crowdy_yyjson)

if(CROWDY_WITH_CURL)
  find_package(CURL REQUIRED)
  add_library(crowdy_http_curl STATIC src/graphql/http_curl.cpp)
  target_include_directories(crowdy_http_curl PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
  target_link_libraries(crowdy_http_curl PUBLIC CURL::libcurl crowdy_graphql)
endif()

# ---- replication (native UDP client)
add_library(crowdy_replication STATIC
  src/replication/udp_socket.cpp
  src/replication/connection.cpp
)
target_include_directories(crowdy_replication PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(crowdy_replication PUBLIC crowdy_wire crowdy_core)
if(WIN32)
  target_link_libraries(crowdy_replication PUBLIC ws2_32)
endif()

# ---- client facade + domains + session + kit
file(GLOB CROWDY_DOMAIN_SOURCES CONFIGURE_DEPENDS src/domains/*.cpp)
file(GLOB CROWDY_SESSION_SOURCES CONFIGURE_DEPENDS src/session/*.cpp)
file(GLOB CROWDY_KIT_SOURCES CONFIGURE_DEPENDS src/kit/*.cpp)
add_library(crowdy STATIC
  src/client.cpp
  ${CROWDY_DOMAIN_SOURCES}
  ${CROWDY_SESSION_SOURCES}
  ${CROWDY_KIT_SOURCES}
)
target_include_directories(crowdy PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(crowdy PUBLIC crowdy_graphql crowdy_replication)
if(CROWDY_WITH_CURL)
  target_link_libraries(crowdy PUBLIC crowdy_http_curl)
endif()
if(CROWDY_WITH_OPENSSL)
  target_link_libraries(crowdy PUBLIC crowdy_crypto_openssl)
endif()

# ---- tests
if(CROWDY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CROWDY_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(CROWDY_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ---- installation + package config (find_package(CrowdyCPP)) ----------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CROWDY_INSTALL_TARGETS crowdy crowdy_core crowdy_graphql crowdy_replication crowdy_yyjson)
if(CROWDY_WITH_CURL)
  list(APPEND CROWDY_INSTALL_TARGETS crowdy_http_curl)
endif()
if(CROWDY_WITH_OPENSSL)
  list(APPEND CROWDY_INSTALL_TARGETS crowdy_crypto_openssl)
endif()

install(TARGETS ${CROWDY_INSTALL_TARGETS} crowdy_wire
        EXPORT CrowdyCPPTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT CrowdyCPPTargets
        NAMESPACE CrowdyCPP::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrowdyCPP)

configure_package_config_file(
  cmake/CrowdyCPPConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/CrowdyCPPConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrowdyCPP)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/CrowdyCPPConfigVersion.cmake
  COMPATIBILITY SameMajorVersion)
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/CrowdyCPPConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/CrowdyCPPConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrowdyCPP)
