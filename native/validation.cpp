#include "manifest.hpp"
#include <winioctl.h>
#include <cstddef>
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

USN fileChangeNumber(HANDLE handle) {
    READ_FILE_USN_DATA versions{2, 3};
    struct { USN_RECORD_UNION record; WCHAR name[MAX_PATH]; } result{};
    DWORD bytes = 0;
    if (!DeviceIoControl(handle, FSCTL_READ_FILE_USN_DATA, &versions, sizeof(versions),
        &result, sizeof(result), &bytes, nullptr)) return 0;
    if (bytes >= offsetof(USN_RECORD_V2, FileName) && result.record.Header.MajorVersion == 2) return result.record.V2.Usn;
    if (bytes >= offsetof(USN_RECORD_V3, FileName) && result.record.Header.MajorVersion == 3) return result.record.V3.Usn;
    return 0;
}

Stamp stamp(HANDLE handle, const std::string& expected) {
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)))
        throw std::runtime_error("Cannot inspect bundle metadata");
    const auto size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const auto identity = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    // Two writes may share a clock tick. Only persist a stamp if the filesystem
    // also supplies a change-journal sequence number for this file.
    const auto usn = fileChangeNumber(handle);
    if (usn <= 0) return {{}, size};
    return {expected + '\t' + std::to_string(info.dwVolumeSerialNumber) + ':' +
        std::to_string(identity) + ':' + std::to_string(size) + ':' + std::to_string(basic.LastWriteTime.QuadPart) + ':' +
        std::to_string(basic.ChangeTime.QuadPart) + ':' + std::to_string(basic.CreationTime.QuadPart) + ':' + std::to_string(usn), size};
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
        // Keep the inspected file readable but closed to writers and renames
        // until the hash (or the cache lookup) is complete.
        FileHandle handle{CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        const auto before = stamp(handle.value, expected);
        if (before.key.empty() || (!previous.contains(before.key) && !current.contains(before.key))) {
            if (sha256(file) != expected)
                throw std::runtime_error("A game resource or generated bundle changed; prepare Mods again");
            ++verified.files;
            verified.bytes += before.size;
        }
        if (!before.key.empty()) current.insert(before.key);
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
