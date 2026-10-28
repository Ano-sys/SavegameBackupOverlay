#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

python3 tests/release_test.py
python3 tests/google_oauth_test.py

test_directory=$(mktemp -d /tmp/sbo-tests.XXXXXX)
trap 'rm -rf -- "$test_directory"' EXIT

cc -std=c99 -c lib/libultrahand/common/cJSON.c -o "$test_directory/cJSON.o"

c++ -std=c++23 -Wall -Wextra -pthread -Wl,--wrap=curl_easy_init -Wl,--wrap=curl_easy_perform -Wl,--wrap=curl_easy_getinfo -Wl,--wrap=socket -Isrc \
    tests/curl_session_test.cpp src/CurlSession.cpp src/Diagnostics.cpp \
    -lcurl -o "$test_directory/curl_session_test"
"$test_directory/curl_session_test"

c++ -std=c++23 -Wall -Wextra -pthread -D__SWITCH__ -Itests/stubs -Isrc \
    tests/socket_init_test.cpp src/CurlSession.cpp -lcurl -o "$test_directory/socket_init_test"
"$test_directory/socket_init_test"

c++ -std=c++23 -Wall -Wextra -pthread -Itests/stubs -Isrc \
    tests/background_job_test.cpp src/BackgroundJob.cpp src/Diagnostics.cpp -o "$test_directory/background_job_test"
"$test_directory/background_job_test"

for bundled in 0 1; do
    test_client_id=""
    test_client_secret=""
    if [ "$bundled" = 1 ]; then
        test_client_id="test-build-client.apps.googleusercontent.com"
        test_client_secret="test-build-secret"
    fi

    cmake -DGOOGLE_OAUTH_HEADER="$test_directory/oauth-$bundled/GoogleOAuthCredentials.hpp" \
        -DGOOGLE_OAUTH_CLIENT_ID="$test_client_id" -DGOOGLE_OAUTH_CLIENT_SECRET="$test_client_secret" \
        -DGOOGLE_OAUTH_CLIENT_FILE= -DGOOGLE_OAUTH_REQUIRED=OFF -P cmake/GoogleOAuth.cmake

    c++ -std=c++23 -Wall -Wextra -pthread -Wl,--wrap=rename -DTEST_BUNDLED_OAUTH="$bundled" \
        -I"$test_directory/oauth-$bundled" -Isrc -Ilib/libultrahand/common \
        tests/drive_config_test.cpp src/GoogleDriveConfig.cpp src/JsonHandler.cpp \
        "$test_directory/cJSON.o" -o "$test_directory/drive_config_test"
    "$test_directory/drive_config_test"
done

c++ -std=c++23 -Wall -Wextra -pthread -I"$test_directory/oauth-1" -Itests/stubs -Isrc -Ilib/libultrahand/common \
    tests/google_auth_network_test.cpp src/GoogleDriveAuth.cpp src/BackgroundJob.cpp src/GoogleDriveConfig.cpp \
    src/JsonHandler.cpp src/Diagnostics.cpp "$test_directory/cJSON.o" -o "$test_directory/google_auth_network_test"
"$test_directory/google_auth_network_test"

c++ -std=c++23 -Wall -Wextra -I"$test_directory/oauth-1" -Isrc -Ilib/libultrahand/common \
    tests/drive_folders_test.cpp src/DriveStorage.cpp src/GoogleDriveConfig.cpp \
    src/SaveArchive.cpp src/JsonHandler.cpp src/Diagnostics.cpp "$test_directory/cJSON.o" \
    -lminizip -lz -lcrypto -o "$test_directory/drive_folders_test"
"$test_directory/drive_folders_test"

c++ -std=c++23 -Wall -Wextra -Wl,--wrap=rename -Isrc -Ilib/libultrahand/common \
    tests/overlay_updater_test.cpp src/OverlayUpdater.cpp src/SaveArchive.cpp src/JsonHandler.cpp \
    src/Diagnostics.cpp "$test_directory/cJSON.o" -lminizip -lz -lcrypto -o "$test_directory/overlay_updater_test"
"$test_directory/overlay_updater_test"

c++ -std=c++23 -Wall -Wextra -Wl,--wrap=zipOpenNewFileInZip3_64 -Wl,--wrap=zipWriteInFileInZip \
    -Wl,--wrap=zipCloseFileInZip -Wl,--wrap=zipClose -Wl,--wrap=fopen -Wl,--wrap=fwrite \
    -Wl,--wrap=fflush -Wl,--wrap=fclose -Wl,--wrap=unlink -Wl,--wrap=fseeko \
    -Wl,--wrap=opendir -Wl,--wrap=closedir -Isrc -Ilib/libultrahand/common \
    tests/save_archive_test.cpp src/SaveArchive.cpp src/JsonHandler.cpp src/Diagnostics.cpp \
    "$test_directory/cJSON.o" -lminizip -lz -lcrypto -o "$test_directory/save_archive_test"
"$test_directory/save_archive_test"

c++ -std=c++23 -Wall -Wextra -pthread -Wl,--wrap=mkdtemp -Wl,--wrap=mkdir -Wl,--wrap=fopen \
    -Wl,--wrap=fwrite -Wl,--wrap=fclose -Wl,--wrap=unlink -Wl,--wrap=rmdir \
    -I"$test_directory/oauth-1" -Itests/stubs -Isrc -Ilib/libultrahand/common \
    tests/backup_task_test.cpp src/BackupTask.cpp src/BackgroundJob.cpp src/DriveStorage.cpp src/GoogleDriveConfig.cpp \
    src/SaveArchive.cpp src/JsonHandler.cpp src/Diagnostics.cpp "$test_directory/cJSON.o" \
    -lminizip -lz -lcrypto -o "$test_directory/backup_task_test"
"$test_directory/backup_task_test"
