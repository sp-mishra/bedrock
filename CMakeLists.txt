cmake_minimum_required(VERSION 4.1)
project(bedrock LANGUAGES C CXX)

# Set the default C++ standard to C++26
set(PROJECT_NAME bedrock)

# Set the default C standard
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# C++26 configuration
# We check if the compiler supports -std=c++26 or -std=c++2c directly.
# We keep CMAKE_CXX_STANDARD at 23 for CMake internal targets / try_compile
# so CMake doesn't error out looking for an "AppleClang CXX26" dialect entry,
# while injecting the C++26 flag into compile options.
# ---------------------------------------------------------------------------
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-std=c++26" _bedrock_have_cxx26)
if(NOT _bedrock_have_cxx26)
    check_cxx_compiler_flag("-std=c++2c" _bedrock_have_cxx2c)
endif()

if(_bedrock_have_cxx26)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=c++26>)
    message(STATUS "[bedrock] C++26 enabled: using -std=c++26")
elseif(_bedrock_have_cxx2c)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=c++2c>)
    message(STATUS "[bedrock] C++26 enabled: using -std=c++2c")
else()
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    message(STATUS "[bedrock] Compiler does not support C++26 — falling back to C++23")
endif()
set(CMAKE_VERBOSE_MAKEFILE ON)

set(BUILD_EXECUTABLE ON)
set(BUILD_EXEMAIN "exemain")
set(BUILD_SHARED_LIBS OFF)
set(BUILD_SHARED OFF)

# ---------------------------------------------------------------------------
# Top-level detection
# When bedrock is consumed via add_subdirectory() by another CMake project,
# CMAKE_CURRENT_SOURCE_DIR != CMAKE_SOURCE_DIR.  We use this to flip the
# default for BUILD_TESTS so that a parent project never accidentally builds
# our test targets.  The parent can still opt-in with -DBEDROCK_BUILD_TESTS=ON.
# ---------------------------------------------------------------------------
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(BEDROCK_IS_TOP_LEVEL ON)
else()
    set(BEDROCK_IS_TOP_LEVEL OFF)
endif()

option(BUILD_TESTS "Build bedrock's tests (auto-disabled when used as a subdirectory)" ${BEDROCK_IS_TOP_LEVEL})
option(BUILD_APP   "Build the application" OFF)
option(BEDROCK_BUILD_EXAMPLES "Build Bedrock's runnable Crank and Lithe examples" OFF)
option(BEDROCK_ENABLE_PRAVAHA_EXTERNAL_ADAPTER_TESTS
       "Build Pravaha tests requiring downstream Lithe, Vulkan, or Sutra headers" OFF)

if (BEDROCK_IS_TOP_LEVEL)
    message(STATUS "[bedrock] Top-level build — BUILD_TESTS default: ${BUILD_TESTS}")
else()
    message(STATUS "[bedrock] Subdirectory build — BUILD_TESTS default: ${BUILD_TESTS} (tests suppressed)")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Catch2
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "disable building tests" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "disable building examples" FORCE)
set(CATCH_BUILD_FUZZERS OFF CACHE BOOL "disable building fuzzers" FORCE)

message("CMAKE_SOURCE_DIR location: ${CMAKE_SOURCE_DIR}")
include(FetchContent)
include(ExternalProject)

# ---------------------------------------------------------------------------
# Dependency bootstrapping via fetch_deps.sh
# ---------------------------------------------------------------------------
# option: set -DDEPS_VERBOSE=OFF to silence dependency output during configure
option(DEPS_VERBOSE "Show verbose dependency fetch output" ON)

set(FETCH_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/fetch_deps.sh")

if (NOT EXISTS "${FETCH_SCRIPT}")
    # Bedrock is intentionally usable with a pre-populated dependencies/
    # directory.  Do not require or invoke a downloader merely to configure a
    # hermetic checkout; report a precise error only if a required dependency is
    # actually absent below.
    set(FETCH_SCRIPT "")
    message(STATUS "[deps] fetch script absent; using checked-in dependencies only")
endif()

if (DEPS_VERBOSE)
    message(STATUS "[deps] ════════════════════════════════════════")
    message(STATUS "[deps] Checking dependencies in: ${CMAKE_SOURCE_DIR}/dependencies")
    message(STATUS "[deps] ════════════════════════════════════════")
endif()

# Map of library-key -> expected folder under dependencies/
set(BEDROCK_DEP_FOLDERS
    Catch2    "Catch2-3.9.0"
    crc32c    "crc32c"
    glaze     "glaze"
    liblmdb   "liblmdb"
    highway   "highway"
    spdlog    "spdlog"
    z3        "z3"
    asmjit    "asmjit"
    metal-cpp "metal-cpp"
    mlx       "mlx"
)

set(MISSING_DEPS "")
list(LENGTH BEDROCK_DEP_FOLDERS _pair_count)
math(EXPR _pair_last "${_pair_count} - 1")

foreach(_idx RANGE 0 ${_pair_last} 2)
    math(EXPR _fidx "${_idx} + 1")
    list(GET BEDROCK_DEP_FOLDERS ${_idx}  _dep_key)
    list(GET BEDROCK_DEP_FOLDERS ${_fidx} _dep_folder)
    set(_dep_path "${CMAKE_SOURCE_DIR}/dependencies/${_dep_folder}")
    if (EXISTS "${_dep_path}")
        if (DEPS_VERBOSE)
            message(STATUS "[deps]   OK       ${_dep_key} → ${_dep_path}")
        endif()
    else()
        message(STATUS "[deps]   MISSING  ${_dep_key} → ${_dep_path}")
        list(APPEND MISSING_DEPS "${_dep_key}")
    endif()
endforeach()

if (MISSING_DEPS)
    if (FETCH_SCRIPT STREQUAL "")
        list(JOIN MISSING_DEPS ", " _missing_names)
        message(FATAL_ERROR
            "Required dependencies are missing: ${_missing_names}\n"
            "This checkout has no dependency fetch script; supply them under dependencies/."
        )
    endif()
    list(LENGTH MISSING_DEPS _missing_count)
    message(STATUS "[deps] ────────────────────────────────────────")
    message(STATUS "[deps] ${_missing_count} missing — invoking fetch_deps.sh")
    message(STATUS "[deps] ────────────────────────────────────────")

    # Build command — pass --quiet when DEPS_VERBOSE is OFF
    set(_fetch_cmd bash "${FETCH_SCRIPT}")
    if (NOT DEPS_VERBOSE)
        list(APPEND _fetch_cmd "--quiet")
    endif()
    list(APPEND _fetch_cmd ${MISSING_DEPS})

    execute_process(
        COMMAND         ${_fetch_cmd}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _fetch_result
        OUTPUT_VARIABLE _fetch_stdout
        ERROR_VARIABLE  _fetch_stderr
        ECHO_OUTPUT_VARIABLE    # stream script output live to the configure log
        ECHO_ERROR_VARIABLE     # stream script errors live to the configure log
    )

    if (NOT _fetch_result EQUAL 0)
        message(FATAL_ERROR
            "[deps] fetch_deps.sh exited with code ${_fetch_result}.\n"
            "stdout:\n${_fetch_stdout}\n"
            "stderr:\n${_fetch_stderr}"
        )
    endif()

    message(STATUS "[deps] ════════════════════════════════════════")
    message(STATUS "[deps] All missing dependencies fetched successfully.")
    message(STATUS "[deps] ════════════════════════════════════════")
else()
    message(STATUS "[deps] ────────────────────────────────────────")
    message(STATUS "[deps] All dependencies already present — no download needed.")
    message(STATUS "[deps] ────────────────────────────────────────")
endif()
include_directories(/usr/local/include)
include_directories(${HOME_INCLUDE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/dependencies)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/lexy/include)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/Catch2-3.9.0/extras)
include_directories(include)
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/highway)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/glaze/include)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/liblmdb)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/z3/include)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/z3)

# Generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BUILD_EXAMPLES OFF)
set(BUILD_SHARED_LIBS OFF)

# Spdlog
set(SPDLOG_BUILD_EXAMPLES OFF)
set(SPDLOG_USE_STD_FORMAT ON)
set(SPDLOG_BUILD_SHARED OFF)
set(SPDLOG_ENABLE_PCH OFF)
set(SPDLOG_BUILD_EXAMPLE OFF)
set(SPDLOG_BUILD_TESTS OFF)
set(SPDLOG_BUILD_BENCH OFF)

# highway
set(HWY_TEST_STANDALONE OFF CACHE BOOL "Disable use of googletest" FORCE)
set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
set(HWY_ENABLE_INSTALL OFF CACHE BOOL "Install library" FORCE)
set(HWY_ENABLE_TESTS OFF CACHE BOOL "Enable HWY tests" FORCE)
set(HWY_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)

add_subdirectory(${CMAKE_SOURCE_DIR}/dependencies/spdlog)
add_subdirectory(${CMAKE_SOURCE_DIR}/dependencies/highway)

# AsmJit is an optional native Lithe backend.  Keep it as a static target so
# users of the portable Lithe core do not pay for a JIT they do not select.
set(ASMJIT_STATIC ON CACHE BOOL "Build asmjit as a static library" FORCE)
set(ASMJIT_EMBED OFF CACHE BOOL "Build asmjit as a compiled static library" FORCE)
set(ASMJIT_NO_NATVIS ON CACHE BOOL "Disable AsmJit natvis support" FORCE)
set(ASMJIT_NO_INSTALL ON CACHE BOOL "Disable AsmJit installation targets" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/dependencies/asmjit asmjit)

# crc32c
set(CRC32C_BUILD_TESTS OFF CACHE BOOL "Build CRC32C's unit tests" FORCE)
set(CRC32C_BUILD_BENCHMARKS OFF CACHE BOOL "Build CRC32C's benchmarks" FORCE)
set(CRC32C_USE_GLOG OFF CACHE BOOL "Build CRC32C's tests with Google Logging" FORCE)
set(CRC32C_INSTALL OFF CACHE BOOL "Install CRC32C's header and library" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/dependencies/crc32c)

# liblmdb
if (EXISTS "${CMAKE_SOURCE_DIR}/dependencies/liblmdb/mdb.c")
    set(LMDB_SOURCE
            "${CMAKE_SOURCE_DIR}/dependencies/liblmdb/mdb.c"
            "${CMAKE_SOURCE_DIR}/dependencies/liblmdb/midl.c"
            "${CMAKE_SOURCE_DIR}/dependencies/liblmdb/lmdb.h"
            "${CMAKE_SOURCE_DIR}/dependencies/liblmdb/midl.h"
    )
    add_library(lmdb STATIC ${LMDB_SOURCE})
    target_include_directories(lmdb PUBLIC "${CMAKE_SOURCE_DIR}/dependencies/liblmdb")
    target_link_libraries(lmdb PUBLIC Threads::Threads)
endif()

# ===================== Z3 SMT Solver (optimized static library) =========================
option(BUILD_Z3 "Build with Z3 SMT solver support" ON)

set(HAS_Z3 FALSE)
if(BUILD_Z3)
    set(Z3_DIR ${CMAKE_SOURCE_DIR}/dependencies/z3)
    if(EXISTS ${Z3_DIR}/CMakeLists.txt)
        set(Z3_BUILD_LIBZ3_SHARED    OFF CACHE BOOL "Build Z3 as static library"         FORCE)
        set(Z3_BUILD_PYTHON_BINDINGS OFF CACHE BOOL "Disable Z3 Python bindings"          FORCE)
        set(Z3_BUILD_DOCUMENTATION   OFF CACHE BOOL "Disable Z3 documentation"            FORCE)
        set(Z3_INCLUDE_GIT_HASH      OFF CACHE BOOL "Disable Z3 git hash in version"      FORCE)
        set(Z3_INCLUDE_GIT_DESCRIBE  OFF CACHE BOOL "Disable Z3 git describe in version"  FORCE)

        # Build Z3 optimized (Release) even when top-level build type is unset to ensure high performance
        set(Z3_SAVED_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(CMAKE_BUILD_TYPE Release)
        endif()

        add_subdirectory(${Z3_DIR} z3_build)

        set(CMAKE_BUILD_TYPE "${Z3_SAVED_BUILD_TYPE}")
        
        # Expose C++ and C API headers
        target_include_directories(libz3 INTERFACE
            $<BUILD_INTERFACE:${Z3_DIR}/src/api/c++>
            $<BUILD_INTERFACE:${Z3_DIR}/src/api>
            $<BUILD_INTERFACE:${Z3_DIR}/include>
        )
        set(HAS_Z3 TRUE)
        add_compile_definitions(HAS_Z3=1)
        message(STATUS "[bedrock] Z3: Building as optimized static library from dependencies/z3 (HAS_Z3=1)")
    else()
        # Fallback: check prebuilt static archives or system libs
        set(Z3_LIB "")
        if (EXISTS "${Z3_DIR}/libz3.a")
            set(Z3_LIB "${Z3_DIR}/libz3.a")
        elseif (EXISTS "${Z3_DIR}/lib/libz3.a")
            set(Z3_LIB "${Z3_DIR}/lib/libz3.a")
        elseif (EXISTS "${Z3_DIR}/libz3.dylib")
            set(Z3_LIB "${Z3_DIR}/libz3.dylib")
        elseif (EXISTS "${Z3_DIR}/lib/libz3.dylib")
            set(Z3_LIB "${Z3_DIR}/lib/libz3.dylib")
        elseif (EXISTS "${Z3_DIR}/libz3.so")
            set(Z3_LIB "${Z3_DIR}/libz3.so")
        elseif (EXISTS "${Z3_DIR}/lib/libz3.so")
            set(Z3_LIB "${Z3_DIR}/lib/libz3.so")
        endif()

        if (NOT Z3_LIB)
            find_library(Z3_SYSTEM_LIB NAMES z3 libz3 PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
            if (Z3_SYSTEM_LIB)
                set(Z3_LIB "${Z3_SYSTEM_LIB}")
            endif()
        endif()

        if (Z3_LIB)
            add_library(z3_imported UNKNOWN IMPORTED)
            set_target_properties(z3_imported PROPERTIES IMPORTED_LOCATION "${Z3_LIB}")
            if (EXISTS "${Z3_DIR}/include")
                set_target_properties(z3_imported PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${Z3_DIR}/include")
            elseif (EXISTS "${Z3_DIR}/src/api/c++")
                set_target_properties(z3_imported PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${Z3_DIR}/src/api/c++")
            elseif (EXISTS "${Z3_DIR}")
                set_target_properties(z3_imported PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${Z3_DIR}")
            endif()
            set(HAS_Z3 TRUE)
            add_compile_definitions(HAS_Z3=1)
            message(STATUS "[bedrock] Z3: Prebuilt library detected: ${Z3_LIB} (HAS_Z3=1)")
        endif()
    endif()
endif()

# ===================== MLX (Apple GPU/CPU ML framework) =======================
option(BUILD_MLX "Build with MLX support" ON)

set(HAS_MLX FALSE)
if(BUILD_MLX AND APPLE)
    set(MLX_DIR ${CMAKE_SOURCE_DIR}/dependencies/mlx)
    if(EXISTS ${MLX_DIR}/CMakeLists.txt)
        # MLX build configuration — static library, minimal footprint
        set(MLX_BUILD_TESTS OFF CACHE BOOL "Disable MLX tests" FORCE)
        set(MLX_BUILD_EXAMPLES OFF CACHE BOOL "Disable MLX examples" FORCE)
        set(MLX_BUILD_BENCHMARKS OFF CACHE BOOL "Disable MLX benchmarks" FORCE)
        set(MLX_BUILD_SAFETENSORS ON CACHE BOOL "Enable safetensors support" FORCE)
        set(MLX_BUILD_GGUF ON CACHE BOOL "Enable GGUF support" FORCE)
        set(MLX_BUILD_METAL ON CACHE BOOL "Enable Metal GPU backend" FORCE)
        set(MLX_METAL_JIT ON CACHE BOOL "Enable Metal JIT compilation" FORCE)
        set(MLX_BUILD_CPU ON CACHE BOOL "Enable CPU backend" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build MLX as static library" FORCE)

        # Use MinSizeRel or Release for MLX
        set(MLX_ORIGINAL_BUILD_TYPE ${CMAKE_BUILD_TYPE})
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(CMAKE_BUILD_TYPE MinSizeRel)
        endif()

        add_subdirectory(${MLX_DIR} mlx_build)

        # Restore original build type
        if(DEFINED MLX_ORIGINAL_BUILD_TYPE)
            set(CMAKE_BUILD_TYPE ${MLX_ORIGINAL_BUILD_TYPE})
        endif()

        include_directories(${MLX_DIR})
        set(HAS_MLX TRUE)
        add_compile_definitions(HAS_MLX=1)
        message(STATUS "[bedrock] MLX: Building as static library from dependencies/mlx")
    endif()
endif()

# ---- metal-cpp (raw Metal, macOS only) --------------------------------------
set(HAS_METAL_CPP FALSE)
if(APPLE)
    set(METAL_CPP_DIR ${CMAKE_SOURCE_DIR}/dependencies/metal-cpp)
    if(EXISTS ${METAL_CPP_DIR}/Metal/Metal.hpp)
        include_directories(${METAL_CPP_DIR})
        find_library(METAL_FRAMEWORK      Metal      REQUIRED)
        find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
        find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
        set(HAS_METAL_CPP TRUE)
        add_compile_definitions(HAS_METAL_CPP=1)
        message(STATUS "[bedrock] metal-cpp: enabled (${METAL_CPP_DIR})")
    else()
        message(STATUS "[bedrock] metal-cpp: headers not found, GPU backend disabled")
    endif()
endif()

find_package(Threads REQUIRED)

# Pebble is the reusable, dependency-light substrate.  Keep its headers out of
# the Bedrock root include directory so application libraries can state their
# dependency boundary precisely.
add_library(bedrock_pebble INTERFACE)
add_library(bedrock::pebble ALIAS bedrock_pebble)
target_include_directories(bedrock_pebble INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/pebble/include>)
target_compile_features(bedrock_pebble INTERFACE cxx_std_23)
target_link_libraries(bedrock_pebble INTERFACE Threads::Threads hwy crc32c)

# Lithe is a local compile / interpret / execute engine.  It depends on Pebble,
# never the reverse.  Backends and durable storage are separate opt-in targets.
add_library(bedrock_lithe INTERFACE)
add_library(bedrock::lithe ALIAS bedrock_lithe)
target_include_directories(bedrock_lithe INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(bedrock_lithe INTERFACE cxx_std_23)
target_link_libraries(bedrock_lithe INTERFACE bedrock::pebble)

# Native CPU JIT is opt-in.  The dependency supplies both its public headers
# and ASMJIT_STATIC to every consumer that selects this backend.
add_library(bedrock_lithe_asmjit INTERFACE)
add_library(bedrock::lithe_asmjit ALIAS bedrock_lithe_asmjit)
target_link_libraries(bedrock_lithe_asmjit INTERFACE
    bedrock::lithe
    asmjit::asmjit)

add_library(bedrock_lithe_simd INTERFACE)
add_library(bedrock::lithe_simd ALIAS bedrock_lithe_simd)
target_link_libraries(bedrock_lithe_simd INTERFACE bedrock::lithe hwy)

add_library(bedrock_lithe_petika INTERFACE)
add_library(bedrock::lithe_petika ALIAS bedrock_lithe_petika)
target_link_libraries(bedrock_lithe_petika INTERFACE bedrock::lithe bedrock::pebble)

if (HAS_METAL_CPP)
    add_library(bedrock_lithe_metal INTERFACE)
    add_library(bedrock::lithe_metal ALIAS bedrock_lithe_metal)
    target_link_libraries(bedrock_lithe_metal INTERFACE
        bedrock::lithe bedrock::pravaha
        ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
endif()

# MoltenVK is intentionally not fetched.  When its headers and Vulkan loader
# are supplied under dependencies/, this target becomes available without
# changing the Lithe public API.
option(BEDROCK_ENABLE_LITHE_VULKAN "Enable the optional Vulkan/MoltenVK Lithe backend" OFF)
if (BEDROCK_ENABLE_LITHE_VULKAN
    AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/MoltenVK"
    AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/Vulkan-Headers")
    add_library(bedrock_lithe_vulkan INTERFACE)
    add_library(bedrock::lithe_vulkan ALIAS bedrock_lithe_vulkan)
    target_include_directories(bedrock_lithe_vulkan INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/dependencies/Vulkan-Headers/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/dependencies/MoltenVK/include>)
    target_link_libraries(bedrock_lithe_vulkan INTERFACE bedrock::lithe)
endif()

# Pravaha is a header-only orchestration and heterogeneous-compute component.
# It may consume Lithe artifacts through explicit adapters, but its core never
# depends on Lithe.
add_library(bedrock_pravaha INTERFACE)
add_library(bedrock::pravaha ALIAS bedrock_pravaha)
target_include_directories(bedrock_pravaha INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/pebble/include>)
# Pravaha's public API is C++23.  The top-level project independently enables
# C++26 on Apple toolchains that support it, so consumers retain that upgrade.
target_compile_features(bedrock_pravaha INTERFACE cxx_std_23)
target_link_libraries(bedrock_pravaha INTERFACE bedrock::pebble)
if (HAS_METAL_CPP)
    target_link_libraries(bedrock_pravaha INTERFACE
        ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
endif()

add_library(bedrock_anukrama INTERFACE)
add_library(bedrock::anukrama ALIAS bedrock_anukrama)
target_include_directories(bedrock_anukrama INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/pebble/include>)
target_compile_features(bedrock_anukrama INTERFACE cxx_std_23)

add_library(bedrock_akruti INTERFACE)
add_library(bedrock::akruti ALIAS bedrock_akruti)
target_include_directories(bedrock_akruti INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/pebble/include>)
target_compile_features(bedrock_akruti INTERFACE cxx_std_23)

add_library(bedrock_prakriti INTERFACE)
add_library(bedrock::prakriti ALIAS bedrock_prakriti)
target_include_directories(bedrock_prakriti INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/pebble/include>)
target_compile_features(bedrock_prakriti INTERFACE cxx_std_23)
target_link_libraries(bedrock_prakriti INTERFACE bedrock::pebble bedrock::akruti)

add_library(bedrock_crank INTERFACE)
add_library(bedrock::crank ALIAS bedrock_crank)
target_include_directories(bedrock_crank INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(bedrock_crank INTERFACE cxx_std_23)
target_link_libraries(bedrock_crank INTERFACE
    bedrock::lithe
    bedrock::pebble
    bedrock::pravaha)


# Medha is a header-only transactional-memory component.  Its core keeps
# dependencies limited to Bedrock containers; integrations remain opt-in.
add_library(bedrock_medha INTERFACE)
add_library(bedrock::medha ALIAS bedrock_medha)
target_include_directories(bedrock_medha INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/pebble/include>)
target_compile_features(bedrock_medha INTERFACE cxx_std_23)

add_library(bedrock_medha_smriti INTERFACE)
add_library(bedrock::medha_smriti ALIAS bedrock_medha_smriti)
target_link_libraries(bedrock_medha_smriti INTERFACE bedrock::medha)

add_library(bedrock_medha_tarka INTERFACE)
add_library(bedrock::medha_tarka ALIAS bedrock_medha_tarka)
target_link_libraries(bedrock_medha_tarka INTERFACE bedrock::medha)

add_library(bedrock_medha_pravaha INTERFACE)
add_library(bedrock::medha_pravaha ALIAS bedrock_medha_pravaha)
target_link_libraries(bedrock_medha_pravaha INTERFACE bedrock::medha bedrock::pravaha)

# Current Lithe lowering is metadata-only; it has no Bedrock Lithe dependency.
add_library(bedrock_medha_lithe_metadata INTERFACE)
add_library(bedrock::medha_lithe_metadata ALIAS bedrock_medha_lithe_metadata)
target_link_libraries(bedrock_medha_lithe_metadata INTERFACE bedrock::medha)

link_directories(/usr/local/lib)
link_directories(${HOME_LIB_DIR})

SET(CATCH2_SOURCE
        "${CMAKE_SOURCE_DIR}/dependencies/Catch2-3.9.0/extras/catch_amalgamated.cpp"
        "${CMAKE_SOURCE_DIR}/dependencies/Catch2-3.9.0/extras/catch_amalgamated.hpp"
)
add_library(Catch2 STATIC ${CATCH2_SOURCE})

if (BEDROCK_BUILD_EXAMPLES)
    set(BEDROCK_EXAMPLE_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/examples/crank/main.cpp")
    if (HAS_METAL_CPP)
        list(APPEND BEDROCK_EXAMPLE_SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/src/examples/crank/metal_cpp_implementation.cpp")
    endif()
    add_executable(bedrock_examples ${BEDROCK_EXAMPLE_SOURCES})
    target_link_libraries(bedrock_examples PRIVATE
        bedrock::pebble
        bedrock::crank
        bedrock::lithe_simd
        bedrock::medha
        spdlog::spdlog_header_only)
    target_compile_features(bedrock_examples PRIVATE cxx_std_23)
endif()

if (BUILD_TESTS)
    # Core tests plus the migrated local Lithe and Crank suites.  Optional
    # backend tests retain their own feature guards, so unavailable providers
    # do not become a hard dependency of the portable test target.
    file(
            GLOB_RECURSE TEST_SOURCE
            "${CMAKE_SOURCE_DIR}/src/tests/*.cpp"
    )
    if (NOT BEDROCK_ENABLE_PRAVAHA_EXTERNAL_ADAPTER_TESTS)
        # These suites exercise downstream Lithe preset/Vulkan APIs or the
        # Turbo Twig Sutra adapter; neither belongs to Bedrock's core graph.
        list(FILTER TEST_SOURCE EXCLUDE REGEX ".*/src/tests/pravaha/(test_npravaha_.*|test_pravaha_ext)\\.cpp$")
    endif()
    add_executable(bedrock_tests ${TEST_SOURCE})
    target_include_directories(bedrock_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/src/tests/lithe")
    target_link_libraries(bedrock_tests PRIVATE
            Catch2
            bedrock::lithe
            bedrock::lithe_asmjit
            bedrock::crank
            bedrock::medha
            bedrock::medha_pravaha
            bedrock::pravaha
            spdlog::spdlog_header_only
            crc32c
    )
    if (TARGET lmdb)
        target_link_libraries(bedrock_tests PRIVATE lmdb)
    endif()
    if (TARGET libz3)
        target_link_libraries(bedrock_tests PRIVATE libz3)
    elseif (TARGET z3_imported)
        target_link_libraries(bedrock_tests PRIVATE z3_imported)
    endif()
    if (HAS_MLX AND TARGET mlx)
        target_link_libraries(bedrock_tests PRIVATE mlx)
    endif()
    if (HAS_METAL_CPP)
        target_link_libraries(bedrock_tests PRIVATE
            ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
    endif()
    target_compile_definitions(bedrock_tests PRIVATE
        BEDROCK_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    enable_testing()
    add_test(NAME bedrock_tests COMMAND bedrock_tests)
endif ()
