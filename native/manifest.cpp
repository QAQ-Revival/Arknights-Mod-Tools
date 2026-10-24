#include "manifest.hpp"
#include <bcrypt.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <cwctype>
#pragma comment(lib, "bcrypt.lib")

namespace ark {
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid Unicode path");
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid UTF-8 path");
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
std::wstring normalized(const fs::path& file) {
    auto value = fs::absolute(file).lexically_normal().wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.rfind(L"\\\\?\\", 0) == 0) value.erase(0, 4);
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    while (value.size() > 3 && value.back() == L'\\') value.pop_back();
    return value;
}
bool within(const fs::path& file, const fs::path& root) {
    const auto value = normalized(file), base = normalized(root);
    return value == base || (value.size() > base.size() && value.compare(0, base.size(), base) == 0 && value[base.size()] == L'\\');
}
bool hasLink(const fs::path& file) {
    auto current = fs::absolute(file).lexically_normal();
    while (!current.empty()) {
        DWORD flags = GetFileAttributesW(current.c_str());
        if (flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
        auto parent = current.parent_path(); if (parent == current) break; current = parent;
    }
    return false;
}
std::string sha256(const fs::path& file) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("SHA-256 unavailable");
    unsigned char digest[32]{};
    try {
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error("SHA-256 initialization failed");
        std::ifstream stream(file, std::ios::binary); if (!stream) throw std::runtime_error("Cannot read manifest resource");
        std::vector<unsigned char> buffer(1024 * 1024);
        while (stream) { stream.read(reinterpret_cast<char*>(buffer.data()), buffer.size()); auto count = stream.gcount(); if (count && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0) < 0) throw std::runtime_error("SHA-256 failed"); }
        if (!stream.eof()) throw std::runtime_error("Resource read failed");
        if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) throw std::runtime_error("SHA-256 failed");
    } catch (...) { if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); throw; }
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    std::ostringstream result; for (auto value : digest) result << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(value); return result.str();
}
static std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields; std::stringstream stream(line); std::string item;
    while (std::getline(stream, item, '\t')) fields.push_back(item);
    return fields;
}
static fs::path relativeFile(const std::string& value, const fs::path& root) {
    auto file = fs::path(wide(value));
    if (file.empty() || file.is_absolute() || file.has_root_name() || value.find(':') != std::string::npos || value.find('\0') != std::string::npos) throw std::runtime_error("Invalid relative resource path");
    for (auto& segment : file) if (segment == L".." || segment == L".") throw std::runtime_error("Traversal in manifest");
    auto result = (root / file).lexically_normal();
    if (!within(result, root) || hasLink(result) || !fs::is_regular_file(result)) throw std::runtime_error("Manifest resource is missing or redirected");
    if (result.extension() != L".ab") throw std::runtime_error("Only full AssetBundle files are supported");
    return result;
}
Manifest readManifest(const fs::path& file, bool verifyHashes) {
    if (hasLink(file) || !fs::is_regular_file(file) || fs::file_size(file) > 4 * 1024 * 1024) throw std::runtime_error("Invalid manifest file");
    std::ifstream stream(file, std::ios::binary); std::string line;
    auto next = [&]() { if (!std::getline(stream, line)) return false; if (!line.empty() && line.back() == '\r') line.pop_back(); return true; };
    if (!next() || (line != "QAQR-ARKNIGHTS-BUNDLES\t1" && line != "QAQR-ARKNIGHTS-PATCH\t1")) throw std::runtime_error("Unsupported manifest version");
    const bool partial = line == "QAQR-ARKNIGHTS-PATCH\t1";
    if (!next()) throw std::runtime_error("Missing game path");
    auto fields = split(line);
    if (fields.size() != 2 || fields[0] != "GAME") throw std::runtime_error("Invalid game entry");
    Manifest result; result.partial = partial; result.file = fs::absolute(file); result.game = fs::path(wide(fields[1]));
    if (!result.game.is_absolute() || normalized(result.game.filename()) != normalized(L"Arknights.exe") || !fs::is_regular_file(result.game) || hasLink(result.game)) throw std::runtime_error("Invalid Arknights executable");
    if (within(file, result.game.parent_path())) throw std::runtime_error("Game directory must remain read-only");
    std::map<std::wstring, bool> seen;
    while (next()) {
        if (line.empty()) continue;
        fields = split(line);
        const bool remove = partial && fields.size() == 3 && fields[0] == "REMOVE";
        if ((!remove && (fields.size() != 5 || fields[0] != "BUNDLE")) || result.entries.size() >= 4096) throw std::runtime_error("Invalid bundle entry");
        Entry entry { relativeFile(fields[1], result.game.parent_path()), remove ? fs::path{} : relativeFile(fields[2], result.file.parent_path()), fields[remove ? 2 : 3], remove ? std::string{} : fields[4] };
        if (!within(entry.source, result.game.parent_path() / L"Arknights_Data") || (!remove && within(entry.replacement, result.game.parent_path()))) throw std::runtime_error("Invalid bundle boundaries");
        if (entry.sourceHash.size() != 64 || (!remove && entry.replacementHash.size() != 64) || !seen.emplace(normalized(entry.source), true).second) throw std::runtime_error("Invalid or duplicate bundle identity");
        if (verifyHashes && (sha256(entry.source) != entry.sourceHash || (!remove && sha256(entry.replacement) != entry.replacementHash))) throw std::runtime_error("A game resource or generated bundle changed; prepare Mods again");
        result.entries.push_back(std::move(entry));
    }
    return result;
}
std::string jsonString(const std::string& value) {
    std::string result = "\"";
    for (unsigned char c : value) { if (c == '"' || c == '\\') { result += '\\'; result += c; } else if (c < 32) result += ' '; else result += c; }
    return result + "\"";
}
void writeStatus(const fs::path& directory, const std::string& state, const std::string& detail) {
    std::ofstream stream(directory / L"runtime-status.json", std::ios::binary | std::ios::trunc);
    stream << "{\"state\":" << jsonString(state) << ",\"detail\":" << jsonString(detail) << "}";
}
}
