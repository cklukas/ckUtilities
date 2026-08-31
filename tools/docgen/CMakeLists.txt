# Documentation capture tooling mirrors ckVision's own pipeline: it drives the
# real application object graph on HeadlessTerminal and writes SVG from the
# decoded terminal display.  It is opt-in and never part of product packages.
option(CKTOOLS_BUILD_DOCUMENTATION_CAPTURES
       "Build deterministic ckUtilities documentation screenshot capturer" OFF)

if(CKTOOLS_BUILD_DOCUMENTATION_CAPTURES)
  if(NOT DEFINED CKTOOLS_CKVISION_SOURCE_DIR)
    message(FATAL_ERROR
      "CKTOOLS_CKVISION_SOURCE_DIR is required for documentation captures; it must name the pinned ckVision source checkout")
  endif()

  set(CKTOOLS_CKVISION_DOCUMENTATION_REVISION
      "4f211569a95ddcd0875d6c5d9778d06d2bf74fec")
  execute_process(
    COMMAND git -C "${CKTOOLS_CKVISION_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE CKTOOLS_CKVISION_SOURCE_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE CKTOOLS_CKVISION_REVISION_RESULT)
  if(NOT CKTOOLS_CKVISION_REVISION_RESULT EQUAL 0 OR
     NOT CKTOOLS_CKVISION_SOURCE_REVISION STREQUAL CKTOOLS_CKVISION_DOCUMENTATION_REVISION)
    message(FATAL_ERROR
      "Documentation captures require ckVision ${CKTOOLS_CKVISION_DOCUMENTATION_REVISION}, not ${CKTOOLS_CKVISION_SOURCE_REVISION}")
  endif()

  set(CKTOOLS_CKVISION_FRAME_SVG
      "${CKTOOLS_CKVISION_SOURCE_DIR}/tools/docgen/frame_svg.cpp")
  if(NOT EXISTS "${CKTOOLS_CKVISION_FRAME_SVG}")
    message(FATAL_ERROR
      "CKTOOLS_CKVISION_SOURCE_DIR does not contain tools/docgen/frame_svg.cpp")
  endif()

  add_library(cktools_docgen_frame_svg STATIC "${CKTOOLS_CKVISION_FRAME_SVG}")
  target_include_directories(cktools_docgen_frame_svg PUBLIC
    "${CKTOOLS_CKVISION_SOURCE_DIR}/tools/docgen")
  target_link_libraries(cktools_docgen_frame_svg PRIVATE ckvision::cvision)
  target_compile_features(cktools_docgen_frame_svg PRIVATE cxx_std_20)

  add_executable(capture_ckutilities_screenshots capture_ckutilities_screenshots.cpp)
  target_include_directories(capture_ckutilities_screenshots PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/src/tools/ck-chat/include"
    "${PROJECT_SOURCE_DIR}/src/tools/ck-du/include"
    "${PROJECT_SOURCE_DIR}/src/tools/ck-find/include"
    "${PROJECT_SOURCE_DIR}/src/tools/json-view/include"
    "${PROJECT_SOURCE_DIR}/src/tools/ck-edit/include")
  target_link_libraries(capture_ckutilities_screenshots PRIVATE
    cktools_docgen_frame_svg
    ck_vision_launcher
    ck_vision_json_view
    ck_vision_edit
    ck_vision_find
    ck_vision_du
    ck_vision_config
    ck_vision_chat
    ck_du_core
    ck_chat_options
    ckvision::cvision)
  target_compile_features(capture_ckutilities_screenshots PRIVATE cxx_std_20)
endif()
