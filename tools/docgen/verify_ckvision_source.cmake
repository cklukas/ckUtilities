if(NOT DEFINED CKTOOLS_CKVISION_SOURCE_DIR)
  message(FATAL_ERROR
    "CKTOOLS_CKVISION_SOURCE_DIR is required for documentation captures")
endif()

file(READ "${CMAKE_CURRENT_LIST_DIR}/../../scripts/ci/dependency-pins.json" _cktools_pins)
string(JSON _cktools_expected_revision GET "${_cktools_pins}" ckvision)
string(LENGTH "${_cktools_expected_revision}" _cktools_revision_length)
if(NOT _cktools_expected_revision MATCHES "^[0-9a-f]+$" OR NOT _cktools_revision_length EQUAL 40)
  message(FATAL_ERROR "Documentation captures require a full ckVision commit in dependency-pins.json")
endif()

execute_process(
  COMMAND git -C "${CKTOOLS_CKVISION_SOURCE_DIR}" rev-parse HEAD
  OUTPUT_VARIABLE _cktools_source_revision
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _cktools_git_result)
if(NOT _cktools_git_result STREQUAL "0" OR
   NOT _cktools_source_revision STREQUAL _cktools_expected_revision)
  message(FATAL_ERROR
    "Documentation captures require ckVision ${_cktools_expected_revision}, not ${_cktools_source_revision}")
endif()

execute_process(
  COMMAND git -C "${CKTOOLS_CKVISION_SOURCE_DIR}" status --porcelain=v1 --untracked-files=all
  OUTPUT_VARIABLE _cktools_source_status
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _cktools_git_result)
if(NOT _cktools_git_result STREQUAL "0" OR NOT _cktools_source_status STREQUAL "")
  message(FATAL_ERROR "Documentation captures require a clean ckVision source checkout")
endif()
