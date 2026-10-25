#pragma once

#include "BackupTypes.hpp"

#include <string>

namespace backup {

struct SaveWriteOptions {
    uint64_t bytes_per_commit = 64 * 1024;
    std::function<bool(const std::string& path, std::string& error)> commit;
};

bool make_directories(const std::string& path, std::string& error);
bool remove_tree(const std::string& path, std::string& error);
bool hash_save(const std::string& root, std::string& hash, std::string& error, const Control& control = {});
bool hash_file(const std::string& path, std::string& hash, std::string& error, const Control& control = {});
bool create_archive(const std::string& root, const std::string& path, const Identity& identity, std::string& save_hash, std::string& error, const Control& control = {});
bool extract_archive(const std::string& path, const std::string& destination, const Identity& identity, const std::string& expected_hash, std::string& error, const Control& control = {});
bool replace_save(const std::string& root, const std::string& staged_save, std::string& error, const Control& control = {}, const SaveWriteOptions& writes = {});

} // namespace backup
