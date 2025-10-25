if (TARGET ckchat)
  get_target_property(_ckchat_runtime_dir ckchat RUNTIME_OUTPUT_DIRECTORY)
  if(NOT _ckchat_runtime_dir)
    set(_ckchat_runtime_dir "${PROJECT_BINARY_DIR}")
  endif()

  get_target_property(_ckchat_output_name ckchat OUTPUT_NAME)
  if(NOT _ckchat_output_name)
    set(_ckchat_output_name "ckchat")
  endif()

  set(_ckchat_executable "${_ckchat_runtime_dir}/${_ckchat_output_name}${CMAKE_EXECUTABLE_SUFFIX}")

  add_test(NAME ck_chat_cli_echo
    COMMAND "${_ckchat_executable}" --prompt "hello"
  )
  set_tests_properties(ck_chat_cli_echo PROPERTIES
    PASS_REGULAR_EXPRESSION "Response to: hello"
    ENVIRONMENT "CK_AI_FORCE_STUB=1"
  )
else()
  message(STATUS "ckchat target not present; skipping ck_chat_cli_echo test")
endif()
