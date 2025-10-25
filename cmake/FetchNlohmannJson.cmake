include(FetchContent)

set(CK_NLOHMANN_JSON_TAG
    "v3.11.3"
    CACHE STRING "Pinned nlohmann/json release used for builds")

if(NOT TARGET nlohmann_json::nlohmann_json)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        ${CK_NLOHMANN_JSON_TAG}
    GIT_SHALLOW    TRUE
  )

  FetchContent_MakeAvailable(nlohmann_json)
endif()
