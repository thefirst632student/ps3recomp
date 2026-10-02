cmake_minimum_required(VERSION 3.20)
project(MyGameRecomp
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Recompiled PS3 game project (ps3recomp)"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fast Debug keeps symbols/assertions but restores the optimized Debug codegen
# that is required for practical static-recompilation performance.  v26's
# VERSIONINFO/provenance resource is retained independently of optimization.
option(PS3RECOMP_FAST_DEBUG
    "Keep Debug symbols/asserts while using optimized Debug codegen" ON)
option(PS3RECOMP_FAST_DEBUG_RUNTIME
    "Also optimize host runtime/HLE in Debug" ON)
option(PS3RECOMP_RELEASE_RUNTIME_BISECT_O1
    "Bisect Defender false-positive by compiling only ps3recomp_runtime at O1 in Release" OFF)
option(PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_O1
    "Legacy whole-lifted-PPU Defender bisect at O1 in Release" OFF)
set(PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_SLICE "A1" CACHE STRING
    "Defender bisect for lifted PPU Release codegen: OFF, A1 (first quarter), A2 (second quarter), or B (second half)")
set_property(CACHE PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_SLICE PROPERTY STRINGS OFF A1 A2 B)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: this project is still sitting inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
# Keep Defender bisects reproducible across filesystems/generators.
list(SORT RECOMP_SOURCES)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(SPU_SOURCES "")
if(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

add_executable(${PROJECT_NAME}
    main.cpp
    stubs.cpp
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
if(SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
    endif()
endif()

# Give Windows executables stable, human-readable VERSIONINFO.  This is useful
# for crash reports, Explorer properties and security-product provenance.  It is
# intentionally ordinary metadata: no packing, obfuscation or post-link byte
# rewriting.  Ports may override the strings from the CMake command line.
if(WIN32)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
        # A port-specific resource wins when present.
        target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    else()
        set(PS3RECOMP_WINDOWS_COMPANY_NAME "PS3Recomp Project" CACHE STRING
            "CompanyName stored in the Windows VERSIONINFO resource")
        set(PS3RECOMP_WINDOWS_PRODUCT_NAME "${PROJECT_NAME}" CACHE STRING
            "ProductName stored in the Windows VERSIONINFO resource")
        set(PS3RECOMP_WINDOWS_FILE_DESCRIPTION "${PROJECT_DESCRIPTION}" CACHE STRING
            "FileDescription stored in the Windows VERSIONINFO resource")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/app.rc.in"
            "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
        target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
    endif()
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
    )
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
if(MSVC)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off")
endif()

# CMake's normal Debug configuration emits -O0 (or /Od), which is too costly
# for a static recompiler: main/scaffold dispatch and generated PPU code execute
# in the same hot target.  Restore v14's target-wide O1 while keeping Debug
# symbols/assertions and frame pointers.  The v26 VERSIONINFO resource remains
# unchanged so this build is a clean A/B test of codegen vs PE provenance.
if(PS3RECOMP_FAST_DEBUG)
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE
            "$<$<CONFIG:Debug>:/O1>"
            "$<$<CONFIG:Debug>:/Oy->")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${PROJECT_NAME} PRIVATE
            "$<$<CONFIG:Debug>:-O1>"
            "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>")
    endif()
endif()

# V36 lifted-PPU binary search.  V35 proved that the Defender-triggering
# Release codegen is inside half A of the numbered lifted PPU chunks.  Split
# that half once more: A1 is the first quarter, A2 is the second quarter, and
# B is retained as the already-cleared second half for reproducibility.
# Only the selected numbered ppu_recomp_NNN TU(s) are lowered to O1; helper
# sources, scaffold, runtime, CRT, NDEBUG and linker settings remain Release.
set(PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_CANDIDATES ${RECOMP_SOURCES})
list(FILTER PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_CANDIDATES INCLUDE
    REGEX "ppu_recomp_[0-9]+\.(c|cpp)$")
list(SORT PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_CANDIDATES)

set(PS3RECOMP_RELEASE_LIFTED_PPU_O1_SOURCES "")
if(PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_O1 AND
   PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_CANDIDATES)
    # Legacy whole-chunk mode retained for reproducibility of v34's result.
    set(PS3RECOMP_RELEASE_LIFTED_PPU_O1_SOURCES
        ${PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_CANDIDATES})
elseif(PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_CANDIDATES AND
       NOT PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_SLICE STREQUAL "OFF")
    if(NOT PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_SLICE STREQUAL "A1" AND
       NOT PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_SLICE STREQUAL "A2" AND
       NOT PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_SLICE STREQUAL "B")
        message(FATAL_ERROR
            "PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_SLICE must be OFF, A1, A2, or B")
    endif()

    list(LENGTH PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_CANDIDATES _recomp_bisect_count)
    # Match v35's half-A boundary exactly: A gets ceil(N/2).
    math(EXPR _recomp_bisect_half "(${_recomp_bisect_count} + 1) / 2")
    # Split that known-suspect half again: A1 gets ceil(A/2).
    math(EXPR _recomp_bisect_quarter "(${_recomp_bisect_half} + 1) / 2")

    if(PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_SLICE STREQUAL "A1")
        list(SUBLIST PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_CANDIDATES
            0 ${_recomp_bisect_quarter}
            PS3RECOMP_RELEASE_LIFTED_PPU_O1_SOURCES)
    elseif(PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_SLICE STREQUAL "A2")
        math(EXPR _recomp_bisect_a2_count
            "${_recomp_bisect_half} - ${_recomp_bisect_quarter}")
        if(_recomp_bisect_a2_count GREATER 0)
            list(SUBLIST PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_CANDIDATES
                ${_recomp_bisect_quarter} ${_recomp_bisect_a2_count}
                PS3RECOMP_RELEASE_LIFTED_PPU_O1_SOURCES)
        endif()
    else()
        math(EXPR _recomp_bisect_b_count
            "${_recomp_bisect_count} - ${_recomp_bisect_half}")
        if(_recomp_bisect_b_count GREATER 0)
            list(SUBLIST PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_CANDIDATES
                ${_recomp_bisect_half} ${_recomp_bisect_b_count}
                PS3RECOMP_RELEASE_LIFTED_PPU_O1_SOURCES)
        endif()
    endif()

    list(LENGTH PS3RECOMP_RELEASE_LIFTED_PPU_O1_SOURCES _recomp_bisect_o1_count)
    message(STATUS
        "Release lifted-PPU Defender bisect slice ${PS3RECOMP_RELEASE_LIFTED_PPU_BISECT_SLICE}: "
        "${_recomp_bisect_o1_count}/${_recomp_bisect_count} TU(s) at O1")
endif()

if(PS3RECOMP_RELEASE_LIFTED_PPU_O1_SOURCES)
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR MSVC)
        set_property(SOURCE ${PS3RECOMP_RELEASE_LIFTED_PPU_O1_SOURCES}
            APPEND PROPERTY COMPILE_OPTIONS "$<$<CONFIG:Release>:/O1>")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        set_property(SOURCE ${PS3RECOMP_RELEASE_LIFTED_PPU_O1_SOURCES}
            APPEND PROPERTY COMPILE_OPTIONS "$<$<CONFIG:Release>:-O1>")
    endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
