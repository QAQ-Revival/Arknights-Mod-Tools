#include "manifest.hpp"
#include "reload-ipc.hpp"
#include <tlhelp32.h>
#include <iostream>
#include <stdexcept>

namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE v) : value(v) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
bool gameRunning() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot enumerate processes");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.value, &entry)) do { if (!_wcsicmp(entry.szExeFile, L"Arknights.exe")) return true; } while (Process32NextW(snapshot.value, &entry));
    return false;
}
DWORD gameProcess(const ark::fs::path& game) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot enumerate processes");
    DWORD found = 0;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.value, &entry)) do {
        if (_wcsicmp(entry.szExeFile, L"Arknights.exe")) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        wchar_t location[32768]{}; DWORD length = 32768;
        if (!process.value || !QueryFullProcessImageNameW(process.value, 0, location, &length) || ark::normalized(location) != ark::normalized(game)) continue;
        if (found) throw std::runtime_error("Multiple matching Arknights processes are running");
        found = entry.th32ProcessID;
    } while (Process32NextW(snapshot.value, &entry));
    if (!found) throw std::runtime_error("Start Arknights using QAQR before reloading a Mod");
    return found;
}
std::vector<wchar_t> environment(const std::wstring& manifest) {
    std::map<std::wstring, std::wstring> values;
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) throw std::runtime_error("Cannot read process environment");
    for (const wchar_t* item = block; *item; item += wcslen(item) + 1) {
        std::wstring line(item); auto separator = line.find(L'=', line[0] == L'=' ? 1 : 0);
        if (separator != std::wstring::npos && _wcsicmp(line.substr(0, separator).c_str(), L"QAQR_ARKNIGHTS_MANIFEST")) values.emplace(line.substr(0, separator), line.substr(separator + 1));
    }
    FreeEnvironmentStringsW(block);
    values[L"QAQR_ARKNIGHTS_MANIFEST"] = manifest;
    std::vector<wchar_t> output;
    for (const auto& [key, value] : values) { auto line = key + L"=" + value; output.insert(output.end(), line.begin(), line.end()); output.push_back(0); }
    output.push_back(0); return output;
}
LPTHREAD_START_ROUTINE remoteLoadLibrary(DWORD pid) {
    auto function = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner = nullptr;
    if (!function || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(function), &owner)) throw std::runtime_error("LoadLibraryW is unavailable");
    wchar_t file[32768]{}; GetModuleFileNameW(owner, file, 32768);
    auto name = ark::fs::path(file).filename().wstring();
    auto offset = reinterpret_cast<uintptr_t>(function) - reinterpret_cast<uintptr_t>(owner);
    for (int attempt = 0; attempt < 100; ++attempt) {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if (snapshot.value != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
            if (Module32FirstW(snapshot.value, &entry)) do {
                if (!_wcsicmp(entry.szModule, name.c_str())) return reinterpret_cast<LPTHREAD_START_ROUTINE>(entry.modBaseAddr + offset);
            } while (Module32NextW(snapshot.value, &entry));
        }
        Sleep(50);
    }
    throw std::runtime_error("Cannot resolve the new process loader; no fallback is attempted");
}
DWORD launch(const ark::Manifest& manifest, const ark::fs::path& runtime) {
    Handle mutex(CreateMutexW(nullptr, TRUE, L"Local\\QAQR_Arknights_Source_Launch"));
    if (!mutex.value || GetLastError() == ERROR_ALREADY_EXISTS || gameRunning()) throw std::runtime_error("Close Arknights before changing or launching Mods");
    auto env = environment(manifest.file.wstring());
    auto command = L"\"" + manifest.game.wstring() + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(manifest.game.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env.data(), manifest.game.parent_path().c_str(), &startup, &process)) throw std::runtime_error("Could not start the selected game");
    Handle game(process.hProcess), mainThread(process.hThread);
    const auto dll = runtime.wstring(); const auto size = (dll.size() + 1) * sizeof(wchar_t);
    void* memory = VirtualAllocEx(game.value, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory) throw std::runtime_error("The game started but runtime loading was denied");
    bool completed = true;
    try {
        SIZE_T written = 0;
        if (!WriteProcessMemory(game.value, memory, dll.c_str(), size, &written) || written != size) throw std::runtime_error("Runtime path transfer failed");
        auto loader = remoteLoadLibrary(process.dwProcessId);
        Handle thread(CreateRemoteThread(game.value, nullptr, 0, loader, memory, 0, nullptr));
        if (!thread.value) { completed = true; throw std::runtime_error("Runtime loading was denied; no protection bypass is implemented"); }
        completed = false;
        if (WaitForSingleObject(thread.value, 15000) != WAIT_OBJECT_0) throw std::runtime_error("Runtime loading timed out");
        completed = true; DWORD exit = 0;
        if (!GetExitCodeThread(thread.value, &exit) || !exit) throw std::runtime_error("Runtime library did not load");
    } catch (...) { if (completed) VirtualFreeEx(game.value, memory, 0, MEM_RELEASE); throw; }
    VirtualFreeEx(game.value, memory, 0, MEM_RELEASE);
    return process.dwProcessId;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--status") { std::cout << "{\"success\":true,\"running\":" << (gameRunning() ? "true" : "false") << "}"; return 0; }
        if ((argc == 4 || argc == 6) && (std::wstring(argv[1]) == L"--reload" || std::wstring(argv[1]) == L"--runtime-status") && std::wstring(argv[2]) == L"--manifest") {
            const auto manifest = ark::readManifest(argv[3], false);
            const bool reload = std::wstring(argv[1]) == L"--reload";
            if (reload && !manifest.partial) throw std::runtime_error("Live reload requires a scoped patch manifest");
            const auto pid = gameProcess(manifest.game);
            if (argc == 6 && (std::wstring(argv[4]) != L"--pid" || std::to_wstring(pid) != argv[5])) throw std::runtime_error("The game restarted while the reload was being prepared; retry the Mod reload");
            std::cout << ark::runtimeRequest(pid, reload ? manifest.file.wstring() : L"");
            return 0;
        }
        if (argc != 4 || (std::wstring(argv[1]) != L"--check" && std::wstring(argv[1]) != L"--launch") || std::wstring(argv[2]) != L"--manifest") throw std::runtime_error("Usage: ArkLoader --check|--launch|--reload|--runtime-status --manifest <manifest.tsv>");
        const auto manifest = ark::readManifest(argv[3], true);
        if (manifest.partial && std::wstring(argv[1]) == L"--launch") throw std::runtime_error("Cannot launch from a partial reload patch");
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
        auto runtime = ark::fs::path(executable).parent_path() / L"ArkRuntime.dll";
        if (!ark::fs::is_regular_file(runtime) || ark::hasLink(runtime)) throw std::runtime_error("Built runtime DLL is missing");
        if (std::wstring(argv[1]) == L"--check") {
            std::cout << "{\"success\":true,\"protocol\":1,\"bundles\":" << manifest.entries.size() << ",\"gameStarted\":false}";
        } else {
            auto pid = launch(manifest, runtime);
            std::cout << "{\"success\":true,\"pid\":" << pid << ",\"runtimePending\":true}";
        }
        return 0;
    } catch (const std::exception& error) { std::cout << "{\"success\":false,\"error\":" << ark::jsonString(error.what()) << "}"; return 1; }
}
