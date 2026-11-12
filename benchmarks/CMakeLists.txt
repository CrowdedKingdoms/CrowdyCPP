if(TARGET crowdy_crypto_openssl)
  add_executable(bench_codec bench_codec.cpp)
  target_link_libraries(bench_codec PRIVATE crowdy_wire crowdy_core crowdy_crypto_openssl)

  # Takes the send path apart: encode vs MAC vs syscall, and the MAC and write
  # alternatives side by side. Uses sendmmsg and OpenSSL EVP_MAC directly.
  if(NOT WIN32)
    add_executable(bench_send bench_send.cpp)
    target_link_libraries(bench_send
      PRIVATE crowdy_wire crowdy_core crowdy_crypto_openssl crowdy_replication OpenSSL::Crypto)
  endif()
endif()

if(NOT WIN32)
  add_executable(bench_e2e_latency bench_e2e_latency.cpp)
  target_link_libraries(bench_e2e_latency PRIVATE crowdy)
  target_include_directories(bench_e2e_latency PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tests/e2e)
endif()

# ChunkStore on the game thread: region loads applied within the tick budget, the parse on the
# store's I/O thread, and what range 8 holds in memory (0.82.0).
add_executable(bench_chunk_store bench_chunk_store.cpp)
target_link_libraries(bench_chunk_store PRIVATE crowdy)
