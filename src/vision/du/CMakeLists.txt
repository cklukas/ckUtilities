add_library(ck_vision_du STATIC
  disk_usage_app.cpp
  disk_usage_services.cpp
)
target_include_directories(ck_vision_du
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${PROJECT_SOURCE_DIR}/src/tools/ck-du/include
)
target_compile_features(ck_vision_du PUBLIC cxx_std_20)
target_link_libraries(ck_vision_du PUBLIC ck_vision_shell ck_du_core)

if(APPLE)
  find_library(CK_VISION_FOUNDATION_FRAMEWORK Foundation REQUIRED)
  target_sources(ck_vision_du PRIVATE disk_usage_cloud_service.mm)
  target_link_libraries(ck_vision_du PRIVATE ${CK_VISION_FOUNDATION_FRAMEWORK})
endif()

if(UNIX)
  add_executable(ckdu_ckvision main.cpp)
  target_compile_features(ckdu_ckvision PRIVATE cxx_std_20)
  target_include_directories(ckdu_ckvision PRIVATE ${PROJECT_SOURCE_DIR}/src/tools/ck-du/include)
  target_link_libraries(ckdu_ckvision PRIVATE ck_vision_du ck_vision_keymap)
  set_target_properties(ckdu_ckvision PROPERTIES
    OUTPUT_NAME "ck-du-ckvision"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
  )
  install(TARGETS ckdu_ckvision RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
  add_executable(ck_vision_du_tests tests/disk_usage_app_tests.cpp)
  target_compile_features(ck_vision_du_tests PRIVATE cxx_std_20)
  target_link_libraries(ck_vision_du_tests PRIVATE ck_vision_du)
  add_test(NAME ck_vision_du_tests COMMAND ck_vision_du_tests)
  set_tests_properties(ck_vision_du_tests PROPERTIES LABELS "ckvision-migration;du")
endif()
