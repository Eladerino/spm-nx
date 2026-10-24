# The Nintendo Switch (Horizon homebrew) layer of the WiiCompiled runtime build.
#
# wiicompiled/runtime/CMakeLists.txt includes this right after it recognises the devkitPro Switch
# toolchain (CMAKE_SYSTEM_NAME NintendoSwitch), and calls the spm_switch_* functions below at the few
# points where the Switch differs. Everything Switch-specific lives under switch/; the upstream tree
# only carries those hook calls and a handful of `__SWITCH__` branches (switch/README.md lists them).
#
# Compilers: devkitA64's GCC stays CMake's compiler, so it drives every link with libnx's specs. Dawn
# (with abseil) and the SDL 3 shim are configured first and compile with GCC, exactly as SwitchWaker
# proved them. Every target created after this file (Crypto++, pugixml, Aurora and its extern
# libraries, the runtime, the translated game) compiles with clang through cmake/clang-launcher.sh,
# because WiiCompiled only supports Clang. Both use devkitA64's newlib and libstdc++ headers, so the
# objects link together.
#
# SPM_SWITCH_AUDIT=ON with MKW_BUILD_PRODUCTS=OFF skips Dawn, the SDL shim and Aurora and compiles
# just the runtime's own sources for the Switch (mkw_switch_audit), which needs no network and no
# translated game: the quick check that the runtime still builds for Horizon after an upstream merge.

cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH SPM_SWITCH_DIR)
# Included from wiicompiled/runtime/CMakeLists.txt, so this is the runtime's directory.
set(MKW_RUNTIME_DIR_FOR_SWITCH "${CMAKE_CURRENT_SOURCE_DIR}")

option(SPM_SWITCH_AUDIT "Switch: compile only the runtime's sources (no Dawn, SDL shim or Aurora)" OFF)
set(SPM_SWITCH_NRO_NAME "spm-nx" CACHE STRING "Switch: file name of the NRO and its ELF")
set(SPM_SWITCH_SDL3_HEADERS "" CACHE PATH
    "Switch: an SDL 3.4.4 source tree for its headers (empty: download the pinned release)")
set(SPM_SWITCH_IMGUI_DIR "" CACHE PATH
    "Switch audit only: an ImGui v1.91.9b-docking tree for its headers (empty: download it)")

set(DEVKITPRO_ROOT "$ENV{DEVKITPRO}" CACHE PATH "devkitPro installation root")
if(NOT DEVKITPRO_ROOT)
    set(DEVKITPRO_ROOT "/opt/devkitpro")
endif()
set(SWITCH_PORTLIBS "${DEVKITPRO_ROOT}/portlibs/switch")

include(FetchContent)
# HostContext's context switch is assembly (spm_switch_runtime_sources); enable_language only works
# at directory scope, so here rather than in the function.
enable_language(ASM)

# ---- SDL 3 headers (Aurora's pin) ---------------------------------------------------------------
if(SPM_SWITCH_SDL3_HEADERS)
    set(SPM_SWITCH_SDL3_INCLUDE "${SPM_SWITCH_SDL3_HEADERS}/include")
else()
    FetchContent_Declare(spm_sdl3_headers
        URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.4.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR do-not-build)
    FetchContent_MakeAvailable(spm_sdl3_headers)
    set(SPM_SWITCH_SDL3_INCLUDE "${spm_sdl3_headers_SOURCE_DIR}/include")
endif()

if(NOT SPM_SWITCH_AUDIT)
    # ---- Dawn: Null backend + Tint's GLSL writer, patched for Horizon (switch/dawn) --------------
    include("${SPM_SWITCH_DIR}/dawn/dawn.cmake")

    # ---- SDL 3 as the libnx shim (switch/sdl3_shim) ----------------------------------------------
    file(GLOB _spm_shim_sources CONFIGURE_DEPENDS "${SPM_SWITCH_DIR}/sdl3_shim/*.c")
    add_library(SDL3-static STATIC ${_spm_shim_sources})
    add_library(SDL3::SDL3-static ALIAS SDL3-static)
    target_include_directories(SDL3-static PUBLIC "${SPM_SWITCH_SDL3_INCLUDE}")
    target_include_directories(SDL3-static PRIVATE "${SPM_SWITCH_DIR}/sdl3_shim")
    target_link_libraries(SDL3-static PUBLIC nx)

    # ---- Tracy: Aurora's profiler is off, and its client has no Horizon port ("Unsupported
    # platform!"). A headers-only TracyClient from Aurora's own pinned Tracy keeps Aurora's no-op
    # profiling macros compiling; Aurora skips its own Tracy when the target exists (as SwitchWaker).
    FetchContent_Declare(tracy
        URL https://github.com/wolfpld/tracy/archive/a64b9a20294d59421a2f57aeca3c6383d8c48169.tar.gz
        URL_HASH SHA256=24d342b5127d7f659dc3cf94f24347b348cc736f25b17a691fd7f69541937658
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        SOURCE_SUBDIR do-not-build)
    FetchContent_MakeAvailable(tracy)
    add_library(TracyClient INTERFACE)
    target_include_directories(TracyClient INTERFACE "${tracy_SOURCE_DIR}/public")
    add_library(Tracy::TracyClient ALIAS TracyClient)

    # Aurora's providers find the targets above and use them as they are.
    set(AURORA_SDL3_PROVIDER vendor CACHE STRING "" FORCE)
    set(AURORA_SDL3_LINKAGE static CACHE STRING "" FORCE)
    set(AURORA_DAWN_PROVIDER vendor CACHE STRING "" FORCE)
    set(AURORA_DAWN_LINKAGE static CACHE STRING "" FORCE)
    # No zstd for the cache entries on Horizon, as in SwitchWaker.
    set(AURORA_CACHE_USE_ZSTD OFF CACHE BOOL "" FORCE)
    set(AURORA_SDL3_LIBUSB OFF CACHE BOOL "" FORCE)
endif()

# mbed TLS (fetched later by the runtime for its guest TLS): Horizon's entropy, clock and socket
# pieces instead of the Unix ones (include/spm_mbedtls_config.h, runtime/horizon_mbedtls.c). The
# config define is PUBLIC on mbed TLS's targets, so the runtime's TLS code sees the same config.
set(MBEDTLS_USER_CONFIG_FILE "${SPM_SWITCH_DIR}/include/spm_mbedtls_config.h" CACHE FILEPATH "" FORCE)

# ---- From here on, every target compiles with clang ----------------------------------------------
# CMAKE_<LANG>_COMPILER_LAUNCHER seeds the property of each target created after this point.
set(CMAKE_C_COMPILER_LAUNCHER "${SPM_SWITCH_DIR}/cmake/clang-launcher.sh")
set(CMAKE_CXX_COMPILER_LAUNCHER "${SPM_SWITCH_DIR}/cmake/clang-launcher.sh")

# The Dawn backend toggles the runtime's aurora block reads: only the Null device exists here.
macro(spm_switch_dawn_backends)
    set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_NULL ON CACHE BOOL "" FORCE)
    set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
endmacro()

# Called where the runtime applies its project-wide definitions (right after Aurora): the shims in
# switch/include come before every other include directory of the project's own targets, and
# spm_switch_compat.h is force-included into each of their translation units.
macro(spm_switch_enter_project_scope)
    include_directories(BEFORE SYSTEM "${SPM_SWITCH_DIR}/include")
    # C and C++ only: the context-switch assembly must not see C declarations.
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include ${SPM_SWITCH_DIR}/include/spm_switch_compat.h>")
    if(TARGET aurora_core)
        foreach(_spm_t aurora_core aurora_gx aurora_pad aurora_si aurora_mtx aurora_vi aurora_main)
            if(TARGET ${_spm_t})
                target_include_directories(${_spm_t} SYSTEM BEFORE PRIVATE "${SPM_SWITCH_DIR}/include")
                target_include_directories(${_spm_t} PRIVATE "${SWITCH_PORTLIBS}/include")
            endif()
        endforeach()
        target_link_libraries(aurora_core PUBLIC nx)
    endif()
endmacro()

# The runtime's source list on Horizon: the Horizon guest-memory layer replaces the desktop one, and
# HostContext uses the AArch64 ELF context switch Android already has (x18 left alone, which suits
# Horizon too). `list_var` is the runtime's SOURCES variable.
function(spm_switch_runtime_sources list_var)
    set(_sources ${${list_var}})
    list(REMOVE_ITEM _sources
        "${MKW_RUNTIME_DIR_FOR_SWITCH}/src/guest_flat_memory.cpp"
        "${MKW_RUNTIME_DIR_FOR_SWITCH}/src/guest_flat_memory_macos.cpp")
    list(APPEND _sources
        "${MKW_RUNTIME_DIR_FOR_SWITCH}/src/platform/android/co_switch.S"
        "${SPM_SWITCH_DIR}/runtime/guest_flat_memory_horizon.cpp"
        "${SPM_SWITCH_DIR}/runtime/horizon_mman.c"
        "${SPM_SWITCH_DIR}/runtime/horizon_net.c"
        "${SPM_SWITCH_DIR}/runtime/horizon_mbedtls.c"
        "${SPM_SWITCH_DIR}/source/spm_switch_init.cpp")
    # The runtime's precompiled header is C++ (mkw_pch.h); these are the only C files in its targets.
    set_source_files_properties(
        "${SPM_SWITCH_DIR}/runtime/horizon_mman.c"
        "${SPM_SWITCH_DIR}/runtime/horizon_net.c"
        "${SPM_SWITCH_DIR}/runtime/horizon_mbedtls.c"
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    set(${list_var} ${_sources} PARENT_SCOPE)
endfunction()

# MKW_BUILD_PRODUCTS=OFF: the compile-only audit of the runtime's sources, modelled on the runtime's
# own macOS audit (mkw_macos_native_compile). abi_bridge.cpp and os_alarm.cpp need the translator's
# generated/RuntimeConfig.h and stay out, as they do there.
function(spm_switch_add_audit_target sources)
    set(_audit ${sources})
    list(REMOVE_ITEM _audit
        "${MKW_RUNTIME_DIR_FOR_SWITCH}/src/abi_bridge.cpp"
        "${MKW_RUNTIME_DIR_FOR_SWITCH}/src/hle/os/os_alarm.cpp")
    if(SPM_SWITCH_IMGUI_DIR)
        set(_imgui "${SPM_SWITCH_IMGUI_DIR}")
    else()
        FetchContent_Declare(spm_imgui_headers
            URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz
            URL_HASH SHA256=466fdef9b18de15f0bb6e288e3d00ffa3d82200ec458ce5e4f724a161d9528a5
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
            SOURCE_SUBDIR do-not-build)
        FetchContent_MakeAvailable(spm_imgui_headers)
        set(_imgui "${spm_imgui_headers_SOURCE_DIR}")
    endif()
    add_library(mkw_switch_audit OBJECT ${_audit})
    target_include_directories(mkw_switch_audit PRIVATE
        "${MKW_RUNTIME_DIR_FOR_SWITCH}/include" "${MKW_RUNTIME_DIR_FOR_SWITCH}/src"
        "${MKW_RUNTIME_DIR_FOR_SWITCH}/.." "${MKW_RUNTIME_DIR_FOR_SWITCH}/../aurora-main/include")
    # xxHash comes from devkitPro's portlibs here; the full build gets Aurora's pinned copy.
    target_include_directories(mkw_switch_audit SYSTEM PRIVATE
        "${SPM_SWITCH_SDL3_INCLUDE}" "${_imgui}" "${SWITCH_PORTLIBS}/include")
    target_compile_features(mkw_switch_audit PRIVATE cxx_std_20)
    target_compile_definitions(mkw_switch_audit PRIVATE SDL_MAIN_HANDLED TARGET_PC)
    target_link_libraries(mkw_switch_audit PRIVATE mkw::pugixml mkw::toml11 mkw::cryptopp mkw::mbedtls)
    set_target_properties(mkw_switch_audit PROPERTIES UNITY_BUILD OFF)
endfunction()

# After cmake/PublicProducts.cmake: the game links libnx and deko3d and becomes an NRO.
function(spm_switch_configure_products)
    if(NOT TARGET WiiCompiled)
        return()
    endif()
    set_target_properties(WiiCompiled PROPERTIES OUTPUT_NAME "${SPM_SWITCH_NRO_NAME}" SUFFIX ".elf")
    target_link_libraries(WiiCompiled PRIVATE SDL3::SDL3-static deko3d nx)
    nx_generate_nacp("${CMAKE_BINARY_DIR}/${SPM_SWITCH_NRO_NAME}.nacp"
        NAME "Super Paper Mario NX" AUTHOR "NewSuperPaperBros + spm-nx" VERSION "0.0.1")
    nx_create_nro(WiiCompiled
        OUTPUT "${CMAKE_BINARY_DIR}/${SPM_SWITCH_NRO_NAME}.nro"
        NACP "${CMAKE_BINARY_DIR}/${SPM_SWITCH_NRO_NAME}.nacp")
endfunction()
