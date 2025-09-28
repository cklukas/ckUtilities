include(FetchContent)

set(CK_LLAMA_CPP_TAG
    "2811c65286ae954bec87049f75b86dc022006dcc"
    CACHE STRING "Pinned llama.cpp commit used for builds")

if(NOT TARGET llama)
  FetchContent_Declare(
    llama_cpp
    GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git
    GIT_TAG        ${CK_LLAMA_CPP_TAG}
    GIT_SHALLOW    TRUE
  )

  FetchContent_MakeAvailable(llama_cpp)
endif()
