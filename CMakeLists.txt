cmake_minimum_required(VERSION 3.21)

project(
    Wayfarer
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds not allowed.")
endif()

# ── Fetch CommonLibSSE-NG ─────────────────────────────────────────────────────
include(FetchContent)

set(BUILD_TESTS          OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK   OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY https://github.com/CharmedBaryon/CommonLibSSE-NG.git
    GIT_TAG        b93280e832f263dbef44e44cbe2936622a02f91a
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(CommonLibSSE)
include("${commonlibsse_SOURCE_DIR}/cmake/CommonLibSSE.cmake")

# ── vcpkg-provided dependencies ───────────────────────────────────────────────
find_package(spdlog CONFIG REQUIRED)

# ── Plugin target ─────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/config/Settings.cpp
    src/hooks/FastTravelHook.cpp
    src/hooks/MapMenuHook.cpp
    src/journey/JourneyController.cpp
    src/journey/RouteMath.cpp
    src/encounters/EncounterManager.cpp
    src/ui/TravelOverlay.cpp
    src/ui/MapMarkerDraw.cpp
    src/papyrus/PapyrusAPI.cpp
)

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES ${SOURCES}
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_precompile_headers(${PROJECT_NAME}
    PRIVATE
        src/PCH.h
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        spdlog::spdlog
)

# /WX is intentionally omitted for the v1 scaffold to keep iteration fast; the
# code targets a clean /W4 build. Re-enable /WX before shipping.
target_compile_options(${PROJECT_NAME}
    PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /external:anglebrackets
        /external:W0
        /wd4200
)

# ── Optional: auto-deploy to Skyrim Data folder ───────────────────────────────
set(SKYRIM_PATH "" CACHE PATH "Path to Skyrim SE/AE Data folder (optional)")

if(SKYRIM_PATH)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}>
            "${SKYRIM_PATH}/SKSE/Plugins/${PROJECT_NAME}.dll"
        COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/data/SKSE/Plugins/Wayfarer.ini
            -DDST=${SKYRIM_PATH}/SKSE/Plugins/Wayfarer.ini
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_missing.cmake"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${SKYRIM_PATH}/MCM/Config/Wayfarer"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/data/MCM/Config/Wayfarer/config.json"
            "${SKYRIM_PATH}/MCM/Config/Wayfarer/config.json"
        VERBATIM
    )
endif()

# ── Auto-deploy to a Vortex mod staging folder (mirrors the Data/ layout) ──────
# Vortex then hardlinks this into the game on deploy. Defaults to the user's
# testing folder; override with -DWAYFARER_DEPLOY_PATH=... or set to "" to skip.
set(WAYFARER_DEPLOY_PATH "H:/Vortex Mods/skyrimse/Wayfarer"
    CACHE PATH "Vortex mod staging folder to deploy Wayfarer into (optional)")

if(WAYFARER_DEPLOY_PATH)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${WAYFARER_DEPLOY_PATH}/SKSE/Plugins"
        # DLL — always overwrite (it is the build output)
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}>
            "${WAYFARER_DEPLOY_PATH}/SKSE/Plugins/${PROJECT_NAME}.dll"
        # INI — only on first install; never clobber test edits
        COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/data/SKSE/Plugins/Wayfarer.ini
            -DDST=${WAYFARER_DEPLOY_PATH}/SKSE/Plugins/Wayfarer.ini
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_missing.cmake"
        # MCM Helper config — always overwrite (structure/content improves per build)
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${WAYFARER_DEPLOY_PATH}/MCM/Config/Wayfarer"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/data/MCM/Config/Wayfarer/config.json"
            "${WAYFARER_DEPLOY_PATH}/MCM/Config/Wayfarer/config.json"
        VERBATIM
    )
    message(STATUS "Wayfarer will deploy to: ${WAYFARER_DEPLOY_PATH}")
endif()
