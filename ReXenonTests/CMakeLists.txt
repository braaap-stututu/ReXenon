project("ReXenonTests")

# Off unless asked for (ReXenonTests/run.py configures build-tests with it on), so other build
# directories are unaffected.
option(REXENON_TESTS "Build the ReXenon instruction test driver" OFF)
if (NOT REXENON_TESTS)
    return()
endif()

# LibXenonAnalyse's sources include fmt but the library doesn't link it (yet); harmless if it does.
target_link_libraries(LibXenonAnalyse PUBLIC fmt::fmt)

# The test driver: assembles Xenia's PPC instruction tests and recompiles them with the real
# emitter (recompiler.cpp), writing C++ and test vectors that run.py compiles and runs.
add_executable(ReXenonTestDriver
    "driver.cpp"
    "assembler.cpp"
    "ppc_tables.c"
    "${XENONRECOMP_ROOT}/recompiler.cpp"
    "${XENONRECOMP_ROOT}/recompiler_config.cpp"
    "${XENONRECOMP_ROOT}/nonvolatile_context.cpp")

target_include_directories(ReXenonTestDriver PRIVATE ${XENONRECOMP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})

# recompiler_config.cpp relies on the precompiled header being force-included, as in XenonRecomp.
target_precompile_headers(ReXenonTestDriver PRIVATE "${XENONRECOMP_ROOT}/pch.h")
set_source_files_properties("ppc_tables.c" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_link_libraries(ReXenonTestDriver PRIVATE
    LibXenonAnalyse
    XenonUtils
    fmt::fmt
    tomlplusplus::tomlplusplus
    xxHash::xxhash)

target_compile_features(ReXenonTestDriver PRIVATE cxx_std_20)
target_compile_definitions(ReXenonTestDriver PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(ReXenonTestDriver PRIVATE -Wno-switch -Wno-unused-variable -Wno-null-arithmetic)
    # Same as XenonRecomp, so the generated code is what a port gets.
    if (NOT APPLE)
        target_compile_definitions(ReXenonTestDriver PRIVATE XENON_RECOMP_USE_ALIAS)
    endif()
endif()
