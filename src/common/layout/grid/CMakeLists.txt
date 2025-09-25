include(FetchTurboVision)
cktools_ensure_tvision()
include(AddCkTool) # Add this line

add_ck_tool(grid_layout_example
  SOURCES example.cpp
  LIBRARIES
    $<$<TARGET_EXISTS:tvision::tvision>:tvision::tvision>
    $<$<TARGET_EXISTS:tvision>:tvision>
    $<$<TARGET_EXISTS:TurboVision::tvision>:TurboVision::tvision>
)

