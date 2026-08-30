include(FetchContent)

set(CK_LLAMA_CPP_TAG
    "b6617"
    CACHE STRING "Pinned llama.cpp commit used for builds")

set(CK_LLAMA_CPP_SOURCE_DIR "" CACHE PATH
    "Optional pre-fetched llama.cpp source directory (disables network retrieval)")

if(APPLE)
  set(GGML_METAL ON CACHE BOOL "Enable ggml Metal backend" FORCE)
endif()

if(NOT TARGET llama)
  if(CK_LLAMA_CPP_SOURCE_DIR)
    get_filename_component(_ck_llama_cpp_source_dir
      "${CK_LLAMA_CPP_SOURCE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(NOT EXISTS "${_ck_llama_cpp_source_dir}/CMakeLists.txt")
      message(FATAL_ERROR
        "CK_LLAMA_CPP_SOURCE_DIR must contain the llama.cpp CMakeLists.txt: "
        "${_ck_llama_cpp_source_dir}")
    endif()

    # FetchContent recognizes this documented override and uses the exact
    # pre-fetched source tree instead of downloading at configure time.  The
    # release formula relies on it to build the revision it declares.
    set(FETCHCONTENT_SOURCE_DIR_LLAMA_CPP "${_ck_llama_cpp_source_dir}"
      CACHE PATH "Pre-fetched llama.cpp source directory" FORCE)
  else()
    # Package managers can provide a compatible CMake package.  Keep the
    # source-pinned FetchContent path as the normal development fallback.
    find_package(llama CONFIG QUIET)
  endif()

endif()

if(NOT TARGET llama)
  FetchContent_Declare(
    llama_cpp
    GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git
    GIT_TAG        ${CK_LLAMA_CPP_TAG}
    GIT_SHALLOW    TRUE
  )

  FetchContent_MakeAvailable(llama_cpp)
endif()
