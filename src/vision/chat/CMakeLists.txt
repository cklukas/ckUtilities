add_library(ck_vision_chat STATIC chat_app.cpp chat_services.cpp chat_markdown.cpp chat_startup_options.cpp)
target_include_directories(ck_vision_chat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ck_vision_chat PUBLIC cxx_std_20)
target_link_libraries(ck_vision_chat PUBLIC ck_vision_shell ck_vision_keymap ck_markdown_core ckai_core ck_chat_options)

if(UNIX)
  add_executable(ckchat_ckvision main.cpp)
  target_compile_features(ckchat_ckvision PRIVATE cxx_std_20)
  target_link_libraries(ckchat_ckvision PRIVATE ck_vision_chat ck_vision_keymap)
  set(_ckvision_chat_output "ck-chat-ckvision")
  if(CKTOOLS_CKVISION_CUTOVER)
    set(_ckvision_chat_output "ck-chat")
  endif()
  set_target_properties(ckchat_ckvision PROPERTIES
    OUTPUT_NAME "${_ckvision_chat_output}"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
  )
  install(TARGETS ckchat_ckvision RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
  add_executable(ck_vision_chat_tests tests/chat_app_tests.cpp)
  target_compile_features(ck_vision_chat_tests PRIVATE cxx_std_20)
  target_link_libraries(ck_vision_chat_tests PRIVATE ck_vision_chat)
  add_test(NAME ck_vision_chat_tests COMMAND ck_vision_chat_tests)
  set_tests_properties(ck_vision_chat_tests PROPERTIES LABELS "ckvision-migration;chat")
endif()
