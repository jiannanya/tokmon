find_package(SDL3 CONFIG REQUIRED)
find_package(lexbor CONFIG REQUIRED)
find_package(yoga CONFIG REQUIRED)
find_package(unofficial-skia CONFIG REQUIRED)

add_library(white
  src/assembly.cpp
  src/document.cpp
  src/renderer.cpp
  src/text_editor.cpp
  src/window.cpp)
add_library(tokmon::white ALIAS white)

target_include_directories(white PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
set_target_properties(white PROPERTIES EXPORT_NAME white)
target_link_libraries(white
  PUBLIC tokmon::arche tokmon::common SDL3::SDL3
  PRIVATE lexbor::lexbor yoga::yogacore unofficial::skia::skia
          unofficial::skia::modules::skparagraph)
target_compile_features(white PUBLIC cxx_std_23)
tokmon_target_defaults(white)

if(TOKMON_BUILD_TESTS)
  add_executable(white_tests tests/white_tests.cpp)
  target_link_libraries(white_tests PRIVATE tokmon::white)
  tokmon_test_defaults(white_tests)
  add_test(NAME white_tests COMMAND white_tests)
endif()
