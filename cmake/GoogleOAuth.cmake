# Inputs may come from the configure command or the build environment. Do not
# write environment-provided credentials into CMakeCache.txt or build commands.
foreach(_oauth_input IN ITEMS GOOGLE_OAUTH_CLIENT_ID GOOGLE_OAUTH_CLIENT_SECRET GOOGLE_OAUTH_CLIENT_FILE GOOGLE_OAUTH_REQUIRED)
    if(NOT DEFINED ${_oauth_input})
        set(${_oauth_input} "$ENV{${_oauth_input}}")
    endif()
endforeach()

if(NOT DEFINED GOOGLE_OAUTH_HEADER)
    message(FATAL_ERROR "GOOGLE_OAUTH_HEADER must specify the generated header path")
endif()

if(GOOGLE_OAUTH_CLIENT_FILE)
    if(NOT GOOGLE_OAUTH_CLIENT_ID STREQUAL "" OR NOT GOOGLE_OAUTH_CLIENT_SECRET STREQUAL "")
        message(FATAL_ERROR "Configure Google OAuth with either a client JSON file or a client ID/secret pair")
    endif()

    if(NOT EXISTS "${GOOGLE_OAUTH_CLIENT_FILE}")
        message(FATAL_ERROR "Google OAuth client JSON file does not exist")
    endif()

    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GOOGLE_OAUTH_CLIENT_FILE}")
    endif()

    file(READ "${GOOGLE_OAUTH_CLIENT_FILE}" _oauth_json LIMIT 65536)
    string(JSON _oauth_type ERROR_VARIABLE _oauth_error TYPE "${_oauth_json}")
    if(_oauth_error OR NOT _oauth_type STREQUAL "OBJECT")
        message(FATAL_ERROR "Google OAuth client file must contain a JSON object")
    endif()

    # Google downloads use 'installed'. Older overlay configurations use 'oauth'.
    set(_oauth_client "${_oauth_json}")
    foreach(_oauth_section IN ITEMS installed oauth)
        string(JSON _oauth_type ERROR_VARIABLE _oauth_error TYPE "${_oauth_json}" "${_oauth_section}")
        if(NOT _oauth_error)
            if(NOT _oauth_type STREQUAL "OBJECT")
                message(FATAL_ERROR "Invalid Google OAuth client section")
            endif()
            string(JSON _oauth_client GET "${_oauth_json}" "${_oauth_section}")
            break()
        endif()
    endforeach()

    foreach(_oauth_field IN ITEMS CLIENT_ID CLIENT_SECRET)
        string(TOLOWER "${_oauth_field}" _oauth_key)
        string(JSON _oauth_type ERROR_VARIABLE _oauth_error TYPE "${_oauth_client}" "${_oauth_key}")
        if(_oauth_error OR NOT _oauth_type STREQUAL "STRING")
            message(FATAL_ERROR "Google OAuth client file must contain client_id and client_secret strings")
        endif()
        string(JSON GOOGLE_OAUTH_${_oauth_field} GET "${_oauth_client}" "${_oauth_key}")
    endforeach()
endif()

if((GOOGLE_OAUTH_CLIENT_ID STREQUAL "") AND NOT (GOOGLE_OAUTH_CLIENT_SECRET STREQUAL "") OR
   (GOOGLE_OAUTH_CLIENT_SECRET STREQUAL "") AND NOT (GOOGLE_OAUTH_CLIENT_ID STREQUAL ""))
    message(FATAL_ERROR "Google OAuth requires both GOOGLE_OAUTH_CLIENT_ID and GOOGLE_OAUTH_CLIENT_SECRET")
endif()

if(GOOGLE_OAUTH_REQUIRED AND GOOGLE_OAUTH_CLIENT_ID STREQUAL "")
    message(FATAL_ERROR "This release requires Google OAuth app credentials. Set GOOGLE_OAUTH_CLIENT_FILE or GOOGLE_OAUTH_CLIENT_ID and GOOGLE_OAUTH_CLIENT_SECRET")
endif()

foreach(_oauth_field IN ITEMS CLIENT_ID CLIENT_SECRET)
    # Escape C++ string literals, including values read from JSON.
    set(_oauth_value "${GOOGLE_OAUTH_${_oauth_field}}")
    string(REPLACE "\\" "\\\\" _oauth_value "${_oauth_value}")
    string(REPLACE "\"" "\\\"" _oauth_value "${_oauth_value}")
    string(REPLACE "\r" "\\r" _oauth_value "${_oauth_value}")
    string(REPLACE "\n" "\\n" _oauth_value "${_oauth_value}")
    string(REPLACE "\t" "\\t" _oauth_value "${_oauth_value}")
    set(GOOGLE_OAUTH_${_oauth_field}_CPP "${_oauth_value}")
endforeach()

configure_file("${CMAKE_CURRENT_LIST_DIR}/GoogleOAuthCredentials.hpp.in" "${GOOGLE_OAUTH_HEADER}" @ONLY)

if(GOOGLE_OAUTH_CLIENT_ID STREQUAL "")
    message(STATUS "Google Drive sign-in: unconfigured development build")
else()
    message(STATUS "Google Drive sign-in: bundled OAuth app")
endif()
