add_executable(usdcut src/main.cpp)
target_link_libraries(usdcut PRIVATE usdprep::core)
if(MSVC)
    target_compile_options(usdcut PRIVATE /wd4244 /wd4305 /wd4996)
    target_compile_definitions(usdcut PRIVATE NOMINMAX)
endif()
target_compile_definitions(usdcut PRIVATE TBB_SUPPRESS_DEPRECATED_MESSAGES)

install(TARGETS usdcut RUNTIME DESTINATION bin)
