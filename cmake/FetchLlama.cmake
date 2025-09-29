include(FetchContent)

set(CK_LLAMA_CPP_TAG
    "b6617"
    CACHE STRING "Pinned llama.cpp commit used for builds")

if(APPLE)
  set(GGML_METAL ON CACHE BOOL "Enable ggml Metal backend" FORCE)
  set(LLAMA_METAL ON CACHE BOOL "Enable llama.cpp Metal backend" FORCE)
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
