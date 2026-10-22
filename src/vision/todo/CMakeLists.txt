# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ck-todo: deterministic workspace, persistence, and transaction semantics.
# Only SystemCalendarClock reads host time; the model, repository,
# controller and tests receive time through CalendarClock.
add_library(ck_vision_todo STATIC
  todo_model.cpp
  todo_codec.cpp
  memory_todo_repository.cpp
  json_todo_repository.cpp
  system_calendar_clock.cpp
  todo_controller.cpp
  todo_lane_view.cpp
  todo_board_view.cpp
  todo_app.cpp
  todo_host.cpp
)
target_include_directories(ck_vision_todo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ck_vision_todo PUBLIC ck_vision_host ckvision::cvision)
target_compile_features(ck_vision_todo PUBLIC cxx_std_20)
cktools_strict(ck_vision_todo)

add_executable(cktodo_ckvision main.cpp)
target_link_libraries(cktodo_ckvision PRIVATE ck_vision_todo ck_vision_host)
set_target_properties(cktodo_ckvision PROPERTIES
  OUTPUT_NAME "ck-todo"
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
)
if(MINGW)
  target_link_options(cktodo_ckvision PRIVATE -municode)
endif()
cktools_strict(cktodo_ckvision)
install(TARGETS cktodo_ckvision RUNTIME DESTINATION bin)

if(BUILD_TESTING)
  set(_ck_todo_suites
    test_todo_model.cpp
    test_todo_codec.cpp
    test_todo_repository.cpp
    test_todo_calendar_clock.cpp
    test_todo_controller.cpp
    test_todo_board.cpp
    test_todo_smoke.cpp
    test_todo_golden.cpp
    test_todo_host.cpp
  )
  if(UNIX)
    list(APPEND _ck_todo_suites test_todo_repository_posix.cpp)
  endif()
  list(TRANSFORM _ck_todo_suites PREPEND tests/ OUTPUT_VARIABLE _ck_todo_suite_sources)
  add_executable(ck_vision_todo_tests ${_ck_todo_suite_sources})
  target_link_libraries(ck_vision_todo_tests PRIVATE ck_vision_todo ck_vision_cktest_main)
  cktools_strict(ck_vision_todo_tests)
  foreach(suite IN LISTS _ck_todo_suites)
    get_filename_component(name "${suite}" NAME_WE)
    add_test(NAME ${name} COMMAND ck_vision_todo_tests --suite ${suite})
    # The goldens are read as golden/<name>.dump from the tests directory.
    set_tests_properties(${name} PROPERTIES
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests
      LABELS "todo;unit")
  endforeach()
  add_test(NAME ck_todo_version COMMAND cktodo_ckvision --version)
  set_tests_properties(ck_todo_version PROPERTIES
    PASS_REGULAR_EXPRESSION "ck-todo ${PROJECT_VERSION} \\(ckVision " LABELS "todo")
endif()
