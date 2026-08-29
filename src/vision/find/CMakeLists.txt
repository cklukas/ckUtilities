add_library(ck_vision_find STATIC
  find_app.cpp
  find_services.cpp
)
target_include_directories(ck_vision_find
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/tools/ck-find/include
)
target_compile_features(ck_vision_find PUBLIC cxx_std_20)
target_link_libraries(ck_vision_find PUBLIC ck_vision_shell ck_vision_keymap ck_find_core)

if(UNIX)
  add_executable(ckfind_ckvision main.cpp)
  target_compile_features(ckfind_ckvision PRIVATE cxx_std_20)
  target_link_libraries(ckfind_ckvision PRIVATE ck_vision_find ck_vision_keymap)
  set(_ckvision_find_output "ck-find-ckvision")
  if(CKTOOLS_CKVISION_CUTOVER)
    set(_ckvision_find_output "ck-find")
  endif()
  set_target_properties(ckfind_ckvision PROPERTIES
    OUTPUT_NAME "${_ckvision_find_output}"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
  )
  install(TARGETS ckfind_ckvision RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
  add_executable(ck_vision_find_tests tests/find_app_tests.cpp)
  target_compile_features(ck_vision_find_tests PRIVATE cxx_std_20)
  target_link_libraries(ck_vision_find_tests PRIVATE ck_vision_find)
  add_test(NAME ck_vision_find_tests COMMAND ck_vision_find_tests)
  set_tests_properties(ck_vision_find_tests PROPERTIES LABELS "ckvision-migration;find")
endif()
