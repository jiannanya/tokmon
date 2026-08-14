add_library(axon INTERFACE)
add_library(tokmon::axon ALIAS axon)
target_include_directories(axon INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
set_target_properties(axon PROPERTIES EXPORT_NAME axon)
target_compile_features(axon INTERFACE cxx_std_23)

if(TOKMON_BUILD_TESTS)
  add_executable(axon_tests tests/axon_tests.cpp)
  target_link_libraries(axon_tests PRIVATE tokmon::axon)
  tokmon_test_defaults(axon_tests)
  add_test(NAME axon_tests COMMAND axon_tests)
endif()
