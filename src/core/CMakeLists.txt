# Vendored meshoptimizer (MIT), the decimator behind the Simplify op.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/meshoptimizer
                 ${CMAKE_CURRENT_BINARY_DIR}/meshoptimizer)

add_library(usdprep-core
    src/Extract.cpp
    src/Prune.cpp
    src/Recipe.cpp
    src/Report.cpp
    src/Select.cpp
    src/Simplify.cpp
    src/UdimAtlas.cpp
    src/RawTextures.cpp
    src/NukeCompat.cpp
    src/NukeGeometry.cpp
    src/MeshExport.cpp
    src/AbcWriter.cpp
    src/StageInfo.cpp
    src/ObjImport.cpp
    src/Import.cpp
    src/ImportShared.cpp
    src/AbcImport.cpp
    src/FbxImport.cpp
    src/FbxScan.cpp
)
add_library(usdprep::core ALIAS usdprep-core)

target_include_directories(usdprep-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(usdprep-core PUBLIC ${PXR_LIBRARIES})
target_link_libraries(usdprep-core PRIVATE meshoptimizer)
target_include_directories(usdprep-core PUBLIC ${PXR_INCLUDE_DIRS})

if(MSVC)
    # /wd*: silence USD header conversion noise (same set usdtweak uses)
    target_compile_options(usdprep-core PRIVATE /MP /wd4244 /wd4305 /wd4996)
    target_compile_definitions(usdprep-core PRIVATE NOMINMAX)
endif()
target_compile_definitions(usdprep-core PRIVATE TBB_SUPPRESS_DEPRECATED_MESSAGES)

# Alembic (.abc export for Nuke's classic 3D) is optional: without it the
# core builds and an .abc output fails with a clear message. It is not on
# conda-forge; third_party/alembic-install is a static build of the
# official source (see M4_LOG.md), Imath comes from the USD environment.
find_package(Alembic CONFIG QUIET PATHS "${CMAKE_CURRENT_LIST_DIR}/../../third_party/alembic-install" NO_DEFAULT_PATH)
if(Alembic_FOUND)
    message(STATUS "usdprep: Alembic found - .abc export enabled")
    target_link_libraries(usdprep-core PRIVATE Alembic::Alembic)
    target_compile_definitions(usdprep-core PRIVATE USDPREP_WITH_ALEMBIC)
else()
    message(STATUS "usdprep: Alembic not found - .abc export disabled")
endif()

# assimp (.fbx import) comes from the USD environment (conda-forge). Without
# it the core builds and an .fbx fails to convert with a clear message.
find_package(assimp CONFIG QUIET)
if(assimp_FOUND)
    message(STATUS "usdprep: assimp ${assimp_VERSION} found - .fbx import enabled")
    target_link_libraries(usdprep-core PRIVATE assimp::assimp)
    # binary FBX packs its arrays with zlib (FbxScan.cpp reads the animation start)
    if(NOT ZLIB_ROOT)
        get_filename_component(ZLIB_ROOT "${assimp_DIR}/../../.." ABSOLUTE)  # the same environment
    endif()
    find_package(ZLIB REQUIRED)
    target_link_libraries(usdprep-core PRIVATE ZLIB::ZLIB)
    target_compile_definitions(usdprep-core PRIVATE USDPREP_WITH_ASSIMP)
else()
    message(STATUS "usdprep: assimp not found - .fbx import disabled")
endif()
