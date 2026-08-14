if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(GLOB_RECURSE forbidden_yaml LIST_DIRECTORIES false
  "${SOURCE_DIR}/*.yaml" "${SOURCE_DIR}/*.yml" "${SOURCE_DIR}/*.toml"
  "${SOURCE_DIR}/*.json5")
list(FILTER forbidden_yaml EXCLUDE REGEX "[/\\](build|vcpkg_installed)[/\\]")
if(forbidden_yaml)
  list(JOIN forbidden_yaml "\n  " rendered)
  message(FATAL_ERROR
    "Structured configuration must be UTF-8 JSON; unsupported files found:\n  ${rendered}")
endif()

file(GLOB_RECURSE json_files LIST_DIRECTORIES false "${SOURCE_DIR}/*.json")
list(FILTER json_files EXCLUDE REGEX "[/\\](build|vcpkg_installed)[/\\]")
foreach(path IN LISTS json_files)
  file(READ "${path}" content)
  string(JSON kind ERROR_VARIABLE parse_error TYPE "${content}")
  if(parse_error)
    message(FATAL_ERROR "Invalid strict JSON in ${path}: ${parse_error}")
  endif()
endforeach()
