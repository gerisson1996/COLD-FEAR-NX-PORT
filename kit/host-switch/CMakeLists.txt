# An original Xbox game, statically recompiled with XboxXBE-Recomp, as a Switch homebrew (NRO). The game's folder
# (PORT_GAME_DIR) has its sources (src/), its recompiler config (config/game.toml), the xbe (assets/default.xbe), the
# recompiled code (generated/game, from ferramentas/recompilar.ps1) and port.cmake, which names the port. This folder
# is the Switch host; the SDK's portable runtime is in sdk/XboxXBE-Recomp.
#
# ferramentas/build_switch.sh <game folder> runs this:
# cmake -S host-switch -B build/<name> -G Ninja -DPORT_GAME_DIR=<game folder>
#       -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release

cmake_minimum_required(VERSION 3.22)
project(xbe_switch_port LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(KIT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(HOST ${CMAKE_CURRENT_SOURCE_DIR})
set(SDK ${KIT}/sdk/XboxXBE-Recomp)
set(THIRD ${KIT}/third_party)
set(NVK_SDK ${KIT}/nvk CACHE PATH "mesa-switch (NVK): include/ and the static libvulkan")

set(PORT_GAME_DIR "" CACHE PATH "the game's folder: src/, config/game.toml, assets/, generated/, port.cmake")
if(NOT PORT_GAME_DIR OR NOT EXISTS ${PORT_GAME_DIR}/port.cmake)
    message(FATAL_ERROR "PORT_GAME_DIR must be a game folder with port.cmake (exemplos/black has one)")
endif()
get_filename_component(PORT_GAME_DIR ${PORT_GAME_DIR} ABSOLUTE)
set(GAME_DIR ${PORT_GAME_DIR})
set(GAME ${GAME_DIR}/src)

# what port.cmake sets; the defaults are for a game that needs nothing more
set(PORT_NAME "")                       # the NRO's name and its folder on the card, sdmc:/switch/<PORT_NAME>
set(PORT_TITLE "")                      # the title in the menu and in the error messages
set(PORT_AUTHOR "XboxXBE-Recomp")
set(PORT_VERSION "0.1.0")
set(PORT_ICON ${GAME_DIR}/icon.jpg)     # 256x256 jpg, optional
set(PORT_XBE_PATCH "")                  # optional: a patch turning another release's xbe into the recompiled one
set(PORT_GYRO_MODE 0)                   # the gyro before the player picks: 0 off, 1 always, 2 ZR held, 3 ZL held
set(PORT_XBE_PATCH_SHA1 "")             # the SHA-1 of the xbe that patch applies to
include(${GAME_DIR}/port.cmake)
if(NOT PORT_NAME OR NOT PORT_TITLE)
    message(FATAL_ERROR "port.cmake must set PORT_NAME and PORT_TITLE")
endif()

# the recompiled code and the xbe it came from
set(GEN ${GAME_DIR}/generated/game)
set(XBE ${GAME_DIR}/assets/default.xbe)
if(NOT EXISTS ${GEN}/x86_func_mapping.cpp OR NOT EXISTS ${XBE})
    message(FATAL_ERROR "No recompiled code in ${GEN}: run ferramentas/recompilar.ps1 ${GAME_DIR}")
endif()
file(STRINGS ${GAME_DIR}/config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
math(EXPR last "${GAME_PARTITIONS} - 1")
set(GEN_SOURCES ${GEN}/x86_func_mapping.cpp)
foreach(i RANGE ${last})
    list(APPEND GEN_SOURCES ${GEN}/x86_recomp.${i}.cpp)
endforeach()
file(SHA1 ${XBE} xbe_sha1)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp CONTENT "extern const char kImageSha1[] = \"${xbe_sha1}\";\n")
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/build_info.h CONTENT
     "#define PORT_BUILD \"switch-${PORT_VERSION}\"\n#define PORT_NAME \"${PORT_NAME}\"\n#define PORT_TITLE \"${PORT_TITLE}\"\n#define PORT_GYRO_MODE ${PORT_GYRO_MODE}\n")

# glslang with its HLSL front end (the SDK's shaders are HLSL), built here for the Switch
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# its precompiled header is named by an MSYS path (/c/...) the Windows-hosted GCC cannot open
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
add_subdirectory(${THIRD}/glslang ${CMAKE_CURRENT_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)
add_subdirectory(${THIRD}/tomlplusplus ${CMAKE_CURRENT_BINARY_DIR}/tomlplusplus EXCLUDE_FROM_ALL)
set(IMGUI_SOURCES ${THIRD}/imgui/imgui.cpp ${THIRD}/imgui/imgui_draw.cpp ${THIRD}/imgui/imgui_tables.cpp
    ${THIRD}/imgui/imgui_widgets.cpp ${THIRD}/imgui/backends/imgui_impl_vulkan.cpp)

# the SDK's runtime but its Windows parts, and those the game's sources replace (src/sound, src/d3d)
file(GLOB RUNTIME_SOURCES ${SDK}/runtime/*/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX
     "/runtime/app/|/core/input\\.cpp$|/gpu/(d3d12|render_thread)\\.cpp$|/audio/xaudio2\\.cpp$|/xdk/(dsound|d3d8)\\.cpp$")
file(GLOB GAME_HOST_SOURCES CONFIGURE_DEPENDS ${GAME}/*/*.cpp)
# the game's own Switch-only code (traces, workarounds): <game>/switch/*.cpp
file(GLOB GAME_SWITCH_SOURCES CONFIGURE_DEPENDS ${GAME_DIR}/switch/*.cpp)
file(GLOB SWITCH_SOURCES CONFIGURE_DEPENDS ${HOST}/*.cpp)
set(HOST_SOURCES ${RUNTIME_SOURCES} ${GAME_HOST_SOURCES} ${GAME_SWITCH_SOURCES} ${SWITCH_SOURCES})
set(HOST_INCLUDES ${HOST}/compat ${SDK}/harness/compat ${HOST} ${GAME} ${SDK}/runtime ${GEN}
    ${CMAKE_CURRENT_BINARY_DIR} ${THIRD}/glslang ${NVK_SDK}/include)

add_executable(${PORT_NAME} ${HOST_SOURCES} ${GEN_SOURCES} ${IMGUI_SOURCES}
               ${HOST}/switch_elf_stubs.c ${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp)
target_include_directories(${PORT_NAME} PRIVATE ${HOST_INCLUDES})
target_include_directories(${PORT_NAME} SYSTEM PRIVATE ${THIRD}/imgui)
# x86 chars are signed, as the game's code and the runtime were written for
target_compile_options(${PORT_NAME} PRIVATE -fno-strict-aliasing -fsigned-char)
# ImGui's open-in-browser uses fork/exec, which the Switch has not
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_DEFINITIONS IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
set_source_files_properties(${HOST_SOURCES} PROPERTIES COMPILE_OPTIONS "-include;${HOST}/compat/crt.h")
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-w;-fno-strict-aliasing;-fsigned-char")
set_source_files_properties(${HOST}/vk_shader.cpp PROPERTIES COMPILE_DEFINITIONS ENABLE_HLSL)
if(PORT_XBE_PATCH)
    if(NOT EXISTS ${PORT_XBE_PATCH} OR NOT PORT_XBE_PATCH_SHA1)
        message(FATAL_ERROR "PORT_XBE_PATCH needs the patch file and PORT_XBE_PATCH_SHA1")
    endif()
    set_source_files_properties(${HOST}/game_files.cpp PROPERTIES
        COMPILE_DEFINITIONS "XBE_PATCH=\"${PORT_XBE_PATCH}\";XBE_PATCH_SHA1=\"${PORT_XBE_PATCH_SHA1}\""
        OBJECT_DEPENDS ${PORT_XBE_PATCH})
endif()

# the host and runtime alone, to find their compile errors before the long build of the recompiled code
add_library(host_check OBJECT EXCLUDE_FROM_ALL ${HOST_SOURCES} ${IMGUI_SOURCES})
target_include_directories(host_check PRIVATE ${HOST_INCLUDES})
target_include_directories(host_check SYSTEM PRIVATE ${THIRD}/imgui)
target_compile_options(host_check PRIVATE -fno-strict-aliasing -fsigned-char)
target_link_libraries(host_check PRIVATE tomlplusplus::tomlplusplus)

# NVK, linked statically; its own (HLSL-less) glslang is left out
target_link_libraries(${PORT_NAME} PRIVATE
    tomlplusplus::tomlplusplus glslang glslang-default-resource-limits
    -Wl,--allow-multiple-definition
    -Wl,--start-group
    ${NVK_SDK}/lib/libvulkan.a
    ${NVK_SDK}/lib/liballoc-af82aa609fb0f811.rlib
    ${NVK_SDK}/lib/libcore-8c20f6407cb781b6.rlib
    ${NVK_SDK}/lib/libcompiler_builtins-389209a593b9fef9.rlib
    ${NVK_SDK}/lib/libpanic_abort-810dcfbec4f99ef7.rlib
    zstd expat z nx stdc++ m pthread
    -Wl,--end-group)

nx_generate_nacp(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${PORT_NAME}.nacp
    NAME "${PORT_TITLE}" AUTHOR "${PORT_AUTHOR}" VERSION "${PORT_VERSION}")
if(EXISTS ${PORT_ICON})
    nx_create_nro(${PORT_NAME} NACP ${CMAKE_CURRENT_BINARY_DIR}/${PORT_NAME}.nacp ICON ${PORT_ICON})
else()
    nx_create_nro(${PORT_NAME} NACP ${CMAKE_CURRENT_BINARY_DIR}/${PORT_NAME}.nacp)
endif()
