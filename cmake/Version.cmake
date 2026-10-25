file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../src/main.cpp" version_lines REGEX "^[ \t]*static[ \t]+constexpr[ \t]+const[ \t]+char[ \t]*\\*[ \t]*APP_VERSION[ \t]*=")
list(LENGTH version_lines version_count)

if(NOT version_count EQUAL 1)
    message(FATAL_ERROR "Define APP_VERSION exactly once in src/main.cpp.")
endif()

string(REGEX MATCH "APP_VERSION[ \t]*=[ \t]*\"([^\"]*)\"" version_match "${version_lines}")
set(APP_VERSION "${CMAKE_MATCH_1}")

if(NOT APP_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "APP_VERSION must use the numeric MAJOR.MINOR.PATCH format, for example 0.1.0.")
endif()

string(LENGTH "${APP_VERSION}" version_length)

if(version_length GREATER 15)
    message(FATAL_ERROR "APP_VERSION must fit in the NACP display version (15 characters).")
endif()

if(DEFINED VERSION_OUTPUT)
    file(APPEND "${VERSION_OUTPUT}" "version=${APP_VERSION}\ntag=v${APP_VERSION}\n")
endif()

message(STATUS "App version: ${APP_VERSION}")
