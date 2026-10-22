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
    if(TARGET llama)
      # An installed llama/ggml package (Homebrew's, for one) carries C
      # compile features on its imported targets, so C must be enabled here,
      # project-wide, before any target uses them. It is not enabled in
      # project(): with OBJCXX enabled there, ggml's Objective-C .m sources
      # would compile as Objective-C++ when llama.cpp is built from source.
      enable_language(C)
    endif()
  endif()

endif()

if(NOT TARGET llama)
  FetchContent_Declare(
    llama_cpp
    GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git
    GIT_TAG        ${CK_LLAMA_CPP_TAG}
    GIT_SHALLOW    TRUE
  )

  # Static llama and ggml libraries: every tool stays one self-contained
  # executable, with no shared library to locate at run time (Windows has no
  # rpath). llama.cpp's BUILD_SHARED_LIBS option defers to this variable
  # (CMP0077), and googletest, configured later, builds static as well.
  set(BUILD_SHARED_LIBS OFF)
  # llama.cpp is written for C++17; under the suite's C++20, MSVC types its
  # u8"" literals as char8_t and the build fails. The standard is scoped to
  # this subproject and restored afterwards.
  set(_ck_saved_cxx_standard "${CMAKE_CXX_STANDARD}")
  set(CMAKE_CXX_STANDARD 17)
  FetchContent_MakeAvailable(llama_cpp)
  set(CMAKE_CXX_STANDARD "${_ck_saved_cxx_standard}")
endif()
