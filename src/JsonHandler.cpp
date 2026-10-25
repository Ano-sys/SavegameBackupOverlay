#include "include/JsonHandler.hpp"

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

JsonHandler::JsonHandler(std::string path) : path(std::move(path)) {}

bool JsonHandler::load(bool create_if_missing) {
    document.reset();
    last_error.clear();

    const std::string backup = path + ".bak";
    bool recovering = false;

    FILE* file = std::fopen(path.c_str(), "rb");

    if(!file && errno == ENOENT) {
        file = std::fopen(backup.c_str(), "rb");
        recovering = file != nullptr;
    }

    if(!file) {
        if(errno == ENOENT && create_if_missing) {
            document.reset(cJSON_CreateObject());

            if(document) return true;
        }

        last_error = "Could not open JSON file.";
        return false;
    }

    std::string contents;
    char buffer[2048];
    size_t count;

    while((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        contents.append(buffer, count);

        if(contents.size() > 64 * 1024) break;
    }

    bool read_ok = !std::ferror(file) && contents.size() <= 64 * 1024;
    std::fclose(file);

    if(!read_ok) {
        last_error = "Could not read JSON file completely (maximum 64 KiB).";
        return false;
    }

    if(contents.find('\0') == std::string::npos) document.reset(cJSON_ParseWithLengthOpts(contents.c_str(), contents.size() + 1, nullptr, true));

    if(!cJSON_IsObject(document.get())) {
        document.reset();
        last_error = "Invalid JSON file. It was not overwritten.";
        return false;
    }

    // Recover a replacement interrupted after the old file was moved aside.

    if(recovering && std::rename(backup.c_str(), path.c_str()) != 0) {
        document.reset();
        last_error = "Could not restore JSON backup.";
        return false;
    }

    return true;
}

bool JsonHandler::save() {
    last_error.clear();

    if(!document) {
        last_error = "No JSON document loaded.";
        return false;
    }

    for(size_t pos = path.find('/'); pos != std::string::npos; pos = path.find('/', pos + 1)) {
        std::string directory = path.substr(0, pos);

        if(directory.empty() || directory.back() == ':') continue;
        if(::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
            last_error = "Could not create JSON directory.";
            return false;
        }
    }

    char* text = cJSON_Print(document.get());

    if(!text) {
        last_error = "Could not serialize JSON.";
        return false;
    }

    const size_t length = std::char_traits<char>::length(text);

    if(length > 64 * 1024) {
        cJSON_free(text);
        last_error = "JSON file is too large (maximum 64 KiB).";
        return false;
    }

    const std::string temporary = path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "wb");

    if(!file) {
        cJSON_free(text);
        last_error = "Could not write JSON file.";
        return false;
    }

    ::chmod(temporary.c_str(), 0600);

    bool ok = std::fwrite(text, 1, length, file) == length;
    cJSON_free(text);

    if(std::fflush(file) != 0) ok = false;

    if(::fsync(::fileno(file)) != 0) ok = false;

    if(std::fclose(file) != 0) ok = false;

    if(ok && std::rename(temporary.c_str(), path.c_str()) != 0) {
        ok = false;

        // Switch fsdev does not replace an existing destination on rename.

        if(errno == EEXIST || errno == ENOTEMPTY) {
            const std::string backup = path + ".bak";

            if((::unlink(backup.c_str()) == 0 || errno == ENOENT) && std::rename(path.c_str(), backup.c_str()) == 0) {
                ok = std::rename(temporary.c_str(), path.c_str()) == 0;

                if(ok) ::unlink(backup.c_str());
                else std::rename(backup.c_str(), path.c_str());
            }
        }
    }

    if(!ok) {
        std::remove(temporary.c_str());
        last_error = "Could not save JSON. Previous data remains in the file or its .bak backup.";
    }

    return ok;
}

cJSON* JsonHandler::object(cJSON* parent, const char* name, bool create) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(parent, name);

    if(!value && create) value = cJSON_AddObjectToObject(parent, name);

    return cJSON_IsObject(value) ? value : nullptr;
}

std::string JsonHandler::string(const cJSON* parent, const char* name) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(parent, name);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

bool JsonHandler::set_string(cJSON* parent, const char* name, const std::string& value) {
    if(!cJSON_IsObject(parent)) return false;

    cJSON* replacement = cJSON_CreateString(value.c_str());

    if(!replacement) return false;

    const bool exists = cJSON_HasObjectItem(parent, name);
    const bool ok = exists ? cJSON_ReplaceItemInObjectCaseSensitive(parent, name, replacement) : cJSON_AddItemToObject(parent, name, replacement);

    if(!ok) cJSON_Delete(replacement);

    return ok;
}
