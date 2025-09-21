function(cktools_version_from_git OUT_VAR)
  set(_fallback "0.1.0")

  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} describe --tags --long --dirty --always
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE _git_version
      RESULT_VARIABLE _git_result
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_git_result EQUAL 0 AND NOT _git_version STREQUAL "")
      string(REGEX REPLACE "^v" "" _git_version "${_git_version}")
      if(_git_version MATCHES "^[0-9]+(\\.[0-9]+)*(-[A-Za-z0-9]+)?(-dirty)?$")
        set(${OUT_VAR} "${_git_version}" PARENT_SCOPE)
        return()
      endif()
    endif()
  endif()

  set(${OUT_VAR} "${_fallback}" PARENT_SCOPE)
endfunction()
