cmake_minimum_required(VERSION 3.20)

project(coldfear_recomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp" CACHE PATH "Path to the xboxrecomp toolkit")
if(NOT XBOXRECOMP_DIR OR NOT EXISTS "${XBOXRECOMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set XBOXRECOMP_DIR to the xboxrecomp toolkit directory.")
endif()

set(COLDFEAR_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen" CACHE PATH
    "Directory containing generated Cold Fear C sources")
if(NOT EXISTS "${COLDFEAR_GEN_DIR}/recomp_funcs.h")
    message(FATAL_ERROR "No generated code in ${COLDFEAR_GEN_DIR}. Run tools/regen.ps1.")
endif()

option(COLDFEAR_VULKAN "Use the Vulkan renderer" OFF)
set(XBOXRECOMP_VULKAN ${COLDFEAR_VULKAN} CACHE BOOL "" FORCE)

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_CURRENT_BINARY_DIR}/xboxrecomp)

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${COLDFEAR_GEN_DIR}/*.c")

set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/coldfear_trace.c
    src/fmod_hle.c
    src/prof.c
)

if(NINTENDO_SWITCH)
    list(APPEND GAME_SOURCES src/switch_nx.c)
endif()

add_executable(${PROJECT_NAME} ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)
target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${COLDFEAR_GEN_DIR}"
    "${XBOXRECOMP_DIR}/include"
)

if(COLDFEAR_VULKAN)
    target_compile_definitions(${PROJECT_NAME} PRIVATE COLDFEAR_VULKAN=1)
endif()

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11 dxgi dxguid xinput winmm dbghelp)
else()
    if(NINTENDO_SWITCH)
        target_link_libraries(${PROJECT_NAME} PRIVATE nx m)
        if(NOT COLDFEAR_VULKAN)
            target_link_libraries(${PROJECT_NAME} PRIVATE EGL glapi drm_nouveau)
        endif()

        target_link_options(${PROJECT_NAME} PRIVATE
            "LINKER:--wrap=SDL_SYS_SetThreadPriority"
            "LINKER:--wrap=audrvVoiceAddWaveBuf"
        )

        set_property(SOURCE src/switch_nx.c APPEND PROPERTY
            INCLUDE_DIRECTORIES "${XBOXRECOMP_DIR}/src/apu")
        target_include_directories(${PROJECT_NAME} PRIVATE
            "$ENV{DEVKITPRO}/portlibs/switch/include")

        set(COLDFEAR_NACP "${CMAKE_CURRENT_BINARY_DIR}/coldfear.nacp")
        nx_generate_nacp(OUTPUT "${COLDFEAR_NACP}"
            NAME "Cold Fear Xbox Port"
            AUTHOR "RECOMP-GUTO"
            VERSION "0.0.2")
        # Homebrew menu icon: the 256x256 JPEG made from the v2 cover.
        nx_create_nro(${PROJECT_NAME} NACP "${COLDFEAR_NACP}"
            ICON "${CMAKE_CURRENT_SOURCE_DIR}/artwork/coldfear-icon-256.jpg")
    else()
        target_link_libraries(${PROJECT_NAME} PRIVATE m)
    endif()
endif()

if(NOT WIN32)
    set(COLDFEAR_GEN_OPT "-O2" CACHE STRING
        "Optimisation level for generated game code")
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES
        COMPILE_FLAGS "${COLDFEAR_GEN_OPT} -w -fno-strict-aliasing -fwrapv")
endif()


