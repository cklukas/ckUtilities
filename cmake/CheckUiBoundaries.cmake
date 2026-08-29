include_guard(GLOBAL)

function(cktools_add_ui_boundary_check)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  add_custom_target(check_ui_boundaries
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_ui_boundaries.py"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Verify that domain libraries do not depend on a UI framework")

  if(BUILD_TESTING)
    add_test(NAME ck_ui_boundary_check
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_ui_boundaries.py")
    set_tests_properties(ck_ui_boundary_check PROPERTIES
      LABELS "architecture;ckvision-migration")
  endif()
endfunction()
