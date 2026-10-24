#pragma once
#include <windows.h>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ark {
namespace fs = std::filesystem;
struct Entry { fs::path source, replacement; std::string sourceHash, replacementHash; };
struct Manifest { fs::path file, game; std::vector<Entry> entries; };
std::wstring normalized(const fs::path& file);
bool within(const fs::path& file, const fs::path& root);
bool hasLink(const fs::path& file);
std::string sha256(const fs::path& file);
Manifest readManifest(const fs::path& file, bool verifyHashes);
std::string jsonString(const std::string& value);
std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
void writeStatus(const fs::path& directory, const std::string& state, const std::string& detail);
}
