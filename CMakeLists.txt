cmake_minimum_required(VERSION 3.25)

project(tokmon
  VERSION 0.1.0
  DESCRIPTION "Arche-based composable native agent platform"
  LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TOKMON_BUILD_TESTS "Build Tokmon tests" ON)
option(TOKMON_BUILD_GUI "Build the SDL3/Skia desktop application" ON)
option(TOKMON_ENABLE_SANITIZERS "Enable supported sanitizers" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(TokmonWarnings)

if(TOKMON_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_test(NAME config_json_only
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ValidateJsonOnly.cmake)
endif()

find_package(nlohmann_json CONFIG REQUIRED)

add_subdirectory(common)
add_subdirectory(axon)
add_subdirectory(arche)
add_subdirectory(snow)
if(TOKMON_BUILD_GUI)
  add_subdirectory(white)
  add_subdirectory(tokmon)
endif()

set(tokmon_library_targets tokmon_common axon arche snow)
if(TOKMON_BUILD_GUI)
  list(APPEND tokmon_library_targets white tokmon_client)
endif()
install(TARGETS ${tokmon_library_targets}
  EXPORT TokmonTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(WIN32)
  install(TARGETS snow_cli
    EXPORT TokmonTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  if(TOKMON_BUILD_GUI)
    install(TARGETS tokmon_app
      EXPORT TokmonTargets
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    install(DIRECTORY
      "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin/"
      DESTINATION ${CMAKE_INSTALL_BINDIR}
      CONFIGURATIONS Debug
      FILES_MATCHING PATTERN "*.dll")
    install(DIRECTORY
      "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/"
      DESTINATION ${CMAKE_INSTALL_BINDIR}
      CONFIGURATIONS Release RelWithDebInfo MinSizeRel
      FILES_MATCHING PATTERN "*.dll")
  else()
    message(WARNING
      "Windows runtime DLL installation requires VCPKG_INSTALLED_DIR and VCPKG_TARGET_TRIPLET")
  endif()
else()
  install(TARGETS snow_cli
    EXPORT TokmonTargets
    RUNTIME_DEPENDENCY_SET SnowRuntimeDependencies
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(RUNTIME_DEPENDENCY_SET SnowRuntimeDependencies
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
    POST_EXCLUDE_REGEXES
      "^/lib/.*" "^/usr/lib/.*")
  if(TOKMON_BUILD_GUI)
    install(TARGETS tokmon_app
      EXPORT TokmonTargets
      RUNTIME_DEPENDENCY_SET TokmonDesktopRuntimeDependencies
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(RUNTIME_DEPENDENCY_SET TokmonDesktopRuntimeDependencies
      DESTINATION ${CMAKE_INSTALL_BINDIR}
      POST_EXCLUDE_REGEXES "^/lib/.*" "^/usr/lib/.*")
  endif()
endif()
install(DIRECTORY
  common/include/ axon/include/ arche/include/ snow/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(TOKMON_BUILD_GUI)
  install(DIRECTORY white/include/ tokmon/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
install(EXPORT TokmonTargets
  FILE TokmonTargets.cmake
  NAMESPACE tokmon::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Tokmon)
set(TOKMON_CONFIG_WITH_GUI ${TOKMON_BUILD_GUI})
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TokmonConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/TokmonConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Tokmon)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/TokmonConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/TokmonConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/TokmonConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Tokmon)
install(DIRECTORY sdk/typescript sdk/python
  DESTINATION ${CMAKE_INSTALL_DATADIR}/tokmon/sdk
  PATTERN "__pycache__" EXCLUDE
  PATTERN "*.pyc" EXCLUDE)
install(FILES docs/DESIGN.md docs/WHITE_UI_ARCHITECTURE.md README.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/tokmon/docs)
install(FILES assets/tokmon-brand-2048.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/tokmon/assets)
install(FILES tokmon/ui/workbench.html tokmon/ui/workbench.css
  DESTINATION ${CMAKE_INSTALL_DATADIR}/tokmon/ui)
install(FILES white/README.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/tokmon/docs/white)
install(DIRECTORY examples/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/tokmon/examples)
