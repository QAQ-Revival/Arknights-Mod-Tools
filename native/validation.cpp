#include "manifest.hpp"
#include <fstream>
#include <set>
#include <stdexcept>

namespace ark {
namespace {
struct FileHandle {
    HANDLE value;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Stamp { std::string key; uint64_t size; };

Stamp stamp(const fs::path& file, const std::string& expected) {
    FileHandle handle{CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    if (handle.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle.value, &info) ||
        !GetFileInformationByHandleEx(handle.value, FileBasicInfo, &basic, sizeof(basic)))
        throw std::runtime_error("Cannot inspect bundle metadata");
    const auto size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const auto identity = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    // ChangeTime also invalidates same-size edits whose LastWriteTime was restored.
    return {expected + '\t' + std::to_string(info.dwVolumeSerialNumber) + ':' +
        std::to_string(identity) + ':' + std::to_string(size) + ':' + std::to_string(basic.LastWriteTime.QuadPart) + ':' +
        std::to_string(basic.ChangeTime.QuadPart) + ':' + std::to_string(basic.CreationTime.QuadPart), size};
}
}

Verification verifyManifest(const Manifest& manifest, bool full) {
    const auto cache = manifest.file.parent_path() / L".qaqr-verified.tsv";
    if (hasLink(cache)) throw std::runtime_error("Linked verification cache is not supported");
    std::set<std::string> previous, current;
    if (!full) {
        std::ifstream input(cache, std::ios::binary);
        for (std::string line; std::getline(input, line);) previous.insert(std::move(line));
    }
    Verification verified;
    auto verify = [&](const fs::path& file, const std::string& expected) {
        const auto before = stamp(file, expected);
        if (!previous.contains(before.key) && !current.contains(before.key)) {
            if (sha256(file) != expected || stamp(file, expected).key != before.key)
                throw std::runtime_error("A game resource or generated bundle changed; prepare Mods again");
            ++verified.files;
            verified.bytes += before.size;
        }
        current.insert(before.key);
    };
    for (const auto& entry : manifest.entries) {
        verify(entry.source, entry.sourceHash);
        if (!entry.replacement.empty()) verify(entry.replacement, entry.replacementHash);
    }
    if (current != previous) {
        // A partial/missing cache is harmless: absent entries are hashed again.
        std::ofstream output(cache, std::ios::binary | std::ios::trunc);
        for (const auto& key : current) output << key << '\n';
    }
    return verified;
}
}
