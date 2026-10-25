#pragma once

#include <initializer_list>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace backup {
std::string diagnostic_text(std::string text, std::initializer_list<std::string_view> secrets = {});
bool log_error(const std::string& message);
void log_memory(const char* stage);
std::string filesystem_error(const char* operation, std::string_view path, int code, std::optional<uint32_t> last_fs_result = {});
} // namespace backup
