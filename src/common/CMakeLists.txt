add_subdirectory(app-info)
add_subdirectory(options)

add_library(ck_file_io STATIC file_io.cpp)
target_include_directories(ck_file_io
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(ck_file_io PUBLIC cxx_std_20)
cktools_strict(ck_file_io)
install(TARGETS ck_file_io ARCHIVE DESTINATION lib COMPONENT Development)
install(FILES ${PROJECT_SOURCE_DIR}/include/ck/file_io.hpp
  DESTINATION include/ck COMPONENT Development)

target_link_libraries(ck_options PUBLIC ck_file_io)

if(BUILD_TESTING)
  set(CKTOOLS_FILE_IO_TEST_ROOT "${PROJECT_BINARY_DIR}/file-io-test-scratch" CACHE PATH
      "Explicit fixture root for the cross-platform file I/O boundary tests")
  if(NOT IS_ABSOLUTE "${CKTOOLS_FILE_IO_TEST_ROOT}")
    message(FATAL_ERROR "CKTOOLS_FILE_IO_TEST_ROOT must be absolute")
  endif()
  add_executable(ck_file_io_tests file_io_tests.cpp)
  target_compile_features(ck_file_io_tests PRIVATE cxx_std_20)
  target_compile_definitions(ck_file_io_tests PRIVATE CKTOOLS_FILE_IO_TEST_ROOT="${CKTOOLS_FILE_IO_TEST_ROOT}")
  target_link_libraries(ck_file_io_tests PRIVATE ck_file_io)
  cktools_strict(ck_file_io_tests)
  add_test(NAME ck_file_io_tests COMMAND ck_file_io_tests)
  set_tests_properties(ck_file_io_tests PROPERTIES LABELS "platform;windows")
endif()
