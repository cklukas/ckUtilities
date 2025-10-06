ck_add_gtest(hotkeys_tests
  hotkeys_tests.cpp
)

target_link_libraries(hotkeys_tests
  PRIVATE
    ck_hotkeys
)
