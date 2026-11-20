cmake_minimum_required(VERSION 3.20)

project(workshop C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# LVGL is configured by the repository-local lv_conf.h.
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/lvgl EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

add_executable(workshop_sim
    src/main.c
    src/launcher.c
    src/manifest.c
    src/icons.c
    src/settings.c
    lib/workshop_ime/workshop_ime.c
    src/platform_sim.c
)

target_include_directories(workshop_sim PRIVATE
    src
    lib/workshop_ime
    ${SDL2_INCLUDE_DIRS}
)

target_compile_options(workshop_sim PRIVATE
    ${SDL2_CFLAGS_OTHER}
)

target_link_directories(workshop_sim PRIVATE
    ${SDL2_LIBRARY_DIRS}
)

target_link_options(workshop_sim PRIVATE
    ${SDL2_LDFLAGS_OTHER}
)

target_link_libraries(workshop_sim PRIVATE
    lvgl
    ${SDL2_LIBRARIES}
    m
)

target_compile_definitions(workshop_sim PRIVATE
    WORKSHOP_SIM_APPS="${CMAKE_CURRENT_SOURCE_DIR}/app"
    WORKSHOP_SIM_FONT_LIGHT="A:${CMAKE_CURRENT_SOURCE_DIR}/fonts/LxgwWenKai/fonts/TTF/LXGWWenKai-Regular.ttf"
    WORKSHOP_SIM_FONT_BOLD="A:${CMAKE_CURRENT_SOURCE_DIR}/fonts/LxgwWenKai/fonts/TTF/LXGWWenKai-Medium.ttf"
)
