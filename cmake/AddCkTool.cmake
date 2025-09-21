include(CMakeParseArguments)

function(add_ck_tool TOOL_NAME)
  set(options NO_INSTALL)
  set(oneValueArgs OUTPUT_NAME)
  set(multiValueArgs SOURCES LIBRARIES INCLUDE_DIRECTORIES DEFINITIONS DEPENDS)
  cmake_parse_arguments(cktool "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT cktool_SOURCES)
    message(FATAL_ERROR "add_ck_tool called without SOURCES for ${TOOL_NAME}")
  endif()

  add_executable(${TOOL_NAME} ${cktool_SOURCES})
  target_compile_features(${TOOL_NAME} PRIVATE cxx_std_20)

  set(_ck_runtime_dir "${PROJECT_BINARY_DIR}/bin")
  file(MAKE_DIRECTORY "${_ck_runtime_dir}")
  set_target_properties(${TOOL_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${_ck_runtime_dir}"
  )
  if(CMAKE_CONFIGURATION_TYPES)
    foreach(_ck_config ${CMAKE_CONFIGURATION_TYPES})
      string(TOUPPER "${_ck_config}" _ck_config_upper)
      set_target_properties(${TOOL_NAME} PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${_ck_config_upper}" "${_ck_runtime_dir}"
      )
    endforeach()
  endif()

  if(cktool_INCLUDE_DIRECTORIES)
    target_include_directories(${TOOL_NAME} PRIVATE ${cktool_INCLUDE_DIRECTORIES})
  endif()

  if(cktool_LIBRARIES)
    target_link_libraries(${TOOL_NAME} PRIVATE ${cktool_LIBRARIES})
  endif()

  if(cktool_DEFINITIONS)
    target_compile_definitions(${TOOL_NAME} PRIVATE ${cktool_DEFINITIONS})
  endif()

  if(cktool_OUTPUT_NAME)
    set_target_properties(${TOOL_NAME} PROPERTIES OUTPUT_NAME "${cktool_OUTPUT_NAME}")
  endif()

  if(cktool_DEPENDS)
    add_dependencies(${TOOL_NAME} ${cktool_DEPENDS})
  endif()

  if(NOT cktool_NO_INSTALL)
    install(TARGETS ${TOOL_NAME} RUNTIME DESTINATION bin)
  endif()

  get_property(_tools GLOBAL PROPERTY CKTOOLS_BINARIES)
  list(APPEND _tools ${TOOL_NAME})
  set_property(GLOBAL PROPERTY CKTOOLS_BINARIES "${_tools}")
endfunction()
