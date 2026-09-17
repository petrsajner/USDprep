# UsdPrep — export the editor selection as a Nuke-ready USD asset.
#
# This folder is synced into usdtweak's src/addons/ by tools/sync-addon.sh
# (the sanctioned way to add addons — no usdtweak host files are edited).
# The usdprep project root must be provided at configure time:
#   cmake -DUSDPREP_SOURCE_DIR=<path-to-usdprep-project> ...
set(USDPREP_SOURCE_DIR "" CACHE PATH "Root of the usdprep project (provides usdprep-core)")
if(NOT USDPREP_SOURCE_DIR)
    message(FATAL_ERROR "UsdPrep addon: pass -DUSDPREP_SOURCE_DIR=<usdprep project root>")
endif()

add_subdirectory(${USDPREP_SOURCE_DIR}/src/core ${CMAKE_CURRENT_BINARY_DIR}/usdprep-core-build)

usdtweak_add_addon(UsdPrep
    SOURCES
        UsdPrep.cpp
)

# NOTE: plain signature on purpose — the usdtweak target uses the plain
# (non-keyword) form of target_link_libraries everywhere, and mixing the
# two signatures on one target is a CMake error.
target_link_libraries(usdtweak usdprep::core)
if(WIN32)
    # IFileSaveDialog (native "Save as..." in the addon)
    target_link_libraries(usdtweak ole32)
endif()
