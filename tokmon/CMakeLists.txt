set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/ui/workbench.white.json)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/ui/workbench.white.json
  TOKMON_WORKBENCH_UI_JSON)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(src/workbench_document_data.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/workbench_document_data.hpp @ONLY)

add_library(tokmon_client
  src/app.cpp
  src/approval.cpp
  src/projection.cpp
  src/settings.cpp
  src/product_assembly.cpp
  src/snow_client.cpp
  src/workbench.cpp
  src/workbench_document.cpp)
add_library(tokmon::client ALIAS tokmon_client)

target_include_directories(tokmon_client PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
set_target_properties(tokmon_client PROPERTIES EXPORT_NAME client)
target_link_libraries(tokmon_client
  PUBLIC tokmon::white tokmon::snow tokmon::arche tokmon::common)
target_compile_features(tokmon_client PUBLIC cxx_std_23)
tokmon_target_defaults(tokmon_client)

add_executable(tokmon_app src/main.cpp)
set_target_properties(tokmon_app PROPERTIES OUTPUT_NAME tokmon)
target_link_libraries(tokmon_app PRIVATE tokmon::client)
tokmon_target_defaults(tokmon_app)
add_dependencies(tokmon_app snow_cli)
add_custom_command(TARGET tokmon_app POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:snow_cli> $<TARGET_FILE_DIR:tokmon_app>
  COMMENT "Deploying the Snow child-process runtime")

if(TOKMON_BUILD_TESTS)
  add_executable(tokmon_tests tests/tokmon_tests.cpp)
  target_link_libraries(tokmon_tests PRIVATE tokmon::client)
  target_compile_definitions(tokmon_tests PRIVATE
    TOKMON_TEST_SNOW_EXE="$<TARGET_FILE:snow_cli>")
  tokmon_test_defaults(tokmon_tests)
  add_dependencies(tokmon_tests snow_cli)
  add_test(NAME tokmon_tests COMMAND tokmon_tests)
  add_test(NAME tokmon_headless_smoke
    COMMAND tokmon_app --headless-smoke
            --workspace ${CMAKE_CURRENT_BINARY_DIR}/headless-smoke-workspace
            --config-dir-name .tokmon)
  set_tests_properties(tokmon_headless_smoke PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy")
  add_test(NAME tokmon_ui_screenshot
    COMMAND tokmon_app --screenshot
            ${CMAKE_CURRENT_BINARY_DIR}/tokmon-workbench.bmp
            --screenshot-demo
            --workspace ${CMAKE_CURRENT_BINARY_DIR}/ui-screenshot-workspace
            --config-dir-name .tokmon)
  set_tests_properties(tokmon_ui_screenshot PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy")
endif()
