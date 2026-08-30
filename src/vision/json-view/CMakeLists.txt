add_library(ck_vision_json_view STATIC
  json_view_app.cpp
)

target_include_directories(ck_vision_json_view
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/tools/json-view/include
)

target_compile_features(ck_vision_json_view PUBLIC cxx_std_20)
target_link_libraries(ck_vision_json_view
  PUBLIC
    ck_vision_shell
    ck_vision_keymap
    ck_json_view_core
)

add_executable(ckjsonview_ckvision
  main.cpp
)

target_compile_features(ckjsonview_ckvision PRIVATE cxx_std_20)
target_link_libraries(ckjsonview_ckvision PRIVATE ck_vision_json_view ck_vision_keymap)
set_target_properties(ckjsonview_ckvision PROPERTIES
  OUTPUT_NAME "ck-json-view"
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
)

install(TARGETS ckjsonview_ckvision RUNTIME DESTINATION bin)

if(BUILD_TESTING)
  add_executable(ck_vision_json_view_tests
    tests/json_view_app_tests.cpp
  )
  target_compile_features(ck_vision_json_view_tests PRIVATE cxx_std_20)
  target_link_libraries(ck_vision_json_view_tests PRIVATE ck_vision_json_view)

  add_test(NAME ck_vision_json_view_tests COMMAND ck_vision_json_view_tests)
  set_tests_properties(ck_vision_json_view_tests PROPERTIES
    LABELS "ckvision-migration;json-view"
  )
endif()
