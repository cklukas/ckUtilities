include(GoogleTest)

function(ck_add_gtest target)
  add_executable(${target} ${ARGN})
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_link_libraries(${target} PRIVATE GTest::gtest_main)
  gtest_discover_tests(${target}
    DISCOVERY_TIMEOUT 60
  )
endfunction()

add_subdirectory(app_info)
add_subdirectory(config)
add_subdirectory(ck_du)
add_subdirectory(json_view)
add_subdirectory(ck_edit)

add_subdirectory(ckai_core)
