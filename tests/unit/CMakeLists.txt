include(GoogleTest)

function(ck_add_gtest target)
  add_executable(${target} ${ARGN})
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_link_libraries(${target} PRIVATE GTest::gtest_main)
  # CMake 4.3's JSON-based post-build discovery is unreliable on the
  # macOS ARM runner for this vendored GoogleTest release.  The suite has no
  # parameterized tests, so source scanning registers the same tests without
  # executing a newly linked binary during the build.
  gtest_add_tests(
    TARGET ${target}
    SOURCES ${ARGN}
    TEST_LIST ${target}_TESTS
  )
endfunction()

add_subdirectory(app_info)
add_subdirectory(config)
add_subdirectory(ck_du)
add_subdirectory(json_view)
add_subdirectory(ck_edit)
add_subdirectory(ck_find)

add_subdirectory(ckai_core)
