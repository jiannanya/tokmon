function(tokmon_target_defaults target)
  if(MSVC)
    # /FS serializes compiler PDB writes. Multi-config Visual Studio generators
    # otherwise let parallel cl.exe instances contend for the target PDB.
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /FS)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow
      -Wnon-virtual-dtor -Wold-style-cast)
  endif()

  if(TOKMON_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

function(tokmon_test_defaults target)
  tokmon_target_defaults(${target})
  # The test suite intentionally uses assert for invariant-dense fixtures.
  # Keep those checks live in Release/RelWithDebInfo test configurations.
  if(MSVC)
    target_compile_options(${target} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${target} PRIVATE -UNDEBUG)
  endif()
endfunction()
