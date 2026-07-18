# Env-gated end-to-end tests. They connect to a deployment you configure via
# CROWDY_E2E_* environment variables and are skipped (exit 77) when unset.
file(GLOB E2E_SOURCES CONFIGURE_DEPENDS *.cpp)
foreach(src ${E2E_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE crowdy)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
