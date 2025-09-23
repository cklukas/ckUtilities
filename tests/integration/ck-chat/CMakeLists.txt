add_test(NAME ck_chat_cli_echo
  COMMAND $<TARGET_FILE:ckchat> --prompt "hello"
)
set_tests_properties(ck_chat_cli_echo PROPERTIES
  PASS_REGULAR_EXPRESSION "Response to: hello"
)
