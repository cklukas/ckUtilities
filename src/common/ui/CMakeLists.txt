set(SOURCES
    calendar.cpp
    clock_aware_application.cpp
    clock_settings.cpp
    clock_view.cpp
    window_menu.cpp
)

add_library(ck_ui STATIC ${SOURCES})

target_include_directories(ck_ui
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(ck_ui PUBLIC cxx_std_20)

target_link_libraries(ck_ui
    PUBLIC
        $<$<TARGET_EXISTS:tvision::tvision>:tvision::tvision>
        $<$<TARGET_EXISTS:tvision>:tvision>
        $<$<TARGET_EXISTS:TurboVision::tvision>:TurboVision::tvision>
)
