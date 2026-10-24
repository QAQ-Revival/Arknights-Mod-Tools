#include "manifest.hpp"
#include <MinHook.h>
#include <atomic>
#include <memory>
#include <cstdint>
#include <stdexcept>

namespace {
using LoadBundle = void* (__fastcall*)(void*, uint32_t, uint64_t);
using DomainGet = void* (*)();
using ThreadAttach = void* (*)(void*);
using ResolveIcall = void* (*)(const char*);
using StringChars = const wchar_t* (*)(void*);
using StringLength = int32_t (*)(void*);
using StringNew = void* (*)(const wchar_t*, int32_t);
using HandleNew = uint32_t (*)(void*, bool);
using HandleTarget = void* (*)(uint32_t);
LoadBundle originalSync = nullptr, originalAsync = nullptr;
StringChars stringChars = nullptr;
StringLength stringLength = nullptr;
HandleTarget handleTarget = nullptr;
struct Target { uint32_t handle; ark::fs::path file; };
std::map<std::wstring, Target> targets;
ark::fs::path statusDirectory;

void* redirect(LoadBundle original, void* input, uint32_t crc, uint64_t offset) {
    // A generated bundle is a complete file. Packed-file offsets are unsupported.
    if (!input || offset != 0) return original(input, crc, offset);
    try {
        const auto count = stringLength(input);
        if (count <= 0 || count > 32760) return original(input, crc, offset);
        auto candidate = ark::fs::path(std::wstring(stringChars(input), count));
        auto found = targets.find(ark::normalized(candidate));
        if (found == targets.end() || !ark::fs::is_regular_file(found->second.file) || ark::hasLink(found->second.file)) return original(input, crc, offset);
        auto replacement = handleTarget(found->second.handle);
        if (!replacement) return original(input, crc, offset);
        // The original CRC describes the original file. The replacement is
        // SHA-256 checked by the launcher and must be loaded as its own bundle.
        return original(replacement, 0, 0);
    } catch (...) { return original(input, crc, offset); }
}
void* __fastcall loadSync(void* file, uint32_t crc, uint64_t offset) { return redirect(originalSync, file, crc, offset); }
void* __fastcall loadAsync(void* file, uint32_t crc, uint64_t offset) { return redirect(originalAsync, file, crc, offset); }
template<class T> T api(HMODULE module, const char* name) {
    auto result = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!result) throw std::runtime_error(std::string("Required IL2CPP export is missing: ") + name);
    return result;
}
DWORD WINAPI initialize(void*) {
    try {
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
        if (_wcsicmp(ark::fs::path(executable).filename().c_str(), L"Arknights.exe")) return 0;
        wchar_t location[32768]{};
        auto length = GetEnvironmentVariableW(L"QAQR_ARKNIGHTS_MANIFEST", location, 32768);
        if (!length || length >= 32768) return 0;
        const auto manifest = ark::readManifest(location, true);
        if (ark::normalized(manifest.game) != ark::normalized(executable)) return 0;
        statusDirectory = manifest.file.parent_path();
        if (manifest.entries.empty()) { ark::writeStatus(statusDirectory, "idle", "No bundle replacements enabled"); return 0; }
        HMODULE assembly = nullptr;
        for (int attempt = 0; attempt < 600 && !assembly; ++attempt) { assembly = GetModuleHandleW(L"GameAssembly.dll"); if (!assembly) Sleep(100); }
        if (!assembly) throw std::runtime_error("GameAssembly.dll did not load");
        auto domainGet = api<DomainGet>(assembly, "il2cpp_domain_get");
        auto attach = api<ThreadAttach>(assembly, "il2cpp_thread_attach");
        void* domain = nullptr;
        for (int attempt = 0; attempt < 600 && !domain; ++attempt) { domain = domainGet(); if (!domain) Sleep(100); }
        if (!domain || !attach(domain)) throw std::runtime_error("IL2CPP runtime is not ready");
        auto resolve = api<ResolveIcall>(assembly, "il2cpp_resolve_icall");
        stringChars = api<StringChars>(assembly, "il2cpp_string_chars");
        stringLength = api<StringLength>(assembly, "il2cpp_string_length");
        auto createString = api<StringNew>(assembly, "il2cpp_string_new_utf16");
        auto createHandle = api<HandleNew>(assembly, "il2cpp_gchandle_new");
        handleTarget = api<HandleTarget>(assembly, "il2cpp_gchandle_get_target");
        void* sync = resolve("UnityEngine.AssetBundle::LoadFromFile_Internal(System.String,System.UInt32,System.UInt64)");
        void* async = resolve("UnityEngine.AssetBundle::LoadFromFileAsync_Internal(System.String,System.UInt32,System.UInt64)");
        if (!sync || !async || sync == async) throw std::runtime_error("Supported Unity AssetBundle file interfaces were not found");
        for (const auto& entry : manifest.entries) {
            auto name = entry.replacement.wstring();
            auto value = createString(name.data(), static_cast<int32_t>(name.size()));
            auto handle = value ? createHandle(value, true) : 0;
            if (!handle) throw std::runtime_error("Could not retain a replacement resource path");
            targets.emplace(ark::normalized(entry.source), Target{handle, entry.replacement});
        }
        if (MH_Initialize() != MH_OK) throw std::runtime_error("Hook initialization failed");
        if (MH_CreateHook(sync, reinterpret_cast<void*>(&loadSync), reinterpret_cast<void**>(&originalSync)) != MH_OK ||
            MH_CreateHook(async, reinterpret_cast<void*>(&loadAsync), reinterpret_cast<void**>(&originalAsync)) != MH_OK) {
            MH_Uninitialize(); throw std::runtime_error("Hook creation failed");
        }
        if (MH_QueueEnableHook(sync) != MH_OK || MH_QueueEnableHook(async) != MH_OK || MH_ApplyQueued() != MH_OK) {
            MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize(); throw std::runtime_error("Hook activation failed");
        }
        ark::writeStatus(statusDirectory, "active", "AssetBundle file redirection active; in-game appearance is not verified by this status");
    } catch (const std::exception& error) {
        if (!statusDirectory.empty()) ark::writeStatus(statusDirectory, "unsupported", error.what());
    }
    return 0;
}
}
extern "C" __declspec(dllexport) const char* QAQR_RuntimeProtocol() { return "qaqr-arknights-bundles-1"; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, initialize, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
