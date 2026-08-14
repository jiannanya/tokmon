add_library(arche
  src/composition.cpp
  src/context.cpp
  src/manifest.cpp
  src/package.cpp
  src/runtime.cpp
  src/native_loader.cpp)
add_library(tokmon::arche ALIAS arche)

target_include_directories(arche PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
set_target_properties(arche PROPERTIES EXPORT_NAME arche)
target_link_libraries(arche
  PUBLIC tokmon::common tokmon::axon nlohmann_json::nlohmann_json)
target_compile_features(arche PUBLIC cxx_std_23)
tokmon_target_defaults(arche)

if(WIN32)
  target_link_libraries(arche PRIVATE kernel32 bcrypt)
elseif(APPLE)
  target_link_libraries(arche PRIVATE dl "-framework Security"
                                      "-framework CoreFoundation")
else()
  find_package(OpenSSL REQUIRED)
  target_link_libraries(arche PRIVATE dl OpenSSL::Crypto)
endif()

if(TOKMON_BUILD_TESTS)
  add_library(arche_test_plugin MODULE tests/native_test_plugin.cpp)
  target_include_directories(arche_test_plugin PRIVATE include)
  target_compile_features(arche_test_plugin PRIVATE cxx_std_23)
  tokmon_target_defaults(arche_test_plugin)

  add_executable(arche_tests tests/arche_tests.cpp)
  target_link_libraries(arche_tests PRIVATE tokmon::arche)
  target_compile_definitions(arche_tests PRIVATE
    ARCHE_TEST_PLUGIN_PATH="$<TARGET_FILE:arche_test_plugin>")
  add_dependencies(arche_tests arche_test_plugin)
  tokmon_test_defaults(arche_tests)
  add_test(NAME arche_tests COMMAND arche_tests)
endif()
