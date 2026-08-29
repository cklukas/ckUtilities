add_library(ck_vision_launcher STATIC
  calculator_model.cpp
  launcher_app.cpp
)

target_include_directories(ck_vision_launcher
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(ck_vision_launcher PUBLIC cxx_std_20)
target_link_libraries(ck_vision_launcher
  PUBLIC
    ck_vision_shell
    ck_vision_keymap
    ck_app_info
)

if(UNIX)
  add_executable(ckutilities_ckvision
    main.cpp
  )
  target_compile_features(ckutilities_ckvision PRIVATE cxx_std_20)
  target_include_directories(ckutilities_ckvision PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/ck-utilities/include
  )
  target_link_libraries(ckutilities_ckvision PRIVATE ck_vision_launcher ck_vision_keymap)
  set(_ckvision_launcher_output "ck-utilities-ckvision")
  if(CKTOOLS_CKVISION_CUTOVER)
    set(_ckvision_launcher_output "ck-utilities")
  endif()
  set_target_properties(ckutilities_ckvision PROPERTIES
    OUTPUT_NAME "${_ckvision_launcher_output}"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
  )
  install(TARGETS ckutilities_ckvision RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
  add_executable(ck_vision_launcher_tests
    tests/launcher_app_tests.cpp
  )
  target_compile_features(ck_vision_launcher_tests PRIVATE cxx_std_20)
  target_link_libraries(ck_vision_launcher_tests PRIVATE ck_vision_launcher)

  add_test(NAME ck_vision_launcher_tests COMMAND ck_vision_launcher_tests)
  set_tests_properties(ck_vision_launcher_tests PROPERTIES
    LABELS "ckvision-migration;launcher"
  )
endif()
