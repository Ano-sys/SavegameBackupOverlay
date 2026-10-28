#include "include/SaveArchive.hpp"
#include "include/RestoreGuard.hpp"
#include "include/JsonHandler.hpp"

#include <minizip/zip.h>
#include <minizip/unzip.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <vector>
#include <string_view>
#include <dirent.h>
#include <set>

namespace fs = std::filesystem;

namespace {
std::string failed_entry, current_entry;
int open_failure = ZIP_OK, write_failure = ZIP_OK, entry_close_failure = ZIP_OK, archive_close_failure = ZIP_OK;
int failure_errno = 0;
enum class RestoreFailure { None,
    Open,
    Reopen,
    Seek,
    Write,
    Flush,
    Close,
    Remove };
RestoreFailure restore_failure = RestoreFailure::None;
std::string restore_prefix, blocked_remove;
FILE* restore_output = nullptr;
uint64_t simulated_write_limit = 0, uncommitted_bytes = 0;
size_t directory_limit = 1, peak_directories = 0;
std::set<DIR*> open_directories;

void write_file(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(contents.data(), contents.size());
    file.close();
    assert(file.good());
}

void make_zip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& entries, bool symlink = false) {
    zipFile archive = zipOpen64(path.c_str(), APPEND_STATUS_CREATE);
    assert(archive);

    for(const auto& [name, contents] : entries) {
        zip_fileinfo info{};

        if(symlink) info.external_fa = 0120777U << 16;

        assert(zipOpenNewFileInZip64(archive, name.c_str(), &info, nullptr, 0, nullptr, 0, nullptr, 0, 0, 0) == ZIP_OK);
        assert(zipWriteInFileInZip(archive, contents.data(), contents.size()) == ZIP_OK);
        assert(zipCloseFileInZip(archive) == ZIP_OK);
    }

    assert(zipClose(archive, nullptr) == ZIP_OK);
}
} // namespace

extern "C" DIR* __real_opendir(const char*);
extern "C" DIR* __wrap_opendir(const char* path) {
    if(open_directories.size() >= directory_limit) {
        errno = ENOSR;
        return nullptr;
    }
    DIR* directory = __real_opendir(path);
    if(directory) {
        open_directories.insert(directory);
        peak_directories = std::max(peak_directories, open_directories.size());
    }
    return directory;
}

extern "C" int __real_closedir(DIR*);
extern "C" int __wrap_closedir(DIR* directory) {
    assert(open_directories.erase(directory) == 1);
    return __real_closedir(directory);
}

extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" FILE* __wrap_fopen(const char* path, const char* mode) {
    const bool destination = !restore_prefix.empty() && std::string_view(path).starts_with(restore_prefix) && (std::strcmp(mode, "wb") == 0 || std::strcmp(mode, "r+b") == 0);
    if(destination && ((restore_failure == RestoreFailure::Open && std::strcmp(mode, "wb") == 0) || (restore_failure == RestoreFailure::Reopen && std::strcmp(mode, "r+b") == 0))) {
        errno = EACCES;
        return nullptr;
    }
    FILE* file = __real_fopen(path, mode);
    if(destination) restore_output = file;
    return file;
}

extern "C" size_t __real_fwrite(const void*, size_t, size_t, FILE*);
extern "C" size_t __wrap_fwrite(const void* data, size_t size, size_t count, FILE* file) {
    if(file == restore_output && restore_failure == RestoreFailure::Write) {
        errno = ENOSPC;
        return 0;
    }
    if(file == restore_output && simulated_write_limit) {
        if(uncommitted_bytes + size * count > simulated_write_limit) {
            errno = EIO;
            return 0;
        }
        uncommitted_bytes += size * count;
    }
    return __real_fwrite(data, size, count, file);
}

extern "C" int __real_fseeko(FILE*, off_t, int);
extern "C" int __wrap_fseeko(FILE* file, off_t offset, int origin) {
    if(file == restore_output && restore_failure == RestoreFailure::Seek) {
        errno = EIO;
        return -1;
    }
    return __real_fseeko(file, offset, origin);
}

extern "C" int __real_fflush(FILE*);
extern "C" int __wrap_fflush(FILE* file) {
    if(file == restore_output && restore_failure == RestoreFailure::Flush) {
        errno = EIO;
        return EOF;
    }
    return __real_fflush(file);
}

extern "C" int __real_fclose(FILE*);
extern "C" int __wrap_fclose(FILE* file) {
    const bool fail = file == restore_output && restore_failure == RestoreFailure::Close;
    if(file == restore_output) restore_output = nullptr;
    const int result = __real_fclose(file);
    if(fail) {
        errno = EIO;
        return EOF;
    }
    return result;
}

extern "C" int __real_unlink(const char*);
extern "C" int __wrap_unlink(const char* path) {
    if(restore_failure == RestoreFailure::Remove && blocked_remove == path) {
        errno = EACCES;
        return -1;
    }
    return __real_unlink(path);
}

extern "C" int __real_zipOpenNewFileInZip3_64(zipFile, const char*, const zip_fileinfo*, const void*, uInt, const void*, uInt, const char*, int, int, int, int, int, int, const char*, uLong, int);
extern "C" int __wrap_zipOpenNewFileInZip3_64(zipFile archive, const char* name, const zip_fileinfo* info, const void* local, uInt local_size, const void* global, uInt global_size, const char* comment, int method, int level, int raw, int window_bits, int memory_level, int strategy, const char* password, uLong crc, int zip64) {
    current_entry = name;
    if(method == Z_DEFLATED) assert(window_bits == -14 && memory_level == 4);
    if(open_failure != ZIP_OK && current_entry == failed_entry) {
        errno = failure_errno;
        return open_failure;
    }
    return __real_zipOpenNewFileInZip3_64(archive, name, info, local, local_size, global, global_size, comment, method, level, raw, window_bits, memory_level, strategy, password, crc, zip64);
}

extern "C" int __real_zipWriteInFileInZip(zipFile, const void*, unsigned);
extern "C" int __wrap_zipWriteInFileInZip(zipFile archive, const void* data, unsigned size) {
    if(write_failure != ZIP_OK && current_entry == failed_entry) {
        errno = failure_errno;
        return write_failure;
    }
    return __real_zipWriteInFileInZip(archive, data, size);
}

extern "C" int __real_zipCloseFileInZip(zipFile);
extern "C" int __wrap_zipCloseFileInZip(zipFile archive) {
    const int result = __real_zipCloseFileInZip(archive);
    if(entry_close_failure != ZIP_OK && current_entry == failed_entry) {
        errno = failure_errno;
        return entry_close_failure;
    }
    return result;
}

extern "C" int __real_zipClose(zipFile, const char*);
extern "C" int __wrap_zipClose(zipFile archive, const char* comment) {
    const int result = __real_zipClose(archive, comment);
    if(archive_close_failure != ZIP_OK) {
        errno = EIO;
        return archive_close_failure;
    }
    return result;
}

int main() {
    char directory[] = "/tmp/sbo-archive-test-XXXXXX";
    assert(mkdtemp(directory));
    const fs::path root(directory), save = root / "original";
    fs::create_directories(save / "empty");
    write_file(save / "nested/binary", std::string("a\0b\xff", 4));
    write_file(save / "slot", std::string(50000, 's'));
    const backup::Identity identity{"test-user", "test-game", "Timo", "Test Game", "Erista"};
    std::string hash, other, error;
    directory_limit = 0;
    assert(!backup::hash_save(save, hash, error));
    assert(error.find("filesystem buffers") != std::string::npos && open_directories.empty());
    directory_limit = 1;
    assert(backup::hash_save(save, hash, error));
    assert(hash.size() == 64);
    fs::last_write_time(save / "slot", fs::file_time_type::clock::now());
    assert(backup::hash_save(save, other, error) && other == hash);
    write_file(save / "slot", std::string(50000, 't'));
    assert(backup::hash_save(save, other, error) && other != hash);
    write_file(save / "slot", std::string(50000, 's'));
    fs::rename(save / "slot", save / "renamed");
    assert(backup::hash_save(save, other, error) && other != hash);
    fs::rename(save / "renamed", save / "slot");
    fs::remove(save / "empty");
    assert(backup::hash_save(save, other, error) && other != hash);
    fs::create_directory(save / "empty");

    const auto archive = root / "backup.zip";
    assert(backup::create_archive(save, archive, identity, other, error) && other == hash);
    unzFile zip = unzOpen64(archive.c_str());
    assert(zip && unzLocateFile(zip, "manifest.json", 1) == UNZ_OK && unzOpenCurrentFile(zip) == UNZ_OK);
    char manifest[2048]{};
    const int length = unzReadCurrentFile(zip, manifest, sizeof(manifest) - 1);
    assert(length > 0 && unzCloseCurrentFile(zip) == UNZ_OK && unzClose(zip) == UNZ_OK);
    auto* metadata = cJSON_Parse(manifest);
    assert(JsonHandler::string(metadata, "user_id") == identity.user_id);
    assert(JsonHandler::string(metadata, "user_name") == identity.user_name);
    assert(JsonHandler::string(metadata, "console_name") == "Erista");
    cJSON_Delete(metadata);
    std::string archive_hash;
    assert(backup::hash_file(archive, archive_hash, error));
    assert(backup::extract_archive(archive, root / "staged", identity, hash, error));
    assert(fs::is_directory(root / "staged/save/empty"));
    assert(backup::hash_save(root / "staged/save", other, error) && other == hash);
    write_file(save / "extra", "must be removed");
    assert(backup::replace_save(save, root / "staged/save", error));
    assert(!fs::exists(save / "extra"));
    assert(backup::hash_save(save, other, error) && other == hash);
    assert(!backup::extract_archive(archive, root / "wrong-user", {"other", "test-game"}, hash, error));
    assert(!backup::extract_archive(archive, root / "wrong-game", {"test-user", "other-game"}, hash, error));
    auto wrong_console = identity;
    wrong_console.console_name = "OLED";
    assert(!backup::extract_archive(archive, root / "wrong-console", wrong_console, hash, error));
    auto legacy = identity;
    legacy.console_name.clear();
    assert(backup::create_archive(save, root / "legacy.zip", legacy, other, error));
    assert(backup::extract_archive(root / "legacy.zip", root / "legacy", identity, other, error));
    assert(!backup::extract_archive(archive, root / "wrong-hash", identity, std::string(64, '0'), error));

    for(const auto& name : {"save/../escaped", "save//escaped", "save/./escaped", "/escaped", "save/C:escaped", "save/dir\\escaped"}) {
        const auto malicious = root / "malicious.zip";
        make_zip(malicious, {{name, "bad"}});
        fs::remove_all(root / "rejected");
        assert(!backup::extract_archive(malicious, root / "rejected", identity, hash, error));
        assert(!fs::exists(root / "escaped"));
    }

    make_zip(root / "duplicate.zip", {{"save/File", "a"}, {"save/file", "b"}});
    assert(!backup::extract_archive(root / "duplicate.zip", root / "duplicate", identity, hash, error));
    assert(error.find("duplicate") != std::string::npos);
    make_zip(root / "symlink.zip", {{"save/link", "../../escaped"}}, true);
    assert(!backup::extract_archive(root / "symlink.zip", root / "symlink", identity, hash, error));
    make_zip(root / "crc.zip", {{"save/slot", "ORIGINAL-PAYLOAD"}});
    {
        std::fstream file(root / "crc.zip", std::ios::in | std::ios::out | std::ios::binary);
        std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        const auto offset = bytes.find("ORIGINAL-PAYLOAD");
        assert(offset != std::string::npos);
        file.seekp(offset);
        file.put('X');
    }

    assert(!backup::extract_archive(root / "crc.zip", root / "crc", identity, hash, error));
    assert(error.find("CRC") != std::string::npos);
    fs::copy_file(archive, root / "truncated.zip");
    fs::resize_file(root / "truncated.zip", fs::file_size(archive) / 2);
    assert(!backup::extract_archive(root / "truncated.zip", root / "truncated", identity, hash, error));
    fs::create_symlink(save / "slot", save / "link");
    assert(!backup::hash_save(save, other, error));
    fs::remove(save / "link");
    std::atomic<bool> cancel{true};
    assert(!backup::create_archive(save, root / "cancelled.zip", identity, other, error, {&cancel, {}}));
    assert(!fs::exists(root / "cancelled.zip"));

    const auto failed_archive = root / "failed.zip";
    open_failure = Z_MEM_ERROR;
    failure_errno = ENOMEM;
    for(const char* name : {"save/", "save/slot", "manifest.json"}) {
        failed_entry = name;
        assert(!backup::create_archive(save, failed_archive, identity, other, error));
        assert(error.find(name) != std::string::npos);
        assert(error.find("Z_MEM_ERROR") != std::string::npos);
        assert(error.find("ZIP code -4") != std::string::npos);
        assert(!fs::exists(failed_archive));
    }

    failed_entry = "save/slot";
    open_failure = ZIP_ERRNO;
    failure_errno = ENOSPC;
    archive_close_failure = ZIP_ERRNO;
    assert(!backup::create_archive(save, failed_archive, identity, other, error));
    assert(error.find("Could not create ZIP entry 'save/slot'") != std::string::npos);
    assert(error.find(std::strerror(ENOSPC)) != std::string::npos);
    assert(error.find(std::strerror(EIO)) == std::string::npos);
    assert(!fs::exists(failed_archive));

    archive_close_failure = ZIP_OK;
    open_failure = ZIP_PARAMERROR;
    failure_errno = 0;
    assert(!backup::create_archive(save, failed_archive, identity, other, error));
    assert(error.find("ZIP_PARAMERROR") != std::string::npos);
    assert(error.find("errno") == std::string::npos);

    open_failure = ZIP_OK;
    write_failure = ZIP_ERRNO;
    failure_errno = ENOSPC;
    assert(!backup::create_archive(save, failed_archive, identity, other, error));
    assert(error.find("Could not write ZIP entry 'save/slot'") != std::string::npos);
    assert(error.find(std::strerror(ENOSPC)) != std::string::npos);
    assert(!fs::exists(failed_archive));

    write_failure = ZIP_OK;
    entry_close_failure = ZIP_ERRNO;
    assert(!backup::create_archive(save, failed_archive, identity, other, error));
    assert(error.find("Could not close ZIP entry 'save/slot'") != std::string::npos);
    assert(!fs::exists(failed_archive));

    entry_close_failure = ZIP_OK;
    archive_close_failure = ZIP_ERRNO;
    assert(!backup::create_archive(save, failed_archive, identity, other, error));
    assert(error.find("Could not finish backup ZIP") != std::string::npos);
    assert(!fs::exists(failed_archive));
    archive_close_failure = ZIP_OK;
    assert(backup::hash_save(save, other, error) && other == hash);

    const auto restore_destination = root / "restore-errors";
    restore_prefix = restore_destination.string() + "/";
    for(const auto failure : {RestoreFailure::Open, RestoreFailure::Write, RestoreFailure::Flush, RestoreFailure::Close}) {
        fs::remove_all(restore_destination);
        fs::create_directory(restore_destination);
        restore_failure = failure;
        assert(!backup::replace_save(restore_destination, root / "staged/save", error));
        assert(error.find((restore_destination / "nested/binary").string()) != std::string::npos);
        const int expected_errno = failure == RestoreFailure::Open ? EACCES : failure == RestoreFailure::Write ? ENOSPC
                                                                                                               : EIO;
        assert(error.find("errno " + std::to_string(expected_errno)) != std::string::npos);
        assert(error.find(std::strerror(expected_errno)) != std::string::npos);
        const char* operation = failure == RestoreFailure::Open ? "open" : failure == RestoreFailure::Write ? "write"
                                                                       : failure == RestoreFailure::Flush   ? "flush"
                                                                                                            : "close";
        assert(error.find(std::string("Could not ") + operation + " restore destination") != std::string::npos);
        restore_failure = RestoreFailure::None;
    }
    fs::remove_all(restore_destination);
    write_file(restore_destination / "keep", "original");
    blocked_remove = (restore_destination / "keep").string();
    restore_failure = RestoreFailure::Remove;
    assert(!backup::replace_save(restore_destination, root / "staged/save", error));
    assert(error.find("Could not remove file") != std::string::npos);
    assert(error.find(blocked_remove) != std::string::npos);
    assert(error.find(std::strerror(EACCES)) != std::string::npos);
    assert(fs::exists(blocked_remove));
    restore_failure = RestoreFailure::None;

    const auto restore_source = root / "restore-source-errors";
    fs::copy(root / "staged/save", restore_source, fs::copy_options::recursive);
    assert(!backup::replace_save(restore_destination, restore_source, error, {nullptr, [&](const std::string&, uint64_t, uint64_t) { fs::remove(restore_source / "slot"); }}));
    assert(error.find("Could not open restore source") != std::string::npos);
    assert(error.find((restore_source / "slot").string()) != std::string::npos);
    assert(error.find("errno " + std::to_string(ENOENT)) != std::string::npos);
    assert(!fs::exists(restore_destination / "slot"));

    const backup::SaveWriteOptions writes{4096, [&](const std::string&, std::string&) {
                                              assert(restore_output == nullptr);
                                              assert(uncommitted_bytes <= simulated_write_limit);
                                              uncommitted_bytes = 0;
                                              return true;
                                          }};
    simulated_write_limit = writes.bytes_per_commit;
    const auto chunk_source = root / "chunk-source";
    for(const auto size : {0U, 1U, 4096U, 4097U, 8192U, 8193U}) {
        std::string contents(size, '\0');
        for(size_t i = 0; i < contents.size(); ++i) contents[i] = static_cast<char>(i * 17 + i / 4096);
        write_file(chunk_source / "slot", contents);
        assert(backup::replace_save(restore_destination, chunk_source, error, {}, writes));
        std::string expected, actual;
        assert(backup::hash_save(chunk_source, expected, error));
        assert(backup::hash_save(restore_destination, actual, error) && actual == expected);
        assert(uncommitted_bytes == 0 && restore_output == nullptr);
    }

    for(const auto failure : {RestoreFailure::Reopen, RestoreFailure::Seek}) {
        restore_failure = failure;
        assert(!backup::replace_save(restore_destination, chunk_source, error, {}, writes));
        assert(error.find((restore_destination / "slot").string()) != std::string::npos);
        assert(error.find(failure == RestoreFailure::Reopen ? "Could not reopen restore destination" : "Could not seek restore destination") != std::string::npos);
        assert(restore_output == nullptr);
        restore_failure = RestoreFailure::None;
    }
    simulated_write_limit = 0;
    restore_prefix.clear();

    using backup::RestoreAction;
    using backup::RestoreChoice;
    backup::CloudBackup cloud;
    cloud.save_hash = hash;
    assert(backup::restore_action(hash, {cloud}, RestoreChoice::Check, "") == RestoreAction::Restore);
    assert(backup::restore_action(hash, {}, RestoreChoice::Check, "") == RestoreAction::Ask);
    assert(backup::restore_action(hash, {}, RestoreChoice::BackupFirst, hash) == RestoreAction::Upload);
    assert(backup::restore_action(hash, {}, RestoreChoice::WithoutBackup, hash) == RestoreAction::Restore);
    assert(backup::restore_action("changed", {}, RestoreChoice::WithoutBackup, hash) == RestoreAction::Ask);
    assert(backup::restore_action("changed", {}, RestoreChoice::BackupFirst, hash) == RestoreAction::Ask);

    // Mimic the small Switch heap: hashing, ZIP creation/extraction, restore and
    // cleanup must work with only one directory iterator, even in deep trees.
    const auto deep = root / "deep";
    write_file(deep / "0/1/2/3/4/slot", "nested save");
    fs::create_directories(deep / "2/empty");
    std::string deep_hash, verified;
    assert(backup::create_archive(deep, root / "deep.zip", identity, deep_hash, error));
    assert(backup::extract_archive(root / "deep.zip", root / "deep-staged", identity, deep_hash, error));
    const auto deep_destination = root / "deep-destination";
    write_file(deep_destination / "old/nested/slot", "previous save");
    assert(backup::replace_save(deep_destination, root / "deep-staged/save", error));
    assert(backup::hash_save(deep_destination, verified, error) && verified == deep_hash);
    assert(backup::remove_tree(deep_destination, error));
    assert(backup::remove_tree(deep, error));
    assert(backup::remove_tree(root / "deep-staged", error));
    assert(!fs::exists(deep_destination) && open_directories.empty() && peak_directories == 1);

    fs::remove_all(root);
    std::puts("Single-iterator directory traversal, deep saves, ZIP validation, restore I/O, cleanup, and restore decisions passed.");
}
