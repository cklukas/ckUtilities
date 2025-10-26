include(FetchTurboVision)
cktools_ensure_tvision()
include(AddCkTool) # Add this line

add_ck_tool(grid_layout_example
  SOURCES example.cpp
  INCLUDE_DIRECTORIES
    ${PROJECT_SOURCE_DIR}/include
  LIBRARIES
    ck_ui
    ck_hotkeys
    $<$<TARGET_EXISTS:tvision::tvision>:tvision::tvision>
    $<$<TARGET_EXISTS:tvision>:tvision>
    $<$<TARGET_EXISTS:TurboVision::tvision>:TurboVision::tvision>
)
