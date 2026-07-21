add_executable(wire_test wire_test.cpp)
target_link_libraries(wire_test PRIVATE crowdy_wire crowdy_core crowdy_crypto_openssl)
add_test(NAME wire_test COMMAND wire_test)

add_executable(core_test core_test.cpp)
target_link_libraries(core_test PRIVATE crowdy_core crowdy_crypto_openssl)
add_test(NAME core_test COMMAND core_test)

add_executable(json_test json_test.cpp)
target_link_libraries(json_test PRIVATE crowdy_graphql)
add_test(NAME json_test COMMAND json_test)

add_executable(graphql_client_test graphql_client_test.cpp)
target_link_libraries(graphql_client_test PRIVATE crowdy_graphql)
add_test(NAME graphql_client_test COMMAND graphql_client_test)

add_executable(player_runtime_surface_test player_runtime_surface_test.cpp)
target_link_libraries(player_runtime_surface_test PRIVATE crowdy)
add_test(NAME player_runtime_surface_test COMMAND player_runtime_surface_test)

add_executable(codec_struct_test codec_struct_test.cpp)
target_link_libraries(codec_struct_test PRIVATE crowdy_core)
add_test(NAME codec_struct_test COMMAND codec_struct_test)

add_executable(kit_test kit_test.cpp)
target_link_libraries(kit_test PRIVATE crowdy)
add_test(NAME kit_test COMMAND kit_test)

add_executable(kit_wire_test kit_wire_test.cpp)
target_link_libraries(kit_wire_test PRIVATE crowdy)
add_test(NAME kit_wire_test COMMAND kit_wire_test)

if(NOT WIN32)
  add_executable(replication_test replication_test.cpp)
  target_link_libraries(replication_test PRIVATE crowdy_replication crowdy_crypto_openssl)
  add_test(NAME replication_test COMMAND replication_test)

  add_executable(session_test session_test.cpp)
  target_link_libraries(session_test PRIVATE crowdy)
  add_test(NAME session_test COMMAND session_test)
endif()

if(CROWDY_BUILD_E2E)
  add_subdirectory(e2e)
endif()
