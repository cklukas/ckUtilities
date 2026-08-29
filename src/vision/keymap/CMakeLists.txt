add_library(ck_vision_keymap STATIC keymap.cpp suite_keymap_catalog.cpp)
target_include_directories(ck_vision_keymap
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/json-view/include
)
target_compile_features(ck_vision_keymap PUBLIC cxx_std_20)
target_link_libraries(ck_vision_keymap PUBLIC ckvision::cvision ck_options)

if(BUILD_TESTING)
  add_executable(ck_vision_keymap_tests tests/keymap_tests.cpp)
  target_compile_features(ck_vision_keymap_tests PRIVATE cxx_std_20)
  target_link_libraries(ck_vision_keymap_tests PRIVATE ck_vision_keymap)
  add_test(NAME ck_vision_keymap_tests COMMAND ck_vision_keymap_tests)
  set_tests_properties(ck_vision_keymap_tests PROPERTIES LABELS "ckvision-migration;keymap")
endif()
