add_library(tokmon_common
  src/digest.cpp
  src/types.cpp
  src/files.cpp)
add_library(tokmon::common ALIAS tokmon_common)

target_include_directories(tokmon_common PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
set_target_properties(tokmon_common PROPERTIES EXPORT_NAME common)
target_link_libraries(tokmon_common PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(tokmon_common PUBLIC cxx_std_23)
tokmon_target_defaults(tokmon_common)
