add_library(usdprep-core
    src/Extract.cpp
    src/Prune.cpp
    src/Report.cpp
    src/StageInfo.cpp
)
add_library(usdprep::core ALIAS usdprep-core)

target_include_directories(usdprep-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(usdprep-core PUBLIC ${PXR_LIBRARIES})
target_include_directories(usdprep-core PUBLIC ${PXR_INCLUDE_DIRS})

if(MSVC)
    # /wd*: silence USD header conversion noise (same set usdtweak uses)
    target_compile_options(usdprep-core PRIVATE /MP /wd4244 /wd4305 /wd4996)
    target_compile_definitions(usdprep-core PRIVATE NOMINMAX)
endif()
target_compile_definitions(usdprep-core PRIVATE TBB_SUPPRESS_DEPRECATED_MESSAGES)
