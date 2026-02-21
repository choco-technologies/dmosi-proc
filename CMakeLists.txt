# =====================================================================
#               DMOD OS Interface - Process Implementation
# =====================================================================
cmake_minimum_required(VERSION 3.10)

# ======================================================================
#               DMOD OS Interface - Process Implementation
# ======================================================================
project(dmosi_proc
    VERSION 1.0
    DESCRIPTION "DMOD OS Interface for Process Management"
    LANGUAGES C CXX)

# ======================================================================
#               For VS Code
# ======================================================================
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ======================================================================
#               Fetch DMOD repository
# ======================================================================
include(FetchContent)

# Only fetch and build dmod if it's not already available as a target
if(NOT TARGET dmod_inc)
    FetchContent_Declare(
        dmod
        GIT_REPOSITORY https://github.com/choco-technologies/dmod.git
        GIT_TAG        develop
    )

    # ======================================================================
    #               DMOD Configuration
    # ======================================================================
    set(DMOD_MODE "DMOD_SYSTEM" CACHE STRING "DMOD build mode")
    set(DMOD_BUILD_TESTS OFF CACHE BOOL "Build tests")
    set(DMOD_BUILD_EXAMPLES OFF CACHE BOOL "Build examples")
    set(DMOD_BUILD_TOOLS OFF CACHE BOOL "Build tools")
    set(DMOD_BUILD_TEMPLATES OFF CACHE BOOL "Build templates")
    set(DMOD_EXTERNAL_REGISTRATION ON CACHE BOOL "Use external registration")
    
    # Pass coverage flags to DMOD if enabled
    if(ENABLE_COVERAGE)
        set(DMOD_ENABLE_COVERAGE ON CACHE BOOL "Enable DMOD coverage")
    endif()

    FetchContent_MakeAvailable(dmod)
    set(DMOD_DIR ${dmod_SOURCE_DIR} CACHE PATH "DMOD source directory" FORCE)
else()
    message(STATUS "dmod target already exists, skipping FetchContent")
endif()

# ======================================================================
#               Fetch DMOD OS Interface
# ======================================================================
FetchContent_Declare(
    dmosi
    GIT_REPOSITORY
    https://github.com/choco-technologies/dmosi.git
    GIT_TAG        main
)

# ======================================================================
#               DMOD OS Interface Configuration
# ======================================================================
set(DMOD_DIR ${dmod_SOURCE_DIR})
FetchContent_MakeAvailable(dmosi)

# ======================================================================
#               DMOSI Proc Library definition
# ======================================================================
add_library(dmosi_proc STATIC
    src/dmosi_proc.c
)

target_include_directories(dmosi_proc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
)

target_link_libraries(dmosi_proc PUBLIC
    dmod
    dmosi
)

# Define version string for the library
target_compile_definitions(dmosi_proc PRIVATE
    DMOSI_PROC_VERSION="${PROJECT_VERSION}"
)

# ======================================================================
#               Tests
# ======================================================================
option(DMOSI_PROC_BUILD_TESTS "Build tests" OFF)

if(DMOSI_PROC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
