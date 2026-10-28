#include "include/SaveArchive.hpp"
#include "include/JsonHandler.hpp"
#include "include/Diagnostics.hpp"

#include <minizip/zip.h>
#include <minizip/unzip.h>
#ifdef __SWITCH__
#include <switch.h>
#include <mbedtls/sha256.h>
#else
#include <openssl/evp.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace backup {
namespace {
constexpr size_t MaxEntries = 8192;
constexpr size_t MaxPath = 480;
using File = std::unique_ptr<FILE, int (*)(FILE*)>;
using Directory = std::unique_ptr<DIR, int (*)(DIR*)>;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

std::string io_error(const char* operation, const std::string& path, int code) {
    // newlib's opendir reports ENOSR when allocating the fsdev iterator fails.
    // No FS request was made in that case; fsdevGetLastResult would be stale.
    if(code == ENOMEM || code == ENOSR) return filesystem_error(operation, path, code) + " Not enough memory for filesystem buffers.";
#ifdef __SWITCH__
    // fsdev exposes its last native failure, which can predate a POSIX error.
    const Result result = fsdevGetLastResult();
    return filesystem_error(operation, path, code, result);
#else
    return filesystem_error(operation, path, code);
#endif
}

bool zip_result(int result, const char* operation, const std::string& name, std::string& error) {
    const int code = errno;
    if(result == ZIP_OK) return true;
    if(!error.empty()) return false;

    const char* detail;
    switch(result) {
        case ZIP_ERRNO:
            detail = "ZIP_ERRNO (I/O error)";
            break;
        case ZIP_PARAMERROR:
            detail = "ZIP_PARAMERROR";
            break;
        case ZIP_BADZIPFILE:
            detail = "ZIP_BADZIPFILE";
            break;
        case ZIP_INTERNALERROR:
            detail = "ZIP_INTERNALERROR";
            break;
        case Z_MEM_ERROR:
            detail = "Z_MEM_ERROR (not enough memory)";
            break;
        case Z_STREAM_ERROR:
            detail = "Z_STREAM_ERROR";
            break;
        case Z_DATA_ERROR:
            detail = "Z_DATA_ERROR";
            break;
        case Z_BUF_ERROR:
            detail = "Z_BUF_ERROR";
            break;
        case Z_VERSION_ERROR:
            detail = "Z_VERSION_ERROR";
            break;
        default:
            detail = "unknown ZIP error";
            break;
    }
    error = std::string(operation) + " '" + name + "': " + detail + " (ZIP code " + std::to_string(result) + ")";
    if(code) error += " [errno " + std::to_string(code) + ": " + std::strerror(code) + "]";
    error += ".";
    return false;
}

bool open_zip_entry(zipFile archive, const std::string& name, bool directory, uint64_t size, std::string& error) {
    // A 16 KiB window and memLevel 4 use about 72 KiB instead of 256 KiB.
    errno = 0;
    const int result = zipOpenNewFileInZip3_64(archive, name.c_str(), nullptr, nullptr, 0, nullptr, 0, nullptr, directory ? 0 : Z_DEFLATED, directory ? 0 : Z_DEFAULT_COMPRESSION, 0, -14, 4, Z_DEFAULT_STRATEGY, nullptr, 0, size >= 0xffffffffULL);
    return zip_result(result, "Could not create ZIP entry", name, error);
}

bool write_zip_entry(zipFile archive, const std::string& name, const void* data, unsigned size, std::string& error) {
    errno = 0;
    const int result = zipWriteInFileInZip(archive, data, size);
    return zip_result(result, "Could not write ZIP entry", name, error);
}

bool close_zip_entry(zipFile archive, const std::string& name, std::string& error) {
    errno = 0;
    const int result = zipCloseFileInZip(archive);
    return zip_result(result, "Could not close ZIP entry", name, error);
}

class Digest {
public:
    Digest() {
#ifdef __SWITCH__
        mbedtls_sha256_init(&context);
        valid = mbedtls_sha256_starts_ret(&context, 0) == 0;
#else
        context = EVP_MD_CTX_new();
        valid = context && EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
#endif
    }

    ~Digest() {
#ifdef __SWITCH__
        mbedtls_sha256_free(&context);
#else
        EVP_MD_CTX_free(context);
#endif
    }

    void update(const void* data, size_t size) {
#ifdef __SWITCH__
        if(valid) valid = mbedtls_sha256_update_ret(&context, static_cast<const unsigned char*>(data), size) == 0;
#else
        if(valid) valid = EVP_DigestUpdate(context, data, size) == 1;
#endif
    }

    void number(uint64_t value) {
        std::array<unsigned char, 8> bytes{};

        for(size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<unsigned char>(value >> (i * 8));
        update(bytes.data(), bytes.size());
    }

    bool finish(std::string& hash) {
        std::array<unsigned char, 32> bytes{};
#ifdef __SWITCH__
        if(valid) valid = mbedtls_sha256_finish_ret(&context, bytes.data()) == 0;
#else
        unsigned int size = 0;

        if(valid) valid = EVP_DigestFinal_ex(context, bytes.data(), &size) == 1 && size == bytes.size();
#endif
        if(!valid) return false;

        static constexpr char digits[] = "0123456789abcdef";
        hash.clear();

        for(unsigned char byte : bytes) {
            hash += digits[byte >> 4];
            hash += digits[byte & 15];
        }

        return true;
    }

private:
    bool valid = false;
#ifdef __SWITCH__
    mbedtls_sha256_context context{};
#else
    EVP_MD_CTX* context = nullptr;
#endif
};

struct Entry {
    std::string path;
    bool directory = false;
    uint64_t size = 0;
};

std::string join(const std::string& root, const std::string& path) { return root + (root.ends_with('/') ? "" : "/") + path; }

bool cancelled(const Control& control, std::string& error) {
    if(!control.stopped()) return false;

    error = "Operation cancelled.";
    return true;
}

bool relative_path(const std::string& path) {
    if(path.empty() || path.size() > MaxPath || path.front() == '/' || path.find_first_of("\\:\r\n") != std::string::npos || path.find('\0') != std::string::npos) return false;

    for(size_t begin = 0; begin < path.size();) {
        size_t end = path.find('/', begin);
        const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);

        if(part.empty() || part == "." || part == "..") return false;
        if(end == std::string::npos) break;

        begin = end + 1;
    }

    return !path.ends_with('/');
}

bool collect(const std::string& root, const std::string& relative, std::vector<Entry>& entries, uint64_t& total, std::string& error, const Control& control, unsigned depth = 0) {
    if(depth > 32 || cancelled(control, error)) {
        if(error.empty()) error = "Save directory nesting is too deep.";

        return false;
    }

    const size_t begin = entries.size();
    {
        const auto path = join(root, relative);
        Directory directory(opendir(path.c_str()), closedir);

        if(!directory) {
            const int code = errno;
            error = io_error("Could not open save directory", path, code);
            return false;
        }

        errno = 0;

        while(dirent* entry = readdir(directory.get())) {
            if(cancelled(control, error)) return false;
            if(std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;

            const std::string name = relative.empty() ? entry->d_name : relative + "/" + entry->d_name;

            if(!relative_path(name) || entries.size() >= MaxEntries) {
                error = "Save has too many entries or an unsupported path.";
                return false;
            }

            struct stat info{};

            const auto entry_path = join(root, name);
            if(lstat(entry_path.c_str(), &info) != 0) {
                const int code = errno;
                error = io_error("Could not inspect save entry", entry_path, code);
                return false;
            }
            if(!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode)) {
                error = "Save entry is not a regular file or directory: '" + entry_path + "'.";
                return false;
            }

            const bool is_directory = S_ISDIR(info.st_mode);
            const uint64_t size = is_directory ? 0 : static_cast<uint64_t>(info.st_size);

            if(size > MaxSaveBytes || total > MaxSaveBytes - size) {
                error = "Save exceeds the supported size (8 GiB).";
                return false;
            }

            total += size;
            entries.push_back({name, is_directory, size});
            errno = 0;
        }

        if(errno != 0) {
            const int code = errno;
            error = io_error("Could not read save directory completely", path, code);
            return false;
        }
    }

    // Close this iterator before descending. Switch directory iterators own
    // entry buffers, so keeping every ancestor open exhausts a small heap.
    const size_t end = entries.size();
    for(size_t index = begin; index < end; ++index) {
        if(!entries[index].directory) continue;
        // Recursion may reallocate entries; never pass a reference into it.
        const auto child = entries[index].path;
        if(!collect(root, child, entries, total, error, control, depth + 1)) return false;
    }

    return true;
}

bool process_tree(const std::string& root, zipFile archive, std::string& hash, std::string& error, const Control& control) {
    std::vector<Entry> entries;
    uint64_t total = 0, done = 0;

    if(!collect(root, "", entries, total, error, control)) return false;

    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) { return left.path < right.path; });

    Digest digest;
    static constexpr char domain[] = "SwitchCloudBackup/save/v1";
    digest.update(domain, sizeof(domain));
    std::array<unsigned char, 16384> buffer{};

    for(const auto& entry : entries) {
        if(cancelled(control, error)) return false;

        const char kind = entry.directory ? 'D' : 'F';
        digest.update(&kind, 1);
        digest.number(entry.path.size());
        digest.update(entry.path.data(), entry.path.size());
        digest.number(entry.size);

        const std::string name = "save/" + entry.path + (entry.directory ? "/" : "");
        if(archive && !open_zip_entry(archive, name, entry.directory, entry.size, error)) return false;

        bool ok = true;

        if(!entry.directory) {
            const auto path = join(root, entry.path);
            errno = 0;
            File file(std::fopen(path.c_str(), "rb"), std::fclose);

            if(!file) {
                const int code = errno;
                error = io_error("Could not open save file", path, code);
                ok = false;
            }

            uint64_t read = 0;

            while(ok && read < entry.size) {
                if(cancelled(control, error)) {
                    ok = false;
                    break;
                }

                const size_t requested = static_cast<size_t>(std::min<uint64_t>(buffer.size(), entry.size - read));
                errno = 0;
                const size_t count = std::fread(buffer.data(), 1, requested, file.get());

                if(count != requested) {
                    const int code = errno;
                    error = std::ferror(file.get()) ? io_error("Could not read save file", path, code) : "Save file size changed while reading '" + path + "'.";
                    ok = false;
                    break;
                }

                digest.update(buffer.data(), count);

                if(archive && !write_zip_entry(archive, name, buffer.data(), count, error)) {
                    ok = false;
                    break;
                }

                read += count;
                done += count;
                control.report(archive ? "Creating backup archive ..." : "Hashing current save ...", done, total);
            }

            if(ok) {
                errno = 0;
                const int next = std::fgetc(file.get());
                const int code = errno;
                if(next != EOF || std::ferror(file.get())) {
                    error = std::ferror(file.get()) ? io_error("Could not read save file", path, code) : "Save file size changed while reading '" + path + "'.";
                    ok = false;
                }
            }
        }

        if(archive && !close_zip_entry(archive, name, error)) ok = false;

        if(!ok) {
            if(error.empty()) error = "Could not read the complete save or write the archive. The save may have changed.";

            return false;
        }
    }

    if(!digest.finish(hash)) {
        error = "Could not calculate save hash.";
        return false;
    }

    return true;
}

bool clear_directory(const std::string& path, std::string& error) {
    std::vector<std::string> children;
    {
        Directory directory(opendir(path.c_str()), closedir);

        if(!directory) {
            const int code = errno;
            error = io_error("Could not open directory for removal", path, code);
            return false;
        }

        errno = 0;

        while(dirent* entry = readdir(directory.get())) {
            if(std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
            children.emplace_back(entry->d_name);
            errno = 0;
        }

        if(errno != 0) {
            const int code = errno;
            error = io_error("Could not read directory for removal", path, code);
            return false;
        }
    }

    for(const auto& child : children) {
        if(!remove_tree(join(path, child), error)) return false;
    }

    return true;
}

bool manifest(const std::string& text, const Identity& expected, std::string& hash) {
    if(text.empty() || text.size() > 16384 || text.find('\0') != std::string::npos) return false;

    Json json(cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, nullptr, true), cJSON_Delete);
    const auto* version = cJSON_GetObjectItemCaseSensitive(json.get(), "version");
    hash = JsonHandler::string(json.get(), "save_hash");
    const auto console = JsonHandler::string(json.get(), "console_name");
    return cJSON_IsNumber(version) && version->valuedouble == 1 && JsonHandler::string(json.get(), "user_id") == expected.user_id && JsonHandler::string(json.get(), "application_id") == expected.application_id && (console.empty() || expected.console_name.empty() || console == expected.console_name) && hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
} // namespace

bool make_directories(const std::string& path, std::string& error) {
    for(size_t pos = 0; pos <= path.size(); ++pos) {
        if(pos != path.size() && path[pos] != '/') continue;

        const auto directory = path.substr(0, pos);

        if(directory.empty() || directory.back() == ':') continue;
        if(::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
            const int code = errno;
            error = io_error("Could not create backup directory", directory, code);
            return false;
        }

        struct stat info{};

        if(lstat(directory.c_str(), &info) != 0) {
            const int code = errno;
            error = io_error("Could not inspect backup directory", directory, code);
            return false;
        }
        if(!S_ISDIR(info.st_mode)) {
            error = "Backup path is not a regular directory: '" + directory + "'.";
            return false;
        }
    }

    return true;
}

bool remove_tree(const std::string& path, std::string& error) {
    struct stat info{};

    if(lstat(path.c_str(), &info) != 0) {
        if(errno == ENOENT) return true;

        const int code = errno;
        error = io_error("Could not inspect file for removal", path, code);
        return false;
    }

    const bool directory = S_ISDIR(info.st_mode);

    if(directory && !clear_directory(path, error)) return false;
    if((directory ? ::rmdir(path.c_str()) : ::unlink(path.c_str())) != 0) {
        const int code = errno;
        error = io_error(directory ? "Could not remove directory" : "Could not remove file", path, code);
        return false;
    }

    return true;
}

bool hash_save(const std::string& root, std::string& hash, std::string& error, const Control& control) {
    error.clear();
    return process_tree(root, nullptr, hash, error, control);
}

bool hash_file(const std::string& path, std::string& hash, std::string& error, const Control& control) {
    error.clear();
    File file(std::fopen(path.c_str(), "rb"), std::fclose);

    if(!file) {
        error = "Could not open archive for hashing.";
        return false;
    }

    Digest digest;
    std::array<unsigned char, 16384> buffer{};
    size_t count;

    while((count = std::fread(buffer.data(), 1, buffer.size(), file.get())) > 0) {
        if(cancelled(control, error)) return false;

        digest.update(buffer.data(), count);
    }

    if(std::ferror(file.get()) || !digest.finish(hash)) {
        error = "Could not hash the complete archive.";
        return false;
    }

    return true;
}

bool create_archive(const std::string& root, const std::string& path, const Identity& identity, std::string& save_hash, std::string& error, const Control& control) {
    error.clear();
    errno = 0;
    zipFile archive = zipOpen64(path.c_str(), APPEND_STATUS_CREATE);

    if(!archive) {
        const int code = errno;
        error = io_error("Could not create backup ZIP", path, code);
        return false;
    }

    bool ok = open_zip_entry(archive, "save/", true, 0, error);

    if(ok) ok = close_zip_entry(archive, "save/", error);

    if(ok) ok = process_tree(root, archive, save_hash, error, control);

    if(ok) {
        Json json(cJSON_CreateObject(), cJSON_Delete);
        ok = json && cJSON_AddNumberToObject(json.get(), "version", 1) && JsonHandler::set_string(json.get(), "user_id", identity.user_id) && JsonHandler::set_string(json.get(), "application_id", identity.application_id) && JsonHandler::set_string(json.get(), "user_name", identity.user_name) && JsonHandler::set_string(json.get(), "console_name", identity.console_name) && JsonHandler::set_string(json.get(), "save_hash", save_hash);
        char* text = ok ? cJSON_PrintUnformatted(json.get()) : nullptr;

        if(text) {
            ok = open_zip_entry(archive, "manifest.json", false, std::strlen(text), error);

            if(ok) {
                ok = write_zip_entry(archive, "manifest.json", text, std::strlen(text), error);

                if(!close_zip_entry(archive, "manifest.json", error)) ok = false;
            }

            cJSON_free(text);
        }
        else {
            error = "Could not allocate or encode backup manifest.";
            ok = false;
        }
    }

    errno = 0;
    const int close_result = zipClose(archive, nullptr);
    if(!zip_result(close_result, "Could not finish backup ZIP", path, error)) ok = false;

    struct stat info{};

    if(ok && stat(path.c_str(), &info) != 0) {
        const int code = errno;
        error = io_error("Could not inspect backup ZIP", path, code);
        ok = false;
    }
    if(ok && static_cast<uint64_t>(info.st_size) > MaxArchiveBytes) {
        error = "Backup ZIP exceeds the maximum size (4 GiB minus one byte).";
        ok = false;
    }

    if(ok) {
        errno = 0;
        File file(std::fopen(path.c_str(), "r+b"), std::fclose);

        if(!file || ::fsync(::fileno(file.get())) != 0) {
            const int code = errno;
            error = io_error("Could not flush backup ZIP to SD", path, code);
            ok = false;
        }
    }

    if(!ok) {
        ::unlink(path.c_str());

        if(error.empty()) error = "Could not finish backup ZIP (maximum archive size is 4 GiB minus one byte).";
    }

    return ok;
}

bool extract_archive(const std::string& path, const std::string& destination, const Identity& identity, const std::string& expected_hash, std::string& error, const Control& control) {
    error.clear();

    if(!make_directories(destination, error)) return false;

    Directory existing(opendir(destination.c_str()), closedir);

    if(!existing) {
        error = "Could not open restore staging directory.";
        return false;
    }

    while(dirent* entry = readdir(existing.get())) {
        if(std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
            error = "Restore staging directory must be empty.";
            return false;
        }
    }

    existing.reset();

    unzFile archive = unzOpen64(path.c_str());

    if(!archive) {
        error = "Downloaded file is not a readable ZIP.";
        return false;
    }

    unz_global_info64 global{};
    bool ok = unzGetGlobalInfo64(archive, &global) == UNZ_OK && global.number_entry > 0 && global.number_entry <= MaxEntries + 2;

    if(!ok) error = "Backup ZIP has an invalid number of entries.";

    int next = ok ? unzGoToFirstFile(archive) : UNZ_END_OF_LIST_OF_FILE;
    uint64_t total = 0;
    bool got_manifest = false;
    std::string recorded_hash;
    std::set<std::string> seen;
    std::array<unsigned char, 16384> buffer{};

    for(uint64_t index = 0; ok && index < global.number_entry; ++index) {
        if(cancelled(control, error)) {
            ok = false;
            break;
        }

        unz_file_info64 info{};
        std::array<char, MaxPath + 16> filename{};

        if(next != UNZ_OK || unzGetCurrentFileInfo64(archive, &info, filename.data(), filename.size(), nullptr, 0, nullptr, 0) != UNZ_OK || info.size_filename == 0 || info.size_filename >= filename.size() || std::strlen(filename.data()) != info.size_filename || (info.flag & 1) || (info.compression_method != 0 && info.compression_method != Z_DEFLATED) || ((info.external_fa >> 16) & 0170000) == 0120000) {
            error = "Backup ZIP contains an unsupported or invalid entry.";
            ok = false;
            break;
        }

        const std::string name(filename.data(), info.size_filename);
        const bool is_manifest = name == "manifest.json";
        const bool directory = name.ends_with('/');
        std::string relative = name.starts_with("save/") ? name.substr(5) : "";

        if(directory && !relative.empty()) relative.pop_back();

        if((!is_manifest && name != "save/" && !relative_path(relative)) || (!is_manifest && !name.starts_with("save/")) || (directory && info.uncompressed_size != 0) || (is_manifest && info.uncompressed_size > 16384) || (!is_manifest && (info.uncompressed_size > MaxSaveBytes || total > MaxSaveBytes - info.uncompressed_size))) {
            error = "Backup ZIP contains an unsafe path or excessive data.";
            ok = false;
            break;
        }

        std::string folded = name;
        std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if(!seen.insert(folded).second) {
            error = "Backup ZIP contains duplicate paths.";
            ok = false;
            break;
        }

        if(!is_manifest) total += info.uncompressed_size;

        const std::string target = is_manifest ? "" : join(destination, "save" + (relative.empty() ? "" : "/" + relative));

        if(directory && !make_directories(target, error)) {
            ok = false;
            break;
        }

        File output(nullptr, std::fclose);

        if(!directory && !is_manifest) {
            if(!make_directories(target.substr(0, target.find_last_of('/')), error)) {
                ok = false;
                break;
            }

            int descriptor = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);

            if(descriptor >= 0) output.reset(::fdopen(descriptor, "wb"));

            if(!output) {
                if(descriptor >= 0) ::close(descriptor);

                error = "Could not create restore staging file. Paths may conflict.";
                ok = false;
                break;
            }
        }

        if(unzOpenCurrentFile(archive) != UNZ_OK) {
            error = "Could not open ZIP entry.";
            ok = false;
            break;
        }

        std::string metadata;
        uint64_t read = 0;
        int count = 0;

        while((count = unzReadCurrentFile(archive, buffer.data(), buffer.size())) > 0) {
            if(cancelled(control, error) || read + count > info.uncompressed_size) {
                ok = false;
                break;
            }

            read += count;

            if(is_manifest) metadata.append(reinterpret_cast<const char*>(buffer.data()), count);
            else if(!directory && std::fwrite(buffer.data(), 1, count, output.get()) != static_cast<size_t>(count)) {
                error = "Could not write restore staging file.";
                ok = false;
                break;
            }
        }

        const int close_result = unzCloseCurrentFile(archive);

        if(count < 0 || read != info.uncompressed_size || close_result != UNZ_OK) {
            error = "Backup ZIP is truncated or failed its CRC check.";
            ok = false;
        }

        if(output) {
            if(std::fflush(output.get()) != 0) ok = false;

            FILE* file = output.release();

            if(std::fclose(file) != 0) ok = false;
        }

        if(ok && is_manifest) {
            got_manifest = manifest(metadata, identity, recorded_hash);

            if(!got_manifest) {
                error = "Backup manifest does not match this user/game or backup format.";
                ok = false;
            }
        }

        control.report("Validating downloaded backup ...", index + 1, global.number_entry);
        next = unzGoToNextFile(archive);
    }

    if(unzClose(archive) != UNZ_OK) ok = false;

    if(ok && (!got_manifest || (!expected_hash.empty() && recorded_hash != expected_hash))) {
        error = "Backup manifest or save hash does not match the Drive metadata.";
        ok = false;
    }

    if(ok) {
        std::string extracted_hash;
        ok = hash_save(join(destination, "save"), extracted_hash, error, control);

        if(ok && extracted_hash != recorded_hash) {
            error = "Extracted save failed its SHA-256 check.";
            ok = false;
        }
    }

    if(!ok && error.empty()) error = "Could not validate downloaded backup.";

    return ok;
}

bool replace_save(const std::string& root, const std::string& staged_save, std::string& error, const Control& control, const SaveWriteOptions& writes) {
    error.clear();
    if(writes.bytes_per_commit == 0) {
        error = "Restore write limit must be greater than zero.";
        return false;
    }

    const auto commit = [&](const std::string& path) { return !writes.commit || writes.commit(path, error); };
    std::vector<Entry> entries;
    uint64_t total = 0, done = 0;

    if(!collect(staged_save, "", entries, total, error, control)) return false;
    {
        std::vector<Entry> existing;
        uint64_t previous_size = 0;
        if(!collect(root, "", existing, previous_size, error, control)) return false;
        // Children sort after their parent. Close all directory handles before
        // mutating the save and committing each metadata operation.
        std::sort(existing.begin(), existing.end(), [](const Entry& left, const Entry& right) { return left.path > right.path; });
        for(const auto& entry : existing) {
            if(cancelled(control, error)) return false;
            const auto path = join(root, entry.path);
            if((entry.directory ? ::rmdir(path.c_str()) : ::unlink(path.c_str())) != 0) {
                const int code = errno;
                error = io_error(entry.directory ? "Could not remove directory" : "Could not remove file", path, code);
                return false;
            }
            if(!commit(path)) return false;
        }
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) { return left.path < right.path; });
    std::array<unsigned char, 16384> buffer{};

    for(const auto& entry : entries) {
        if(cancelled(control, error)) return false;

        const auto target = join(root, entry.path);

        if(entry.directory) {
            if(::mkdir(target.c_str(), 0700) != 0) {
                const int code = errno;
                error = io_error("Could not create restore directory", target, code);
                return false;
            }
            if(!commit(target)) return false;

            continue;
        }

        const auto source = join(staged_save, entry.path);
        errno = 0;
        File input(std::fopen(source.c_str(), "rb"), std::fclose);
        if(!input) {
            const int code = errno;
            error = io_error("Could not open restore source", source, code);
            return false;
        }

        errno = 0;
        File output(std::fopen(target.c_str(), "wb"), std::fclose);
        if(!output) {
            const int code = errno;
            error = io_error("Could not open restore destination", target, code);
            return false;
        }

        const auto close_and_commit = [&]() {
            errno = 0;
            if(std::fflush(output.get()) != 0) {
                const int code = errno;
                error = io_error("Could not flush restore destination", target, code);
                return false;
            }
            FILE* file = output.release();
            errno = 0;
            if(std::fclose(file) != 0) {
                const int code = errno;
                error = io_error("Could not close restore destination", target, code);
                return false;
            }
            return commit(target);
        };

        // Persist file creation separately from its data. Never commit with a
        // writable handle open, and never reopen with truncation after a commit.
        if(writes.commit) {
            if(!close_and_commit()) return false;
            output.reset(std::fopen(target.c_str(), "r+b"));
            if(!output) {
                const int code = errno;
                error = io_error("Could not reopen restore destination", target, code);
                return false;
            }
        }

        uint64_t copied = 0;
        uint64_t pending = 0;
        while(true) {
            errno = 0;
            const size_t requested = static_cast<size_t>(std::min<uint64_t>(buffer.size(), writes.bytes_per_commit - pending));
            const size_t count = std::fread(buffer.data(), 1, requested, input.get());
            const int read_error = errno;
            if(std::ferror(input.get())) {
                error = io_error("Could not read restore source", source, read_error);
                return false;
            }
            if(count == 0) break;
            if(cancelled(control, error)) return false;
            errno = 0;
            if(std::fwrite(buffer.data(), 1, count, output.get()) != count) {
                const int code = errno;
                error = io_error("Could not write restore destination", target, code);
                return false;
            }

            copied += count;
            pending += count;
            done += count;
            control.report("Restoring complete save ...", done, total);

            if(pending == writes.bytes_per_commit) {
                if(!close_and_commit()) return false;
                pending = 0;
                errno = 0;
                output.reset(std::fopen(target.c_str(), "r+b"));
                if(!output) {
                    const int code = errno;
                    error = io_error("Could not reopen restore destination", target, code);
                    return false;
                }
                errno = 0;
                if(::fseeko(output.get(), static_cast<off_t>(copied), SEEK_SET) != 0) {
                    const int code = errno;
                    error = io_error("Could not seek restore destination", target, code);
                    return false;
                }
            }
        }

        if(copied != entry.size) {
            error = "Restore source '" + source + "' changed size (expected " + std::to_string(entry.size) + " bytes, read " + std::to_string(copied) + ").";
            return false;
        }
        if(!close_and_commit()) return false;
    }

    return true;
}

} // namespace backup
