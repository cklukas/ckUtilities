add_library(ck_vision_shell STATIC
  suite_shell.cpp
)

target_include_directories(ck_vision_shell
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(ck_vision_shell PUBLIC cxx_std_20)
target_link_libraries(ck_vision_shell PUBLIC ckvision::cvision)

add_subdirectory(keymap)
add_subdirectory(json-view)
add_subdirectory(launcher)
add_subdirectory(find)
add_subdirectory(du)
add_subdirectory(config)
add_subdirectory(edit)
add_subdirectory(chat)

if(UNIX)
  add_executable(ck_vision_terminal_smoke
    tests/terminal_smoke.cpp
  )
  target_compile_features(ck_vision_terminal_smoke PRIVATE cxx_std_20)

  if(NOT APPLE)
    find_library(CKTOOLS_UTIL_LIBRARY NAMES util REQUIRED)
    target_link_libraries(ck_vision_terminal_smoke PRIVATE "${CKTOOLS_UTIL_LIBRARY}")
  endif()
endif()

if(BUILD_TESTING)
  add_executable(ck_vision_shell_tests
    tests/suite_shell_tests.cpp
  )
  target_link_libraries(ck_vision_shell_tests PRIVATE ck_vision_shell)
  target_compile_features(ck_vision_shell_tests PRIVATE cxx_std_20)

  add_test(NAME ck_vision_shell_tests COMMAND ck_vision_shell_tests)
  set_tests_properties(ck_vision_shell_tests PROPERTIES
    LABELS "ckvision-migration;shell")
endif()
