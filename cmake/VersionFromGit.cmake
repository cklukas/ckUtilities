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

      # A git hash won't contain any dots.
      # A proper git describe tag will be like <semver>-<count>-g<hash>
      if(NOT _git_version MATCHES "\.")
        # This is likely a hash because no tags were found.
        # Or a tag that is not a semver, like "my-tag".
        # Let's get the number of commits as a tweak version.
        execute_process(
          COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
          WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
          OUTPUT_VARIABLE _commit_count
          RESULT_VARIABLE _git_result2
          OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_git_result2 EQUAL 0)
            set(${OUT_VAR} "0.1.0.${_commit_count}" PARENT_SCOPE)
        else()
            set(${OUT_VAR} "${_fallback}" PARENT_SCOPE)
        endif()
        return()
      endif()

      # It has dots, so it's probably based on a tag like `1.2.3` or `1.2`.
      # The format is <tag>-<commits>-g<hash>[-dirty]
      # Let's convert it to <tag>.<commits>
      string(REGEX REPLACE "-[0-9]+-g[a-f0-9]+(-dirty)?$" "" _cmake_version "${_git_version}")
      string(REGEX MATCH "-([0-9]+)-g" _commit_count_match "${_git_version}")
      if(CMAKE_MATCH_COUNT GREATER 0)
        set(_commit_count "${CMAKE_MATCH_1}")
        set(_cmake_version "${_cmake_version}.${_commit_count}")
      endif()

      # remove -dirty if present (e.g. on a dirty tag)
      string(REGEX REPLACE "-dirty$" "" _cmake_version "${_cmake_version}")

      if(_cmake_version MATCHES "^[0-9]+(\.[0-9]+)*$")
        set(${OUT_VAR} "${_cmake_version}" PARENT_SCOPE)
        return()
      endif()
    endif()
  endif()

  set(${OUT_VAR} "${_fallback}" PARENT_SCOPE)
endfunction()