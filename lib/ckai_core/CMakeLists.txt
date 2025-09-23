add_library(ckai_core STATIC
  src/config_loader.cpp
  src/llm.cpp
)

target_include_directories(ckai_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(ckai_core
  PUBLIC
    llama_cpp
)

target_compile_features(ckai_core PUBLIC cxx_std_20)

install(TARGETS ckai_core
  ARCHIVE DESTINATION lib
)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ck/ai/
  DESTINATION include/ck/ai
  FILES_MATCHING PATTERN "*.hpp"
)
