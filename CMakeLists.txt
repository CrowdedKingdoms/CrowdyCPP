cmake_minimum_required(VERSION 3.22)
project(CrowdyCPP VERSION 0.14.0 LANGUAGES C CXX)

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
option(CROWDY_BUILD_PARITY_TOOLS "Build maintainer-only parity tools" OFF)
option(CROWDY_WITH_CURL "Build the default libcurl HTTP transport" ON)
option(CROWDY_WITH_CURL_WEBSOCKETS
       "Build the optional libcurl GraphQL-WebSocket transport when supported" ON)
option(CROWDY_WITH_OPENSSL "Build the default OpenSSL crypto provider" ON)
option(CROWDY_NO_EXCEPTIONS
       "Use non-throwing GraphQL request compatibility paths" OFF)

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wshadow -Wconversion)
endif()

find_package(Threads REQUIRED)

# HTTP requires curl; WebSockets merely use it when a compatible build exists.
if(CROWDY_WITH_CURL)
  find_package(CURL REQUIRED)
elseif(CROWDY_WITH_CURL_WEBSOCKETS)
  find_package(CURL QUIET)
endif()

set(CROWDY_HAS_CURL_WEBSOCKETS OFF)
if(CROWDY_WITH_CURL_WEBSOCKETS AND CURL_FOUND AND
   CURL_VERSION_STRING VERSION_GREATER_EQUAL "7.86.0")
  include(CheckCXXSourceCompiles)
  include(CheckCXXSourceRuns)
  set(_crowdy_required_libraries "${CMAKE_REQUIRED_LIBRARIES}")
  set(CMAKE_REQUIRED_LIBRARIES CURL::libcurl)
  check_cxx_source_compiles([=[
    #include <curl/curl.h>
    #if LIBCURL_VERSION_NUM < 0x075600
    #error libcurl WebSocket API is too old
    #endif
    int main() {
      CURL* handle = nullptr;
      char buffer[1]{};
      size_t count = 0;
      const curl_ws_frame* metadata = nullptr;
      (void)curl_ws_recv(handle, buffer, sizeof(buffer), &count, &metadata);
      (void)curl_ws_send(handle, buffer, sizeof(buffer), &count, 0, CURLWS_TEXT);
      return 0;
    }
  ]=] CROWDY_CURL_WEBSOCKET_APIS_COMPILE)
  set(CROWDY_CURL_WEBSOCKET_PROTOCOLS_AVAILABLE ON)
  if(CROWDY_CURL_WEBSOCKET_APIS_COMPILE AND NOT CMAKE_CROSSCOMPILING)
    check_cxx_source_runs([=[
      #include <curl/curl.h>
      #include <cstring>
      int main() {
        const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
        if (!info || !info->protocols) return 1;
        bool ws = false;
        bool wss = false;
        for (const char* const* protocol = info->protocols; *protocol; ++protocol) {
          ws = ws || std::strcmp(*protocol, "ws") == 0;
          wss = wss || std::strcmp(*protocol, "wss") == 0;
        }
        return ws && wss ? 0 : 1;
      }
    ]=] CROWDY_CURL_WEBSOCKET_PROTOCOLS_RUN)
    set(CROWDY_CURL_WEBSOCKET_PROTOCOLS_AVAILABLE
        ${CROWDY_CURL_WEBSOCKET_PROTOCOLS_RUN})
  endif()
  set(CMAKE_REQUIRED_LIBRARIES "${_crowdy_required_libraries}")
  unset(_crowdy_required_libraries)
  if(CROWDY_CURL_WEBSOCKET_APIS_COMPILE AND
     CROWDY_CURL_WEBSOCKET_PROTOCOLS_AVAILABLE)
    set(CROWDY_HAS_CURL_WEBSOCKETS ON)
  endif()
endif()

if(CROWDY_HAS_CURL_WEBSOCKETS)
  message(STATUS
    "CrowdyCPP: libcurl WebSocket API backend enabled "
    "(runtime ws/wss protocol support is checked by the factory)")
else()
  message(STATUS
    "CrowdyCPP: no compatible default libcurl WebSocket transport; "
    "injected IWebSocketTransport implementations remain available")
endif()

# Embed the committed canonical 28-tool fixture in the native registry. This
# configure step is offline and does not require CrowdyJS or Node.
file(READ
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/parity/fixtures/crowdyjs-agent-tools.v1.json"
  CROWDY_AGENT_FIXTURE_JSON)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
  cmake/agent_fixture.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/crowdy_agent_fixture.hpp"
  @ONLY)

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
target_compile_features(crowdy_core PUBLIC cxx_std_20)
target_link_libraries(crowdy_core PUBLIC Threads::Threads)

# wire is header-only; provide an interface target for consumers
add_library(crowdy_wire INTERFACE)
target_include_directories(crowdy_wire INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(crowdy_wire INTERFACE cxx_std_20)
target_link_libraries(crowdy_wire INTERFACE crowdy_core)

# ---- default crypto provider (OpenSSL)
if(CROWDY_WITH_OPENSSL)
  find_package(OpenSSL REQUIRED)
  add_library(crowdy_crypto_openssl STATIC src/core/crypto_openssl.cpp)
  target_include_directories(crowdy_crypto_openssl PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
  target_compile_features(crowdy_crypto_openssl PUBLIC cxx_std_20)
  target_link_libraries(crowdy_crypto_openssl PUBLIC OpenSSL::Crypto crowdy_core)
endif()

# ---- GraphQL core + generated surface + domains
add_library(crowdy_graphql STATIC
  src/graphql/json.cpp
  src/graphql/graphql_client.cpp
  src/graphql/subscription_client.cpp
  src/graphql/async_transport.cpp
  src/graphql/auth_state.cpp
  src/graphql/errors.cpp
)
if(NOT CROWDY_HAS_CURL_WEBSOCKETS)
  target_sources(crowdy_graphql PRIVATE src/graphql/websocket_none.cpp)
endif()
if(NOT CROWDY_WITH_CURL)
  target_sources(crowdy_graphql PRIVATE src/graphql/http_none.cpp)
endif()
target_include_directories(crowdy_graphql PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(crowdy_graphql PUBLIC cxx_std_20)
target_link_libraries(crowdy_graphql PUBLIC crowdy_core crowdy_yyjson)
if(CROWDY_NO_EXCEPTIONS)
  target_compile_definitions(crowdy_graphql PUBLIC CROWDY_NO_EXCEPTIONS=1)
endif()

if(CROWDY_WITH_CURL)
  add_library(crowdy_http_curl STATIC src/graphql/http_curl.cpp)
  target_include_directories(crowdy_http_curl PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
  target_compile_features(crowdy_http_curl PUBLIC cxx_std_20)
  target_link_libraries(crowdy_http_curl PUBLIC CURL::libcurl crowdy_graphql)
endif()

if(CROWDY_HAS_CURL_WEBSOCKETS)
  add_library(crowdy_websocket_curl STATIC src/graphql/websocket_curl.cpp)
  target_include_directories(crowdy_websocket_curl PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_compile_features(crowdy_websocket_curl PUBLIC cxx_std_20)
  target_link_libraries(crowdy_websocket_curl PUBLIC CURL::libcurl crowdy_graphql)
  target_compile_definitions(crowdy_websocket_curl
    PUBLIC CROWDY_HAS_CURL_WEBSOCKETS=1)
endif()

# ---- replication (native UDP client)
add_library(crowdy_replication STATIC
  src/replication/udp_socket.cpp
  src/replication/connection.cpp
)
target_include_directories(crowdy_replication PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(crowdy_replication PUBLIC cxx_std_20)
target_link_libraries(crowdy_replication PUBLIC crowdy_wire crowdy_core)
if(WIN32)
  target_link_libraries(crowdy_replication PUBLIC ws2_32)
endif()

# ---- client facade + domains + session + kit
file(GLOB CROWDY_DOMAIN_SOURCES CONFIGURE_DEPENDS src/domains/*.cpp)
file(GLOB CROWDY_AGENT_SOURCES CONFIGURE_DEPENDS src/agent/*.cpp)
file(GLOB CROWDY_SESSION_SOURCES CONFIGURE_DEPENDS src/session/*.cpp)
file(GLOB CROWDY_KIT_SOURCES CONFIGURE_DEPENDS src/kit/*.cpp)
file(GLOB CROWDY_PLAYER_HOST_SOURCES CONFIGURE_DEPENDS src/player_host/*.cpp)
add_library(crowdy STATIC
  src/client.cpp
  ${CROWDY_AGENT_SOURCES}
  ${CROWDY_DOMAIN_SOURCES}
  ${CROWDY_SESSION_SOURCES}
  ${CROWDY_KIT_SOURCES}
  ${CROWDY_PLAYER_HOST_SOURCES}
)
target_include_directories(crowdy PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(crowdy PUBLIC cxx_std_20)
target_include_directories(crowdy PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(crowdy PUBLIC crowdy_graphql crowdy_replication)
if(CROWDY_WITH_CURL)
  target_link_libraries(crowdy PUBLIC crowdy_http_curl)
endif()
if(CROWDY_HAS_CURL_WEBSOCKETS)
  target_link_libraries(crowdy PUBLIC crowdy_websocket_curl)
endif()
if(CROWDY_WITH_OPENSSL)
  target_link_libraries(crowdy PUBLIC crowdy_crypto_openssl)
endif()

# ---- maintainer parity tools (never required by external builds)
if(CROWDY_BUILD_PARITY_TOOLS)
  add_executable(crowdy_blueprint_dump tools/parity/dump_blueprints.cpp)
  target_link_libraries(crowdy_blueprint_dump PRIVATE crowdy)
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
if(CROWDY_HAS_CURL_WEBSOCKETS)
  list(APPEND CROWDY_INSTALL_TARGETS crowdy_websocket_curl)
endif()
if(CROWDY_WITH_OPENSSL)
  list(APPEND CROWDY_INSTALL_TARGETS crowdy_crypto_openssl)
endif()

install(TARGETS ${CROWDY_INSTALL_TARGETS} crowdy_wire
        EXPORT CrowdyCPPTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES
        tools/parity/fixtures/crowdyjs-agent-tools.v1.json
        tools/parity/fixtures/crowdyjs-descriptor-digests.v1.json
        tools/parity/fixtures/crowdyjs-preemption-reasons.v1.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/crowdy/agent)
install(EXPORT CrowdyCPPTargets
        NAMESPACE CrowdyCPP::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrowdyCPP)

set(CROWDY_CONFIG_NEEDS_CURL OFF)
if(CROWDY_WITH_CURL OR CROWDY_HAS_CURL_WEBSOCKETS)
  set(CROWDY_CONFIG_NEEDS_CURL ON)
endif()
set(CROWDY_CONFIG_NEEDS_OPENSSL ${CROWDY_WITH_OPENSSL})
configure_package_config_file(
  cmake/CrowdyCPPConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/CrowdyCPPConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrowdyCPP)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/CrowdyCPPConfigVersion.cmake
  # CrowdyCPP is pre-1.0: patch releases preserve source/ABI compatibility,
  # while each minor may break it. Do not accept every 0.x package.
  COMPATIBILITY SameMinorVersion)
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/CrowdyCPPConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/CrowdyCPPConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrowdyCPP)
