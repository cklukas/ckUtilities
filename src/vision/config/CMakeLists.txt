add_library(ck_vision_config STATIC config_app.cpp config_persistence.cpp)
target_include_directories(ck_vision_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ck_vision_config PUBLIC cxx_std_20)
target_link_libraries(ck_vision_config PUBLIC ck_vision_shell ck_vision_keymap ck_options)

if(UNIX)
  add_executable(ckconfig_ckvision main.cpp)
  target_compile_features(ckconfig_ckvision PRIVATE cxx_std_20)
  target_include_directories(ckconfig_ckvision PRIVATE ${PROJECT_SOURCE_DIR}/src/tools/ck-du/include)
  target_link_libraries(ckconfig_ckvision PRIVATE ck_vision_config ck_du_core)
  set(_ckvision_config_output "ck-config-ckvision")
  if(CKTOOLS_CKVISION_CUTOVER)
    set(_ckvision_config_output "ck-config")
  endif()
  set_target_properties(ckconfig_ckvision PROPERTIES
    OUTPUT_NAME "${_ckvision_config_output}"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
  )
  install(TARGETS ckconfig_ckvision RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
  add_executable(ck_vision_config_tests tests/config_app_tests.cpp)
  target_compile_features(ck_vision_config_tests PRIVATE cxx_std_20)
  target_link_libraries(ck_vision_config_tests PRIVATE ck_vision_config)
  add_test(NAME ck_vision_config_tests COMMAND ck_vision_config_tests)
  set_tests_properties(ck_vision_config_tests PROPERTIES LABELS "ckvision-migration;config")
endif()
