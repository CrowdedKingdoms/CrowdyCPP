add_executable(bench_codec bench_codec.cpp)
target_link_libraries(bench_codec PRIVATE crowdy_wire crowdy_core crowdy_crypto_openssl)

if(NOT WIN32)
  add_executable(bench_e2e_latency bench_e2e_latency.cpp)
  target_link_libraries(bench_e2e_latency PRIVATE crowdy)
  target_include_directories(bench_e2e_latency PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tests/e2e)
endif()
