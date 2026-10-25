#pragma once

#include <cJSON.h>
#include <memory>
#include <string>

class JsonHandler {
public:
    explicit JsonHandler(std::string path);

    bool load(bool create_if_missing = false);
    bool save();
    cJSON* root() const { return document.get(); }
    const std::string& error() const { return last_error; }

    static cJSON* object(cJSON* parent, const char* name, bool create = false);
    static std::string string(const cJSON* parent, const char* name);
    static bool set_string(cJSON* parent, const char* name, const std::string& value);

private:
    struct Deleter {
        void operator()(cJSON* value) const { cJSON_Delete(value); }
    };

    std::string path;
    std::string last_error;
    std::unique_ptr<cJSON, Deleter> document;
};
