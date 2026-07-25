if(NOT DEFINED CROWDY_BUILD_DIR OR
   NOT DEFINED CROWDY_CMAKE_COMMAND)
  message(FATAL_ERROR "no-exception install test is missing build inputs")
endif()

set(_prefix "${CROWDY_BUILD_DIR}/no-exceptions-install-prefix")
set(_source "${CROWDY_BUILD_DIR}/no-exceptions-install-consumer")
set(_build "${CROWDY_BUILD_DIR}/no-exceptions-install-consumer-build")
file(REMOVE_RECURSE "${_prefix}" "${_source}" "${_build}")

set(_install_command
    "${CROWDY_CMAKE_COMMAND}" --install "${CROWDY_BUILD_DIR}"
    --prefix "${_prefix}")
if(DEFINED CROWDY_CONFIG AND NOT CROWDY_CONFIG STREQUAL "")
  list(APPEND _install_command --config "${CROWDY_CONFIG}")
endif()
execute_process(
  COMMAND ${_install_command}
  RESULT_VARIABLE _install_result)
if(NOT _install_result EQUAL 0)
  message(FATAL_ERROR "no-exception package install failed")
endif()

set(_layout "${_prefix}/include/crowdy/studio/layout.hpp")
if(NOT EXISTS "${_layout}")
  message(FATAL_ERROR "installed no-exception package omitted layout.hpp")
endif()

set(_throwing_studio_headers
    agent_projection.hpp
    controller.hpp
    diagnostics.hpp
    editor.hpp
    host_adapter.hpp
    integration.hpp
    models.hpp
    runtime.hpp)
foreach(_header IN LISTS _throwing_studio_headers)
  if(EXISTS "${_prefix}/include/crowdy/studio/${_header}")
    message(FATAL_ERROR
            "installed no-exception package retained throwing ${_header}")
  endif()
endforeach()

file(MAKE_DIRECTORY "${_source}")
set(_consumer_cmake [=[
cmake_minimum_required(VERSION 3.22)
project(CrowdyNoExceptionsInstallConsumer LANGUAGES CXX)
find_package(CrowdyCPP CONFIG REQUIRED)
set(CROWDY_NO_EXCEPTION_SOURCES
  main.cpp
]=])

file(GLOB_RECURSE _installed_headers
     RELATIVE "${_prefix}/include"
     "${_prefix}/include/crowdy/*.hpp")
if(NOT _installed_headers)
  message(FATAL_ERROR
          "installed no-exception package did not contain public headers")
endif()
foreach(_header IN LISTS _installed_headers)
  string(MAKE_C_IDENTIFIER "${_header}" _identifier)
  set(_source_name "${_identifier}.cpp")
  file(WRITE "${_source}/${_source_name}"
       "#ifndef CROWDY_NO_EXCEPTIONS\n"
       "#error \"installed target must export CROWDY_NO_EXCEPTIONS\"\n"
       "#endif\n"
       "#include <${_header}>\n"
       "void ${_identifier}_installed_header() {}\n")
  string(APPEND _consumer_cmake "  ${_source_name}\n")
endforeach()

string(APPEND _consumer_cmake [=[
)
add_executable(
  no_exceptions_install_consumer ${CROWDY_NO_EXCEPTION_SOURCES})
target_link_libraries(
  no_exceptions_install_consumer PRIVATE CrowdyCPP::crowdy)
target_compile_features(
  no_exceptions_install_consumer PRIVATE cxx_std_20)
]=])
file(WRITE "${_source}/CMakeLists.txt" "${_consumer_cmake}")
file(WRITE "${_source}/main.cpp" [=[
#include <crowdy/crowdy.hpp>
#include <crowdy/studio/layout.hpp>

#ifndef CROWDY_NO_EXCEPTIONS
#error "installed target must export CROWDY_NO_EXCEPTIONS"
#endif

int main() {
  crowdy::studio::StudioLayoutController layout;
  layout.setVisible(crowdy::studio::StudioPaneId::Agent, true);
  return layout.getState().isVisible(
             crowdy::studio::StudioPaneId::Agent)
             ? 0
             : 1;
}
]=])

execute_process(
  COMMAND "${CROWDY_CMAKE_COMMAND}"
          -S "${_source}" -B "${_build}"
          "-DCMAKE_PREFIX_PATH=${_prefix}"
  RESULT_VARIABLE _configure_result)
if(NOT _configure_result EQUAL 0)
  message(FATAL_ERROR
          "installed no-exception consumer configure failed")
endif()
execute_process(
  COMMAND "${CROWDY_CMAKE_COMMAND}" --build "${_build}"
  RESULT_VARIABLE _build_result)
if(NOT _build_result EQUAL 0)
  message(FATAL_ERROR "installed no-exception consumer build failed")
endif()
execute_process(
  COMMAND
    "${_build}/no_exceptions_install_consumer${CROWDY_EXECUTABLE_SUFFIX}"
  RESULT_VARIABLE _run_result)
if(NOT _run_result EQUAL 0)
  message(FATAL_ERROR "installed no-exception consumer run failed")
endif()
