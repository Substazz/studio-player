cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/buildspec.cmake" OPTIONAL)

project(obs-studio-player VERSION 1.0.0 LANGUAGES CXX)

if(WIN32)
    set(OS_WINDOWS TRUE)
elseif(APPLE)
    set(OS_MACOS TRUE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ── OBS ───────────────────────────────────────────────────────────────────────
# Ubuntu PPA installs LibObsConfig.cmake (mixed case); source builds produce libobsConfig.cmake
find_package(libobs QUIET)
if(NOT libobs_FOUND)
  find_package(LibObs REQUIRED)
endif()
find_package(obs-frontend-api REQUIRED)

# ── Qt6 ───────────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

# ── Plugin target ─────────────────────────────────────────────────────────────
add_library(obs-studio-player MODULE
    src/plugin-main.cpp
    src/StudioPlayerDock.cpp
)

target_include_directories(obs-studio-player PRIVATE src)

target_link_libraries(obs-studio-player PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

# ── Install paths (per-platform OBS conventions) ──────────────────────────────
if(OS_WINDOWS)
    set(OBS_PLUGIN_DESTINATION "obs-plugins/64bit")
    set(OBS_DATA_DESTINATION   "data/obs-plugins/obs-studio-player")
elseif(OS_MACOS)
    set(OBS_PLUGIN_DESTINATION "obs-plugins")
    set(OBS_DATA_DESTINATION   "obs-plugins/obs-studio-player")
else()
    include(GNUInstallDirs)
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install path prefix." FORCE)
    endif()
    set(OBS_PLUGIN_DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
    set(OBS_DATA_DESTINATION   "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/obs-studio-player")
endif()

install(TARGETS obs-studio-player
    LIBRARY DESTINATION "${OBS_PLUGIN_DESTINATION}"
)

install(DIRECTORY data/
    DESTINATION "${OBS_DATA_DESTINATION}"
)
