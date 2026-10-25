cmake_minimum_required(VERSION 3.22)

include(cmake/Version.cmake)
project(SavegameBackupOverlay VERSION "${APP_VERSION}" LANGUAGES C CXX)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "Configure with devkitPro Switch.cmake")
endif()

set(ULTRA "${CMAKE_CURRENT_SOURCE_DIR}/lib/libultrahand")

file(GLOB ULTRA_SOURCES CONFIGURE_DEPENDS
    "${ULTRA}/common/*.c"
    "${ULTRA}/common/*.cpp"
    "${ULTRA}/libultra/source/*.c"
    "${ULTRA}/libultra/source/*.cpp"
    "${ULTRA}/libtesla/source/*.c"
    "${ULTRA}/libtesla/source/*.cpp"
)

file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(backup ${APP_SOURCES} ${ULTRA_SOURCES})

set(GOOGLE_OAUTH_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/GoogleOAuthCredentials.hpp")
include(cmake/GoogleOAuth.cmake)

target_compile_features(backup PRIVATE cxx_std_23)

target_include_directories(backup PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${ULTRA}/common"
    "${ULTRA}/libultra/include"
    "${ULTRA}/libtesla/include"
    "${DEVKITPRO}/portlibs/switch/include"
    "${DEVKITPRO}/portlibs/switch/include/minizip"
)

target_compile_options(backup PRIVATE
    -Wall
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>"
)

target_link_options(backup PRIVATE -Wl,--gc-sections)

target_link_libraries(backup PRIVATE
    curl minizip z mbedtls mbedx509 mbedcrypto nx
)

set(NACP "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.nacp")
set(OVL "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.ovl")
set(APPEND_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/append-ultr.cmake")

nx_generate_nacp(
    OUTPUT "${NACP}"
    NAME "Savegame Cloud Backup"
    AUTHOR "timo"
    VERSION "${PROJECT_VERSION}"
)

file(GENERATE OUTPUT "${APPEND_SCRIPT}"
    CONTENT "file(APPEND \"${OVL}\" \"ULTR\")\n"
)

add_custom_command(
    OUTPUT "${OVL}"
    COMMAND "${NX_ELF2NRO_EXE}" "$<TARGET_FILE:backup>"
            "${OVL}" "--nacp=${NACP}"
    COMMAND "${CMAKE_COMMAND}" -P "${APPEND_SCRIPT}"
    DEPENDS backup "${NACP}" "${APPEND_SCRIPT}"
    VERBATIM
)

add_custom_target(overlay ALL DEPENDS "${OVL}")
