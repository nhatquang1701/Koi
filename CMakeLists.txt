cmake_minimum_required(VERSION 3.31)

project(koi_engine VERSION 1.2.0 LANGUAGES CXX)

# Build identity for the UCI handshake: the version is the single project()
# pin above, the commit is the checkout HEAD at configure time (best effort so
# a source tarball without .git still builds).
set(KOI_ENGINE_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE KOI_ENGINE_GIT_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE KOI_ENGINE_GIT_RESULT)
    if(KOI_ENGINE_GIT_RESULT EQUAL 0 AND KOI_ENGINE_GIT_OUTPUT)
        set(KOI_ENGINE_GIT_COMMIT "${KOI_ENGINE_GIT_OUTPUT}")
    endif()
endif()
message(STATUS "Koi build identity: ${PROJECT_VERSION}+${KOI_ENGINE_GIT_COMMIT}")

# Koi Engine supports Windows x64 with MSVC (the primary development platform),
# x86-64 Linux with GCC 14+ or Clang 18+, and macOS on Apple Silicon or Intel
# with AppleClang 18+ or Homebrew LLVM.
if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR
            "Koi Engine on Windows requires the MSVC x64 toolchain; unsupported "
            "compiler ID: ${CMAKE_CXX_COMPILER_ID}")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
        message(FATAL_ERROR "Koi Engine requires an x86-64 compiler target")
    endif()
elseif(APPLE)
    # AppleClang reports its own version series, which does not track upstream
    # LLVM.  18 is the first series CMake maps to -std=c++26, so older Xcode
    # toolchains are pointed at Homebrew LLVM, which reports itself as Clang.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
            message(FATAL_ERROR
                "Koi Engine needs a C++26-capable Clang; AppleClang "
                "${CMAKE_CXX_COMPILER_VERSION} is older than 18. Install Homebrew "
                "LLVM (brew install llvm) and reconfigure with "
                "-DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++.")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
            message(FATAL_ERROR
                "Koi Engine needs Clang 18 or newer (found "
                "${CMAKE_CXX_COMPILER_VERSION})")
        endif()
    else()
        message(FATAL_ERROR
            "Koi Engine supports Clang 18+ on macOS (AppleClang or Homebrew "
            "LLVM); unsupported compiler ID: ${CMAKE_CXX_COMPILER_ID}")
    endif()
elseif(UNIX)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        message(FATAL_ERROR
            "Koi Engine targets x86-64 Linux; detected: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
            message(FATAL_ERROR
                "Koi Engine needs GCC 14 or newer for C++26 (found "
                "${CMAKE_CXX_COMPILER_VERSION})")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
            message(FATAL_ERROR
                "Koi Engine needs Clang 18 or newer (found "
                "${CMAKE_CXX_COMPILER_VERSION})")
        endif()
    else()
        message(FATAL_ERROR
            "Koi Engine supports GCC 14+ or Clang 18+ on Linux; unsupported "
            "compiler ID: ${CMAKE_CXX_COMPILER_ID}")
    endif()
else()
    message(FATAL_ERROR
        "Koi Engine targets Windows x64, x86-64 Linux, and macOS")
endif()

# The AVX2/AVX-512 variants and their -m flags exist only on x86-64.  macOS
# arm64 (and any other non-x86 target) builds the scalar baseline alone; every
# AVX-specific target and flag below is gated on this variable.
if(WIN32)
    set(KOI_TARGET_X86_64 TRUE)
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
       AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        set(KOI_TARGET_X86_64 TRUE)
    else()
        set(KOI_TARGET_X86_64 FALSE)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(KOI_TARGET_X86_64 TRUE)
else()
    set(KOI_TARGET_X86_64 FALSE)
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Koi Engine requires a 64-bit toolchain")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC AND NOT DEFINED CMAKE_CXX26_STANDARD_COMPILE_OPTION)
    set(CMAKE_CXX26_STANDARD_COMPILE_OPTION "/std:c++latest")
endif()

# --- AddressSanitizer (Windows/MSVC) ------------------------------------------
# The sanitizer build exists to catch the memory errors behind the intermittent
# crashes (for example the transposition-table storage resizes). It is wired for
# MSVC only: the ASan runtime ships as a DLL, so the static CRT is impossible,
# and /RTC (a Debug default) as well as LTCG must be disabled.
option(KOI_SANITIZE "Build with AddressSanitizer (MSVC only)" OFF)

if(KOI_SANITIZE AND NOT MSVC)
    message(FATAL_ERROR "KOI_SANITIZE is currently wired for MSVC builds only")
endif()

# --- ThreadSanitizer (Linux/GCC) -----------------------------------------------
# KOI_TSAN only disables Release IPO for the TSan CI leg; the sanitizer flags
# themselves are passed by that workflow via CMAKE_CXX_FLAGS.
option(KOI_TSAN "Disable Release IPO for a ThreadSanitizer build" OFF)

# --- PEXT slider attacks (x86-64) ----------------------------------------------
# The AVX2/AVX-512 variant cores answer slider queries from PEXT bitboard
# tables when this is ON; the baseline core always keeps the ray path, and
# non-x86 targets (macOS arm64) are unaffected. Turning it OFF builds every
# variant with the ray path so the CI can test the fallback configuration.
option(KOI_PEXT_ATTACKS "Use PEXT bitboards for slider attacks in x86-64 variants" ON)

# --- Profile-guided optimization (MSVC, Clang/AppleClang, GCC) ------------------
# KOI_PGO=generate builds Release with PGO instrumentation; KOI_PGO=use rebuilds
# Release using the collected profile data (passed via KOI_PGO_PROFILE_FILE for
# Clang/AppleClang .profdata or optional GCC profile directory). Profile data is
# a build artifact (/build/ keeps it out of the repository) and CI never sets
# KOI_PGO; tools/README.md documents the per-compiler workflows.
set(KOI_PGO "OFF" CACHE STRING "Profile-guided optimization mode (OFF, generate, use)")
set_property(CACHE KOI_PGO PROPERTY STRINGS OFF generate use)
set(KOI_PGO_PROFILE_FILE "" CACHE FILEPATH
    "Merged profile data file (.profdata for Clang/AppleClang) or profile directory (GCC) for KOI_PGO=use")
set(KOI_PGO_MANAGES_LTO FALSE)
if(NOT KOI_PGO STREQUAL "OFF" AND NOT KOI_PGO STREQUAL "generate" AND NOT KOI_PGO STREQUAL "use")
    message(FATAL_ERROR "KOI_PGO must be OFF, generate, or use")
endif()
if(NOT KOI_PGO STREQUAL "OFF" AND (KOI_SANITIZE OR KOI_TSAN))
    message(FATAL_ERROR "KOI_PGO cannot be combined with KOI_SANITIZE or KOI_TSAN")
endif()
if(KOI_PGO STREQUAL "generate")
    if(MSVC)
        add_compile_options("$<$<CONFIG:Release>:/GL>")
        # /LTCG:PGI is the linker alias for "/LTCG /GENPROFILE"; /GENPROFILE and
        # /USEPROFILE are link.exe options and are rejected by cl.exe (D9002).
        add_link_options("$<$<CONFIG:Release>:/LTCG:PGI>")
        set(KOI_PGO_MANAGES_LTO TRUE)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options("$<$<CONFIG:Release>:-fprofile-instr-generate>")
        add_link_options("$<$<CONFIG:Release>:-fprofile-instr-generate>")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if(KOI_PGO_PROFILE_FILE)
            add_compile_options("$<$<CONFIG:Release>:-fprofile-generate=${KOI_PGO_PROFILE_FILE}>")
            add_link_options("$<$<CONFIG:Release>:-fprofile-generate=${KOI_PGO_PROFILE_FILE}>")
        else()
            add_compile_options("$<$<CONFIG:Release>:-fprofile-generate>")
            add_link_options("$<$<CONFIG:Release>:-fprofile-generate>")
        endif()
    else()
        message(FATAL_ERROR "KOI_PGO is unsupported for compiler ${CMAKE_CXX_COMPILER_ID}")
    endif()
    message(STATUS "Koi Engine PGO instrumentation enabled (Release)")
elseif(KOI_PGO STREQUAL "use")
    if(MSVC)
        add_compile_options("$<$<CONFIG:Release>:/GL>")
        add_link_options("$<$<CONFIG:Release>:/LTCG>" "$<$<CONFIG:Release>:/USEPROFILE>")
        set(KOI_PGO_MANAGES_LTO TRUE)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(NOT KOI_PGO_PROFILE_FILE OR NOT EXISTS "${KOI_PGO_PROFILE_FILE}")
            message(FATAL_ERROR
                "KOI_PGO=use on Clang/AppleClang requires KOI_PGO_PROFILE_FILE "
                "to point to an existing merged .profdata file")
        endif()
        add_compile_options(
            "$<$<CONFIG:Release>:-fprofile-instr-use=${KOI_PGO_PROFILE_FILE}>"
            "$<$<CONFIG:Release>:-Wno-profile-instr-out-of-date>"
            "$<$<CONFIG:Release>:-Wno-profile-instr-unprofiled>"
        )
        add_link_options("$<$<CONFIG:Release>:-fprofile-instr-use=${KOI_PGO_PROFILE_FILE}>")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if(KOI_PGO_PROFILE_FILE)
            add_compile_options(
                "$<$<CONFIG:Release>:-fprofile-use=${KOI_PGO_PROFILE_FILE}>"
                "$<$<CONFIG:Release>:-fprofile-correction>"
            )
            add_link_options(
                "$<$<CONFIG:Release>:-fprofile-use=${KOI_PGO_PROFILE_FILE}>"
                "$<$<CONFIG:Release>:-fprofile-correction>"
            )
        else()
            add_compile_options(
                "$<$<CONFIG:Release>:-fprofile-use>"
                "$<$<CONFIG:Release>:-fprofile-correction>"
            )
            add_link_options(
                "$<$<CONFIG:Release>:-fprofile-use>"
                "$<$<CONFIG:Release>:-fprofile-correction>"
            )
        endif()
    else()
        message(FATAL_ERROR "KOI_PGO is unsupported for compiler ${CMAKE_CXX_COMPILER_ID}")
    endif()
    message(STATUS "Koi Engine PGO optimization enabled (Release)")
endif()

if(MSVC)
    if(KOI_SANITIZE)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT)
        message(STATUS "Koi Engine AddressSanitizer build enabled (dynamic CRT, no /RTC, no LTO)")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        # Control Flow Guard, CET shadow-stack compatibility, high-entropy ASLR,
        # and scrubbed PDB paths harden release binaries and reduce false-positive
        # heuristic flags on Windows.
        add_compile_options(/guard:cf)
        add_link_options(
            /GUARD:CF
            /DYNAMICBASE
            /HIGHENTROPYVA
            /NXCOMPAT
            /CETCOMPAT
            "$<$<CONFIG:Release>:/PDBALTPATH:%_PDB%>"
        )
    endif()
    # CMake's MSVC Release default injects /Ob2; strip it so /Ob3 below does not
    # emit D9025 override warnings on every translation unit.
    string(REPLACE "/Ob2" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REPLACE "/Ob2" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    add_compile_options(
        /EHsc
        /permissive-
        /utf-8
        "$<$<CONFIG:Release>:/O2>"
        "$<$<CONFIG:Release>:/Ob3>"
        "$<$<CONFIG:Release>:/DNDEBUG>"
    )
    # The recursive search can legitimately descend through check, singular,
    # and quiet-forcing extensions. A 1 MiB default stack is not enough for a
    # deep tactical line, so reserve a larger stack for the image (worker
    # threads inherit this reserve on Windows).
    add_link_options("/STACK:16777216")
else()
    add_compile_options(
        "$<$<CONFIG:Release>:-O2>"
        "$<$<CONFIG:Release>:-O3>"
        "$<$<CONFIG:Release>:-DNDEBUG>"
        "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=."
    )
    find_package(Threads REQUIRED)
    # glibc ignores the ELF stack-size header, so the engine raises the default
    # worker-thread stack at startup instead (see KOI_LINUX_STACK_BYTES in
    # src/main.cpp); the main thread follows the shell's `ulimit -s`.
endif()

# Prebuilt Linux binaries should run on any modern distribution, so the C++
# runtime is linked statically by default. The C library stays dynamic for the
# portable prebuilt configuration. Sanitizer builds should pass
# -DKOI_STATIC_RUNTIME=OFF. macOS links libc++/libSystem dynamically and has no
# static runtime pair, so the option defaults off and stays off there.
if(MSVC OR APPLE)
    set(KOI_STATIC_RUNTIME_DEFAULT OFF)
else()
    set(KOI_STATIC_RUNTIME_DEFAULT ON)
endif()
option(KOI_STATIC_RUNTIME
    "Link the C++ runtime statically for portable Linux binaries"
    ${KOI_STATIC_RUNTIME_DEFAULT})
if(APPLE AND KOI_STATIC_RUNTIME)
    message(FATAL_ERROR
        "KOI_STATIC_RUNTIME is not supported on macOS: libc++ has no static "
        "runtime to link. Reconfigure with -DKOI_STATIC_RUNTIME=OFF.")
endif()
if(KOI_STATIC_RUNTIME AND NOT MSVC)
    add_link_options("-static-libstdc++" "-static-libgcc")
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT KOI_IPO_SUPPORTED OUTPUT KOI_IPO_ERROR LANGUAGES CXX)
if(KOI_IPO_SUPPORTED AND NOT KOI_SANITIZE AND NOT KOI_PGO_MANAGES_LTO AND NOT KOI_TSAN)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
elseif(KOI_PGO_MANAGES_LTO)
    message(STATUS "Koi Engine PGO manages the Release IPO/LTO flags")
elseif(KOI_IPO_SUPPORTED)
    message(STATUS "Koi Engine Release IPO/LTO is disabled for the sanitizer build")
else()
    message(WARNING "Koi Engine Release IPO/LTO is unavailable: ${KOI_IPO_ERROR}")
endif()

include(CTest)
enable_testing()

# Shared helpers for the C++ test binaries: assertion helpers and the absolute
# location of the repository test data so no test has to walk `__FILE__`.
add_library(koi_test_support INTERFACE)
target_include_directories(koi_test_support INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
)
target_compile_definitions(koi_test_support INTERFACE
    KOI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)

set(KOI_CORE_SOURCES
    src/koi/detail/compatibility_mirror.cpp
    src/koi/detail/consistency.cpp
    src/koi/detail/debug_log.cpp
    src/koi/detail/fen_validation.cpp
    src/koi/detail/nnue_io.cpp
    src/koi/detail/root_fallback.cpp
    src/koi/detail/uci_support.cpp
    src/koi/detail/attack_tables.cpp
    src/koi/detail/evaluation_context.cpp
    src/koi/detail/feature_state.cpp
    src/koi/detail/root_coordinator.cpp
    src/koi/detail/search_context.cpp
    src/koi/detail/search_context_support.cpp
    src/koi/detail/search_ordering_tables.cpp
    src/koi/detail/search_runner.cpp
    src/koi/detail/search_session.cpp
    src/koi/classical_evaluator.cpp
    src/koi/completion_gate.cpp
    src/koi/cpu_variant.cpp
    src/koi/executable_path.cpp
    src/koi/evaluation_features.cpp
    src/koi/game_state.cpp
    src/koi/move.cpp
    src/koi/move_annotation.cpp
    src/koi/nnue.cpp
    src/koi/opening_book.cpp
    src/koi/perft.cpp
    src/koi/position.cpp
    src/koi/search_service.cpp
    src/koi/search_types.cpp
    src/koi/search_ordering.cpp
    src/koi/static_exchange.cpp
    src/koi/syzygy_tablebase.cpp
    src/koi/time_manager.cpp
    src/koi/transposition_table.cpp
    src/koi/uci_controller.cpp
)

set(KOI_MODULE_SOURCES
    src/koi/modules/types.ixx
    src/koi/modules/position.ixx
    src/koi/modules/eval.ixx
    src/koi/modules/tablebase.ixx
    src/koi/modules/search.ixx
    src/koi/modules/runtime.ixx
    src/koi/modules/koi.ixx
)

# Fathom is vendored C; compiling it as C++ keeps the toolchain single-language.
set_source_files_properties(third_party/fathom/tbprobe.c PROPERTIES LANGUAGE CXX)
if(NOT MSVC)
    set_source_files_properties(third_party/fathom/tbprobe.c
        PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations")
endif()

# Translates the MSVC /arch flag used by the Windows build into the GCC/Clang
# equivalent so both platforms share one set of call sites. The AVX-512 flag set
# is the x86-64-v4 baseline (F/BW/CD/DQ/VL), matching MSVC /arch:AVX512.  Off
# x86-64 (macOS arm64) the AVX flags do not exist and the output is empty.
function(koi_arch_compile_option msvc_flag output)
    if(msvc_flag STREQUAL "")
        set(${output} "" PARENT_SCOPE)
    elseif(NOT KOI_TARGET_X86_64)
        set(${output} "" PARENT_SCOPE)
    elseif(MSVC)
        set(${output} "${msvc_flag}" PARENT_SCOPE)
    elseif(msvc_flag STREQUAL "/arch:AVX2")
        set(${output} "-mavx2" PARENT_SCOPE)
    elseif(msvc_flag STREQUAL "/arch:AVX512")
        set(${output}
            "-mavx512f;-mavx512bw;-mavx512cd;-mavx512dq;-mavx512vl" PARENT_SCOPE)
    else()
        message(FATAL_ERROR "Unknown architecture flag: ${msvc_flag}")
    endif()
endfunction()

# CPU detection is compiled for the baseline instruction set and shared by every
# variant: a binary built for AVX2/AVX-512 must be able to ask whether the host
# supports its instruction set before executing anything else.
# Benchmark/strength fixtures are tooling, not part of the engine runtime.  Keep
# them out of koi_core so the engine target contains only engine code; koi-bench
# and the strength tests link this small static library explicitly.
add_library(koi_strength_suite STATIC
    src/koi/strength_suite.cpp
)
target_include_directories(koi_strength_suite PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_library(koi_cpu_features OBJECT src/koi/cpu_features.cpp)
target_include_directories(koi_cpu_features PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# GCC and Clang vectorize the library's static initializers with the instructions
# enabled by the build flags, so a variant binary can execute an unsupported
# instruction before main() reaches its gate.  These baseline objects install a
# pre-main guard (constructor priority 101) into the variant executables, where
# MSVC needs no equivalent because its startup code stays baseline-safe.  The
# AVX variants only exist on x86-64.
if(NOT MSVC AND KOI_TARGET_X86_64)
    add_library(koi_cpu_guard_avx2 OBJECT src/koi/cpu_variant_guard.cpp)
    target_include_directories(koi_cpu_guard_avx2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(koi_cpu_guard_avx2 PRIVATE KOI_CPU_REQUIRES_AVX2=1)
    add_library(koi_cpu_guard_avx512 OBJECT src/koi/cpu_variant_guard.cpp)
    target_include_directories(koi_cpu_guard_avx512 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(koi_cpu_guard_avx512 PRIVATE KOI_CPU_REQUIRES_AVX512=1)
endif()

# Defines one static library from the shared core sources. `arch_option` is the
# MSVC /arch flag applied in Release only (empty for the baseline variant);
# koi_arch_compile_option maps it to the GCC/Clang equivalent. Named modules are
# attached to the default koi_core target alone: no implementation file imports
# them, so the engine variants do not need a second module provider.
function(koi_add_core_target target arch_option)
    add_library(${target} ${KOI_CORE_SOURCES})
    target_include_directories(${target} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/chess-library
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fathom
    )
    target_sources(${target} PRIVATE third_party/fathom/tbprobe.c)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:koi_cpu_features>)
    add_dependencies(${target} koi_cpu_features)
    target_compile_definitions(${target} PUBLIC
        KOI_ENGINE_BUILD_VERSION="${PROJECT_VERSION}"
        KOI_ENGINE_BUILD_COMMIT="${KOI_ENGINE_GIT_COMMIT}"
    )
    if(NOT MSVC)
        target_link_libraries(${target} PUBLIC Threads::Threads)
    endif()
    if(arch_option)
        koi_arch_compile_option("${arch_option}" KOI_ARCH_COMPILE_OPTION)
        target_compile_options(${target} PRIVATE
            "$<$<CONFIG:Release>:${KOI_ARCH_COMPILE_OPTION}>")
        if(KOI_PEXT_ATTACKS)
            target_compile_definitions(${target} PRIVATE
                "$<$<CONFIG:Release>:KOI_PEXT_ATTACKS=1>")
            if(NOT MSVC)
                # -mavx2 does not imply BMI2 on GCC/Clang.
                target_compile_options(${target} PRIVATE
                    "$<$<CONFIG:Release>:-mbmi2>")
            endif()
        endif()
    endif()
endfunction()

# koi_core is the AVX2 build on x86-64 and the scalar baseline elsewhere; the
# developer tools and every test binary link it.
if(KOI_TARGET_X86_64)
    koi_add_core_target(koi_core "/arch:AVX2")
else()
    koi_add_core_target(koi_core "")
endif()

# C++26 named modules are a verified part of the MSVC build. GCC's module
# implementation is still experimental and Clang's module scanning depends on
# the toolchain, so the interface is auto-detected here and can always be
# disabled with -DKOI_BUILD_MODULES=OFF. Nothing in the core library imports
# the modules, so the engine itself never depends on them.
if(MSVC)
    set(KOI_MODULES_DEFAULT ON)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    # AppleClang's C++26 named modules are not wired into this build yet.
    set(KOI_MODULES_DEFAULT OFF)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang"
        AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 18)
    set(KOI_MODULES_DEFAULT ON)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
        AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
    set(KOI_MODULES_DEFAULT ON)
else()
    set(KOI_MODULES_DEFAULT OFF)
endif()
option(KOI_BUILD_MODULES
    "Build the C++26 named-module interface and its test"
    ${KOI_MODULES_DEFAULT})
if(KOI_BUILD_MODULES)
    target_sources(koi_core PUBLIC
        FILE_SET CXX_MODULES TYPE CXX_MODULES FILES ${KOI_MODULE_SOURCES}
    )
else()
    message(STATUS
        "KOI_BUILD_MODULES=OFF: skipping the C++26 module interface "
        "(${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
endif()

# koi_core_avx512 backs koi-engine-avx512.exe on x86-64 hosts.
if(KOI_TARGET_X86_64)
    koi_add_core_target(koi_core_avx512 "/arch:AVX512")
endif()

# koi_core_generic backs the baseline koi-engine executable that runs on any
# host CPU (x64 or arm64).
# Fathom's vendored popcount selects `_mm_popcnt_u64` unconditionally on MSVC
# x64, so the baseline build asks for its software fallback; the standard
# library's own std::popcount/countr_zero already dispatch on __isa_available.
koi_add_core_target(koi_core_generic "")

# Deterministic evaluator + controller shim used by lightweight unit tests that
# construct UciController directly without loading an NNUE file from disk.
add_library(koi_material_evaluator_test_support STATIC
    tests/support/uci_controller_test_constructor.cpp
    tests/support/nnue_synthetic.cpp
)
target_link_libraries(koi_material_evaluator_test_support PUBLIC koi_core)

target_compile_definitions(koi_core_generic PRIVATE TB_NO_HW_POP_COUNT)

option(KOI_BUILD_SHADOW_DIFF
    "Build the optional chess-library differential oracle tests" OFF)

# Release and Debug binaries do not deserve the same budgets: the Debug build is
# several times slower and the process tests launch real searches. The scale
# defaults to 3 for Debug trees (and 1 otherwise) so that only genuine hangs
# trip the limits instead of expected slowness; set -DKOI_TIMEOUT_SCALE=N to
# override it explicitly.
set(KOI_TEST_TIMEOUT_SECONDS "600" CACHE STRING
    "Default timeout in seconds for tests without an explicit budget")
set(KOI_TIMEOUT_SCALE "auto" CACHE STRING
    "Multiplier applied to explicit process-test and default timeouts (auto: 3 for Debug, else 1)")
if(KOI_TIMEOUT_SCALE STREQUAL "auto")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(KOI_TIMEOUT_SCALE "3")
    else()
        set(KOI_TIMEOUT_SCALE "1")
    endif()
endif()

function(koi_set_timeout test_name release_seconds)
    math(EXPR KOI_SCALED_TIMEOUT "${release_seconds} * ${KOI_TIMEOUT_SCALE}")
    set_tests_properties(${test_name} PROPERTIES TIMEOUT ${KOI_SCALED_TIMEOUT})
endfunction()

function(koi_add_unit_test target_name source_relpath)
    add_executable(${target_name} "${source_relpath}")
    target_link_libraries(${target_name} PRIVATE koi_core koi_test_support ${ARGN})
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

if(KOI_BUILD_SHADOW_DIFF)
    add_executable(koi_shadow_diff_tests
        tests/integration/rules/native_shadow_diff_tests.cpp
    )
    target_link_libraries(koi_shadow_diff_tests PRIVATE koi_core koi_test_support)
    target_include_directories(koi_shadow_diff_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/chess-library
    )
    add_test(NAME koi_shadow_diff_tests COMMAND koi_shadow_diff_tests)
endif()

koi_add_unit_test(koi_core_tests tests/unit/core/koi_core_tests.cpp)
koi_add_unit_test(evaluation_architecture_tests tests/unit/evaluation/evaluation_architecture_tests.cpp)
koi_add_unit_test(classical_evaluator_tests tests/unit/evaluation/classical_evaluator_tests.cpp)
koi_add_unit_test(nnue_boundary_tests tests/unit/evaluation/nnue_boundary_tests.cpp koi_material_evaluator_test_support)
koi_add_unit_test(koi_cpu_features_tests tests/unit/runtime/cpu_features_tests.cpp)
koi_add_unit_test(koi_cpu_variant_tests tests/unit/runtime/cpu_variant_tests.cpp)

if(KOI_BUILD_MODULES)
    add_executable(koi_module_tests
        tests/unit/core/koi_module_tests.cpp
    )
    target_link_libraries(koi_module_tests PRIVATE koi_core koi_test_support)
    if(KOI_TARGET_X86_64)
        # The default koi_core module BMI is built with AVX2 in Release. Clang
        # rejects imports into a translation unit with a different target feature,
        # so compile the module consumer for the same ISA (Debug stays baseline).
        koi_arch_compile_option("/arch:AVX2" KOI_MODULE_TEST_ARCH_OPTION)
        target_compile_options(koi_module_tests PRIVATE
            "$<$<CONFIG:Release>:${KOI_MODULE_TEST_ARCH_OPTION}>"
        )
        if(KOI_PEXT_ATTACKS AND NOT MSVC)
            # Match the PEXT-enabled koi_core ISA in the same way.
            target_compile_options(koi_module_tests PRIVATE
                "$<$<CONFIG:Release>:-mbmi2>")
        endif()
    endif()
    add_test(NAME koi_module_tests COMMAND koi_module_tests)
endif()

koi_add_unit_test(syzygy_tablebase_tests tests/unit/runtime/syzygy_tablebase_tests.cpp)
koi_add_unit_test(koi_rules_tests tests/unit/rules/koi_rules_tests.cpp)
koi_add_unit_test(native_rule_state_tests tests/unit/rules/native_rule_state_tests.cpp)
target_include_directories(native_rule_state_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/chess-library
)
koi_add_unit_test(opening_book_tests tests/unit/runtime/opening_book_tests.cpp)
koi_add_unit_test(uci_controller_tests tests/unit/runtime/uci_controller_tests.cpp koi_material_evaluator_test_support)
koi_add_unit_test(completion_gate_tests tests/unit/runtime/completion_gate_tests.cpp)

add_executable(koi_search_tests
    tests/unit/search/koi_search_tests.cpp
)
target_link_libraries(koi_search_tests PRIVATE koi_core koi_test_support)

# The search suite is by far the largest target. Run it as independent shards so
# a multi-core host finishes it in parallel, and so each shard stays well inside
# the per-test timeout even in the slower Debug configuration.
foreach(KOI_SEARCH_SHARD_INDEX RANGE 0 3)
    math(EXPR KOI_SEARCH_SHARD_LABEL "${KOI_SEARCH_SHARD_INDEX} + 1")
    add_test(
        NAME koi_search_tests_${KOI_SEARCH_SHARD_LABEL}of4
        COMMAND koi_search_tests --shard=${KOI_SEARCH_SHARD_INDEX}/4
    )
endforeach()

koi_add_unit_test(search_ordering_tests tests/unit/search/search_ordering_tests.cpp)
koi_add_unit_test(search_architecture_tests tests/unit/search/search_architecture_tests.cpp)
koi_add_unit_test(search_policy_tests tests/unit/search/search_policy_tests.cpp)
koi_add_unit_test(search_runtime_tests tests/unit/search/search_runtime_tests.cpp)
koi_add_unit_test(static_exchange_tests tests/unit/search/static_exchange_tests.cpp)
koi_add_unit_test(transposition_table_tests tests/unit/runtime/transposition_table_tests.cpp)
koi_add_unit_test(search_service_tests tests/unit/runtime/search_service_tests.cpp)

# The soak suite drives the real search lifecycle with real threads and real
# wall clock (long replays past the snapshot window, hash resizes under a live
# search, ponder cycles, a thread-count sweep).  It is bounded to a few seconds
# so the sanitizer and no-retry flake jobs can run it too.
koi_add_unit_test(koi_soak_tests tests/unit/runtime/koi_soak_tests.cpp)
koi_set_timeout(koi_soak_tests 120)

koi_add_unit_test(evaluation_features_tests tests/unit/evaluation/evaluation_features_tests.cpp)
koi_add_unit_test(time_manager_tests tests/unit/search/time_manager_tests.cpp)
koi_add_unit_test(koi_strength_tests tests/unit/search/koi_strength_tests.cpp koi_strength_suite)
koi_add_unit_test(perft_tests tests/unit/rules/perft_tests.cpp)

add_executable(koi_replay_tests
    tests/integration/tools/koi_replay_tests.cpp
)
target_link_libraries(koi_replay_tests PRIVATE koi_test_support)
add_dependencies(koi_replay_tests koi_replay)
add_test(
    NAME koi_replay_tests
    COMMAND koi_replay_tests "$<TARGET_FILE:koi_replay>"
)

add_executable(koi_nnue_fixture_tests
    tests/integration/tools/koi_nnue_fixture_tests.cpp
)
target_link_libraries(koi_nnue_fixture_tests PRIVATE koi_core koi_test_support)
add_test(
    NAME koi_nnue_fixture_tests
    COMMAND koi_nnue_fixture_tests
            "$<TARGET_FILE:koi_nnue_fixture>"
)

add_executable(uci_match_fixture
    tests/integration/tools/uci_match_fixture.cpp
)

add_executable(koi_perft
    tools/engine/perft.cpp
)

target_link_libraries(koi_perft PRIVATE koi_core)
set_target_properties(koi_perft PROPERTIES OUTPUT_NAME "koi-perft")

add_executable(koi_bench
    tools/engine/koi_bench.cpp
)

target_link_libraries(koi_bench PRIVATE koi_core koi_strength_suite)
set_target_properties(koi_bench PROPERTIES OUTPUT_NAME "koi-bench")

add_executable(koi_replay
    tools/engine/koi_replay.cpp
)

target_link_libraries(koi_replay PRIVATE koi_core)
set_target_properties(koi_replay PROPERTIES OUTPUT_NAME "koi-replay")

add_executable(koi_eval_features
    tools/engine/koi_eval_features.cpp
)

target_link_libraries(koi_eval_features PRIVATE koi_core)
set_target_properties(koi_eval_features PROPERTIES OUTPUT_NAME "koi-eval-features")

add_executable(koi_nnue_fixture
    tools/engine/koi_nnue_fixture.cpp
)
target_link_libraries(koi_nnue_fixture PRIVATE koi_core koi_material_evaluator_test_support)
set_target_properties(koi_nnue_fixture PROPERTIES OUTPUT_NAME "koi-nnue-fixture")
add_dependencies(koi_nnue_fixture_tests koi_nnue_fixture)

# Embed PE VersionInfo metadata and an asInvoker application manifest on Windows
# shipping binaries so Defender, SmartScreen, and file Properties identify them
# cleanly as Koi Chess Engine without heuristic elevation guesses.
function(koi_configure_shipping_metadata target file_description output_filename)
    if(MSVC)
        set(KOI_PE_FILE_DESCRIPTION "${file_description}")
        set(KOI_PE_INTERNAL_NAME "${target}")
        set(KOI_PE_ORIGINAL_FILENAME "${output_filename}")
        set(generated_rc "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}.rc")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/src/koi/resources/koi_engine.rc.in"
            "${generated_rc}"
            @ONLY
        )
        target_sources(${target} PRIVATE
            "${generated_rc}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/koi/resources/koi_engine.manifest"
        )
    endif()
endfunction()

# Engine flavors: the baseline `koi-engine` selects the best sibling at startup,
# while on x86-64 the AVX2/AVX-512 binaries gate themselves when launched
# directly on a CPU that cannot execute them without linking process-launch APIs.
# macOS arm64 builds the baseline alone because those instruction sets do not
# exist there.
add_executable(koi_engine
    src/main.cpp
    src/cli_support.cpp
    src/koi/cpu_variant_selector.cpp
)

target_link_libraries(koi_engine PRIVATE koi_core_generic)
target_compile_definitions(koi_engine PRIVATE KOI_CPU_SELECTOR=1)
set_target_properties(koi_engine PROPERTIES OUTPUT_NAME "koi-engine")
koi_configure_shipping_metadata(koi_engine "Koi UCI Chess Engine" "koi-engine.exe")

if(KOI_TARGET_X86_64)
    add_executable(koi_engine_avx2
        src/main.cpp
        src/cli_support.cpp
    )

    target_link_libraries(koi_engine_avx2 PRIVATE koi_core)
    target_compile_definitions(koi_engine_avx2 PRIVATE KOI_CPU_REQUIRES_AVX2=1)
    if(NOT MSVC)
        target_sources(koi_engine_avx2 PRIVATE $<TARGET_OBJECTS:koi_cpu_guard_avx2>)
        add_dependencies(koi_engine_avx2 koi_cpu_guard_avx2)
    endif()
    set_target_properties(koi_engine_avx2 PROPERTIES OUTPUT_NAME "koi-engine-avx2")
    koi_configure_shipping_metadata(
        koi_engine_avx2 "Koi UCI Chess Engine (AVX2)" "koi-engine-avx2.exe")

    add_executable(koi_engine_avx512
        src/main.cpp
        src/cli_support.cpp
    )

    target_link_libraries(koi_engine_avx512 PRIVATE koi_core_avx512)
    target_compile_definitions(koi_engine_avx512 PRIVATE KOI_CPU_REQUIRES_AVX512=1)
    if(NOT MSVC)
        target_sources(koi_engine_avx512 PRIVATE $<TARGET_OBJECTS:koi_cpu_guard_avx512>)
        add_dependencies(koi_engine_avx512 koi_cpu_guard_avx512)
    endif()
    set_target_properties(koi_engine_avx512 PROPERTIES OUTPUT_NAME "koi-engine-avx512")
    koi_configure_shipping_metadata(
        koi_engine_avx512 "Koi UCI Chess Engine (AVX-512)" "koi-engine-avx512.exe")
endif()

# PowerShell drives the process-level integration tests. It ships on Windows and
# on GitHub's Linux runners, but the C++ and Python suites already cover the
# engine, so a machine without pwsh still configures and tests the build.
find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell)
if(NOT POWERSHELL_EXECUTABLE)
    message(STATUS "pwsh not found; the PowerShell process tests are omitted")
endif()

if(POWERSHELL_EXECUTABLE)

function(koi_add_ps_test test_name script_relpath release_seconds)
    add_test(
        NAME ${test_name}
        COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
                "${CMAKE_CURRENT_SOURCE_DIR}/${script_relpath}"
                ${ARGN}
    )
    if(release_seconds GREATER 0)
        koi_set_timeout(${test_name} ${release_seconds})
    endif()
endfunction()

function(koi_add_engine_ps_test test_name script_relpath release_seconds)
    koi_add_ps_test(${test_name} "${script_relpath}" ${release_seconds}
        -EnginePath "$<TARGET_FILE:koi_engine>" ${ARGN})
endfunction()

koi_add_engine_ps_test(koi_engine_process tests/integration/uci/uci_process_test.ps1 60)
koi_add_engine_ps_test(koi_hce_process tests/integration/uci/hce_process_test.ps1 60)
koi_add_ps_test(koi_hce_bench_process tests/integration/tools/hce_bench_process_test.ps1 240
    -BenchPath "$<TARGET_FILE:koi_bench>")
koi_add_ps_test(koi_hce_eval_features_process tests/integration/tools/hce_eval_features_process_test.ps1 60
    -ToolPath "$<TARGET_FILE:koi_eval_features>")
koi_add_engine_ps_test(koi_protocol_fuzz_process tests/integration/uci/protocol_fuzz_test.ps1 180)
koi_add_engine_ps_test(koi_protocol_soak_process tests/integration/uci/protocol_soak_test.ps1 300)
koi_add_engine_ps_test(koi_protocol_option_churn_process tests/integration/uci/protocol_option_churn_test.ps1 180)
koi_add_engine_ps_test(koi_protocol_broken_pipe_process tests/integration/uci/protocol_broken_pipe_test.ps1 60)

# Architecture-boundary guard for the 1.0.0 baseline (public-header seams,
# core target purity, test-tree placement, documented contract).  It reads the
# source/CMake tree only and must stay fast.
koi_add_ps_test(koi_architecture_boundaries tests/integration/packaging/architecture_boundaries_test.ps1 60)
set_tests_properties(koi_architecture_boundaries PROPERTIES LABELS "packaging;architecture")

koi_add_engine_ps_test(koi_engine_perft_responsiveness tests/integration/uci/perft_responsiveness_test.ps1 15)
koi_add_ps_test(run_tests_artifact_path tests/integration/tools/run_tests_artifact_path_test.ps1 30
    -RunTestsPath "${CMAKE_CURRENT_SOURCE_DIR}/tools/test/run_tests.ps1"
    -CMakePath "${CMAKE_COMMAND}")
set_tests_properties(run_tests_artifact_path PROPERTIES
    LABELS "integration;tools"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# The variant selector test needs the x86-64 AVX2/AVX-512 siblings, which are
# not built on arm64.
if(KOI_TARGET_X86_64)
    koi_add_engine_ps_test(koi_engine_variant_process tests/integration/uci/engine_variant_process_test.ps1 60
        -Avx2EnginePath "$<TARGET_FILE:koi_engine_avx2>"
        -Avx512EnginePath "$<TARGET_FILE:koi_engine_avx512>")
endif()

koi_add_engine_ps_test(koi_engine_en_croissant_process tests/integration/uci/en_croissant_uci_test.ps1 30)
koi_add_engine_ps_test(koi_engine_time_safety_process tests/integration/uci/time_safety_test.ps1 180)
koi_add_ps_test(koi_benchmark_process tests/integration/tools/koi_bench_process_test.ps1 300
    -BenchPath "$<TARGET_FILE:koi_bench>")
koi_add_engine_ps_test(koi_uci_match_process tests/integration/matches/uci_match_process_test.ps1 120)
koi_add_engine_ps_test(koi_stockfish_strength_option tests/integration/matches/stockfish_strength_test.ps1 60)
koi_add_engine_ps_test(koi_uci_match_clock tests/integration/matches/uci_match_clock_test.ps1 120)
koi_add_ps_test(koi_sprt_compare_decision tests/integration/matches/sprt_compare_decision_test.ps1 30)

# The Windows-only harnesses: Cutechess fake engines are .cmd files, the memory
# soak reads Windows process counters, and the packaging tests exercise the
# MSVC-specific release layout.
if(WIN32)
    # The diagnostic half of the Cutechess harness uses fabricated engines and runs
    # everywhere.  The real smoke requires cutechess-cli, so it is only registered
    # when the executable is actually present instead of reporting a silent SKIP.
    koi_add_engine_ps_test(cutechess_stability_diagnostics tests/integration/matches/cutechess_stability_test.ps1 120 -DiagnosticsOnly)

    find_program(
        CUTECHESS_EXECUTABLE
        NAMES cutechess-cli.exe
        HINTS "C:/Program Files (x86)/Cute Chess"
    )
    if(CUTECHESS_EXECUTABLE)
        koi_add_engine_ps_test(cutechess_stability_smoke tests/integration/matches/cutechess_stability_test.ps1 180
            -CutechessPath "${CUTECHESS_EXECUTABLE}")
    else()
        message(STATUS "cutechess-cli not found; the real Cutechess smoke test is omitted")
    endif()

    koi_add_engine_ps_test(hash_memory_stability tests/integration/runtime/hash_memory_stability_test.ps1 60)
endif() # WIN32

koi_add_ps_test(windows_ci_configuration tests/integration/packaging/ci_configuration_test.ps1 0)
koi_add_ps_test(install_book_script tests/integration/packaging/install_book_test.ps1 30)
koi_add_ps_test(package_release_layout tests/integration/packaging/package_release_test.ps1 60
    -BuildDirectory "${CMAKE_BINARY_DIR}"
    -PackageScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/build/package_release.ps1")

endif() # POWERSHELL_EXECUTABLE

if(APPLE)
    # Prefer an interpreter on PATH (Homebrew or a standalone install) over the
    # Command Line Tools framework Python, which is still 3.9 and lacks
    # int.bit_count() used by the measurement tooling.
    set(Python3_FIND_FRAMEWORK LAST)
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    function(koi_add_python_unittest test_name script_relpath)
        add_test(
            NAME ${test_name}
            COMMAND "${Python3_EXECUTABLE}" -m unittest "${script_relpath}" -v
        )
        set_tests_properties(${test_name} PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        )
    endfunction()

    koi_add_python_unittest(release_candidate_games_python tests/python/stability/release_candidate_games_test.py)
    koi_add_python_unittest(release_strength_openings_python tests/python/stability/release_strength_openings_test.py)
    koi_add_python_unittest(strength_bound_python tests/python/stability/strength_bound_test.py)
    koi_add_python_unittest(elo_oracle_python tests/python/measurement/elo_oracle_test.py)
    koi_add_python_unittest(elo_estimate_python tests/python/measurement/elo_estimate_test.py)
    koi_add_python_unittest(stockfish_match_python tests/python/measurement/stockfish_match_test.py)
    koi_add_python_unittest(elo_openings_python tests/python/measurement/elo_openings_test.py)
    set_property(
        TEST elo_openings_python
        APPEND
        PROPERTY ENVIRONMENT "KOI_REPLAY_PATH=$<TARGET_FILE:koi_replay>"
    )
    koi_add_python_unittest(spsa_search_python tests/python/measurement/spsa_search_test.py)
    koi_add_python_unittest(nnue_training_python tests/python/evaluation/nnue_training_test.py)
    koi_add_python_unittest(nnue_info_python tests/python/evaluation/nnue_info_test.py)
    koi_add_python_unittest(nnue_wrapper_python tests/python/evaluation/nnue_wrapper_test.py)
    koi_add_python_unittest(koi_dataset_python tests/python/evaluation/koi_dataset_test.py)
    koi_add_python_unittest(koi_chess_python tests/python/rules/koi_chess_test.py)
    koi_add_python_unittest(koi_chess_pgn_python tests/python/rules/koi_chess_pgn_test.py)
    koi_add_python_unittest(koi_chess_engine_python tests/python/rules/koi_chess_engine_test.py)
    koi_add_python_unittest(gen_training_data_python tests/python/evaluation/gen_training_data_test.py)
    koi_add_python_unittest(measurement_phase_python tests/python/measurement/measurement_phase_test.py)
    koi_add_python_unittest(measurement_forensics_python tests/python/measurement/measurement_forensics_test.py)
    koi_add_python_unittest(uci_benchmark_python tests/python/measurement/uci_benchmark_test.py)
    koi_add_python_unittest(strength_report_python tests/python/evaluation/strength_report_test.py)
    koi_add_python_unittest(koi_trainer_python tests/python/nnue/koi_trainer_test.py)
    # Launches several subprocesses importing PyTorch (~100s on Windows CPU).
    koi_set_timeout(koi_trainer_python 240)
    # The trainer parity test shells out to the built boundary executable to
    # compare the Python encoder and integer reference against C++ scores.
    set_property(
        TEST koi_trainer_python
        APPEND
        PROPERTY ENVIRONMENT "KOI_NNUE_BOUNDARY_EXE=$<TARGET_FILE:nnue_boundary_tests>"
    )
    koi_add_python_unittest(bullet_data_python tests/python/nnue/bullet_data_test.py)
    koi_add_python_unittest(bullet_conversion_python tests/python/nnue/bullet_conversion_test.py)
    koi_add_python_unittest(build_verified_book_python tests/python/book/build_verified_book_test.py)
    koi_add_python_unittest(cli_subcommands_python tests/python/runtime/cli_subcommands_test.py)
    set_property(
        TEST cli_subcommands_python
        APPEND
        PROPERTY ENVIRONMENT
            "KOI_ENGINE_EXE=$<TARGET_FILE:koi_engine>"
            "KOI_PERFT_EXE=$<TARGET_FILE:koi_perft>"
            "KOI_REPLAY_EXE=$<TARGET_FILE:koi_replay>"
            "KOI_BENCH_EXE=$<TARGET_FILE:koi_bench>"
            "KOI_EVAL_FEATURES_EXE=$<TARGET_FILE:koi_eval_features>"
            "KOI_NNUE_FIXTURE_EXE=$<TARGET_FILE:koi_nnue_fixture>"
    )
else()
    message(STATUS
        "Skipping all Python tests: the Python 3 interpreter is unavailable."
    )
endif()

# --- Labels and scheduling hints ---------------------------------------------
# Labels let a developer (and CI) select focused subsets with `ctest -L`; the
# PROCESSORS hints keep the heavy search/process tests from oversubscribing a
# `ctest -j` run, and RUN_SERIAL protects the one test that writes into the
# build tree next to the engine binary.
set_tests_properties(
    koi_core_tests koi_cpu_features_tests koi_cpu_variant_tests syzygy_tablebase_tests
    koi_rules_tests native_rule_state_tests opening_book_tests completion_gate_tests
    search_ordering_tests search_architecture_tests search_policy_tests
    search_runtime_tests static_exchange_tests time_manager_tests perft_tests
    PROPERTIES LABELS "unit"
)
if(TARGET koi_module_tests)
    set_tests_properties(koi_module_tests PROPERTIES LABELS "unit")
endif()
set_tests_properties(
    classical_evaluator_tests evaluation_architecture_tests nnue_boundary_tests
    evaluation_features_tests
    PROPERTIES LABELS "unit;evaluation"
)
set_tests_properties(transposition_table_tests PROPERTIES LABELS "unit;runtime")
set_tests_properties(search_service_tests PROPERTIES LABELS "unit;search")
set_tests_properties(koi_soak_tests PROPERTIES LABELS "unit;runtime")
set_tests_properties(uci_controller_tests PROPERTIES LABELS "unit;runtime")
set_tests_properties(
    koi_search_tests_1of4 koi_search_tests_2of4 koi_search_tests_3of4 koi_search_tests_4of4
    PROPERTIES LABELS "unit;search;heavy"
)
set_tests_properties(
    koi_search_tests_1of4 koi_search_tests_2of4 koi_search_tests_3of4 koi_search_tests_4of4
    koi_strength_tests
    PROPERTIES PROCESSORS 2
)
set_tests_properties(koi_strength_tests PROPERTIES LABELS "unit;search;heavy")
set_tests_properties(koi_replay_tests koi_nnue_fixture_tests PROPERTIES LABELS "integration;tools")
if(POWERSHELL_EXECUTABLE)
    set(KOI_UCI_PROCESS_TESTS
        koi_engine_process koi_engine_perft_responsiveness
        koi_engine_en_croissant_process koi_protocol_fuzz_process
        koi_protocol_soak_process koi_protocol_option_churn_process
        koi_protocol_broken_pipe_process)
    if(KOI_TARGET_X86_64)
        list(APPEND KOI_UCI_PROCESS_TESTS koi_engine_variant_process)
    endif()
    set_tests_properties(${KOI_UCI_PROCESS_TESTS}
        PROPERTIES LABELS "process;uci")
    # The protocol fuzz corpus is the fast adversarial-regression alarm; keep a
    # dedicated sub-label so it can be run on its own.
    set_tests_properties(koi_protocol_fuzz_process PROPERTIES LABELS "process;uci;reliability")
    # Resource-churn soak: RUN_SERIAL keeps the RSS growth bound meaningful.
    set_tests_properties(koi_protocol_soak_process
        PROPERTIES LABELS "process;uci;heavy;reliability" RUN_SERIAL TRUE)
    set_tests_properties(koi_protocol_option_churn_process
        PROPERTIES LABELS "process;uci;reliability")
    # OS-level allocation failure: Linux-only, because it relies on
    # RLIMIT_AS via `ulimit -v`; the engine must report allocation_failed and
    # keep searching with the previous table.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        koi_add_engine_ps_test(koi_protocol_memory_limit_process tests/integration/uci/protocol_memory_limit_test.ps1 120)
        set_tests_properties(koi_protocol_memory_limit_process
            PROPERTIES LABELS "process;uci;reliability" SKIP_RETURN_CODE 77)
    endif()
    set_tests_properties(koi_protocol_broken_pipe_process
        PROPERTIES LABELS "process;uci;reliability")
    set_tests_properties(koi_hce_process PROPERTIES LABELS "process;uci")
    set_tests_properties(koi_hce_bench_process PROPERTIES LABELS "process;tools;evaluation" PROCESSORS 2)
    set_tests_properties(koi_hce_eval_features_process PROPERTIES LABELS "process;tools;evaluation")
    set_tests_properties(koi_engine_process koi_engine_perft_responsiveness PROPERTIES RUN_SERIAL TRUE)
    set_tests_properties(koi_engine_time_safety_process
        PROPERTIES LABELS "process;uci;heavy" PROCESSORS 2)
    set_tests_properties(koi_benchmark_process
        PROPERTIES LABELS "process;tools;heavy" PROCESSORS 2)
    set_tests_properties(koi_uci_match_process koi_uci_match_clock koi_sprt_compare_decision
        PROPERTIES LABELS "process;matches")
    set_tests_properties(koi_uci_match_clock PROPERTIES PROCESSORS 2)
    set_tests_properties(koi_stockfish_strength_option PROPERTIES LABELS "process;matches")
    if(WIN32)
        set_tests_properties(cutechess_stability_diagnostics
            PROPERTIES LABELS "process;matches")
        if(TEST cutechess_stability_smoke)
            set_tests_properties(cutechess_stability_smoke
                PROPERTIES LABELS "process;matches;heavy" PROCESSORS 2)
        endif()
        set_tests_properties(hash_memory_stability PROPERTIES LABELS "process;runtime")
    endif()
    set_tests_properties(windows_ci_configuration install_book_script package_release_layout
        PROPERTIES LABELS "process;packaging")
endif()

if(TARGET koi_shadow_diff_tests)
    set_tests_properties(koi_shadow_diff_tests PROPERTIES LABELS "integration;rules")
endif()

if(Python3_Interpreter_FOUND)
    set_tests_properties(
        elo_estimate_python stockfish_match_python elo_openings_python
        spsa_search_python nnue_training_python nnue_info_python nnue_wrapper_python
        koi_dataset_python gen_training_data_python
        koi_chess_python koi_chess_pgn_python koi_chess_engine_python
        measurement_phase_python measurement_forensics_python strength_report_python
        uci_benchmark_python
        koi_trainer_python
        bullet_data_python bullet_conversion_python build_verified_book_python cli_subcommands_python release_candidate_games_python release_strength_openings_python strength_bound_python
        PROPERTIES LABELS "python"
    )
    if(TEST elo_oracle_python)
        set_tests_properties(elo_oracle_python PROPERTIES LABELS "python;measurement")
    endif()
    set_tests_properties(
        uci_benchmark_python
        PROPERTIES LABELS "python;measurement"
    )
endif()

# Dependency-free adversarial fuzzers (FEN, move lists, setoption, go/state,
# raw commands, and multi-protocol modes); skipped only when Python 3 is absent.
if(Python3_Interpreter_FOUND)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/reliability")
    set(KOI_RELIABILITY_FUZZ_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/reliability")
    foreach(KOI_FUZZ_SPEC IN ITEMS "fen:150" "move:150" "option:100" "go:100" "command:150" "protocol_mode:100")
        string(REPLACE ":" ";" KOI_FUZZ_PARTS "${KOI_FUZZ_SPEC}")
        list(GET KOI_FUZZ_PARTS 0 KOI_FUZZ_NAME)
        list(GET KOI_FUZZ_PARTS 1 KOI_FUZZ_CASES)
        add_test(
            NAME "koi_${KOI_FUZZ_NAME}_fuzz_process"
            COMMAND "${Python3_EXECUTABLE}"
                    "${KOI_RELIABILITY_FUZZ_DIR}/${KOI_FUZZ_NAME}_fuzz.py"
                    --engine "$<TARGET_FILE:koi_engine>"
                    --cases "${KOI_FUZZ_CASES}"
                    --workers 2
                    --out "${CMAKE_BINARY_DIR}/reliability/${KOI_FUZZ_NAME}-fuzz.json"
        )
        koi_set_timeout("koi_${KOI_FUZZ_NAME}_fuzz_process" 300)
        set_tests_properties("koi_${KOI_FUZZ_NAME}_fuzz_process"
            PROPERTIES LABELS "process;reliability" PROCESSORS 4)
    endforeach()
endif()

# Engine-spawning tests need a valid NNUE network for default boot (when
# KOI_EVALUATOR is not overridden). Generate a small fixture network in the
# build directory and point every test at it explicitly (an explicit
# KOI_NNUE_PATH keeps the UCI handshake's EvalFile default empty, matching the
# fixture).
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/koi.nnue"
    COMMAND koi_nnue_fixture --arch v4 --output "${CMAKE_BINARY_DIR}/koi.nnue"
    DEPENDS koi_nnue_fixture
    COMMENT "Generating the NNUE test fixture network"
)
add_custom_target(koi_test_nnue_fixture ALL DEPENDS "${CMAKE_BINARY_DIR}/koi.nnue")

# Bound every registered test. A pathological search, an unresponsive child
# process, or a stalled network call must fail the test instead of hanging the
# whole suite. Tests that need a different budget set an explicit TIMEOUT above;
# python tests keep their shorter dedicated budget, and this loop fills in the
# default bound for everything else.
get_property(KOI_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)

set_property(
    TEST ${KOI_REGISTERED_TESTS}
    APPEND
    PROPERTY ENVIRONMENT "KOI_NNUE_PATH=${CMAKE_BINARY_DIR}/koi.nnue"
)

set(KOI_PYTHON_TEST_TIMEOUT_SECONDS "90" CACHE STRING
    "Timeout in seconds for tests labeled python")
math(EXPR KOI_PYTHON_TIMEOUT
    "${KOI_PYTHON_TEST_TIMEOUT_SECONDS} * ${KOI_TIMEOUT_SCALE}")
math(EXPR KOI_DEFAULT_TIMEOUT
    "${KOI_TEST_TIMEOUT_SECONDS} * ${KOI_TIMEOUT_SCALE}")
foreach(KOI_TEST_NAME IN LISTS KOI_REGISTERED_TESTS)
    get_test_property(${KOI_TEST_NAME} TIMEOUT KOI_TEST_TIMEOUT)
    if(KOI_TEST_TIMEOUT STREQUAL "NOTFOUND")
        get_test_property(${KOI_TEST_NAME} LABELS KOI_TEST_LABELS)
        if(KOI_TEST_LABELS MATCHES "(^|;)python(;|$)")
            set_tests_properties(${KOI_TEST_NAME} PROPERTIES TIMEOUT ${KOI_PYTHON_TIMEOUT})
        else()
            set_tests_properties(${KOI_TEST_NAME} PROPERTIES TIMEOUT ${KOI_DEFAULT_TIMEOUT})
        endif()
    endif()
endforeach()
