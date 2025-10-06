set(SOURCES
    hotkeys.cpp
    default_schemes.cpp
)

add_library(ck_hotkeys STATIC ${SOURCES})

target_include_directories(ck_hotkeys
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(ck_hotkeys
    PUBLIC
        tvision
)
