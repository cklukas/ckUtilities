add_library(ck_vision_edit STATIC edit_app.cpp markdown_normalization.cpp markdown_profile.cpp)
target_include_directories(ck_vision_edit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ck_vision_edit PUBLIC cxx_std_20)
target_link_libraries(ck_vision_edit PUBLIC ck_vision_shell ck_vision_keymap ck_markdown_core)

if(UNIX)
  add_executable(ckedit_ckvision main.cpp)
  target_compile_features(ckedit_ckvision PRIVATE cxx_std_20)
  target_link_libraries(ckedit_ckvision PRIVATE ck_vision_edit ck_vision_keymap)
  set(_ckvision_edit_output "ck-edit-ckvision")
  if(CKTOOLS_CKVISION_CUTOVER)
    set(_ckvision_edit_output "ck-edit")
  endif()
  set_target_properties(ckedit_ckvision PROPERTIES
    OUTPUT_NAME "${_ckvision_edit_output}"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
  )
  install(TARGETS ckedit_ckvision RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
  add_executable(ck_vision_edit_tests tests/edit_app_tests.cpp)
  target_compile_features(ck_vision_edit_tests PRIVATE cxx_std_20)
  target_link_libraries(ck_vision_edit_tests PRIVATE ck_vision_edit)
  add_test(NAME ck_vision_edit_tests COMMAND ck_vision_edit_tests)
  set_tests_properties(ck_vision_edit_tests PROPERTIES LABELS "ckvision-migration;edit")
endif()
