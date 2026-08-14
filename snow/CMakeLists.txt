find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(CURL REQUIRED)

add_library(snow
  src/agent.cpp
  src/artifact.cpp
  src/assembly.cpp
  src/c_api.cpp
  src/config.cpp
  src/context.cpp
  src/mcp.cpp
  src/model.cpp
  src/protocol.cpp
  src/replay.cpp
  src/surface.cpp
  src/tools.cpp
  src/trajectory.cpp)
add_library(tokmon::snow ALIAS snow)

target_include_directories(snow PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
set_target_properties(snow PROPERTIES EXPORT_NAME snow)
target_link_libraries(snow
  PUBLIC tokmon::arche tokmon::common nlohmann_json::nlohmann_json
  PRIVATE unofficial::sqlite3::sqlite3 CURL::libcurl)
target_compile_features(snow PUBLIC cxx_std_23)
tokmon_target_defaults(snow)
if(WIN32)
  target_link_libraries(snow PRIVATE Crypt32)
endif()

add_executable(snow_cli src/main.cpp)
set_target_properties(snow_cli PROPERTIES OUTPUT_NAME snow)
target_link_libraries(snow_cli PRIVATE tokmon::snow)
tokmon_target_defaults(snow_cli)

if(TOKMON_BUILD_TESTS)
  add_executable(snow_mcp_fixture tests/mcp_fixture.cpp)
  target_link_libraries(snow_mcp_fixture PRIVATE nlohmann_json::nlohmann_json)
  tokmon_target_defaults(snow_mcp_fixture)
  add_executable(snow_tests tests/snow_tests.cpp)
  target_link_libraries(snow_tests PRIVATE tokmon::snow)
  target_compile_definitions(snow_tests PRIVATE
    SNOW_MCP_FIXTURE_PATH="$<TARGET_FILE:snow_mcp_fixture>")
  add_dependencies(snow_tests snow_mcp_fixture)
  tokmon_test_defaults(snow_tests)
  add_test(NAME snow_tests COMMAND snow_tests)
  find_program(TOKMON_NODE_EXECUTABLE node)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(TOKMON_NODE_EXECUTABLE)
    add_test(NAME snow_typescript_sdk_golden
      COMMAND ${CMAKE_COMMAND} -E env
              SNOW_EXE=$<TARGET_FILE:snow_cli>
              ${TOKMON_NODE_EXECUTABLE}
              ${PROJECT_SOURCE_DIR}/sdk/tests/typescript_golden.mjs)
  endif()
  if(Python3_Interpreter_FOUND)
    add_test(NAME snow_python_sdk_golden
      COMMAND ${CMAKE_COMMAND} -E env
              SNOW_EXE=$<TARGET_FILE:snow_cli>
              ${Python3_EXECUTABLE}
              ${PROJECT_SOURCE_DIR}/sdk/tests/python_golden.py)
  endif()
endif()
